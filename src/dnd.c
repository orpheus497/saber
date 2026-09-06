/* Script function and purpose: Drag-and-drop destination. Binds the seat's
wl_data_device, negotiates text/uri-list with whatever is being dragged over the
panel, pulls the data down a pipe on the GLib loop and hands the parsed URIs to
its owner. The protocol half only -- nothing here launches anything. */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <glib-unix.h>
#include <glib.h>
#include <wayland-client.h>

#include <saber/dnd.h>

/* A source that never closes its end would otherwise pin the fd and the offer
for the life of the session. */
#define SABER_DND_READ_TIMEOUT_MS 5000
#define SABER_DND_READ_CHUNK 4096

struct saber_dnd_offer {
  struct wl_data_offer *offer;
  uint32_t source_actions; /* enum wl_data_device_manager_dnd_action */
  uint32_t action;         /* the one the compositor settled on */
  bool uri_list;           /* the source offers text/uri-list */
  bool accepted;           /* the last accept named a mime type, not NULL */
};

struct saber_dnd_read {
  struct saber_dnd *dnd;
  struct saber_dnd_offer *offer;

  struct wl_surface *surface;
  double x, y;

  int fd;
  guint io;
  guint timeout;
  GString *buffer;
};

struct saber_dnd {
  struct saber_display *display;
  struct wl_data_device *device;

  const struct saber_dnd_listener *listener;
  void *data;

  struct saber_dnd_offer *pending; /* introduced, not yet claimed */
  struct saber_dnd_offer *current; /* claimed by the drag over our surface */

  struct wl_surface *surface;
  double x, y;
  uint32_t enter_serial;
  bool inside;

  GSList *reads; /* struct saber_dnd_read *, transfers in flight */
};

/* -- offers -------------------------------------------------------------- */

/* Function purpose: End an offer. `finished` says the transfer completed, which
is the only case that may send finish -- finishing an offer that was refused, or
one the compositor chose no action for, is a protocol error and kills the
client. Destroy is unconditional: an offer that is never destroyed leaks in both
processes. */
static void
offer_close(struct saber_dnd_offer *self, bool finished)
{
  if (finished && self->accepted &&
      self->action != WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE &&
      wl_data_offer_get_version(self->offer) >=
          WL_DATA_OFFER_FINISH_SINCE_VERSION) {
    wl_data_offer_finish(self->offer);
  }

  wl_data_offer_destroy(self->offer);
  g_free(self);
}

static void
offer_handle_offer(void *data, struct wl_data_offer *offer, const char *mime)
{
  (void)offer;

  struct saber_dnd_offer *self = data;

  if (strcmp(mime, SABER_DND_MIME_URI_LIST) == 0) {
    self->uri_list = true;
  }
}

static void
offer_handle_source_actions(void *data,
    struct wl_data_offer *offer,
    uint32_t actions)
{
  (void)offer;

  struct saber_dnd_offer *self = data;
  self->source_actions = actions;
}

static void
offer_handle_action(void *data, struct wl_data_offer *offer, uint32_t action)
{
  (void)offer;

  struct saber_dnd_offer *self = data;
  self->action = action;
}

static const struct wl_data_offer_listener offer_listener = {
  .offer = offer_handle_offer,
  .source_actions = offer_handle_source_actions,
  .action = offer_handle_action,
};

/* Below version 3 there are no action events at all, so demanding copy there
would refuse every drag. */
static bool
offer_can_copy(const struct saber_dnd_offer *self)
{
  if (wl_data_offer_get_version(self->offer) <
      WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION) {
    return true;
  }

  return (self->source_actions & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY) != 0;
}

/* -- target feedback ------------------------------------------------------ */

static void
dnd_notify_motion(struct saber_dnd *dnd)
{
  if (dnd->listener != NULL && dnd->listener->motion != NULL) {
    dnd->listener->motion(dnd->data, dnd->inside ? dnd->surface : NULL, dnd->x,
        dnd->y);
  }
}

