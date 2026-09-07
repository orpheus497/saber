/* Script function and purpose: zwlr_layer_surface_v1 lifecycle. One of these
backs the panel column, the reveal strip, the Dash and the spread. Owns the
buffer pool, the fractional scale and the viewport, and calls the owner back to
paint in logical coordinates. The second half of the file is the xdg_popup
side, which is what lets a quicklist escape the column's width. */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "fractional-scale-v1-protocol.h"
#include "viewporter-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"

#include <saber/surface.h>

#define SABER_SCALE_UNIT 120

int
saber_surface_panel_width(int icon_size)
{
  return icon_size + SABER_PANEL_PADDING;
}

void
saber_surface_panel_params(struct saber_surface_params *params,
    const struct saber_config *config,
    struct saber_output *output)
{
  int width = saber_surface_panel_width(config->panel.icon_size);

  memset(params, 0, sizeof(*params));

  params->output = output;
  params->layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
  params->anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
      ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
      (config->panel.edge == SABER_EDGE_RIGHT
              ? ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT
              : ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
  params->keyboard_interactivity =
      ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE;
  params->width = width;
  params->height = 0; /* TOP|BOTTOM spans the output */
  params->exclusive_zone =
      config->panel.autohide == SABER_AUTOHIDE_NEVER ? width : 0;
  params->layer_namespace = "saber";
}

double
saber_surface_scale(const struct saber_surface *surface)
{
  return (double)surface->scale_120 / SABER_SCALE_UNIT;
}

static void
surface_frame_done(void *data, struct wl_callback *callback, uint32_t time);

static const struct wl_callback_listener frame_listener = {
  .done = surface_frame_done,
};

/* Action purpose: Buffer pixels are ceil(logical x scale), never round: a
short buffer leaves an unpainted seam along the bottom or right edge of the
surface, which the compositor renders as garbage. The viewport destination is
then set to the logical size, so the compositor scales the slightly-oversized
buffer down to exactly the area it allocated. */
static void
surface_pixel_size(const struct saber_surface *surface,
    int *pixel_width,
    int *pixel_height)
{
  double scale = saber_surface_scale(surface);

  *pixel_width = (int)ceil((double)surface->width * scale);
  *pixel_height = (int)ceil((double)surface->height * scale);
}

static bool
surface_paint(struct saber_surface *surface)
{
  if (!surface->configured || surface->closed || surface->painting) {
    return false;
  }

  if (surface->width <= 0 || surface->height <= 0) {
    return false;
  }

  if (surface->listener == NULL || surface->listener->render == NULL) {
    return false;
  }

  int pixel_width, pixel_height;
  surface_pixel_size(surface, &pixel_width, &pixel_height);

  struct saber_buffer *buffer =
      saber_buffer_pool_acquire(&surface->pool, pixel_width, pixel_height);

  if (buffer == NULL) {
    return false;
  }

  surface->painting = true;
  surface->dirty = false;
  surface->pixel_width = pixel_width;
  surface->pixel_height = pixel_height;

  cairo_t *cr = cairo_create(buffer->surface);

  /* Action purpose: The slot may hold the previous frame's pixels, and the
  panel is translucent -- painting over stale content with OVER would compose
  the two frames. Clear first, unconditionally. */
  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  cairo_scale(cr,
      (double)pixel_width / (double)surface->width,
      (double)pixel_height / (double)surface->height);

  surface->listener->render(surface->listener_data, surface, cr, surface->width,
      surface->height);

  cairo_destroy(cr);
  cairo_surface_flush(buffer->surface);

  if (surface->viewport != NULL) {
    wl_surface_set_buffer_scale(surface->wl_surface, 1);
    wp_viewport_set_destination(surface->viewport, surface->width,
        surface->height);
  } else {
    wl_surface_set_buffer_scale(surface->wl_surface, surface->buffer_scale);
  }

  if (surface->frame_callback == NULL) {
    surface->frame_callback = wl_surface_frame(surface->wl_surface);
    wl_callback_add_listener(surface->frame_callback, &frame_listener, surface);
  }

  wl_surface_attach(surface->wl_surface, buffer->wl_buffer, 0, 0);
  wl_surface_damage_buffer(surface->wl_surface, 0, 0, pixel_width,
      pixel_height);
  wl_surface_commit(surface->wl_surface);

  saber_buffer_submit(buffer);
  surface->painting = false;

  return true;
}

/* Action purpose: A repaint refused for want of a free slot -- both held, or a
resize while the old size is still on screen -- has no frame callback to wake
it, because the refusal happened before any commit. The release is the only
event that says the situation has changed. */
static void
surface_buffer_released(void *data)
{
  struct saber_surface *surface = data;

  if (surface->dirty && surface->frame_callback == NULL) {
    surface_paint(surface);
  }
}

static void
surface_frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
  struct saber_surface *surface = data;

