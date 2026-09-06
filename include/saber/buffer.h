/* Script function and purpose: Double-buffered wl_shm pool backed by POSIX
shared memory, with a cairo image surface mapped over each slot. */

#if !defined(SABER_BUFFER_H)
#define SABER_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

#include <cairo.h>
#include <wayland-client.h>

/* Two slots: one on screen, one being painted. A third would only add latency
between paint and presentation. */
#define SABER_BUFFER_SLOTS 2

struct saber_buffer_pool;

struct saber_buffer {
  struct saber_buffer_pool *pool;
  struct wl_buffer *wl_buffer;
  cairo_surface_t *surface;
  unsigned char *data;
  int width, height, stride;
  bool busy;
};

void
saber_buffer_pool_init(struct saber_buffer_pool *pool, struct wl_shm *shm);

void
saber_buffer_pool_fini(struct saber_buffer_pool *pool);

/* Function purpose: Hand back a free slot sized width x height, recreating the
shared mapping when the geometry changed. Returns NULL when every slot is still
held by the compositor or when the mapping could not be made -- the caller must
skip the frame rather than paint into a buffer that is being scanned out. */
struct saber_buffer *
saber_buffer_pool_acquire(struct saber_buffer_pool *pool,
    int width,
    int height);

/* Function purpose: Mark a slot as owned by the compositor. Call once the
buffer has been attached and the surface committed; wl_buffer.release clears it
again. */
void
saber_buffer_submit(struct saber_buffer *buffer);

/* Function purpose: Called when the compositor releases a slot. Without it an
owner that was refused a buffer -- both slots held, or a resize while one is
still on screen -- has nothing to wake it, and the pending repaint stalls until
the next unrelated damage. */
void
saber_buffer_pool_set_release_handler(struct saber_buffer_pool *pool,
    void (*handler)(void *data),
    void *data);

/* Action purpose: The pool is defined here, not hidden, so an owner can embed
it in its own struct and avoid a second allocation per surface. Its fields are
private to buffer.c. */
struct saber_buffer_pool {
  struct wl_shm *shm;
  struct wl_shm_pool *shm_pool;
  unsigned char *data;
  size_t size;
  int width, height, stride;
  struct saber_buffer slots[SABER_BUFFER_SLOTS];
  void (*on_release)(void *data);
  void *release_data;
};

#endif
