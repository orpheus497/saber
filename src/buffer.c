/* Script function and purpose: wl_shm buffer pool. Anonymous shared memory,
one mapping split into SABER_BUFFER_SLOTS equal slots, a cairo image surface
over each. Resize destroys and rebuilds; release tracking keeps the compositor
and the painter off the same bytes. */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>

#include <saber/buffer.h>

/* Action purpose: FreeBSD's SHM_ANON creates an unnamed object with no window
in which another process could open it by name. The named fallback exists only
so the file still compiles where SHM_ANON is absent, and unlinks immediately
for the same reason. */
static int
shm_fd_create(size_t size)
{
  int fd = -1;

#if defined(SHM_ANON)
  fd = shm_open(SHM_ANON, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#else
  for (int attempt = 0; attempt < 16 && fd < 0; attempt++) {
    struct timespec now;
    char name[64];

    clock_gettime(CLOCK_MONOTONIC, &now);
    snprintf(name,
        sizeof(name),
        "/saber-%d-%ld-%d",
        (int)getpid(),
        (long)now.tv_nsec,
        attempt);

    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

    if (fd >= 0) {
      shm_unlink(name);
    } else if (errno != EEXIST) {
      break;
    }
  }
#endif

  if (fd < 0) {
    return -1;
  }

  while (ftruncate(fd, (off_t)size) < 0) {
    if (errno != EINTR) {
      close(fd);
      return -1;
    }
  }

  return fd;
}

static void
buffer_release(void *data, struct wl_buffer *wl_buffer)
{
  (void)wl_buffer;

  struct saber_buffer *buffer = data;
  struct saber_buffer_pool *pool = buffer->pool;

  buffer->busy = false;

  if (pool != NULL && pool->on_release != NULL) {
    pool->on_release(pool->release_data);
  }
}

static const struct wl_buffer_listener buffer_listener = {
  .release = buffer_release,
};

static void
pool_teardown(struct saber_buffer_pool *pool)
{
  for (int i = 0; i < SABER_BUFFER_SLOTS; i++) {
    struct saber_buffer *slot = &pool->slots[i];

    if (slot->surface != NULL) {
      cairo_surface_destroy(slot->surface);
    }

    if (slot->wl_buffer != NULL) {
      /* Action purpose: A slot still marked busy is attached to a surface and
      the compositor has not released it. Destroying it here leaves that
      surface's last frame reading undefined content -- the acquire path
      already refuses to reuse a busy slot, and teardown is the one path that
      does not check. Nothing can be done about it here: the surface is going
      away and the pool with it. Recorded so the asymmetry with
      saber_buffer_pool_acquire is deliberate and visible, rather than looking
      like an oversight to the next reader. */
      wl_buffer_destroy(slot->wl_buffer);
    }

    memset(slot, 0, sizeof(*slot));
  }

  if (pool->shm_pool != NULL) {
    wl_shm_pool_destroy(pool->shm_pool);
    pool->shm_pool = NULL;
  }

  if (pool->data != NULL) {
    munmap(pool->data, pool->size);
    pool->data = NULL;
  }

  pool->size = 0;
  pool->width = 0;
  pool->height = 0;
  pool->stride = 0;
}

static bool
pool_build(struct saber_buffer_pool *pool, int width, int height)
{
  /* Action purpose: cairo's own stride rule, not width * 4. Some
  architectures want more alignment than four bytes and cairo will render
  through the stride it was given, so the wl_shm_pool must be laid out with
  the same one or every row after the first lands in the wrong place. */
  int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, width);

  if (stride <= 0 || height <= 0) {
    return false;
  }

  size_t slot_size = (size_t)stride * (size_t)height;
  size_t total = slot_size * SABER_BUFFER_SLOTS;

  /* Action purpose: wl_shm_create_pool and wl_shm_pool_create_buffer both take
  int32_t, and the pool offset for the last slot is an int32_t too. The
  multiplications above are done in size_t so they cannot wrap, but the casts
  below can -- a full-output OVERLAY surface on a very large or heavily scaled
  output would hand the compositor a negative size, which is a protocol error at
  best. Refuse the allocation instead of truncating it. */
  if (total > (size_t)INT32_MAX) {
    return false;
  }

  int fd = shm_fd_create(total);

  if (fd < 0) {
    return false;
  }

  void *data = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (data == MAP_FAILED) {
    close(fd);
    return false;
  }

  pool->shm_pool = wl_shm_create_pool(pool->shm, fd, (int32_t)total);
  close(fd);

  if (pool->shm_pool == NULL) {
    munmap(data, total);
    return false;
  }

  pool->data = data;
  pool->size = total;
  pool->width = width;
  pool->height = height;
  pool->stride = stride;

  for (int i = 0; i < SABER_BUFFER_SLOTS; i++) {
    struct saber_buffer *slot = &pool->slots[i];

    slot->pool = pool;
    slot->data = pool->data + slot_size * (size_t)i;
    slot->width = width;
    slot->height = height;
    slot->stride = stride;
    slot->busy = false;

    slot->wl_buffer = wl_shm_pool_create_buffer(pool->shm_pool,
        (int32_t)(slot_size * (size_t)i),
        width,
        height,
        stride,
        WL_SHM_FORMAT_ARGB8888);

    slot->surface = cairo_image_surface_create_for_data(slot->data,
        CAIRO_FORMAT_ARGB32,
        width,
        height,
        stride);

    if (slot->wl_buffer == NULL || slot->surface == NULL ||
        cairo_surface_status(slot->surface) != CAIRO_STATUS_SUCCESS) {
      pool_teardown(pool);
      return false;
    }

    wl_buffer_add_listener(slot->wl_buffer, &buffer_listener, slot);
  }

  return true;
}

void
saber_buffer_pool_init(struct saber_buffer_pool *pool, struct wl_shm *shm)
{
  memset(pool, 0, sizeof(*pool));
  pool->shm = shm;
}

void
saber_buffer_pool_set_release_handler(struct saber_buffer_pool *pool,
    void (*handler)(void *data),
    void *data)
{
  pool->on_release = handler;
  pool->release_data = data;
}

void
saber_buffer_pool_fini(struct saber_buffer_pool *pool)
{
  pool_teardown(pool);
  pool->shm = NULL;
  pool->on_release = NULL;
  pool->release_data = NULL;
}

struct saber_buffer *
saber_buffer_pool_acquire(struct saber_buffer_pool *pool, int width, int height)
{
  if (pool->shm == NULL || width <= 0 || height <= 0) {
    return NULL;
  }

  if (pool->data == NULL || pool->width != width || pool->height != height) {
    /* Action purpose: A slot the compositor still holds cannot be unmapped
    from under it. Refusing the frame is correct: the surface is about to be
    repainted at the new size anyway, and the next commit after the release
    arrives will rebuild the pool. */
    for (int i = 0; i < SABER_BUFFER_SLOTS; i++) {
      if (pool->slots[i].busy) {
        return NULL;
      }
    }

    pool_teardown(pool);

    if (!pool_build(pool, width, height)) {
      return NULL;
    }
  }

  for (int i = 0; i < SABER_BUFFER_SLOTS; i++) {
    if (!pool->slots[i].busy) {
      return &pool->slots[i];
    }
  }

  return NULL;
}

void
saber_buffer_submit(struct saber_buffer *buffer)
{
  buffer->busy = true;
}