/* Action purpose: Re-asked on every motion, because whether the drop lands
anywhere useful is a per-tile question. accept() with a NULL mime type is not a
formality: from version 3 it is what decides the outcome, and a source whose
offer is never accepted is told the drag was cancelled. Motion carries no serial
of its own, so the enter serial is quoted throughout, which is what the protocol
expects. */
static void
dnd_update_target(struct saber_dnd *dnd)
{
  struct saber_dnd_offer *offer = dnd->current;

  if (offer == NULL) {
    return;
  }

  bool take = offer->uri_list && offer_can_copy(offer) &&
      dnd->listener != NULL && dnd->listener->accepts != NULL &&
      dnd->listener->accepts(dnd->data, dnd->surface, dnd->x, dnd->y);

  wl_data_offer_accept(offer->offer, dnd->enter_serial,
      take ? SABER_DND_MIME_URI_LIST : NULL);
  offer->accepted = take;

  if (wl_data_offer_get_version(offer->offer) >=
      WL_DATA_OFFER_SET_ACTIONS_SINCE_VERSION) {
    uint32_t actions = take ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY
                            : WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;

    wl_data_offer_set_actions(offer->offer, actions, actions);
  }
}

static void
dnd_drop_current(struct saber_dnd *dnd)
{
  if (dnd->current != NULL) {
    offer_close(dnd->current, false);
    dnd->current = NULL;
  }
}

/* -- the transfer --------------------------------------------------------- */

/* Function purpose: RFC 2483. Lines are CRLF separated and a line whose first
character is '#' is a comment; a bare LF is accepted as well, because that is
what several file managers actually send. Returns a NULL-terminated array, empty
when nothing survived. */
static char **
uri_list_parse(const char *text, size_t length)
{
  GPtrArray *uris = g_ptr_array_new();

  /* Sources that NUL-terminate the payload are common; without this the NUL
  ends up inside the last URI. */
  length = strnlen(text, length);

  const char *end = text + length;
  const char *line = text;

  while (line < end) {
    const char *stop = memchr(line, '\n', (size_t)(end - line));
    const char *tail = stop != NULL ? stop : end;

    while (tail > line &&
        (tail[-1] == '\r' || tail[-1] == ' ' || tail[-1] == '\t')) {
      tail--;
    }

    if (tail > line && *line != '#') {
      g_ptr_array_add(uris, g_strndup(line, (size_t)(tail - line)));
    }

    if (stop == NULL) {
      break;
    }

    line = stop + 1;
  }

  g_ptr_array_add(uris, NULL);

  return (char **)g_ptr_array_free(uris, FALSE);
}

/* Function purpose: Tear a transfer down and end its offer with it. `delivered`
is false for every abandoned path -- timeout, read error, shutdown -- so the
source is told the drop failed rather than being left waiting on a finish that
never comes. The caller clears whichever GLib source id it is running inside
before calling, so this never removes a source that is already unwinding. */
static void
read_dispose(struct saber_dnd_read *transfer, bool delivered)
{
  if (transfer->io != 0) {
    g_source_remove(transfer->io);
    transfer->io = 0;
  }

  if (transfer->timeout != 0) {
    g_source_remove(transfer->timeout);
    transfer->timeout = 0;
  }

  if (transfer->fd >= 0) {
    close(transfer->fd);
    transfer->fd = -1;
  }

  transfer->dnd->reads = g_slist_remove(transfer->dnd->reads, transfer);

  offer_close(transfer->offer, delivered);
  g_string_free(transfer->buffer, TRUE);
  g_free(transfer);
}

static void
read_deliver(struct saber_dnd_read *transfer)
{
  struct saber_dnd *dnd = transfer->dnd;
  char **uris = uri_list_parse(transfer->buffer->str, transfer->buffer->len);
  bool delivered = uris[0] != NULL;

  if (delivered && dnd->listener != NULL && dnd->listener->drop != NULL) {
    dnd->listener->drop(dnd->data, transfer->surface, transfer->x, transfer->y,
        uris);
  }

  g_strfreev(uris);
  read_dispose(transfer, delivered);
}

