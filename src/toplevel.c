/* Script function and purpose: zwlr_foreign_toplevel_management_v1 client --
one record per open window, kept current from the handle events and published
to the owner only at the protocol's `done` commit point. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <glib.h>
#include <wayland-client.h>

#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"

#include <saber/toplevel.h>

struct saber_toplevels {
  struct zwlr_foreign_toplevel_manager_v1 *manager;
  struct wl_seat *seat;

  struct wl_list toplevels; /* struct saber_toplevel.link */
  size_t count;

  const struct saber_toplevel_listener *listener;
  void *data;
};

static void
toplevel_free(struct saber_toplevel *toplevel)
{
  struct saber_toplevels *toplevels = toplevel->toplevels;
  struct saber_toplevel *other = NULL;

  /* Action purpose: the protocol emits no parent event when a parent handle
  goes away, so every child must be detached here or it keeps a pointer to
  freed memory. The detachment is silent: the owner is about to be told this
  toplevel closed, which is the same news. */
  wl_list_for_each(other, &toplevels->toplevels, link) {
    if (other->parent == toplevel) {
      other->parent = NULL;
    }
  }

  wl_list_remove(&toplevel->link);
  toplevels->count--;

  zwlr_foreign_toplevel_handle_v1_destroy(toplevel->handle);

  if (toplevel->outputs != NULL) {
    g_ptr_array_free(toplevel->outputs, TRUE);
  }

  g_free(toplevel->title);
  g_free(toplevel->app_id);
  g_free(toplevel);
}

static void
handle_title(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *handle,
    const char *title)
{
  (void)handle;

  struct saber_toplevel *toplevel = data;

  if (g_strcmp0(toplevel->title, title) == 0) {
    return;
  }

  /* Action purpose: Any Wayland client can set this to an arbitrary byte string,
  and it reaches g_utf8_casefold and pango further down. glib's UTF-8 walkers
  document their input as required-valid and read past the terminator on a
  truncated sequence, so the bytes are made valid here, at the one place they
  enter the process, rather than at each consumer. Same for app_id below. */
  g_free(toplevel->title);
  toplevel->title = title != NULL ? g_utf8_make_valid(title, -1) : NULL;
  toplevel->pending |= SABER_TOPLEVEL_CHANGE_TITLE;
}

static void
handle_app_id(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *handle,
    const char *app_id)
{
  (void)handle;

  struct saber_toplevel *toplevel = data;

  if (g_strcmp0(toplevel->app_id, app_id) == 0) {
    return;
  }

  g_free(toplevel->app_id);
  toplevel->app_id = app_id != NULL ? g_utf8_make_valid(app_id, -1) : NULL;
  toplevel->pending |= SABER_TOPLEVEL_CHANGE_APP_ID;
}

static void
handle_output_enter(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *handle,
    struct wl_output *output)
{
  (void)handle;

  struct saber_toplevel *toplevel = data;

  if (saber_toplevel_on_output(toplevel, output)) {
    return;
  }

  g_ptr_array_add(toplevel->outputs, output);
  toplevel->pending |= SABER_TOPLEVEL_CHANGE_OUTPUTS;
}

static void
handle_output_leave(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *handle,
    struct wl_output *output)
{
  (void)handle;

  struct saber_toplevel *toplevel = data;

  if (g_ptr_array_remove(toplevel->outputs, output)) {
    toplevel->pending |= SABER_TOPLEVEL_CHANGE_OUTPUTS;
  }
}

static void
handle_state(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *handle,
    struct wl_array *state)
{
  (void)handle;

  struct saber_toplevel *toplevel = data;
  uint32_t states = 0;
  uint32_t *entry = NULL;

  /* The event carries the whole state, not a delta, so anything absent from
  the array is off. */
  wl_array_for_each(entry, state) {
    switch (*entry) {
      case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED:
        states |= SABER_TOPLEVEL_MAXIMIZED;
        break;
      case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED:
        states |= SABER_TOPLEVEL_MINIMIZED;
        break;
      case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED:
        states |= SABER_TOPLEVEL_ACTIVATED;
        break;
      case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN:
        states |= SABER_TOPLEVEL_FULLSCREEN;
        break;
      default:
        break;
    }
  }

