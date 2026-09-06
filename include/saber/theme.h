/* Script function and purpose: The colour contract. Saber has no theme file of
its own -- it imports hikari.conf's sixteen-slot `ui { palette }` and maps it
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
  struct saber_color dim;          /* empty sheets, unavailable actions */
  struct saber_color overlay;      /* dash/spread backdrop (translucent) */

  double opacity;
};

/* Function purpose: Parse "#rgb", "#rrggbb" or "#rrggbbaa". Returns false and
leaves *out untouched on anything else, so a malformed palette entry costs one
colour rather than the whole theme. */
bool
saber_color_parse(const char *spec, struct saber_color *out);

/* Function purpose: Build the theme from a palette. `palette` may be NULL, in
which case the built-in fallback is used -- a panel must come up with no
hikari.conf present and no configuration of its own. `opacity` applies to the
background and overlay roles only. */
void
saber_theme_init(struct saber_theme *theme,
    const struct saber_color *palette,
    double opacity);

/* Function purpose: Read `ui { palette }` out of a hikari.conf. Returns the
number of slots read; anything less than SABER_PALETTE_SLOTS means the caller
should fall back rather than render with half a palette. A missing or
unreadable file yields 0, not an error. */
int
saber_theme_import_hikari(const char *path,
    struct saber_color palette[SABER_PALETTE_SLOTS]);

/* Function purpose: Set a cairo source from a role in one call, so no call site
can accidentally paint at full opacity when the theme asked for less. */
void
saber_theme_set_source(cairo_t *cr, const struct saber_color *color);

#endif