  wl_callback_destroy(callback);
  surface->frame_callback = NULL;

  /* Action purpose: Before the pending repaint, not after. The owner advances
  its animation here and marks the surface dirty from inside the call, so the
  paint below settles the whole frame at once instead of committing the old
  values and then immediately committing again. */
  if (surface->listener != NULL && surface->listener->frame != NULL) {
    surface->listener->frame(surface->listener_data, time);
  }

  if (surface->dirty && surface->frame_callback == NULL) {
    surface_paint(surface);
  }
}

void
saber_surface_damage(struct saber_surface *surface)
{
  surface->dirty = true;

  if (surface->frame_callback == NULL) {
    surface_paint(surface);
  }
}

bool
saber_surface_commit(struct saber_surface *surface)
{
  surface->dirty = true;

  return surface_paint(surface);
}

static void
layer_surface_handle_configure(void *data,
    struct zwlr_layer_surface_v1 *layer_surface,
    uint32_t serial,
    uint32_t width,
    uint32_t height)
{
  struct saber_surface *surface = data;

  zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

  /* A zero on an axis means the compositor deferred to the size we asked
  for, which for an anchored panel is the strip's width. */
  if (width > 0) {
    surface->width = (int)width;
  } else if (surface->requested_width > 0) {
    surface->width = surface->requested_width;
  }

  if (height > 0) {
    surface->height = (int)height;
  } else if (surface->requested_height > 0) {
    surface->height = surface->requested_height;
  }

  surface->configured = true;

  if (surface->listener != NULL && surface->listener->configure != NULL) {
    surface->listener->configure(surface->listener_data, surface,
        surface->width, surface->height);
  }

  /* Action purpose: A configure must be answered with a commit carrying a
  buffer of the acked size, so this paints rather than scheduling. Deferring
  to a frame callback would deadlock: no frame callback arrives for a surface
  that has never committed a buffer. */
  surface->dirty = true;
  surface_paint(surface);
}

static void
layer_surface_handle_closed(void *data,
    struct zwlr_layer_surface_v1 *layer_surface)
{
  (void)layer_surface;

  struct saber_surface *surface = data;
  surface->closed = true;

  if (surface->listener != NULL && surface->listener->closed != NULL) {
    surface->listener->closed(surface->listener_data, surface);
  }
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
  .configure = layer_surface_handle_configure,
  .closed = layer_surface_handle_closed,
};

static void
fractional_scale_handle_preferred(void *data,
    struct wp_fractional_scale_v1 *fractional_scale,
    uint32_t scale)
{
  (void)fractional_scale;

  struct saber_surface *surface = data;

  if (scale == 0 || scale == surface->scale_120) {
    return;
  }

  surface->scale_120 = scale;

  saber_display_set_cursor_scale(surface->display,
      (int)ceil((double)scale / SABER_SCALE_UNIT));

  if (surface->configured) {
    surface->dirty = true;
    surface_paint(surface);
  }
}

static const struct wp_fractional_scale_v1_listener
    fractional_scale_listener = {
  .preferred_scale = fractional_scale_handle_preferred,
};

