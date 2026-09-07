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
  /* Action purpose: wl_pointer carries a surface on enter and leave but NOT on
  motion, button or axis, so a dispatcher holding one listener has no way to
  tell whose surface an event belongs to and hands every module's input to
  whichever registered last. This predicate is how display.c resolves the owner
  once, on enter, and routes the rest of the stream to that module alone.
  Every registration must implement it; a listener that claims no surface
  receives nothing. */
  bool (*owns)(void *data, struct wl_surface *surface);
  void (*enter)(void *data, struct wl_surface *surface, double x, double y);
  void (*leave)(void *data, struct wl_surface *surface);
  void (*motion)(void *data, uint32_t time, double x, double y);
  void (*button)(void *data, uint32_t time, uint32_t button, uint32_t state);
  void (*axis)(void *data, uint32_t time, uint32_t axis, double value);

  /* Action purpose: The high-resolution half of a scroll, normalised. The wire
  carries it two incompatible ways -- wl_pointer.axis_discrete on v5 to v7 and
  wl_pointer.axis_value120 from v8, and a compositor sends one or the other,
  never both -- so display.c converts a discrete notch to 120 and this one
  callback covers every version. It arrives BEFORE the `axis` event it
  describes, in the same frame; a source with no detail (a touchpad) sends
  nothing here at all. */
  void (*axis_value120)(void *data, uint32_t axis, int32_t value120);
  void (*axis_source)(void *data, uint32_t axis_source);
  /* The fingers left the touchpad: whatever remainder an accumulator is
  holding belongs to a gesture that is over and must not be carried into the
  next one. */
  void (*axis_stop)(void *data, uint32_t time, uint32_t axis);
  void (*frame)(void *data);
};

/* Action purpose: wl_pointer reports scroll three ways that cannot be compared
directly -- a wheel notch as 10.0 continuous units, a touchpad as dozens of
fractional ones per gesture, and a detented source as a separate discrete or
v120 event ahead of the axis event. Reduced to a bare sign, as every call site
here did until Phase 12, one two-finger swipe fires dozens of steps. This holds
the conversion into one currency, v120 units where 120 is one notch, plus the
sub-notch remainder that makes a step cost a whole notch of travel.

One accumulator belongs to one scroll TARGET: reset it when the target changes,
or a half-notch left over from the tile above is spent on the tile below. */
struct saber_scroll_accum {
  int32_t detail; /* v120 the current frame announced ahead of its axis event */
  uint32_t detail_axis;
  bool has_detail;
  int32_t pending; /* accumulated v120 not yet worth a whole step */
};

/* Function purpose: Record the v120 detail for the axis event that is about to
follow it. Call from a listener's `axis_value120`; it never acts by itself,
because the axis event is what says the scroll happened. */
void
saber_scroll_detail(struct saber_scroll_accum *accum,
    uint32_t axis,
    int32_t value120);

/* Function purpose: The event's delta in v120 units -- the detail the frame
announced when it had one, and otherwise the continuous value converted at the
10.0-units-per-notch rate every wheel without detail reports. Call once per
`axis` event; it consumes the detail, so a frame's detail can never be spent
twice. Returns 0 for an axis the detail did not belong to. */
int32_t
saber_scroll_delta(struct saber_scroll_accum *accum,
    uint32_t axis,
    double value);

/* Function purpose: Whole steps from a v120 delta, keeping what is left over
for the next event. `per_step` is 120 for one step per notch; a caller wanting
a coarser gesture asks for a larger one. A reversal drops the remainder rather
than making the user unwind it. */
int
saber_scroll_steps(struct saber_scroll_accum *accum,
    int32_t value120,
    int32_t per_step);

void
saber_scroll_reset(struct saber_scroll_accum *accum);

/* One registered module. Ownership is resolved by asking each entry's `owns`,
so registration order carries no meaning and a module removing itself can never
displace another's registration. */
struct saber_pointer_registration {
  const struct saber_pointer_listener *listener;
  void *data;
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

