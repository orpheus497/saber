/* Script function and purpose: The configuration contract, as BLUEPRINT.md
section 6 defines it. UCL via libucl -- the same dialect and the same parser as
hikari.conf, so one syntax covers the desktop.

Every key has a default and the whole file is optional: a user with no
~/.config/saber/saber.conf gets a working panel. */

#if !defined(SABER_CONFIG_H)
#define SABER_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#include <saber/theme.h>

enum saber_edge {
  SABER_EDGE_LEFT,
  SABER_EDGE_RIGHT,
};

enum saber_autohide {
  /* Reserve the strip: exclusive_zone is the tile width. */
  SABER_AUTOHIDE_NEVER,
  /* Do not reserve: exclusive_zone is 0. Never -1 -- that would place the
  panel outside the usable area and let it cover the compositor's top bar. */
  SABER_AUTOHIDE_AUTO,
};

enum saber_backlight {
  SABER_BACKLIGHT_PALETTE,
  SABER_BACKLIGHT_DOMINANT,
  SABER_BACKLIGHT_OFF,
};

/* One portion of the column. SABER_PORTION_APPS is the band of favourites and
running applications; every other portion is a single special tile. The band is
also what splits the column -- the portions configured before it are pinned to
the top and the ones after it are anchored to the bottom -- so the order is not
decoration, it decides the layout. */
enum saber_portion {
  SABER_PORTION_BFB,
  SABER_PORTION_APPS,
  SABER_PORTION_SHEETS,
  SABER_PORTION_DEVICES,
  SABER_PORTION_TRASH,
  SABER_PORTION_TRAY,
  SABER_PORTION_SESSION,
};

#define SABER_PORTION_COUNT 7

/* The column is icon_size plus a symmetric gutter. This is the DEFAULT gutter,
not the gutter: panel { padding } overrides it, so a user who wants a tighter
or a roomier column is not asking for a recompile. */
#define SABER_PANEL_PADDING 12

struct saber_config {
  struct {
    char *output; /* "all", "primary", or an output name */
    enum saber_edge edge;
    int icon_size; /* 24-64, clamped on load */
    int padding;   /* added to icon_size for the column's width */
    enum saber_autohide autohide;
    int reveal_pressure;
    int animation_ms;
  } panel;

  struct {
    bool inherit_hikari;
    enum saber_backlight backlight;
    double opacity;
    /* Populated either from hikari.conf or from an explicit theme { palette }
    block; falls back to the built-in palette when neither is readable. */
    struct saber_color palette[SABER_PALETTE_SLOTS];
    bool palette_valid;
  } theme;

  struct {
    char **favourites; /* desktop-file IDs, NULL-terminated */
    size_t favourites_len;
  } launcher;

  /* Action purpose: `order` is the whole answer to which portions the column
  carries and in what sequence. The six booleans are the older per-portion form
  and go on working: a false one drops that portion from the order wherever it
  sits, and config.c then rewrites all six from the resolved order, so a reader
  that only asks "is the tray on?" gets the same answer whichever form the user
  wrote. There is no boolean for the application band -- it is not optional,
  and its place in `order` is what splits head from tail. */
  struct {
    bool bfb, sheets, devices, trash, tray, session;

    enum saber_portion *order; /* no duplicates; always contains _APPS */
    size_t order_len;
  } items;

  /* Action purpose: An empty string means "use the built-in operator-group
  command" for the three power actions, and "hide this entry" for lock and
  logout. See DECISIONS_LOG D-013 and BLUEPRINT.md 4.2 -- Saber ships no
  privileged code, and hides what it cannot do rather than offering buttons
  that fail. */
  struct {
    char *suspend, *reboot, *poweroff;
    char *lock, *logout;
  } session;
};

/* Function purpose: Load configuration, applying defaults for everything
absent. Never fails in a way that costs the user their panel: a malformed file
is reported and skipped, not fatal. `path` may be NULL to use the standard
search order (XDG_CONFIG_HOME, then the system default). */
bool
saber_config_load(struct saber_config *config, const char *path);

void
saber_config_fini(struct saber_config *config);

/* Function purpose: Resolve the favourites list actually in force. The config
list SEEDS first run; $XDG_DATA_HOME/saber/favourites holds the live order and
wins thereafter, because drag-to-reorder must persist and rewriting a user's
hand-commented config file is hostile (D-009). */
bool
saber_config_load_favourites(const struct saber_config *config,
    char ***out,
    size_t *out_len);

bool
saber_config_save_favourites(char *const *favourites, size_t len);

#endif
