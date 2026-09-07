/* Script function and purpose: A com.canonical.dbusmenu client over GDBus. The
protocol is read here as a wire format, not linked as a library: libdbusmenu is
deliberately not a dependency.

An application publishes a DESCRIPTION of a menu -- there is no method in this
protocol that asks it to display anything. Rendering is entirely the host's
job, and that is precisely what lets Saber theme the menu instead of hosting
somebody else's widgets. */

#if !defined(SABER_DBUSMENU_H)
#define SABER_DBUSMENU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gio/gio.h>
#include <glib.h>

enum saber_dbusmenu_type {
  SABER_DBUSMENU_STANDARD,
  SABER_DBUSMENU_SEPARATOR,
};

enum saber_dbusmenu_toggle {
  SABER_DBUSMENU_TOGGLE_NONE,
  SABER_DBUSMENU_TOGGLE_CHECKMARK,
  SABER_DBUSMENU_TOGGLE_RADIO,
};

/* Absent toggle-state means indeterminate, which is also what an application
sends for a toggle it has not resolved yet. */
#define SABER_DBUSMENU_TOGGLE_INDETERMINATE (-1)

struct saber_dbusmenu_item {
  int32_t id;
  enum saber_dbusmenu_type type;

  /* Mnemonic markers already stripped; never NULL. */
  char *label;

  bool enabled;
  /* False means the application is hiding the row, not greying it out: it must
  not be drawn at all. */
  bool visible;

  char *icon_name; /* never NULL; empty when unset */
  /* Raw bytes of a PNG, exactly as sent. NULL when the item shipped none. */
  uint8_t *icon_data;
  size_t icon_data_len;

  enum saber_dbusmenu_toggle toggle_type;
  int toggle_state;

  /* children-display == "submenu". True even before the children have been
  fetched, which is what AboutToShow is for. */
  bool submenu;

  /* struct saber_dbusmenu_item *, in menu order. Never NULL; may be empty. */
  GPtrArray *children;
};

struct saber_dbusmenu;

typedef void (*saber_dbusmenu_updated_func)(struct saber_dbusmenu *menu,
    void *user);

/* Function purpose: Bind to one menu object and read its layout once.
`connection` may be NULL to use the session bus. The callback fires after the
layout has been re-read in response to LayoutUpdated or
ItemsPropertiesUpdated. */
struct saber_dbusmenu *
saber_dbusmenu_open(GDBusConnection *connection,
    const char *bus_name,
    const char *object_path,
    saber_dbusmenu_updated_func cb,
    void *user);

void
saber_dbusmenu_close(struct saber_dbusmenu *menu);

/* Function purpose: What an asynchronous layout read or AboutToShow reports
back. `refreshed` is true when the tree inside `menu` was re-read as part of
this call -- for AboutToShow that means the application answered TRUE and the
new layout has already landed, so the children must be looked up again now. The
callback runs from the main loop, never from inside the call that issued it,
and `menu` is still open whenever it runs. */
typedef void (*saber_dbusmenu_done_func)(struct saber_dbusmenu *menu,
    bool refreshed,
    void *user);

/* Function purpose: Re-read the whole layout with GetLayout, synchronously.
False leaves the previous tree in place, so a peer that stops answering shows a
stale menu rather than an empty one. This blocks the caller's main loop for up
to the call timeout, so it is for the one read that has to have finished before
a menu can be composed at all -- everything reachable from pointer motion must
use saber_dbusmenu_refresh_async instead. */
bool
saber_dbusmenu_refresh(struct saber_dbusmenu *menu);

/* Function purpose: The same layout read with the reply taken off the caller's
frame, which is what keeps Wayland dispatch and frame callbacks running while a
slow application is answering. `user` is released with `notify` exactly once --
after `done`, or instead of it when the menu is closed before the reply lands
-- so the caller never has to track whether its context was consumed. */
void
saber_dbusmenu_refresh_async(struct saber_dbusmenu *menu,
    saber_dbusmenu_done_func done,
    void *user,
    GDestroyNotify notify);

/* The synthetic root; its children are the top-level rows. NULL before the
first successful read. */
const struct saber_dbusmenu_item *
saber_dbusmenu_root(const struct saber_dbusmenu *menu);

const struct saber_dbusmenu_item *
saber_dbusmenu_find(const struct saber_dbusmenu *menu, int32_t id);

/* Function purpose: Tell the application a submenu is about to open, so one
that builds its menu lazily gets the chance. Re-reads the layout when the reply
is TRUE, which is the application saying the tree just changed under us.
Returns true when the layout was re-read. Synchronous, and therefore not for
anything a pointer can drive; saber_dbusmenu_about_to_show_async is. */
bool
saber_dbusmenu_about_to_show(struct saber_dbusmenu *menu, int32_t id);

/* Function purpose: The same notice with the reply taken off the caller's
frame. `done` reports refreshed=true only once the TRUE reply's re-read has
landed, so a caller that looks the children up from inside it sees the tree the
application has just published rather than the one it replaced. Ownership of
`user` is as for saber_dbusmenu_refresh_async. */
void
saber_dbusmenu_about_to_show_async(struct saber_dbusmenu *menu,
    int32_t id,
    saber_dbusmenu_done_func done,
    void *user,
    GDestroyNotify notify);

/* Function purpose: Activate a row -- Event(id, "clicked", ...). Fire and
forget; the application's own reaction is the only feedback there is. */
void
saber_dbusmenu_event(struct saber_dbusmenu *menu, int32_t id);

const char *
saber_dbusmenu_bus_name(const struct saber_dbusmenu *menu);

const char *
saber_dbusmenu_object_path(const struct saber_dbusmenu *menu);

#endif