  if (states == toplevel->states) {
    return;
  }

  toplevel->states = states;
  toplevel->pending |= SABER_TOPLEVEL_CHANGE_STATE;
}

static void
handle_parent(void *data,
    struct zwlr_foreign_toplevel_handle_v1 *handle,
    struct zwlr_foreign_toplevel_handle_v1 *parent)
{
  (void)handle;

  struct saber_toplevel *toplevel = data;
  struct saber_toplevel *found =
      parent != NULL ? saber_toplevels_find(toplevel->toplevels, parent) : NULL;

  if (found == toplevel->parent) {
    return;
  }

  toplevel->parent = found;
  toplevel->pending |= SABER_TOPLEVEL_CHANGE_PARENT;
}

/* Function purpose: The commit point. Every other event only stages a field;
nothing outside this file is told anything until `done`, or the panel would
repaint on a half-applied update -- a title without its new app_id, a state
without the output it moved to. */
static void
handle_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
  (void)handle;

  struct saber_toplevel *toplevel = data;
  struct saber_toplevels *toplevels = toplevel->toplevels;
  uint32_t changes = toplevel->pending;

  toplevel->pending = 0;

  if (!toplevel->announced) {
    toplevel->announced = true;

    if (toplevels->listener != NULL && toplevels->listener->added != NULL) {
      toplevels->listener->added(toplevels->data, toplevel);
    }

    return;
  }

  /* Action purpose: SABER_TOPLEVEL_CHANGE_STATE across the toplevel set is the
  only signal a sheet switch produces -- hiding the views of the sheet being
  left is published as their minimized bits. A caller wanting that edge trigger
  hooks this mask; it is not interpreted here. */
  if (changes != 0 && toplevels->listener != NULL &&
      toplevels->listener->changed != NULL) {
    toplevels->listener->changed(toplevels->data, toplevel, changes);
  }
}

static void
handle_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle)
{
  (void)handle;

  struct saber_toplevel *toplevel = data;
  struct saber_toplevels *toplevels = toplevel->toplevels;

  /* Announced only when a `done` has been seen; a toplevel closed before its
  first `done` was never published, so there is nothing to withdraw. */
  if (toplevel->announced && toplevels->listener != NULL &&
      toplevels->listener->closed != NULL) {
    toplevels->listener->closed(toplevels->data, toplevel);
  }

  toplevel_free(toplevel);
}

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
  .title = handle_title,
  .app_id = handle_app_id,
  .output_enter = handle_output_enter,
  .output_leave = handle_output_leave,
  .state = handle_state,
  .done = handle_done,
  .closed = handle_closed,
  .parent = handle_parent,
};

static void
manager_handle_toplevel(void *data,
    struct zwlr_foreign_toplevel_manager_v1 *manager,
    struct zwlr_foreign_toplevel_handle_v1 *handle)
{
  (void)manager;

  struct saber_toplevels *toplevels = data;
  struct saber_toplevel *toplevel = g_new0(struct saber_toplevel, 1);

  toplevel->toplevels = toplevels;
  toplevel->handle = handle;
  toplevel->outputs = g_ptr_array_new();

  wl_list_insert(toplevels->toplevels.prev, &toplevel->link);
  toplevels->count++;

  zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener,
      toplevel);
}

static void
manager_handle_finished(void *data,
    struct zwlr_foreign_toplevel_manager_v1 *manager)
{
  (void)manager;

  struct saber_toplevels *toplevels = data;

  /* Action purpose: the server destroys the manager the moment it sends this,
  so the proxy must go with it. The handles it created stay live and keep
  reporting until each sends its own `closed`. */
  if (toplevels->manager != NULL) {
    zwlr_foreign_toplevel_manager_v1_destroy(toplevels->manager);
    toplevels->manager = NULL;
  }
}

static const struct zwlr_foreign_toplevel_manager_v1_listener
    manager_listener = {
      .toplevel = manager_handle_toplevel,
      .finished = manager_handle_finished,
    };

