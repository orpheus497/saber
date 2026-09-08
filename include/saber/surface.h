/* Script function and purpose: Layer-surface lifecycle -- creation on a given
output, configure and closed handling, fractional-scale-exact buffer sizing
through wp_viewporter, and the damage-and-commit entry point that asks the
owner to paint. Also the xdg_popup half: menus parented to a layer surface,
which is the only way anything can be drawn outside the panel's column. */

#if !defined(SABER_SURFACE_H)
#define SABER_SURFACE_H

#include <stdbool.h>
#include <stdint.h>

#include <cairo.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-shell-protocol.h"

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

  /* Function purpose: The compositor finished a frame and `time` is the
  timestamp it paced it with, in the same millisecond domain as the input
  events. Animation must advance on this and on nothing else -- a timer runs
  ahead of the refresh and stutters. Calling saber_surface_damage from here
  keeps the loop turning; returning without it lets it stop. */
  void (*frame)(void *data, uint32_t time);

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

  uint32_t keyboard_interactivity;
  int keyboard_holds; /* see saber_surface_hold_keyboard */

  const struct saber_surface_listener *listener;
  void *listener_data;
};

/* Function purpose: The column's width for an icon size alone, using the
built-in SABER_PANEL_PADDING (config.h) gutter. Only for a caller with no
configuration to hand; anything that has one wants the function below, because
the gutter is a configurable key and this ignores it. */
int
saber_surface_panel_width(int icon_size);

/* Function purpose: The column's width under `config`, which is the tile pitch
as well -- the panel's slots, its hit testing and the layer surface's own width
are all this one number, so nothing may arrive at it a second way. */
int
saber_surface_panel_width_for(const struct saber_config *config);

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

/* Function purpose: Raise the panel to ON_DEMAND for as long as a menu is up
and drop it back to NONE afterwards. The panel is normally not keyboard
focusable at all, and a popup parented to a layer surface inherits that
setting -- so without this the menu maps and then silently swallows nothing,
with no way to drive it from the keyboard. Nested calls are counted, so a
submenu closing does not drop focus out from under its parent. */
void
saber_surface_hold_keyboard(struct saber_surface *surface, bool hold);

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

/* Action purpose: An xdg_popup parented to the panel's layer surface through
zwlr_layer_surface_v1.get_popup. This exists because the column is around 64
logical pixels wide and a menu is not: a popup is constrained against the
OUTPUT, not against its parent surface, so it is the only construct in either
protocol that can paint outside the strip. */

struct saber_popup;

struct saber_popup_listener {
  void (*configure)(void *data,
      struct saber_popup *popup,
      int width,
      int height);

  /* `cr` is already scaled; the owner draws in logical pixels. */
  void (*render)(void *data,
      struct saber_popup *popup,
      cairo_t *cr,
      int width,
      int height);

  /* Function purpose: The compositor finished a frame and `time` is the
  timestamp it paced it with. Same contract as the layer surface's hook:
  animation advances on this and on nothing else, and calling
  saber_popup_damage from here is what asks for the next frame -- returning
  without it lets the loop stop. Optional; a popup that does not animate leaves
  it NULL and repaints only when its contents change. */
  void (*frame)(void *data, uint32_t time);

  /* xdg_popup.popup_done -- the compositor dismissed the popup, typically
  because the user clicked elsewhere. It must be destroyed, not remapped. */
  void (*done)(void *data, struct saber_popup *popup);
};

struct saber_popup_params {
  int32_t width, height;

  /* The rectangle the popup hangs off, in the PARENT's logical coordinates:
  the tile for a menu, the row for a submenu. */
  int32_t anchor_x, anchor_y, anchor_width, anchor_height;

  uint32_t anchor;                /* enum xdg_positioner_anchor */
  uint32_t gravity;               /* enum xdg_positioner_gravity */
  uint32_t constraint_adjustment; /* enum xdg_positioner_constraint_adjustment */
  int32_t offset_x, offset_y;

  /* Take an explicit grab, which is what makes a click anywhere else dismiss
  the menu and hands it the keyboard. A grab must quote the serial of the
  input event that asked for the menu; 0 falls back to the last pointer enter,
  which the compositor may refuse. */
  bool grab;
  uint32_t grab_serial;
};

struct saber_popup {
  struct saber_display *display;
  struct saber_surface *parent_surface; /* NULL for a nested popup */
  struct saber_popup *parent_popup;     /* NULL for a menu on the panel */

  struct wl_surface *wl_surface;
  struct xdg_surface *xdg_surface;
  struct xdg_popup *xdg_popup;
  struct wp_viewport *viewport;
  struct wp_fractional_scale_v1 *fractional_scale;
  struct wl_callback *frame_callback;

  struct saber_buffer_pool pool;

  int width, height;                 /* logical, from the last acked configure */
  int pending_width, pending_height; /* xdg_popup.configure, not yet acked */
  int x, y;                          /* placement the compositor settled on */
  int pending_x, pending_y;
  int pixel_width, pixel_height;
  uint32_t scale_120;
  int32_t buffer_scale;
  uint32_t reposition_token;

  bool configured;
  bool dirty;
  bool done;
  bool painting;

  const struct saber_popup_listener *listener;
  void *listener_data;
};

/* Function purpose: Fill in the positioner a panel menu wants. The menu is
placed beside the column rather than over it -- anchored to the strip's inner
edge and growing away from it -- and the constraint adjustment is the whole
point of the call: FLIP_X sends the menu out the other side of a tile sitting
against the screen edge, while SLIDE_Y then RESIZE_Y keep a long menu on
screen near the bottom instead of letting the compositor clip it. Submenus use
the same rules, with the parent row as the anchor rectangle. */
void
saber_popup_menu_params(struct saber_popup_params *params,
    enum saber_edge edge,
    int32_t width,
    int32_t height,
    int32_t anchor_x,
    int32_t anchor_y,
    int32_t anchor_width,
    int32_t anchor_height);

struct saber_popup *
saber_popup_create(struct saber_surface *parent,
    const struct saber_popup_params *params,
    const struct saber_popup_listener *listener,
    void *data);

/* Function purpose: A submenu. Nested popups must be destroyed innermost
first, or the compositor raises not_the_topmost_popup and kills the client. */
struct saber_popup *
saber_popup_create_nested(struct saber_popup *parent,
    const struct saber_popup_params *params,
    const struct saber_popup_listener *listener,
    void *data);

void
saber_popup_destroy(struct saber_popup *popup);

/* Function purpose: Move or resize a mapped popup, for a menu whose contents
changed under it. False on an xdg_popup below version 3, where the request
does not exist and the caller must live with the size it opened at. */
bool
saber_popup_reposition(struct saber_popup *popup,
    const struct saber_popup_params *params);

void
saber_popup_damage(struct saber_popup *popup);

bool
saber_popup_commit(struct saber_popup *popup);

double
saber_popup_scale(const struct saber_popup *popup);

#endif
