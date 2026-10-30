/* Script function and purpose: The colour contract. Saber has no theme file of
its own -- it imports hikari.lua's sixteen-slot `ui.palette` and maps it
onto semantic roles, so retheming the compositor retints the panel with it. */

#if !defined(SABER_THEME_H)
#define SABER_THEME_H

#include <stdbool.h>

#include <cairo.h>

struct saber_color {
  double r, g, b, a;
};

/* The conventional terminal layout: 0-7 normal, 8-15 bright. */
#define SABER_PALETTE_SLOTS 16

/* The eleven semantic roles, named so configuration can address one. Every
drawing module still reads the struct fields below; this enum exists for the
override block and for nothing else. */
enum saber_role {
  SABER_ROLE_BACKGROUND,
  SABER_ROLE_FOREGROUND,
  SABER_ROLE_ACCENT,
  SABER_ROLE_BACKLIGHT,
  SABER_ROLE_URGENT,
  SABER_ROLE_BADGE_BG,
  SABER_ROLE_BADGE_FG,
  SABER_ROLE_PROGRESS,
  SABER_ROLE_HINT,
  SABER_ROLE_DIM,
  SABER_ROLE_OVERLAY,
};

#define SABER_ROLE_COUNT 11

/* Action purpose: One override, as `theme.roles` wrote it. A palette
reference is kept as the slot number and resolved when the theme is built, not
when the file is read, because the palette may still arrive from hikari.lua
afterwards -- so `urgent = color9` keeps following a retheme the way the rest of
the panel does. A literal is kept in `color` and `slot` is negative. */
struct saber_role_override {
  bool set;
  int slot;
  struct saber_color color;
};

struct saber_theme_roles {
  struct saber_role_override role[SABER_ROLE_COUNT];
};

struct saber_theme {
  struct saber_color palette[SABER_PALETTE_SLOTS];

  /* Semantic roles, each resolved from a palette slot at load time. */
  struct saber_color background;   /* the column itself */
  struct saber_color foreground;   /* pips, separators, text */
  struct saber_color accent;       /* focus arrow, current sheet */
  struct saber_color backlight;    /* running-tile tile fill */
  struct saber_color urgent;       /* wiggle tint, urgent badge */
  struct saber_color badge_bg;     /* count disc */
  struct saber_color badge_fg;
  struct saber_color progress;     /* progress overlay */

  /* Recessed content the user still has to read: the dash placeholder, its
  empty-state line, an unselected filter chip, the number of an empty sheet.
  Split out of `dim` because the two wanted opposite things from the same
  colour -- see the derivation in src/theme.c. */
  struct saber_color hint;

  /* Structural quiet: hairlines, frame outlines, scrollbar tracks, the
  progress track, and a menu item that is genuinely unavailable -- which WCAG
  2.1 exempts from any contrast floor, unlike everything `hint` now carries. */
  struct saber_color dim;
  struct saber_color overlay;      /* dash/spread backdrop: colour only */

  /* Whether the resolved background is a light ground. Derived from its
  luminance rather than configured, so a palette saber has never seen -- a
  user's own, or a terminal colour scheme dropped into hikari.lua -- gets the
  right answer too. Read it through saber_theme_is_light(). */
  bool light_ground;

  double opacity;

  /* The alpha the dash strip and the spread backdrop are painted at, written
  to the surface as given rather than arrived at by compositing several
  translucent passes. 1.0 is the only value that guarantees nothing behind
  those two surfaces shows through; every value below it is the fraction of
  the desktop the user has chosen to keep visible. */
  double overlay_opacity;
};

/* Function purpose: Parse "#rgb", "#rrggbb" or "#rrggbbaa". Returns false and
leaves *out untouched on anything else, so a malformed palette entry costs one
colour rather than the whole theme. */
bool
saber_color_parse(const char *spec, struct saber_color *out);

/* Function purpose: Build the theme from a palette. `palette` may be NULL, in
which case the built-in fallback is used -- a panel must come up with no
hikari.lua present and no configuration of its own. `opacity` applies to the
background role only: the overlay role carries colour and `overlay_opacity`
carries its alpha, so 1.0 there is a guarantee that nothing behind the dash
strip or the spread backdrop bleeds through. */
void
saber_theme_init(struct saber_theme *theme,
    const struct saber_color *palette,
    double opacity,
    double overlay_opacity);

/* Function purpose: saber_theme_init, with the `theme.roles` overrides
applied over the derived defaults. `roles` may be NULL, which is exactly
saber_theme_init. An override wins over the default, and the roles are resolved
in dependency order, so overriding a role the map derives from carries the
derived ones with it -- a new accent retints the backlight and the progress
bar, a new urgent takes the count disc. */
void
saber_theme_init_with_roles(struct saber_theme *theme,
    const struct saber_color *palette,
    double opacity,
    double overlay_opacity,
    const struct saber_theme_roles *roles);

/* Function purpose: Whether the theme paints onto a light ground. A hover
highlight has to lighten on a dark ground and darken on a light one, and a
shadow inverts outright, so every such decision asks this rather than assuming
the dark case. */
bool
saber_theme_is_light(const struct saber_theme *theme);

/* Function purpose: Read `ui.palette` out of a hikari.lua, falling back to a
top-level `palette` because the shape of that file is still settling. Returns
the number of slots read; anything less than SABER_PALETTE_SLOTS means the
caller should fall back rather than render with half a palette. A missing,
unparseable or unexpectedly shaped file yields 0, not an error -- that is the
ordinary first run. */
int
saber_theme_import_hikari(const char *path,
    struct saber_color palette[SABER_PALETTE_SLOTS]);

/* Function purpose: Read `ui.animation` out of a hikari.lua, falling back to a
top-level `animation` for the same reason, so the panel moves at the speed the
compositor moves at instead of at an unrelated one of its own. Returns false and
leaves *duration_ms untouched when the file carries no usable animation block.
`enabled = false` resolves to 0, which the tween code already treats as "settle
immediately", so the compositor's two keys arrive here as the one number saber
has always had. */
bool
saber_theme_import_hikari_animation(const char *path, int *duration_ms);

/* Function purpose: Set a cairo source from a role in one call, so no call site
can accidentally paint at full opacity when the theme asked for less. */
void
saber_theme_set_source(cairo_t *cr, const struct saber_color *color);

#endif