struct saber_surface *
saber_surface_create(struct saber_display *display,
    const struct saber_surface_params *params,
    const struct saber_surface_listener *listener,
    void *data)
{
  if (display->compositor == NULL || display->layer_shell == NULL) {
    return NULL;
  }

  struct saber_surface *surface = g_new0(struct saber_surface, 1);

  surface->display = display;
  surface->output = params->output;
  surface->listener = listener;
  surface->listener_data = data;
  surface->requested_width = params->width;
  surface->requested_height = params->height;
  surface->width = params->width;
  surface->height = params->height;
  surface->buffer_scale = 1;

  if (params->output != NULL && params->output->scale > 0) {
    surface->buffer_scale = params->output->scale;
  }

  surface->scale_120 = (uint32_t)surface->buffer_scale * SABER_SCALE_UNIT;
  surface->exclusive_zone =
      params->exclusive_zone > 0 ? params->exclusive_zone : 0;
  surface->keyboard_interactivity = params->keyboard_interactivity;

  surface->wl_surface = wl_compositor_create_surface(display->compositor);

  if (surface->wl_surface == NULL) {
    g_free(surface);
    return NULL;
  }

  saber_buffer_pool_init(&surface->pool, display->shm);
  saber_buffer_pool_set_release_handler(&surface->pool, surface_buffer_released,
      surface);

  /* Action purpose: The viewport is what lets a fractionally scaled surface
  be rendered at an integral pixel size and still occupy exactly its logical
  area -- the buffer is ceil-scaled, the destination is the logical size, and
  the compositor absorbs the sub-pixel difference. Without wp_viewporter there
  is no way to express that, so the surface falls back to the output's integer
  scale. */
  if (display->viewporter != NULL) {
    surface->viewport = wp_viewporter_get_viewport(display->viewporter,
        surface->wl_surface);
  }

  if (display->fractional_scale_manager != NULL && surface->viewport != NULL) {
    surface->fractional_scale =
        wp_fractional_scale_manager_v1_get_fractional_scale(
            display->fractional_scale_manager, surface->wl_surface);
    wp_fractional_scale_v1_add_listener(surface->fractional_scale,
        &fractional_scale_listener, surface);
  }

  surface->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
      display->layer_shell,
      surface->wl_surface,
      params->output != NULL ? params->output->wl_output : NULL,
      params->layer,
      params->layer_namespace != NULL ? params->layer_namespace : "saber");

  if (surface->layer_surface == NULL) {
    saber_surface_destroy(surface);
    return NULL;
  }

  zwlr_layer_surface_v1_add_listener(surface->layer_surface,
      &layer_surface_listener, surface);

  zwlr_layer_surface_v1_set_anchor(surface->layer_surface, params->anchor);
  zwlr_layer_surface_v1_set_size(surface->layer_surface,
      (uint32_t)(params->width > 0 ? params->width : 0),
      (uint32_t)(params->height > 0 ? params->height : 0));
  zwlr_layer_surface_v1_set_exclusive_zone(surface->layer_surface,
      surface->exclusive_zone);
  zwlr_layer_surface_v1_set_keyboard_interactivity(surface->layer_surface,
      params->keyboard_interactivity);
  zwlr_layer_surface_v1_set_margin(surface->layer_surface, params->margin_top,
      params->margin_right, params->margin_bottom, params->margin_left);

  /* The first commit carries no buffer: it asks for the configure that tells
  us what size to paint. */
  wl_surface_commit(surface->wl_surface);

  return surface;
}

void
saber_surface_destroy(struct saber_surface *surface)
{
  if (surface == NULL) {
    return;
  }

  if (surface->frame_callback != NULL) {
    wl_callback_destroy(surface->frame_callback);
  }

  saber_buffer_pool_fini(&surface->pool);

  if (surface->fractional_scale != NULL) {
    wp_fractional_scale_v1_destroy(surface->fractional_scale);
  }

  if (surface->viewport != NULL) {
    wp_viewport_destroy(surface->viewport);
  }

  if (surface->layer_surface != NULL) {
    zwlr_layer_surface_v1_destroy(surface->layer_surface);
  }

  if (surface->wl_surface != NULL) {
    wl_surface_destroy(surface->wl_surface);
  }

  g_free(surface);
}

void
saber_surface_set_exclusive_zone(struct saber_surface *surface, int32_t zone)
{
  if (zone < 0) {
    zone = 0;
  }

  if (zone == surface->exclusive_zone) {
    return;
  }

  surface->exclusive_zone = zone;
  zwlr_layer_surface_v1_set_exclusive_zone(surface->layer_surface, zone);
  wl_surface_commit(surface->wl_surface);
}