struct saber_toplevels *
saber_toplevels_create(struct zwlr_foreign_toplevel_manager_v1 *manager,
    const struct saber_toplevel_listener *listener,
    void *data)
{
  if (manager == NULL) {
    return NULL;
  }

  struct saber_toplevels *toplevels = g_new0(struct saber_toplevels, 1);

  toplevels->manager = manager;
  toplevels->listener = listener;
  toplevels->data = data;
  wl_list_init(&toplevels->toplevels);

  zwlr_foreign_toplevel_manager_v1_add_listener(manager, &manager_listener,
      toplevels);

  return toplevels;
}

void
saber_toplevels_destroy(struct saber_toplevels *toplevels)
{
  if (toplevels == NULL) {
    return;
  }

  struct saber_toplevel *toplevel = NULL;
  struct saber_toplevel *tmp = NULL;

  wl_list_for_each_safe(toplevel, tmp, &toplevels->toplevels, link) {
    toplevel_free(toplevel);
  }

  if (toplevels->manager != NULL) {
    zwlr_foreign_toplevel_manager_v1_stop(toplevels->manager);
    zwlr_foreign_toplevel_manager_v1_destroy(toplevels->manager);
  }

  g_free(toplevels);
}

void
saber_toplevels_set_seat(struct saber_toplevels *toplevels,
    struct wl_seat *seat)
{
  toplevels->seat = seat;
}

const struct wl_list *
saber_toplevels_list(const struct saber_toplevels *toplevels)
{
  return &toplevels->toplevels;
}

size_t
saber_toplevels_count(const struct saber_toplevels *toplevels)
{
  return toplevels->count;
}

struct saber_toplevel *
saber_toplevels_find(const struct saber_toplevels *toplevels,
    struct zwlr_foreign_toplevel_handle_v1 *handle)
{
  struct saber_toplevel *toplevel = NULL;

  wl_list_for_each(toplevel, &toplevels->toplevels, link) {
    if (toplevel->handle == handle) {
      return toplevel;
    }
  }

  return NULL;
}

struct saber_toplevel *
saber_toplevels_activated(const struct saber_toplevels *toplevels)
{
  struct saber_toplevel *toplevel = NULL;

  wl_list_for_each(toplevel, &toplevels->toplevels, link) {
    if (saber_toplevel_has_state(toplevel, SABER_TOPLEVEL_ACTIVATED)) {
      return toplevel;
    }
  }

  return NULL;
}

void
saber_toplevel_activate(struct saber_toplevel *toplevel)
{
  struct wl_seat *seat = toplevel->toplevels->seat;

  if (seat != NULL) {
    zwlr_foreign_toplevel_handle_v1_activate(toplevel->handle, seat);
  }
}

void
saber_toplevel_close(struct saber_toplevel *toplevel)
{
  zwlr_foreign_toplevel_handle_v1_close(toplevel->handle);
}

void
saber_toplevel_set_minimized(struct saber_toplevel *toplevel)
{
  zwlr_foreign_toplevel_handle_v1_set_minimized(toplevel->handle);
}

void
saber_toplevel_unset_minimized(struct saber_toplevel *toplevel)
{
  zwlr_foreign_toplevel_handle_v1_unset_minimized(toplevel->handle);
}

void
saber_toplevel_set_maximized(struct saber_toplevel *toplevel)
{
  zwlr_foreign_toplevel_handle_v1_set_maximized(toplevel->handle);
}

void
saber_toplevel_unset_maximized(struct saber_toplevel *toplevel)
{
  zwlr_foreign_toplevel_handle_v1_unset_maximized(toplevel->handle);
}

static bool
supports_fullscreen(const struct saber_toplevel *toplevel)
{
  return zwlr_foreign_toplevel_handle_v1_get_version(toplevel->handle) >=
      ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_SET_FULLSCREEN_SINCE_VERSION;
}

void
saber_toplevel_set_fullscreen(struct saber_toplevel *toplevel,
    struct wl_output *output)
{
  if (supports_fullscreen(toplevel)) {
    zwlr_foreign_toplevel_handle_v1_set_fullscreen(toplevel->handle, output);
  }
}

void
saber_toplevel_unset_fullscreen(struct saber_toplevel *toplevel)
{
  if (supports_fullscreen(toplevel)) {
    zwlr_foreign_toplevel_handle_v1_unset_fullscreen(toplevel->handle);
  }
}
