/* Script function and purpose: The Wayland connection -- registry globals,
outputs, the seat, the cursor theme, and the GSource that drives the whole
thing from GLib's main loop. */

#if !defined(SABER_DISPLAY_H)
#define SABER_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include <glib.h>
#include <wayland-client.h>
#include <wayland-cursor.h>

struct wp_viewporter;
struct wp_fractional_scale_manager_v1;
struct xdg_wm_base;
struct xdg_activation_v1;
struct zwlr_layer_shell_v1;
struct zwlr_foreign_toplevel_manager_v1;

struct saber_display;

struct saber_output {
  struct wl_list link;
  struct saber_display *display;
  struct wl_output *wl_output;
  uint32_t global; /* registry name, for matching the remove event */

  char *name;        /* wl_output.name from v4; NULL below that */
  char *description;
  int32_t scale;
  int32_t width, height; /* current mode, in physical pixels */
  int32_t transform;
  bool configured; /* at least one wl_output.done has arrived */
};

/* Pointer coordinates are surface-local logical pixels; the surface layer, not
the caller, owns the conversion to buffer pixels. */
struct saber_pointer_listener {
  void (*enter)(void *data, struct wl_surface *surface, double x, double y);
  void (*leave)(void *data, struct wl_surface *surface);
  void (*motion)(void *data, uint32_t time, double x, double y);
  void (*button)(void *data, uint32_t time, uint32_t button, uint32_t state);
  void (*axis)(void *data, uint32_t time, uint32_t axis, double value);
  void (*frame)(void *data);
};

struct saber_keyboard_listener {
  void (*keymap)(void *data, uint32_t format, int fd, uint32_t size);
  void (*enter)(void *data, struct wl_surface *surface);
  void (*leave)(void *data, struct wl_surface *surface);
  void (*key)(void *data, uint32_t time, uint32_t key, uint32_t state);
  void (*modifiers)(void *data,
      uint32_t depressed,
      uint32_t latched,
      uint32_t locked,
      uint32_t group);
  void (*repeat_info)(void *data, int32_t rate, int32_t delay);
};

struct saber_output_listener {
  void (*added)(void *data, struct saber_output *output);
  /* The output is still valid here and is destroyed as soon as this returns. */
  void (*removed)(void *data, struct saber_output *output);
  void (*changed)(void *data, struct saber_output *output);
};

struct saber_display {
  struct wl_display *wl_display;
  struct wl_registry *registry;

  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct wl_seat *seat;
  struct zwlr_layer_shell_v1 *layer_shell;
  struct wp_viewporter *viewporter;
  struct wp_fractional_scale_manager_v1 *fractional_scale_manager;
  struct xdg_wm_base *wm_base;
  struct xdg_activation_v1 *activation;
  struct wl_data_device_manager *data_device_manager;

  /* Bound here so all registry handling stays in one place, then OWNERSHIP
  TRANSFERS to saber_toplevels_create(), which stops and destroys it. Do not
  destroy it from saber_display_destroy() -- that is a double free. NULL on a
  compositor that does not advertise it, which costs the window list only. */
  struct zwlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;

  struct wl_list outputs; /* struct saber_output.link */

  struct wl_pointer *pointer;
  struct wl_keyboard *keyboard;
  uint32_t pointer_enter_serial;
  struct wl_surface *pointer_focus;
  struct wl_surface *keyboard_focus;

  struct wl_cursor_theme *cursor_theme;
  struct wl_surface *cursor_surface;
  char *cursor_theme_name;
  char *cursor_name;
  int cursor_base_size;
  int cursor_scale;

  const struct saber_pointer_listener *pointer_listener;
  void *pointer_data;
  const struct saber_keyboard_listener *keyboard_listener;
  void *keyboard_data;
  const struct saber_output_listener *output_listener;
  void *output_data;

  void (*on_disconnect)(void *data);
  void *disconnect_data;

  GSource *source;
};

/* Function purpose: Connect, bind every global the panel needs, and round-trip
twice so that outputs report their modes before the caller builds surfaces on
them. Returns NULL when the connection or a required global is missing. */
struct saber_display *
saber_display_create(const char *name);

void
saber_display_destroy(struct saber_display *display);

/* Function purpose: Drive the connection from a GLib main loop. This is the
integration point that makes a blocking g_main_loop_run safe against the
Wayland queue: the source uses wl_display_prepare_read / read_events /
dispatch_pending rather than wl_display_dispatch, which would block inside the
poll and deadlock against any other GLib source. */
bool
saber_display_attach(struct saber_display *display, GMainContext *context);

void
saber_display_detach(struct saber_display *display);

/* Function purpose: Called when the compositor goes away or the protocol
errors out. Saber has no session to save, so the handler's job is to stop the
main loop rather than to recover. */
void
saber_display_set_disconnect_handler(struct saber_display *display,
    void (*handler)(void *data),
    void *data);

void
saber_display_set_pointer_listener(struct saber_display *display,
    const struct saber_pointer_listener *listener,
    void *data);

void
saber_display_set_keyboard_listener(struct saber_display *display,
    const struct saber_keyboard_listener *listener,
    void *data);

/* Function purpose: Register for output arrival and departure. Set before the
caller starts creating surfaces; `added` fires immediately for every output
already known, so no arrival is missed between create and registration. */
void
saber_display_set_output_listener(struct saber_display *display,
    const struct saber_output_listener *listener,
    void *data);

struct saber_output *
saber_display_find_output(struct saber_display *display, const char *name);

/* Function purpose: Set the pointer image from the loaded theme. The compositor
does not advertise wp_cursor_shape_v1, so this is the only route: the theme is
loaded through libwayland-cursor honouring XCURSOR_THEME and XCURSOR_SIZE, and
reloaded when the output scale under the pointer changes. */
bool
saber_display_set_cursor(struct saber_display *display, const char *name);

void
saber_display_set_cursor_scale(struct saber_display *display, int scale);

void
saber_display_flush(struct saber_display *display);

#endif