void
saber_surface_set_keyboard_interactivity(struct saber_surface *surface,
    uint32_t interactivity)
{
  if (interactivity == surface->keyboard_interactivity) {
    return;
  }

  /* Action purpose: ON_DEMAND is `since="4"` in the layer-shell protocol, and
  the shell is bound with version_min(version, 4) -- so on a compositor that
  advertises 1, 2 or 3 this request carries a value the compositor must reject
  with `invalid_keyboard_interactivity`, which kills the client. Fall back to
  EXCLUSIVE there: it is the strongest thing v1 has and it at least delivers
  the keys the caller asked for, rather than terminating the panel. */
  if (interactivity == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND &&
      zwlr_layer_surface_v1_get_version(surface->layer_surface) <
          ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND_SINCE_VERSION) {
    interactivity = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;

    if (interactivity == surface->keyboard_interactivity) {
      return;
    }
  }

  surface->keyboard_interactivity = interactivity;
  zwlr_layer_surface_v1_set_keyboard_interactivity(surface->layer_surface,
      interactivity);
  wl_surface_commit(surface->wl_surface);
}

void
saber_surface_hold_keyboard(struct saber_surface *surface, bool hold)
{
  if (hold) {
    surface->keyboard_holds++;
  } else if (surface->keyboard_holds > 0) {
    surface->keyboard_holds--;
  }

  saber_surface_set_keyboard_interactivity(surface,
      surface->keyboard_holds > 0
          ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND
          : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
}

void
saber_surface_set_anchor(struct saber_surface *surface, uint32_t anchor)
{
  zwlr_layer_surface_v1_set_anchor(surface->layer_surface, anchor);
  wl_surface_commit(surface->wl_surface);
}

void
saber_surface_set_size(struct saber_surface *surface,
    int32_t width,
    int32_t height)
{
  surface->requested_width = width;
  surface->requested_height = height;

  zwlr_layer_surface_v1_set_size(surface->layer_surface,
      (uint32_t)(width > 0 ? width : 0), (uint32_t)(height > 0 ? height : 0));
  wl_surface_commit(surface->wl_surface);
}

void
saber_surface_set_margin(struct saber_surface *surface,
    int32_t top,
    int32_t right,
    int32_t bottom,
    int32_t left)
{
  zwlr_layer_surface_v1_set_margin(surface->layer_surface, top, right, bottom,
      left);
  wl_surface_commit(surface->wl_surface);
}

void
saber_surface_set_input_region(struct saber_surface *surface,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height)
{
  struct wl_region *region =
      wl_compositor_create_region(surface->display->compositor);

  if (region == NULL) {
    return;
  }

  if (width > 0 && height > 0) {
    wl_region_add(region, x, y, width, height);
  }

  wl_surface_set_input_region(surface->wl_surface, region);
  wl_region_destroy(region);
  wl_surface_commit(surface->wl_surface);
}

void
saber_popup_menu_params(struct saber_popup_params *params,
    enum saber_edge edge,
    int32_t width,
    int32_t height,
    int32_t anchor_x,
    int32_t anchor_y,
    int32_t anchor_width,
    int32_t anchor_height)
{
  memset(params, 0, sizeof(*params));

  params->width = width;
  params->height = height;
  params->anchor_x = anchor_x;
  params->anchor_y = anchor_y;
  params->anchor_width = anchor_width;
  params->anchor_height = anchor_height;

  if (edge == SABER_EDGE_RIGHT) {
    params->anchor = XDG_POSITIONER_ANCHOR_TOP_LEFT;
    params->gravity = XDG_POSITIONER_GRAVITY_BOTTOM_LEFT;
  } else {
    params->anchor = XDG_POSITIONER_ANCHOR_TOP_RIGHT;
    params->gravity = XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT;
  }

  /* Action purpose: FLIP_X is the one that matters -- a menu that would run
  off the far side of the output reappears on the other side of the column
  instead of being cut in half. Vertically a menu is flipped only as a last
  resort: sliding it up keeps the row the pointer is on where the user left
  it, and RESIZE_Y is the floor for a menu taller than the output. */
  params->constraint_adjustment =
      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_FLIP_X |
      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y |
      XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_RESIZE_Y;
}

double
saber_popup_scale(const struct saber_popup *popup)
{
  return (double)popup->scale_120 / SABER_SCALE_UNIT;
}

static void
popup_frame_done(void *data, struct wl_callback *callback, uint32_t time);

static const struct wl_callback_listener popup_frame_listener = {
  .done = popup_frame_done,
};

static bool
popup_paint(struct saber_popup *popup)
{
  if (!popup->configured || popup->done || popup->painting) {
    return false;
  }

  if (popup->width <= 0 || popup->height <= 0) {
    return false;
  }

  if (popup->listener == NULL || popup->listener->render == NULL) {
    return false;
  }

  double scale = saber_popup_scale(popup);
  int pixel_width = (int)ceil((double)popup->width * scale);
  int pixel_height = (int)ceil((double)popup->height * scale);

  struct saber_buffer *buffer =
      saber_buffer_pool_acquire(&popup->pool, pixel_width, pixel_height);

  if (buffer == NULL) {
    return false;
  }

  popup->painting = true;
  popup->dirty = false;
  popup->pixel_width = pixel_width;
  popup->pixel_height = pixel_height;

  cairo_t *cr = cairo_create(buffer->surface);

  cairo_set_operator(cr, CAIRO_OPERATOR_CLEAR);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  cairo_scale(cr, (double)pixel_width / (double)popup->width,
      (double)pixel_height / (double)popup->height);

  popup->listener->render(popup->listener_data, popup, cr, popup->width,
      popup->height);

  cairo_destroy(cr);
  cairo_surface_flush(buffer->surface);

  if (popup->viewport != NULL) {
    wl_surface_set_buffer_scale(popup->wl_surface, 1);
    wp_viewport_set_destination(popup->viewport, popup->width, popup->height);
  } else {
    wl_surface_set_buffer_scale(popup->wl_surface, popup->buffer_scale);
  }

  if (popup->frame_callback == NULL) {
    popup->frame_callback = wl_surface_frame(popup->wl_surface);
    wl_callback_add_listener(popup->frame_callback, &popup_frame_listener,
        popup);
  }

  wl_surface_attach(popup->wl_surface, buffer->wl_buffer, 0, 0);
  wl_surface_damage_buffer(popup->wl_surface, 0, 0, pixel_width, pixel_height);
  wl_surface_commit(popup->wl_surface);

  saber_buffer_submit(buffer);
  popup->painting = false;

  return true;
}

static void
popup_frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
  (void)time;

  struct saber_popup *popup = data;

  wl_callback_destroy(callback);
  popup->frame_callback = NULL;

  if (popup->dirty) {
    popup_paint(popup);
  }
}