static gboolean
read_ready(gint fd, GIOCondition condition, gpointer user)
{
  struct saber_dnd_read *transfer = user;
  char chunk[SABER_DND_READ_CHUNK];

  for (;;) {
    ssize_t got = read(fd, chunk, sizeof(chunk));

    if (got > 0) {
      g_string_append_len(transfer->buffer, chunk, got);
      continue;
    }

    if (got == 0) {
      break;
    }

    if (errno == EINTR) {
      continue;
    }

    /* Action purpose: The fd is non-blocking, so EAGAIN only means the source
    has not written the next chunk yet -- unless the other end is also gone, in
    which case there will never be one. */
    if (errno == EAGAIN) {
      if ((condition & (G_IO_HUP | G_IO_ERR)) == 0) {
        return G_SOURCE_CONTINUE;
      }

      break;
    }

    transfer->io = 0;
    read_dispose(transfer, false);

    return G_SOURCE_REMOVE;
  }

  transfer->io = 0;
  read_deliver(transfer);

  return G_SOURCE_REMOVE;
}

static gboolean
read_expired(gpointer user)
{
  struct saber_dnd_read *transfer = user;

  transfer->timeout = 0;
  read_dispose(transfer, false);

  return G_SOURCE_REMOVE;
}

/* Function purpose: Ask for the data and read it on the main loop rather than
spinning here. Takes ownership of `offer` on success. */
static bool
read_start(struct saber_dnd *dnd, struct saber_dnd_offer *offer)
{
  int fds[2];

  /* g_unix_open_pipe rather than pipe2: the flag spellings for the latter are
  not visible in every libc's default namespace. */
  if (!g_unix_open_pipe(fds, FD_CLOEXEC, NULL)) {
    return false;
  }

  if (!g_unix_set_fd_nonblocking(fds[0], TRUE, NULL)) {
    close(fds[0]);
    close(fds[1]);

    return false;
  }

  wl_data_offer_receive(offer->offer, SABER_DND_MIME_URI_LIST, fds[1]);
  close(fds[1]);

  /* The source cannot write until the request actually reaches it, and the
  next flush is a whole main-loop iteration away. */
  saber_display_flush(dnd->display);

  struct saber_dnd_read *transfer = g_new0(struct saber_dnd_read, 1);

  transfer->dnd = dnd;
  transfer->offer = offer;
  transfer->surface = dnd->surface;
  transfer->x = dnd->x;
  transfer->y = dnd->y;
  transfer->fd = fds[0];
  transfer->buffer = g_string_new(NULL);

  dnd->reads = g_slist_prepend(dnd->reads, transfer);

  transfer->io = g_unix_fd_add(fds[0], G_IO_IN | G_IO_HUP | G_IO_ERR,
      read_ready, transfer);
  transfer->timeout = g_timeout_add(SABER_DND_READ_TIMEOUT_MS, read_expired,
      transfer);

  return true;
}

/* -- the data device ------------------------------------------------------ */

static void
device_handle_data_offer(void *data,
    struct wl_data_device *device,
    struct wl_data_offer *offer)
{
  (void)device;

  struct saber_dnd *dnd = data;
  struct saber_dnd_offer *self = g_new0(struct saber_dnd_offer, 1);

  self->offer = offer;
  wl_data_offer_add_listener(offer, &offer_listener, self);

  /* One offer is introduced per enter and per selection, and each is claimed
  by the event that follows it. Anything still sitting here belongs to a
  session that never started. */
  if (dnd->pending != NULL) {
    offer_close(dnd->pending, false);
  }

  dnd->pending = self;
}

static void
device_handle_enter(void *data,
    struct wl_data_device *device,
    uint32_t serial,
    struct wl_surface *surface,
    wl_fixed_t x,
    wl_fixed_t y,
    struct wl_data_offer *offer)
{
  (void)device;

  struct saber_dnd *dnd = data;

  dnd_drop_current(dnd);

  /* A drag started by this client, or one with no source at all, enters with a
  NULL offer: there is a position to report but nothing to accept. */
  struct saber_dnd_offer *entered = offer != NULL
      ? wl_data_offer_get_user_data(offer)
      : NULL;

  if (entered != NULL && entered == dnd->pending) {
    dnd->pending = NULL;
  }

  dnd->current = entered;
  dnd->surface = surface;
  dnd->x = wl_fixed_to_double(x);
  dnd->y = wl_fixed_to_double(y);
  dnd->enter_serial = serial;
  dnd->inside = true;

