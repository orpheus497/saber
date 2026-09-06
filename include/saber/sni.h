/* Script function and purpose: The StatusNotifierItem host contract -- Saber as
`org.kde.StatusNotifierWatcher` plus a registered host, and one proxy per tray
item with its properties already read and its icon already decoded. */

#if !defined(SABER_SNI_H)
#define SABER_SNI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum saber_sni_status {
  SABER_SNI_PASSIVE,
  SABER_SNI_ACTIVE,
  SABER_SNI_NEEDS_ATTENTION,
};

enum saber_sni_orientation {
  SABER_SNI_VERTICAL,
  SABER_SNI_HORIZONTAL,
};

struct saber_sni;
struct saber_sni_item;

typedef void (*saber_sni_changed_func)(void *user);

/* Function purpose: Take the watcher name, register as a host, and start
accepting item registrations. Returns NULL only when the session bus is
unreachable; losing the watcher name to another tray is reported through the
changed callback leaving the list empty, not by failing here. */
struct saber_sni *
saber_sni_create(saber_sni_changed_func cb, void *user);

void
saber_sni_destroy(struct saber_sni *sni);

/* Function purpose: True while Saber holds org.kde.StatusNotifierWatcher.
False means another tray host owns the session's tray and Saber's tray zone has
nothing to draw -- worth saying out loud, since the failure is otherwise
silent. */
bool
saber_sni_is_watcher(const struct saber_sni *sni);

unsigned int
saber_sni_count(const struct saber_sni *sni);

struct saber_sni_item *
saber_sni_nth(const struct saber_sni *sni, unsigned int n);

/* `service` is the string reported by RegisteredStatusNotifierItems: the item's
bus name concatenated with its object path. */
struct saber_sni_item *
saber_sni_find(const struct saber_sni *sni, const char *service);

const char *
saber_sni_item_service(const struct saber_sni_item *item);

const char *
saber_sni_item_bus_name(const struct saber_sni_item *item);

const char *
saber_sni_item_object_path(const struct saber_sni_item *item);

const char *
saber_sni_item_id(const struct saber_sni_item *item);

/* Falls back to Id, which is mandatory, when Title is absent. */
const char *
saber_sni_item_title(const struct saber_sni_item *item);

enum saber_sni_status
saber_sni_item_status(const struct saber_sni_item *item);

/* The effective name: AttentionIconName while the item needs attention,
IconName otherwise. Never NULL. */
const char *
saber_sni_item_icon_name(const struct saber_sni_item *item);

const char *
saber_sni_item_overlay_icon_name(const struct saber_sni_item *item);

/* A private directory the item's icon names resolve against, outside the
XDG icon theme search path. Empty when the item ships no such directory. */
const char *
saber_sni_item_icon_theme_path(const struct saber_sni_item *item);

/* Function purpose: The effective icon as premultiplied native-endian ARGB32,
decoded on first use and cached until the item changes. False when the item
offered no usable pixmap, in which case the caller falls back to the icon
name. */
bool
saber_sni_item_icon_argb32(struct saber_sni_item *item,
    const uint8_t **data,
    size_t *length,
    int *width,
    int *height);

bool
saber_sni_item_overlay_argb32(struct saber_sni_item *item,
    const uint8_t **data,
    size_t *length,
    int *width,
    int *height);

const char *
saber_sni_item_menu_path(const struct saber_sni_item *item);

/* Function purpose: Whether a DBusMenu can be opened for this item. This, and
not ItemIsMenu, is the test -- see the comment on rule (c) in sni.c. */
bool
saber_sni_item_has_menu(const struct saber_sni_item *item);

/* The raw ItemIsMenu property: advisory only. It says a left click should show
the menu rather than activate, and nothing about whether a menu exists. */
bool
saber_sni_item_is_menu(const struct saber_sni_item *item);

void
saber_sni_item_activate(struct saber_sni_item *item, int x, int y);

void
saber_sni_item_secondary_activate(struct saber_sni_item *item, int x, int y);

void
saber_sni_item_context_menu(struct saber_sni_item *item, int x, int y);

void
saber_sni_item_scroll(struct saber_sni_item *item,
    int delta,
    enum saber_sni_orientation orientation);

#endif