static void
popup_buffer_released(void *data)
{
  struct saber_popup *popup = data;

  if (popup->dirty && popup->frame_callback == NULL) {
    popup_paint(popup);
  }
}

void
saber_popup_damage(struct saber_popup *popup)
{
  popup->dirty = true;

  if (popup->frame_callback == NULL) {
    popup_paint(popup);
  }
}

bool
saber_popup_commit(struct saber_popup *popup)
{
  popup->dirty = true;

  return popup_paint(popup);
}

static void
popup_handle_xdg_configure(void *data,
    struct xdg_surface *xdg_surface,
    uint32_t serial)
{
  struct saber_popup *popup = data;

  xdg_surface_ack_configure(xdg_surface, serial);

  if (popup->pending_width > 0) {
    popup->width = popup->pending_width;
  }

  if (popup->pending_height > 0) {
    popup->height = popup->pending_height;
  }

  popup->x = popup->pending_x;
  popup->y = popup->pending_y;
  popup->configured = true;

  /* The window geometry is the whole surface: a menu has no shadow margin. */
  xdg_surface_set_window_geometry(xdg_surface, 0, 0, popup->width,
      popup->height);

  if (popup->listener != NULL && popup->listener->configure != NULL) {
    popup->listener->configure(popup->listener_data, popup, popup->width,
        popup->height);
  }

  /* Same rule as a layer surface: an acked configure must be answered with a
  buffer now, not on a frame callback that will never arrive. */
  popup->dirty = true;
  popup_paint(popup);
}

static const struct xdg_surface_listener popup_xdg_surface_listener = {
  .configure = popup_handle_xdg_configure,
};

static void
popup_handle_configure(void *data,
    struct xdg_popup *xdg_popup,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height)
{
  (void)xdg_popup;

  struct saber_popup *popup = data;

  popup->pending_x = x;
  popup->pending_y = y;
  popup->pending_width = width;
  popup->pending_height = height;
}

static void
popup_handle_done(void *data, struct xdg_popup *xdg_popup)
{
  (void)xdg_popup;

  struct saber_popup *popup = data;
  popup->done = true;

  if (popup->listener != NULL && popup->listener->done != NULL) {
    popup->listener->done(popup->listener_data, popup);
  }
}

