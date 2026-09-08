/* Script function and purpose: All drawing. Cairo for the tile decoration,
pango for the two places text actually appears (the count badge and the sheet
number), gdk-pixbuf and librsvg behind an XDG icon-theme resolver for the icons
themselves.

Every colour comes from a struct saber_theme role. There are no literal colours
below this line anywhere in the panel, because retheming hikari has to retint
Saber with it (BLUEPRINT.md 5.2). */

#if !defined(SABER_RENDER_H)
#define SABER_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <cairo.h>

#include <saber/config.h>
#include <saber/model.h>
#include <saber/theme.h>

struct saber_icons;

struct saber_icons *
saber_icons_create(void);

void
saber_icons_destroy(struct saber_icons *icons);

/* Function purpose: Resolve `name` through the XDG icon theme and rasterise it
at exactly `pixel_size` device pixels -- SVG is rendered at that size rather
than scaled from a nominal one, which is the whole point of fractional-scale
support (D-010). Returns a surface owned by the cache, or NULL. Misses are
cached too, so a missing icon costs one directory walk per size, not one per
frame. `name` may also be an absolute path. */
cairo_surface_t *
saber_icons_lookup(struct saber_icons *icons, const char *name, int pixel_size);

/* Function purpose: Wrap a StatusNotifierItem's decoded pixmap. Not cached:
sni.c already caches the decode, and the surface is cheap next to a theme
lookup. The caller owns the result. */
cairo_surface_t *
saber_icon_from_argb32(const uint8_t *data,
    size_t length,
    int width,
    int height);

struct saber_render {
  const struct saber_config *config;
  const struct saber_theme *theme;
  struct saber_icons *icons;

  double scale; /* device pixels per logical pixel */
  enum saber_edge edge;
  int icon_size; /* logical */
  int tile;      /* logical tile box; the column's width */
};

void
saber_render_init(struct saber_render *render,
    const struct saber_config *config,
    const struct saber_theme *theme,
    struct saber_icons *icons);

/* One tile's worth of everything the decoration rules in BLUEPRINT.md 5.2 can
depend on. The animated members arrive already evaluated from anim.c; nothing
here knows what a tween is. */
struct saber_tile {
  double x, y, width, height; /* logical, the whole tile box */

  cairo_surface_t *icon; /* device-pixel surface; wins over icon_name */
  const char *icon_name; /* resolved through the theme when icon is NULL */
  const char *label;     /* drawn when neither icon resolves */

  /* A "-symbolic" icon is a monochrome stencil the host is expected to tint;
  drawn as-is it is black-on-black on this palette. */
  bool symbolic;

  int windows;
  bool running;
  bool focused;
  bool hovered;
  bool pressed;
  bool dim;

  struct saber_badge badge;

  double hover;  /* 0..1 */
  double throb;  /* scale multiplier; 1.0 for none */
  double wiggle; /* horizontal offset in logical pixels */

  /* The number this tile answers to from `saberctl launch N`, drawn over the
  icon while the overlay is up and 0 the rest of the time. Deliberately not the
  badge: a tile can carry a count and a number at once, and they mean different
  things. */
  int overlay_number;
};

void
saber_render_column(const struct saber_render *render,
    cairo_t *cr,
    int width,
    int height);

void
saber_render_separator(const struct saber_render *render,
    cairo_t *cr,
    double y,
    double width);

void
saber_render_tile(const struct saber_render *render,
    cairo_t *cr,
    const struct saber_tile *tile);

#endif