  /* Take this through saber_display_take_foreign_toplevels(), never by hand:
  the field is cleared on the way out, and reading it directly is how a caller
  ends up with a pointer another module has already destroyed. */
  struct zwlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;

  /* The registry entry the manager is bound from, kept so the bind can happen
  after the round trips rather than during them. */
  uint32_t foreign_toplevel_global;
  uint32_t foreign_toplevel_version;
  bool foreign_toplevel_advertised;

  struct wl_list outputs; /* struct saber_output.link */

  struct wl_pointer *pointer;
  struct wl_keyboard *keyboard;
  /* Action purpose: Two serials, deliberately. wl_pointer.set_cursor is only
  honoured against the serial of the ENTER that put the pointer on the surface,
  while xdg_popup.grab and xdg_activation want the serial of the PRESS that
  asked for them. One field served both until 2026-09-07 and the button handler
  overwrote it, so the cursor stopped changing after the first click and every
  popup grab quoted a button-RELEASE serial. `pointer_press_serial` is written
  on press only, so it survives the release that opens a menu. */
  uint32_t pointer_enter_serial;
  uint32_t pointer_press_serial;
  struct wl_surface *pointer_focus;
  struct wl_surface *keyboard_focus;

  /* The keymap, cached so it can be replayed to a listener that registers
  after the seat bound -- wl_keyboard.keymap fires only once. Only ever holds
  a real XKB_V1 keymap; a NO_KEYMAP seat leaves the last good one in place. */
  char *keymap_data;
  size_t keymap_size;
  uint32_t keymap_format;

  /* The last modifier state, replayed with the keymap: a listener handed a
  fresh xkb_state would otherwise sit at group 0 with nothing latched, and
  type the wrong characters until the user next touched a modifier. */
  uint32_t mods_depressed, mods_latched, mods_locked, mods_group;
  bool mods_seen;

  struct wl_cursor_theme *cursor_theme;
  struct wl_surface *cursor_surface;
  char *cursor_theme_name;
  char *cursor_name;
  int cursor_base_size;
  int cursor_scale;

  /* Registered modules, and the one that owns the surface the pointer is on.
  `pointer_target` is resolved on enter and cleared on leave; it is the only
  route motion, button and axis events take. */
  GPtrArray *pointer_listeners;
  struct saber_pointer_registration *pointer_target;
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

/* Function purpose: Register a module for pointer input. Every registration
must supply `owns`, because that is what decides which module an event belongs
to; registrations are unordered and independent, so two modules can be open at
once without either consuming the other's clicks. Pair with
saber_display_remove_pointer_listener when the module's surfaces go away. */
void
saber_display_add_pointer_listener(struct saber_display *display,
    const struct saber_pointer_listener *listener,
    void *data);

/* Function purpose: Withdraw a registration made by
saber_display_add_pointer_listener. Matched on the listener/data pair, so a
module can only ever remove its own, and removing one that is not registered is
a no-op rather than an error. If the withdrawn module currently owns the
pointer, the target is dropped so no event reaches freed memory. */
void
saber_display_remove_pointer_listener(struct saber_display *display,
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

/* Function purpose: Hand the foreign-toplevel manager to whoever will listen on
it, transferring ownership. NULL when the compositor does not advertise the
protocol, and NULL on every call after the first.

Action purpose: This exists because the compositor answers the bind by
immediately sending one `toplevel` event per window that is already open. Bind
the manager inside saber_display_create's round trips and that burst is
dispatched before any listener exists, so every pre-existing window is lost and
the column comes up empty -- silently, looking exactly like a compositor without
the protocol. So the bind is deferred to the last statement of
saber_display_create, after both round trips, and the proxy is handed out only
through this call: the caller attaches its listener with no dispatch in between,
and cannot take the same proxy twice. */
struct zwlr_foreign_toplevel_manager_v1 *
saber_display_take_foreign_toplevels(struct saber_display *display);

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