/* The reposition round trip ends in an ordinary configure pair, so the token
is of no use to us -- but the handler must exist, or the event dispatches
through a NULL function pointer. */
static void
popup_handle_repositioned(void *data, struct xdg_popup *xdg_popup,
    uint32_t token)
{
  (void)data;
  (void)xdg_popup;
  (void)token;
}

static const struct xdg_popup_listener popup_listener = {
  .configure = popup_handle_configure,
  .popup_done = popup_handle_done,
  .repositioned = popup_handle_repositioned,
};

static void
popup_fractional_scale_handle_preferred(void *data,
    struct wp_fractional_scale_v1 *fractional_scale,
    uint32_t scale)
{
  (void)fractional_scale;

  struct saber_popup *popup = data;

  if (scale == 0 || scale == popup->scale_120) {
    return;
  }

  popup->scale_120 = scale;

  if (popup->configured) {
    popup->dirty = true;
    popup_paint(popup);
  }
}

static const struct wp_fractional_scale_v1_listener
    popup_fractional_scale_listener = {
  .preferred_scale = popup_fractional_scale_handle_preferred,
};

static struct xdg_positioner *
popup_positioner(struct saber_display *display,
    const struct saber_popup_params *params)
{
  struct xdg_positioner *positioner =
      xdg_wm_base_create_positioner(display->wm_base);

  if (positioner == NULL) {
    return NULL;
  }

  /* A positioner with a zero size is incomplete and is a protocol error. */
  xdg_positioner_set_size(positioner, params->width > 0 ? params->width : 1,
      params->height > 0 ? params->height : 1);
  xdg_positioner_set_anchor_rect(positioner, params->anchor_x, params->anchor_y,
      params->anchor_width > 0 ? params->anchor_width : 0,
      params->anchor_height > 0 ? params->anchor_height : 0);
  xdg_positioner_set_anchor(positioner, params->anchor);
  xdg_positioner_set_gravity(positioner, params->gravity);
  xdg_positioner_set_constraint_adjustment(positioner,
      params->constraint_adjustment);
  xdg_positioner_set_offset(positioner, params->offset_x, params->offset_y);

  return positioner;
}

static struct saber_popup *
popup_new(struct saber_display *display,
    const struct saber_popup_params *params,
    struct xdg_surface *parent,
    int32_t buffer_scale,
    const struct saber_popup_listener *listener,
    void *data)
{
  if (display->compositor == NULL || display->wm_base == NULL) {
    return NULL;
  }

  struct saber_popup *popup = g_new0(struct saber_popup, 1);

  popup->display = display;
  popup->listener = listener;
  popup->listener_data = data;
  popup->width = params->width;
  popup->height = params->height;
  popup->buffer_scale = buffer_scale > 0 ? buffer_scale : 1;
  popup->scale_120 = (uint32_t)popup->buffer_scale * SABER_SCALE_UNIT;

  popup->wl_surface = wl_compositor_create_surface(display->compositor);

  if (popup->wl_surface == NULL) {
    g_free(popup);
    return NULL;
  }

  saber_buffer_pool_init(&popup->pool, display->shm);
  saber_buffer_pool_set_release_handler(&popup->pool, popup_buffer_released,
      popup);

  if (display->viewporter != NULL) {
    popup->viewport =
        wp_viewporter_get_viewport(display->viewporter, popup->wl_surface);
  }

  if (display->fractional_scale_manager != NULL && popup->viewport != NULL) {
    popup->fractional_scale =
        wp_fractional_scale_manager_v1_get_fractional_scale(
            display->fractional_scale_manager, popup->wl_surface);
    wp_fractional_scale_v1_add_listener(popup->fractional_scale,
        &popup_fractional_scale_listener, popup);
  }

  popup->xdg_surface =
      xdg_wm_base_get_xdg_surface(display->wm_base, popup->wl_surface);

  if (popup->xdg_surface == NULL) {
    saber_popup_destroy(popup);
    return NULL;
  }

  xdg_surface_add_listener(popup->xdg_surface, &popup_xdg_surface_listener,
      popup);

  struct xdg_positioner *positioner = popup_positioner(display, params);

  if (positioner == NULL) {
    saber_popup_destroy(popup);
    return NULL;
  }

  popup->xdg_popup =
      xdg_surface_get_popup(popup->xdg_surface, parent, positioner);
  xdg_positioner_destroy(positioner);

  if (popup->xdg_popup == NULL) {
    saber_popup_destroy(popup);
    return NULL;
  }

  xdg_popup_add_listener(popup->xdg_popup, &popup_listener, popup);

  return popup;
}

