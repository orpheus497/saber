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

struct saber_config {
  struct {
    char *output; /* "all", "primary", or an output name */
    enum saber_edge edge;
    int icon_size; /* 24-64, clamped on load */
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

  struct {
    bool bfb, sheets, devices, trash, tray, session;
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
