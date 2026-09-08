/* Script function and purpose: The quicklist -- the popup menu a tile or a
tray icon opens. One widget serves both: a tray item is a quicklist with only
its DBusMenu half filled in, and an application tile is the same menu with the
desktop entry's Actions, its window list and the pin and quit rows composed
underneath (BLUEPRINT.md 5.4).

Under com.canonical.dbusmenu an application publishes a DESCRIPTION of its
menu; no method in that protocol asks it to display anything. Drawing is the
host's job, which is exactly what lets the panel theme somebody else's menu
instead of hosting their widgets. */

#if !defined(SABER_QUICKLIST_H)
#define SABER_QUICKLIST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gio/gio.h>

#include <saber/appinfo.h>
#include <saber/config.h>
#include <saber/render.h>
#include <saber/surface.h>
#include <saber/theme.h>

/* The rows the menu cannot act on by itself: pinning is the model's business
and quitting is the window layer's. */
enum saber_quicklist_action {
  SABER_QUICKLIST_PIN,
  SABER_QUICKLIST_UNPIN,
  SABER_QUICKLIST_QUIT,
};

struct saber_quicklist_window {
  const char *title;
  /* Opaque to the menu, handed straight back to the `window` callback. */
  void *handle;
};

struct saber_quicklist_handlers {
  void (*action)(void *user, enum saber_quicklist_action action);
  void (*window)(void *user, void *handle);
  /* The menu has closed. It is freed as this returns, so the owner must drop
  its pointer here and must not call saber_quicklist_close again. */
  void (*closed)(void *user);
};

struct saber_quicklist_params {
  struct saber_surface *parent; /* the panel's layer surface */
  enum saber_edge edge;
  const struct saber_theme *theme;

  /* The configured animation duration in milliseconds, for the level's fade-in.
  0 switches it off, which is what `panel { animation-ms = 0 }` means. */
  int animation_ms;

  /* The shared icon resolver. Optional, and when it is NULL a menu row whose
  icon is named by theme rather than by absolute path simply draws no icon --
  which is what every menu did before this was passed in, because the DBusMenu
  spec names icons by theme and a tray menu therefore showed none of them. */
  struct saber_icons *icons;

  /* The tile, in the panel's logical coordinates. */
  int32_t anchor_x, anchor_y, anchor_width, anchor_height;

  /* Serial of the click that asked for the menu. 0 falls back to the last
  pointer event the display saw. */
  uint32_t serial;

  /* The dynamic half. `connection` may be NULL for the session bus; the menu
  is skipped unless both a bus name and an object path are given. For a tray
  item these three are the only fields set. */
  GDBusConnection *connection;
  const char *menu_bus_name;
  const char *menu_object_path;

  /* The static half, all optional. */
  struct saber_appinfo *app;
  const struct saber_quicklist_window *windows;
  size_t windows_len;
  bool pinned;
  bool running;
  /* Whether to offer "Keep in launcher" / "Unpin" at all -- false for the
  special tiles and for tray items, which are not launcher entries. */
  bool offer_pin;
};

struct saber_quicklist;

void
saber_quicklist_params_init(struct saber_quicklist_params *params);

/* Function purpose: Compose, map and take over input. Returns NULL when the
menu would have been empty or the popup could not be created; on success the
menu owns the pointer and keyboard until it closes. */
struct saber_quicklist *
saber_quicklist_open(const struct saber_quicklist_params *params,
    const struct saber_quicklist_handlers *handlers,
    void *user);

void
saber_quicklist_close(struct saber_quicklist *quicklist);

#endif