/* Action purpose: The grab is what turns a surface into a menu -- it routes
the keyboard to the popup and makes the compositor send popup_done when the
user clicks anywhere else. It must be requested before the first commit: the
protocol raises invalid_grab on an already-mapped popup. */
static void
popup_grab(struct saber_popup *popup, const struct saber_popup_params *params)
{
  struct saber_display *display = popup->display;

  if (!params->grab || display->seat == NULL) {
    return;
  }

  /* Action purpose: A popup grab must quote the serial of a button PRESS. The
  fallback used to be pointer_enter_serial, which the button handler overwrote
  on press AND release -- so a menu opened from a release quoted a release
  serial and a compositor that validates it refused the grab, leaving the menu
  with no click-outside dismissal. */
  uint32_t serial = params->grab_serial != 0 ? params->grab_serial
                                             : display->pointer_press_serial;

  xdg_popup_grab(popup->xdg_popup, display->seat, serial);
}

struct saber_popup *
saber_popup_create(struct saber_surface *parent,
    const struct saber_popup_params *params,
    const struct saber_popup_listener *listener,
    void *data)
{
  if (parent == NULL || parent->layer_surface == NULL) {
    return NULL;
  }

  struct saber_popup *popup = popup_new(parent->display, params, NULL,
      parent->buffer_scale, listener, data);

  if (popup == NULL) {
    return NULL;
  }

  popup->parent_surface = parent;
  popup->scale_120 = parent->scale_120;

  /* Action purpose: The popup was created with a NULL xdg_surface parent, so
  it has no parent at all until this request gives it one. It has to happen
  before the initial commit, or the compositor sees an unparented popup. */
  zwlr_layer_surface_v1_get_popup(parent->layer_surface, popup->xdg_popup);

  popup_grab(popup, params);
  wl_surface_commit(popup->wl_surface);

  return popup;
}

struct saber_popup *
saber_popup_create_nested(struct saber_popup *parent,
    const struct saber_popup_params *params,
    const struct saber_popup_listener *listener,
    void *data)
{
  if (parent == NULL || parent->xdg_surface == NULL) {
    return NULL;
  }

  struct saber_popup *popup = popup_new(parent->display, params,
      parent->xdg_surface, parent->buffer_scale, listener, data);

  if (popup == NULL) {
    return NULL;
  }

  popup->parent_popup = parent;
  popup->scale_120 = parent->scale_120;

  popup_grab(popup, params);
  wl_surface_commit(popup->wl_surface);

  return popup;
}

bool
saber_popup_reposition(struct saber_popup *popup,
    const struct saber_popup_params *params)
{
  if (popup->done || popup->xdg_popup == NULL) {
    return false;
  }

  if (xdg_popup_get_version(popup->xdg_popup) <
      XDG_POPUP_REPOSITION_SINCE_VERSION) {
    return false;
  }

  struct xdg_positioner *positioner =
      popup_positioner(popup->display, params);

  if (positioner == NULL) {
    return false;
  }

  xdg_popup_reposition(popup->xdg_popup, positioner, ++popup->reposition_token);
  xdg_positioner_destroy(positioner);

  return true;
}

void
saber_popup_destroy(struct saber_popup *popup)
{
  if (popup == NULL) {
    return;
  }

  if (popup->frame_callback != NULL) {
    wl_callback_destroy(popup->frame_callback);
  }

  saber_buffer_pool_fini(&popup->pool);

  if (popup->fractional_scale != NULL) {
    wp_fractional_scale_v1_destroy(popup->fractional_scale);
  }

  if (popup->viewport != NULL) {
    wp_viewport_destroy(popup->viewport);
  }

  if (popup->xdg_popup != NULL) {
    xdg_popup_destroy(popup->xdg_popup);
  }

  if (popup->xdg_surface != NULL) {
    xdg_surface_destroy(popup->xdg_surface);
  }

  if (popup->wl_surface != NULL) {
    wl_surface_destroy(popup->wl_surface);
  }

  g_free(popup);
}