  dnd_update_target(dnd);
  dnd_notify_motion(dnd);
}

/* Action purpose: `drop` hands the offer to the transfer and clears `current`,
so the leave that follows a drop -- which is the normal order -- finds nothing
to destroy and cannot pull the offer out from under a read that is still
running. Getting that wrong is the classic use-after-free in this protocol. */
static void
device_handle_leave(void *data, struct wl_data_device *device)
{
  (void)device;

  struct saber_dnd *dnd = data;

  dnd_drop_current(dnd);

  dnd->inside = false;
  dnd->surface = NULL;

  dnd_notify_motion(dnd);
}

static void
device_handle_motion(void *data,
    struct wl_data_device *device,
    uint32_t time,
    wl_fixed_t x,
    wl_fixed_t y)
{
  (void)device;
  (void)time;

  struct saber_dnd *dnd = data;

  dnd->x = wl_fixed_to_double(x);
  dnd->y = wl_fixed_to_double(y);

  dnd_update_target(dnd);
  dnd_notify_motion(dnd);
}

static void
device_handle_drop(void *data, struct wl_data_device *device)
{
  (void)device;

  struct saber_dnd *dnd = data;
  struct saber_dnd_offer *offer = dnd->current;

  dnd->current = NULL;

  if (offer != NULL && (!offer->accepted || !read_start(dnd, offer))) {
    offer_close(offer, false);
  }

  dnd->inside = false;
  dnd->surface = NULL;

  dnd_notify_motion(dnd);
}

/* Saber is not a clipboard client, but the offer still arrives and still has to
be destroyed, or every selection change in the session leaks one. */
static void
device_handle_selection(void *data,
    struct wl_data_device *device,
    struct wl_data_offer *offer)
{
  (void)device;

  struct saber_dnd *dnd = data;

  if (offer == NULL) {
    return;
  }

  struct saber_dnd_offer *self = wl_data_offer_get_user_data(offer);

  if (self == NULL) {
    wl_data_offer_destroy(offer);
    return;
  }

  if (self == dnd->pending) {
    dnd->pending = NULL;
  }

  offer_close(self, false);
}

static const struct wl_data_device_listener device_listener = {
  .data_offer = device_handle_data_offer,
  .enter = device_handle_enter,
  .leave = device_handle_leave,
  .motion = device_handle_motion,
  .drop = device_handle_drop,
  .selection = device_handle_selection,
};

/* -- lifecycle ------------------------------------------------------------ */

struct saber_dnd *
saber_dnd_create(struct saber_display *display,
    const struct saber_dnd_listener *listener,
    void *data)
{
  if (display->data_device_manager == NULL || display->seat == NULL) {
    return NULL;
  }

  struct saber_dnd *dnd = g_new0(struct saber_dnd, 1);

  dnd->display = display;
  dnd->listener = listener;
  dnd->data = data;
  dnd->device = wl_data_device_manager_get_data_device(
      display->data_device_manager, display->seat);

  wl_data_device_add_listener(dnd->device, &device_listener, dnd);

  return dnd;
}

void
saber_dnd_destroy(struct saber_dnd *dnd)
{
  if (dnd == NULL) {
    return;
  }

  while (dnd->reads != NULL) {
    read_dispose(dnd->reads->data, false);
  }

  dnd_drop_current(dnd);

  if (dnd->pending != NULL) {
    offer_close(dnd->pending, false);
    dnd->pending = NULL;
  }

  if (wl_data_device_get_version(dnd->device) >=
      WL_DATA_DEVICE_RELEASE_SINCE_VERSION) {
    wl_data_device_release(dnd->device);
  } else {
    wl_data_device_destroy(dnd->device);
  }

  g_free(dnd);
}

bool
saber_dnd_hover(const struct saber_dnd *dnd,
    struct wl_surface **surface,
    double *x,
    double *y)
{
  if (dnd == NULL || !dnd->inside) {
    return false;
  }

  if (surface != NULL) {
    *surface = dnd->surface;
  }

  if (x != NULL) {
    *x = dnd->x;
  }

  if (y != NULL) {
    *y = dnd->y;
  }

  return true;
}
