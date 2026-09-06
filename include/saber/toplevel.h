/* Script function and purpose: The window model -- a client of
zwlr_foreign_toplevel_management_v1. Every window Saber knows of enters through
here and nowhere else; ext-foreign-toplevel-list is deliberately not used, so
this is the single source of truth for the window set. */

#if !defined(SABER_TOPLEVEL_H)
#define SABER_TOPLEVEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <glib.h>
#include <wayland-client.h>

struct zwlr_foreign_toplevel_manager_v1;
struct zwlr_foreign_toplevel_handle_v1;

struct saber_toplevels;

enum saber_toplevel_state {
  SABER_TOPLEVEL_MAXIMIZED = 1u << 0,
  /* Action purpose: On hikari-sakura `minimized` does not mean the user
  minimised the window. Hiding a view because the sheet being looked at changed
  is published as this bit, so a tile whose windows are all "minimized" is
  usually a tile whose windows are on another sheet. Presented, not smoothed
  away: nothing here translates it into anything else. */
  SABER_TOPLEVEL_MINIMIZED = 1u << 1,
  SABER_TOPLEVEL_ACTIVATED = 1u << 2,
  SABER_TOPLEVEL_FULLSCREEN = 1u << 3,
};

/* Which properties the `done` batch actually altered, so a caller can react to
one kind of change without diffing the whole toplevel itself. */
enum saber_toplevel_change {
  SABER_TOPLEVEL_CHANGE_TITLE = 1u << 0,
  SABER_TOPLEVEL_CHANGE_APP_ID = 1u << 1,
  SABER_TOPLEVEL_CHANGE_STATE = 1u << 2,
  SABER_TOPLEVEL_CHANGE_OUTPUTS = 1u << 3,
  SABER_TOPLEVEL_CHANGE_PARENT = 1u << 4,
};

struct saber_toplevel {
  struct wl_list link;
  struct saber_toplevels *toplevels;
  struct zwlr_foreign_toplevel_handle_v1 *handle;

  char *title; /* NULL until the compositor has sent one */

  /* Action purpose: This protocol carries no pid, so there is no
  process-to-window link to be had and no pid path to add later. app_id is the
  whole of what Saber can correlate on; src/match.c does the correlating. */
  char *app_id;

  uint32_t states;    /* enum saber_toplevel_state bitmask */
  GPtrArray *outputs; /* struct wl_output *, borrowed from the display */

  /* NULL when the toplevel has no parent, and cleared when the parent closes:
  the protocol sends no event for a parent going away. */
  struct saber_toplevel *parent;

  bool announced;   /* the first `done` has been delivered */
  uint32_t pending; /* enum saber_toplevel_change, since the last `done` */
};

/* Function purpose: The owner's view of the window set. `changed` carries the
mask of what the batch altered; a caller wanting the sheet edge trigger hooks
`changes & SABER_TOPLEVEL_CHANGE_STATE` and invalidates from there, because a
sheet switch reaches this client only as minimized bits moving. None of these
fire before the toplevel's `done`. */
struct saber_toplevel_listener {
  void (*added)(void *data, struct saber_toplevel *toplevel);
  void (*changed)(void *data,
      struct saber_toplevel *toplevel,
      uint32_t changes);
  /* The toplevel is still valid here and is destroyed as soon as this
  returns. */
  void (*closed)(void *data, struct saber_toplevel *toplevel);
};

/* Function purpose: Start tracking windows on an already-bound manager. The
display module owns registry binding; this takes the proxy and never does a
roundtrip of its own. Takes ownership of `manager`, and returns NULL when it is
NULL -- a compositor without the global leaves the panel with no window set,
which is inert rather than fatal. */
struct saber_toplevels *
saber_toplevels_create(struct zwlr_foreign_toplevel_manager_v1 *manager,
    const struct saber_toplevel_listener *listener,
    void *data);

void
saber_toplevels_destroy(struct saber_toplevels *toplevels);

/* Function purpose: Supply the seat that activation is requested on. Without
one saber_toplevel_activate is a no-op, since the protocol has no seatless
form. */
void
saber_toplevels_set_seat(struct saber_toplevels *toplevels,
    struct wl_seat *seat);

/* Head of a list of struct saber_toplevel.link, in arrival order. */
const struct wl_list *
saber_toplevels_list(const struct saber_toplevels *toplevels);

size_t
saber_toplevels_count(const struct saber_toplevels *toplevels);

struct saber_toplevel *
saber_toplevels_find(const struct saber_toplevels *toplevels,
    struct zwlr_foreign_toplevel_handle_v1 *handle);

/* The activated toplevel, or NULL. Only one can hold the bit at a time. */
struct saber_toplevel *
saber_toplevels_activated(const struct saber_toplevels *toplevels);

static inline bool
saber_toplevel_has_state(const struct saber_toplevel *toplevel, uint32_t state)
{
  return (toplevel->states & state) != 0;
}

static inline bool
saber_toplevel_on_output(const struct saber_toplevel *toplevel,
    const struct wl_output *output)
{
  for (guint i = 0; toplevel->outputs != NULL && i < toplevel->outputs->len;
      i++) {
    if (g_ptr_array_index(toplevel->outputs, i) == output) {
      return true;
    }
  }

  return false;
}

/* Every request below is a request: the compositor answers, if it answers at
all, with a state event and a `done`. Nothing here updates local state. */
void
saber_toplevel_activate(struct saber_toplevel *toplevel);

void
saber_toplevel_close(struct saber_toplevel *toplevel);

void
saber_toplevel_set_minimized(struct saber_toplevel *toplevel);

void
saber_toplevel_unset_minimized(struct saber_toplevel *toplevel);

void
saber_toplevel_set_maximized(struct saber_toplevel *toplevel);

void
saber_toplevel_unset_maximized(struct saber_toplevel *toplevel);

/* `output` is a hint only and may be NULL. Both are no-ops below manager
version 2, where the requests do not exist. */
void
saber_toplevel_set_fullscreen(struct saber_toplevel *toplevel,
    struct wl_output *output);

void
saber_toplevel_unset_fullscreen(struct saber_toplevel *toplevel);

#endif
