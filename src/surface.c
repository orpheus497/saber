/* Script function and purpose: zwlr_layer_surface_v1 lifecycle. One of these
backs the panel column, the reveal strip, the Dash and the spread. Owns the
buffer pool, the fractional scale and the viewport, and calls the owner back to
paint in logical coordinates. */

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
  (void)time;

  struct saber_surface *surface = data;

  wl_callback_destroy(callback);
  surface->frame_callback = NULL;

  if (surface->dirty) {
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
  zwlr_layer_surface_v1_set_keyboard_interactivity(surface->layer_surface,
      interactivity);
  wl_surface_commit(surface->wl_surface);
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
