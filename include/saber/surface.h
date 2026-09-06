/* Script function and purpose: Layer-surface lifecycle -- creation on a given
output, configure and closed handling, fractional-scale-exact buffer sizing
through wp_viewporter, and the damage-and-commit entry point that asks the
owner to paint. */

#if !defined(SABER_SURFACE_H)
#define SABER_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

#include <cairo.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-protocol.h"

#include <saber/buffer.h>
#include <saber/config.h>
#include <saber/display.h>

struct wp_viewport;
struct wp_fractional_scale_v1;

struct saber_surface;

struct saber_surface_listener {
  /* The surface has a size. Fires on every configure, not only the first. */
  void (*configure)(void *data,
      struct saber_surface *surface,
      int width,
      int height);

  /* Function purpose: Paint the surface. `cr` is already scaled, so the owner
  draws in logical pixels and never sees the buffer's pixel size or the
  fractional scale factor. */
  void (*render)(void *data,
      struct saber_surface *surface,
      cairo_t *cr,
      int width,
      int height);

  /* The compositor withdrew the surface -- output gone, or session ending.
  The surface must be destroyed; it cannot be reused. */
  void (*closed)(void *data, struct saber_surface *surface);
};

struct saber_surface_params {
  struct saber_output *output; /* NULL lets the compositor choose */
  uint32_t layer;              /* enum zwlr_layer_shell_v1_layer */
  uint32_t anchor;             /* ZWLR_LAYER_SURFACE_V1_ANCHOR_* bitmask */
  uint32_t keyboard_interactivity;
  int32_t exclusive_zone; /* negative values are clamped to 0 -- see below */
  int32_t width, height;  /* 0 on an axis lets the anchor decide it */
  int32_t margin_top, margin_right, margin_bottom, margin_left;
  const char *layer_namespace;
};

struct saber_surface {
  struct saber_display *display;
  struct saber_output *output;

  struct wl_surface *wl_surface;
  struct zwlr_layer_surface_v1 *layer_surface;
  struct wp_viewport *viewport;
  struct wp_fractional_scale_v1 *fractional_scale;
  struct wl_callback *frame_callback;

  struct saber_buffer_pool pool;

  int32_t requested_width, requested_height;
  int width, height;             /* logical size, from the last configure */
  int pixel_width, pixel_height; /* buffer size */
  uint32_t scale_120;            /* scale x 120, as fractional-scale sends it */
  int32_t buffer_scale;          /* integer fallback when no viewporter */
  int32_t exclusive_zone;

  bool configured;
  bool dirty;
  bool closed;
  bool painting;

  const struct saber_surface_listener *listener;
  void *listener_data;
};

/* Icon plus a symmetric 8px gutter each side; the column's whole width. */
#define SABER_PANEL_PADDING 16

int
saber_surface_panel_width(int icon_size);

/* Function purpose: Fill in the panel's own geometry from configuration.
Anchors LEFT|TOP|BOTTOM (or RIGHT|TOP|BOTTOM), which is what makes the strip
span the full height and sit under the compositor's top bar for free.

exclusive_zone is the panel width when the panel is reserving space and 0 when
it autohides. It is NEVER -1: that asks to be placed outside the usable area,
which would let the panel cover the top bar. */
void
saber_surface_panel_params(struct saber_surface_params *params,
    const struct saber_config *config,
    struct saber_output *output);

struct saber_surface *
saber_surface_create(struct saber_display *display,
    const struct saber_surface_params *params,
    const struct saber_surface_listener *listener,
    void *data);

void
saber_surface_destroy(struct saber_surface *surface);

/* Negative zones are clamped to 0 -- see saber_surface_panel_params. */
void
saber_surface_set_exclusive_zone(struct saber_surface *surface, int32_t zone);

void
saber_surface_set_keyboard_interactivity(struct saber_surface *surface,
    uint32_t interactivity);

void
saber_surface_set_anchor(struct saber_surface *surface, uint32_t anchor);

void
saber_surface_set_size(struct saber_surface *surface,
    int32_t width,
    int32_t height);

void
saber_surface_set_margin(struct saber_surface *surface,
    int32_t top,
    int32_t right,
    int32_t bottom,
    int32_t left);

/* Function purpose: Restrict the region that receives pointer and touch input.
Passing width or height of 0 makes the surface entirely click-through, which is
how the autohide reveal strip stays out of the way of the windows behind it. */
void
saber_surface_set_input_region(struct saber_surface *surface,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height);

/* Function purpose: Mark the surface dirty and repaint. Paints immediately
when no frame is in flight, otherwise defers to the pending frame callback, so
a burst of damage in one main-loop iteration costs one paint and the panel
never outruns the compositor's refresh. */
void
saber_surface_damage(struct saber_surface *surface);

/* Function purpose: Paint and commit now, ignoring frame-callback throttling.
For the cases that must not wait a frame -- the first paint after a configure,
and a resize. Returns false when the surface is not configured or no buffer was
free, in which case the damage stays pending. */
bool
saber_surface_commit(struct saber_surface *surface);

/* The effective scale, 1.0 for an unscaled output. */
double
saber_surface_scale(const struct saber_surface *surface);

#endif
