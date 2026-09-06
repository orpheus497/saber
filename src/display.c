/* Script function and purpose: Wayland connection and registry. Binds the
globals the panel needs, tracks outputs and the seat, loads a cursor theme by
hand because the compositor advertises no wp_cursor_shape_v1, and pumps the
connection from a GLib GSource. */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cairo.h>
#include <glib.h>
#include <wayland-client.h>
#include <wayland-cursor.h>

#include "fractional-scale-v1-protocol.h"
#include "viewporter-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"
#include "wlr-layer-shell-unstable-v1-protocol.h"
#include "xdg-activation-v1-protocol.h"
#include "xdg-shell-protocol.h"

#include <saber/display.h>

#define SABER_CURSOR_DEFAULT_SIZE 24
#define SABER_CURSOR_DEFAULT_NAME "left_ptr"

static uint32_t
version_min(uint32_t have, uint32_t want)
{
  return have < want ? have : want;
}

/* -- outputs ------------------------------------------------------------- */

static void
output_notify_changed(struct saber_output *output)
{
  struct saber_display *display = output->display;

  if (output->configured && display->output_listener != NULL &&
      display->output_listener->changed != NULL) {
    display->output_listener->changed(display->output_data, output);
  }
}

static void
output_handle_geometry(void *data,
    struct wl_output *wl_output,
    int32_t x,
    int32_t y,
    int32_t physical_width,
    int32_t physical_height,
    int32_t subpixel,
    const char *make,
    const char *model,
    int32_t transform)
{
  (void)wl_output;
  (void)x;
  (void)y;
  (void)physical_width;
  (void)physical_height;
  (void)subpixel;
  (void)make;
  (void)model;

  struct saber_output *output = data;
  output->transform = transform;
}

static void
output_handle_mode(void *data,
    struct wl_output *wl_output,
    uint32_t flags,
    int32_t width,
    int32_t height,
    int32_t refresh)
{
  (void)wl_output;
  (void)refresh;

  struct saber_output *output = data;

  if ((flags & WL_OUTPUT_MODE_CURRENT) != 0) {
    output->width = width;
    output->height = height;
  }
}

/* Action purpose: wl_output batches its properties and marks the end with
`done`. Reporting a change before it arrives would hand the caller a half
described output -- a name but no mode, or a mode but no scale. */
static void
output_handle_done(void *data, struct wl_output *wl_output)
{
  (void)wl_output;

  struct saber_output *output = data;
  struct saber_display *display = output->display;
  bool first = !output->configured;

  output->configured = true;

  if (first) {
    if (display->output_listener != NULL &&
        display->output_listener->added != NULL) {
      display->output_listener->added(display->output_data, output);
    }
  } else {
    output_notify_changed(output);
  }
}

static void
output_handle_scale(void *data, struct wl_output *wl_output, int32_t factor)
{
  (void)wl_output;

  struct saber_output *output = data;
  output->scale = factor > 0 ? factor : 1;
}

static void
output_handle_name(void *data, struct wl_output *wl_output, const char *name)
{
  (void)wl_output;

  struct saber_output *output = data;
  g_free(output->name);
  output->name = g_strdup(name);
}

static void
output_handle_description(void *data,
    struct wl_output *wl_output,
    const char *description)
{
  (void)wl_output;

  struct saber_output *output = data;
  g_free(output->description);
  output->description = g_strdup(description);
}

static const struct wl_output_listener output_listener = {
  .geometry = output_handle_geometry,
  .mode = output_handle_mode,
  .done = output_handle_done,
  .scale = output_handle_scale,
  .name = output_handle_name,
  .description = output_handle_description,
};

static void
output_destroy(struct saber_output *output)
{
  struct saber_display *display = output->display;

  if (output->configured && display->output_listener != NULL &&
      display->output_listener->removed != NULL) {
    display->output_listener->removed(display->output_data, output);
  }

  wl_list_remove(&output->link);

  if (wl_output_get_version(output->wl_output) >=
      WL_OUTPUT_RELEASE_SINCE_VERSION) {
    wl_output_release(output->wl_output);
  } else {
    wl_output_destroy(output->wl_output);
  }

  g_free(output->name);
  g_free(output->description);
  g_free(output);
}

/* -- cursor -------------------------------------------------------------- */

static bool
cursor_theme_reload(struct saber_display *display, int scale)
{
  if (display->shm == NULL || scale <= 0) {
    return false;
  }

  struct wl_cursor_theme *theme = wl_cursor_theme_load(
      display->cursor_theme_name, display->cursor_base_size * scale,
      display->shm);

  if (theme == NULL) {
    return false;
  }

  if (display->cursor_theme != NULL) {
    wl_cursor_theme_destroy(display->cursor_theme);
  }

  display->cursor_theme = theme;
  display->cursor_scale = scale;

  return true;
}

/* Action purpose: XCURSOR_THEME and XCURSOR_SIZE are the only configuration a
Wayland client gets for cursors -- there is no per-output theme and no protocol
to ask the compositor. Read them once here so the whole panel agrees. */
static void
cursor_init(struct saber_display *display)
{
  const char *theme = g_getenv("XCURSOR_THEME");
  const char *size = g_getenv("XCURSOR_SIZE");

  display->cursor_theme_name = (theme != NULL && *theme != '\0')
      ? g_strdup(theme)
      : NULL;

  display->cursor_base_size = SABER_CURSOR_DEFAULT_SIZE;

  if (size != NULL && *size != '\0') {
    int parsed = atoi(size);

    if (parsed > 0) {
      display->cursor_base_size = parsed;
    }
  }

  display->cursor_name = g_strdup(SABER_CURSOR_DEFAULT_NAME);
  display->cursor_scale = 0;

  if (display->compositor != NULL) {
    display->cursor_surface = wl_compositor_create_surface(display->compositor);
  }

  cursor_theme_reload(display, 1);
}

bool
saber_display_set_cursor(struct saber_display *display, const char *name)
{
  if (name == NULL) {
    name = SABER_CURSOR_DEFAULT_NAME;
  }

  if (display->cursor_name == NULL || strcmp(display->cursor_name, name) != 0) {
    g_free(display->cursor_name);
    display->cursor_name = g_strdup(name);
  }

  if (display->pointer == NULL || display->cursor_theme == NULL ||
      display->cursor_surface == NULL) {
    return false;
  }

  struct wl_cursor *cursor =
      wl_cursor_theme_get_cursor(display->cursor_theme, name);

  /* Action purpose: Theme sets are inconsistent about the pointer's name --
  the X11 legacy name and the CSS name both appear in the wild, and a theme
  carrying only one of them would otherwise leave the panel with no cursor at
  all. */
  if (cursor == NULL) {
    const char *fallback = strcmp(name, "default") == 0
        ? SABER_CURSOR_DEFAULT_NAME
        : "default";
    cursor = wl_cursor_theme_get_cursor(display->cursor_theme, fallback);
  }

  if (cursor == NULL || cursor->image_count == 0) {
    return false;
  }

  struct wl_cursor_image *image = cursor->images[0];
  struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);

  if (buffer == NULL) {
    return false;
  }

  int scale = display->cursor_scale > 0 ? display->cursor_scale : 1;

  wl_surface_set_buffer_scale(display->cursor_surface, scale);
  wl_surface_attach(display->cursor_surface, buffer, 0, 0);
  wl_surface_damage_buffer(display->cursor_surface, 0, 0,
      (int32_t)image->width, (int32_t)image->height);
  wl_surface_commit(display->cursor_surface);

  wl_pointer_set_cursor(display->pointer,
      display->pointer_enter_serial,
      display->cursor_surface,
      (int32_t)image->hotspot_x / scale,
      (int32_t)image->hotspot_y / scale);

  return true;
}

void
saber_display_set_cursor_scale(struct saber_display *display, int scale)
{
  if (scale <= 0 || scale == display->cursor_scale) {
    return;
  }

  if (cursor_theme_reload(display, scale)) {
    saber_display_set_cursor(display, display->cursor_name);
  }
}

/* -- seat ---------------------------------------------------------------- */

static void
pointer_handle_enter(void *data,
    struct wl_pointer *wl_pointer,
    uint32_t serial,
    struct wl_surface *surface,
    wl_fixed_t x,
    wl_fixed_t y)
{
  (void)wl_pointer;

  struct saber_display *display = data;

  display->pointer_enter_serial = serial;
  display->pointer_focus = surface;

  saber_display_set_cursor(display, display->cursor_name);

  if (display->pointer_listener != NULL &&
      display->pointer_listener->enter != NULL) {
    display->pointer_listener->enter(display->pointer_data, surface,
        wl_fixed_to_double(x), wl_fixed_to_double(y));
  }
}

static void
pointer_handle_leave(void *data,
    struct wl_pointer *wl_pointer,
    uint32_t serial,
    struct wl_surface *surface)
{
  (void)wl_pointer;
  (void)serial;

  struct saber_display *display = data;

  display->pointer_focus = NULL;

  if (display->pointer_listener != NULL &&
      display->pointer_listener->leave != NULL) {
    display->pointer_listener->leave(display->pointer_data, surface);
  }
}

static void
pointer_handle_motion(void *data,
    struct wl_pointer *wl_pointer,
    uint32_t time,
    wl_fixed_t x,
    wl_fixed_t y)
{
  (void)wl_pointer;

  struct saber_display *display = data;

  if (display->pointer_listener != NULL &&
      display->pointer_listener->motion != NULL) {
    display->pointer_listener->motion(display->pointer_data, time,
        wl_fixed_to_double(x), wl_fixed_to_double(y));
  }
}

static void
pointer_handle_button(void *data,
    struct wl_pointer *wl_pointer,
    uint32_t serial,
    uint32_t time,
    uint32_t button,
    uint32_t state)
{
  (void)wl_pointer;

  struct saber_display *display = data;

  /* Action purpose: The button serial, not the enter serial, is what a
  compositor accepts for a drag or a popup grab, so it replaces the stored
  one. */
  display->pointer_enter_serial = serial;

  if (display->pointer_listener != NULL &&
      display->pointer_listener->button != NULL) {
    display->pointer_listener->button(display->pointer_data, time, button,
        state);
  }
}

static void
pointer_handle_axis(void *data,
    struct wl_pointer *wl_pointer,
    uint32_t time,
    uint32_t axis,
    wl_fixed_t value)
{
  (void)wl_pointer;

  struct saber_display *display = data;

  if (display->pointer_listener != NULL &&
      display->pointer_listener->axis != NULL) {
    display->pointer_listener->axis(display->pointer_data, time, axis,
        wl_fixed_to_double(value));
  }
}

static void
pointer_handle_frame(void *data, struct wl_pointer *wl_pointer)
{
  (void)wl_pointer;

  struct saber_display *display = data;

  if (display->pointer_listener != NULL &&
      display->pointer_listener->frame != NULL) {
    display->pointer_listener->frame(display->pointer_data);
  }
}

static void
pointer_handle_axis_source(void *data,
    struct wl_pointer *wl_pointer,
    uint32_t axis_source)
{
  (void)data;
  (void)wl_pointer;
  (void)axis_source;
}

static void
pointer_handle_axis_stop(void *data,
    struct wl_pointer *wl_pointer,
    uint32_t time,
    uint32_t axis)
{
  (void)data;
  (void)wl_pointer;
  (void)time;
  (void)axis;
}

static void
pointer_handle_axis_discrete(void *data,
    struct wl_pointer *wl_pointer,
    uint32_t axis,
    int32_t discrete)
{
  (void)data;
  (void)wl_pointer;
  (void)axis;
  (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
  .enter = pointer_handle_enter,
  .leave = pointer_handle_leave,
  .motion = pointer_handle_motion,
  .button = pointer_handle_button,
  .axis = pointer_handle_axis,
  .frame = pointer_handle_frame,
  .axis_source = pointer_handle_axis_source,
  .axis_stop = pointer_handle_axis_stop,
  .axis_discrete = pointer_handle_axis_discrete,
};

/* Function purpose: Re-create a mappable fd holding the cached keymap, so a
listener registered after the seat bound still receives one.

Action purpose: wl_keyboard.keymap fires exactly once, when the seat announces
its keyboard, and that is long before the Dash or a menu exists. Forwarding it
only to whoever happened to be listening at that instant leaves every later
listener falling back to xkb_keymap_new_from_names(NULL) -- the compiled-in US
default -- so a search field types the wrong characters for anyone whose layout
is not US, silently and only for text entry. Caching the bytes and handing out
a fresh fd is what makes registration order stop mattering. */
static int
keymap_replay_fd(struct saber_display *display)
{
  int fd = -1;

#if defined(SHM_ANON)
  fd = shm_open(SHM_ANON, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
#else
  char name[64];

  g_snprintf(name, sizeof(name), "/saber-keymap-%d", (int)getpid());
  fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

  if (fd >= 0) {
    shm_unlink(name);
  }
#endif

  if (fd < 0) {
    return -1;
  }

  if (ftruncate(fd, (off_t)display->keymap_size) < 0) {
    close(fd);
    return -1;
  }

  ssize_t written = write(fd, display->keymap_data, display->keymap_size);

  if (written < 0 || (size_t)written != display->keymap_size) {
    close(fd);
    return -1;
  }

  /* The consumer mmaps from offset 0; write(2) left the cursor at the end. */
  if (lseek(fd, 0, SEEK_SET) == (off_t)-1) {
    close(fd);
    return -1;
  }

  return fd;
}

/* Action purpose: The keymap and the modifier state travel together. Handing a
listener the keymap alone leaves it compiling a fresh xkb_state -- group 0, no
lock, nothing latched -- while the seat may be on a second layout group or have
Caps Lock down, and every keystroke it then resolves is the wrong character. */
static void
keymap_deliver(struct saber_display *display)
{
  if (display->keymap_data == NULL || display->keyboard_listener == NULL ||
      display->keyboard_listener->keymap == NULL) {
    return;
  }

  int fd = keymap_replay_fd(display);

  if (fd < 0) {
    return;
  }

  /* Snapshotted, because the keymap handler is entitled to install a different
  listener and the modifiers must not then be handed to it with the previous
  listener's data. */
  const struct saber_keyboard_listener *listener = display->keyboard_listener;
  void *listener_data = display->keyboard_data;

  listener->keymap(listener_data, display->keymap_format, fd,
      (uint32_t)display->keymap_size);

  if (display->mods_seen && listener->modifiers != NULL) {
    listener->modifiers(listener_data, display->mods_depressed,
        display->mods_latched, display->mods_locked, display->mods_group);
  }
}

static void
keyboard_handle_keymap(void *data,
    struct wl_keyboard *wl_keyboard,
    uint32_t format,
    int32_t fd,
    uint32_t size)
{
  (void)wl_keyboard;

  struct saber_display *display = data;

  /* Action purpose: Cache only a real keymap. A seat left with no keyboard --
  which happens whenever a virtual-keyboard client comes and goes -- announces
  itself as NO_KEYMAP with size 0, and mmapping that is invalid; caching it
  would replace the good keymap every later listener depends on with nothing.
  Cache before forwarding, because the listener owns the fd once it has it and
  is entitled to close it. */
  if (format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 && size > 0) {
    void *mapped = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

    if (mapped != MAP_FAILED) {
      g_free(display->keymap_data);
      display->keymap_data = g_memdup2(mapped, size);
      display->keymap_size = size;
      display->keymap_format = format;
      munmap(mapped, size);
    }
  }

  if (display->keyboard_listener != NULL &&
      display->keyboard_listener->keymap != NULL) {
    display->keyboard_listener->keymap(display->keyboard_data, format, fd,
        size);
  } else {
    close(fd);
  }
}

static void
keyboard_handle_enter(void *data,
    struct wl_keyboard *wl_keyboard,
    uint32_t serial,
    struct wl_surface *surface,
    struct wl_array *keys)
{
  (void)wl_keyboard;
  (void)serial;
  (void)keys;

  struct saber_display *display = data;
  display->keyboard_focus = surface;

  if (display->keyboard_listener != NULL &&
      display->keyboard_listener->enter != NULL) {
    display->keyboard_listener->enter(display->keyboard_data, surface);
  }
}

static void
keyboard_handle_leave(void *data,
    struct wl_keyboard *wl_keyboard,
    uint32_t serial,
    struct wl_surface *surface)
{
  (void)wl_keyboard;
  (void)serial;

  struct saber_display *display = data;
  display->keyboard_focus = NULL;

  if (display->keyboard_listener != NULL &&
      display->keyboard_listener->leave != NULL) {
    display->keyboard_listener->leave(display->keyboard_data, surface);
  }
}

static void
keyboard_handle_key(void *data,
    struct wl_keyboard *wl_keyboard,
    uint32_t serial,
    uint32_t time,
    uint32_t key,
    uint32_t state)
{
  (void)wl_keyboard;
  (void)serial;

  struct saber_display *display = data;

  if (display->keyboard_listener != NULL &&
      display->keyboard_listener->key != NULL) {
    display->keyboard_listener->key(display->keyboard_data, time, key, state);
  }
}

static void
keyboard_handle_modifiers(void *data,
    struct wl_keyboard *wl_keyboard,
    uint32_t serial,
    uint32_t depressed,
    uint32_t latched,
    uint32_t locked,
    uint32_t group)
{
  (void)wl_keyboard;
  (void)serial;

  struct saber_display *display = data;

  display->mods_depressed = depressed;
  display->mods_latched = latched;
  display->mods_locked = locked;
  display->mods_group = group;
  display->mods_seen = true;

  if (display->keyboard_listener != NULL &&
      display->keyboard_listener->modifiers != NULL) {
    display->keyboard_listener->modifiers(display->keyboard_data, depressed,
        latched, locked, group);
  }
}

static void
keyboard_handle_repeat_info(void *data,
    struct wl_keyboard *wl_keyboard,
    int32_t rate,
    int32_t delay)
{
  (void)wl_keyboard;

  struct saber_display *display = data;

  if (display->keyboard_listener != NULL &&
      display->keyboard_listener->repeat_info != NULL) {
    display->keyboard_listener->repeat_info(display->keyboard_data, rate,
        delay);
  }
}

static const struct wl_keyboard_listener keyboard_listener = {
  .keymap = keyboard_handle_keymap,
  .enter = keyboard_handle_enter,
  .leave = keyboard_handle_leave,
  .key = keyboard_handle_key,
  .modifiers = keyboard_handle_modifiers,
  .repeat_info = keyboard_handle_repeat_info,
};

static void
seat_handle_capabilities(void *data,
    struct wl_seat *wl_seat,
    uint32_t capabilities)
{
  struct saber_display *display = data;
  bool has_pointer = (capabilities & WL_SEAT_CAPABILITY_POINTER) != 0;
  bool has_keyboard = (capabilities & WL_SEAT_CAPABILITY_KEYBOARD) != 0;

  if (has_pointer && display->pointer == NULL) {
    display->pointer = wl_seat_get_pointer(wl_seat);
    wl_pointer_add_listener(display->pointer, &pointer_listener, display);
  } else if (!has_pointer && display->pointer != NULL) {
    if (wl_pointer_get_version(display->pointer) >=
        WL_POINTER_RELEASE_SINCE_VERSION) {
      wl_pointer_release(display->pointer);
    } else {
      wl_pointer_destroy(display->pointer);
    }
    display->pointer = NULL;
    display->pointer_focus = NULL;
  }

  if (has_keyboard && display->keyboard == NULL) {
    display->keyboard = wl_seat_get_keyboard(wl_seat);
    wl_keyboard_add_listener(display->keyboard, &keyboard_listener, display);
  } else if (!has_keyboard && display->keyboard != NULL) {
    if (wl_keyboard_get_version(display->keyboard) >=
        WL_KEYBOARD_RELEASE_SINCE_VERSION) {
      wl_keyboard_release(display->keyboard);
    } else {
      wl_keyboard_destroy(display->keyboard);
    }
    display->keyboard = NULL;
    display->keyboard_focus = NULL;
  }
}

static void
seat_handle_name(void *data, struct wl_seat *wl_seat, const char *name)
{
  (void)data;
  (void)wl_seat;
  (void)name;
}

static const struct wl_seat_listener seat_listener = {
  .capabilities = seat_handle_capabilities,
  .name = seat_handle_name,
};

static void
wm_base_handle_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
  (void)data;
  xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
  .ping = wm_base_handle_ping,
};

/* -- registry ------------------------------------------------------------ */

static void
registry_handle_global(void *data,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version)
{
  struct saber_display *display = data;

  if (strcmp(interface, wl_compositor_interface.name) == 0) {
    display->compositor = wl_registry_bind(registry, name,
        &wl_compositor_interface, version_min(version, 4));
  } else if (strcmp(interface, wl_shm_interface.name) == 0) {
    display->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
  } else if (strcmp(interface, wl_seat_interface.name) == 0 &&
      display->seat == NULL) {
    display->seat = wl_registry_bind(registry, name, &wl_seat_interface,
        version_min(version, 7));
    wl_seat_add_listener(display->seat, &seat_listener, display);
  } else if (strcmp(interface, wl_output_interface.name) == 0) {
    struct saber_output *output = g_new0(struct saber_output, 1);

    output->display = display;
    output->global = name;
    output->scale = 1;
    output->wl_output = wl_registry_bind(registry, name, &wl_output_interface,
        version_min(version, 4));

    wl_list_insert(display->outputs.prev, &output->link);
    wl_output_add_listener(output->wl_output, &output_listener, output);
  } else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
    display->layer_shell = wl_registry_bind(registry, name,
        &zwlr_layer_shell_v1_interface, version_min(version, 4));
  } else if (strcmp(interface, wp_viewporter_interface.name) == 0) {
    display->viewporter = wl_registry_bind(registry, name,
        &wp_viewporter_interface, 1);
  } else if (strcmp(interface,
                 wp_fractional_scale_manager_v1_interface.name) == 0) {
    display->fractional_scale_manager = wl_registry_bind(registry, name,
        &wp_fractional_scale_manager_v1_interface, 1);
  } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
    display->wm_base = wl_registry_bind(registry, name, &xdg_wm_base_interface,
        version_min(version, 5));
    xdg_wm_base_add_listener(display->wm_base, &wm_base_listener, display);
  } else if (strcmp(interface, xdg_activation_v1_interface.name) == 0) {
    display->activation = wl_registry_bind(registry, name,
        &xdg_activation_v1_interface, 1);
  } else if (strcmp(interface,
                 zwlr_foreign_toplevel_manager_v1_interface.name) == 0) {
    /* Recorded, not bound -- see saber_display_take_foreign_toplevels. */
    display->foreign_toplevel_global = name;
    display->foreign_toplevel_version = version_min(version, 3);
    display->foreign_toplevel_advertised = true;
  } else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
    display->data_device_manager = wl_registry_bind(registry, name,
        &wl_data_device_manager_interface, version_min(version, 3));
  }
}

static void
registry_handle_global_remove(void *data,
    struct wl_registry *registry,
    uint32_t name)
{
  (void)registry;

  struct saber_display *display = data;
  struct saber_output *output, *tmp;

  wl_list_for_each_safe(output, tmp, &display->outputs, link) {
    if (output->global == name) {
      output_destroy(output);
      return;
    }
  }
}

static const struct wl_registry_listener registry_listener = {
  .global = registry_handle_global,
  .global_remove = registry_handle_global_remove,
};

/* -- GLib main loop integration ------------------------------------------ */

/* Action purpose: The whole point of a hand-written GSource rather than
g_unix_fd_add(). wl_display_dispatch() blocks in its own poll, so calling it
from a GLib callback stalls every other source; and reading the Wayland fd
without the prepare_read / read_events handshake races any other thread and
loses the "one reader" guarantee libwayland depends on. This source therefore
splits the operation exactly across GLib's three phases:

  prepare  announce the intent to read, then flush outgoing requests
  check    read what arrived, or cancel the announced read if nothing did
  dispatch drain the queue into the protocol listeners

`queued` exists because prepare may find events already in the queue -- from a
round trip, or from a request made inside a callback -- in which case there is
nothing to poll for and check must still ask for a dispatch, or the loop spins
with the events never delivered. */
struct saber_wayland_source {
  GSource base;
  struct saber_display *display;
  gpointer fd_tag;
  bool reading;
  bool queued;
  bool failed;
};

static gboolean
wayland_source_prepare(GSource *source, gint *timeout)
{
  struct saber_wayland_source *self = (struct saber_wayland_source *)source;
  struct wl_display *wl_display = self->display->wl_display;

  *timeout = -1;

  if (self->failed) {
    return TRUE;
  }

  if (self->reading) {
    return FALSE;
  }

  if (wl_display_prepare_read(wl_display) != 0) {
    self->queued = true;
    return TRUE;
  }

  self->reading = true;

  GIOCondition events = G_IO_IN | G_IO_ERR | G_IO_HUP;

  /* Action purpose: A partial flush is not an error -- the socket buffer is
  full. Polling for writability as well lets the next iteration finish the
  flush instead of dropping the requests. Anything else means the connection
  is gone, and the announced read must be cancelled before unwinding or
  libwayland is left holding the reader lock. */
  if (wl_display_flush(wl_display) < 0) {
    if (errno == EAGAIN) {
      events |= G_IO_OUT;
    } else {
      wl_display_cancel_read(wl_display);
      self->reading = false;
      self->failed = true;
      return TRUE;
    }
  }

  g_source_modify_unix_fd(source, self->fd_tag, events);

  return FALSE;
}

static gboolean
wayland_source_check(GSource *source)
{
  struct saber_wayland_source *self = (struct saber_wayland_source *)source;
  struct wl_display *wl_display = self->display->wl_display;
  GIOCondition revents = g_source_query_unix_fd(source, self->fd_tag);

  if (self->reading) {
    if ((revents & G_IO_IN) != 0) {
      if (wl_display_read_events(wl_display) < 0) {
        self->failed = true;
      } else {
        self->queued = true;
      }
    } else {
      wl_display_cancel_read(wl_display);
    }

    self->reading = false;
  }

  if ((revents & (G_IO_ERR | G_IO_HUP)) != 0) {
    self->failed = true;
  }

  return self->failed || self->queued || (revents & G_IO_OUT) != 0;
}

static gboolean
wayland_source_dispatch(GSource *source,
    GSourceFunc callback,
    gpointer user_data)
{
  (void)callback;
  (void)user_data;

  struct saber_wayland_source *self = (struct saber_wayland_source *)source;
  struct saber_display *display = self->display;

  if (!self->failed) {
    self->queued = false;

    if (wl_display_dispatch_pending(display->wl_display) < 0) {
      self->failed = true;
    } else if (wl_display_flush(display->wl_display) < 0 && errno != EAGAIN) {
      self->failed = true;
    }
  }

  if (self->failed) {
    if (display->on_disconnect != NULL) {
      display->on_disconnect(display->disconnect_data);
    }

    return G_SOURCE_REMOVE;
  }

  return G_SOURCE_CONTINUE;
}

/* Action purpose: Balance a read announced in prepare that no check ever
consumed, which is what happens when the source is destroyed between the two.
Leaving it unbalanced would strand libwayland's reader lock and hang the next
prepare_read. saber_display_detach runs before the connection is closed, so the
display is still valid here. */
static void
wayland_source_finalize(GSource *source)
{
  struct saber_wayland_source *self = (struct saber_wayland_source *)source;

  if (self->reading) {
    wl_display_cancel_read(self->display->wl_display);
    self->reading = false;
  }
}

static GSourceFuncs wayland_source_funcs = {
  .prepare = wayland_source_prepare,
  .check = wayland_source_check,
  .dispatch = wayland_source_dispatch,
  .finalize = wayland_source_finalize,
};

bool
saber_display_attach(struct saber_display *display, GMainContext *context)
{
  if (display->source != NULL) {
    return true;
  }

  GSource *source = g_source_new(&wayland_source_funcs,
      sizeof(struct saber_wayland_source));
  struct saber_wayland_source *self = (struct saber_wayland_source *)source;

  self->display = display;
  self->reading = false;
  self->queued = false;
  self->failed = false;

  g_source_set_name(source, "saber-wayland");
  g_source_set_priority(source, G_PRIORITY_DEFAULT);
  g_source_set_can_recurse(source, FALSE);

  self->fd_tag = g_source_add_unix_fd(source,
      wl_display_get_fd(display->wl_display),
      G_IO_IN | G_IO_ERR | G_IO_HUP);

  g_source_attach(source, context);
  display->source = source;

  return true;
}

void
saber_display_detach(struct saber_display *display)
{
  if (display->source == NULL) {
    return;
  }

  g_source_destroy(display->source);
  g_source_unref(display->source);
  display->source = NULL;
}

/* -- lifecycle ----------------------------------------------------------- */

void
saber_display_set_disconnect_handler(struct saber_display *display,
    void (*handler)(void *data),
    void *data)
{
  display->on_disconnect = handler;
  display->disconnect_data = data;
}

void
saber_display_set_pointer_listener(struct saber_display *display,
    const struct saber_pointer_listener *listener,
    void *data)
{
  display->pointer_listener = listener;
  display->pointer_data = data;
}

void
saber_display_set_keyboard_listener(struct saber_display *display,
    const struct saber_keyboard_listener *listener,
    void *data)
{
  display->keyboard_listener = listener;
  display->keyboard_data = data;

  /* Replay the keymap the seat already sent, so registration order does not
  decide whether this listener knows the user's layout. */
  keymap_deliver(display);
}

void
saber_display_set_output_listener(struct saber_display *display,
    const struct saber_output_listener *listener,
    void *data)
{
  display->output_listener = listener;
  display->output_data = data;

  if (listener == NULL || listener->added == NULL) {
    return;
  }

  struct saber_output *output, *tmp;

  wl_list_for_each_safe(output, tmp, &display->outputs, link) {
    if (output->configured) {
      listener->added(data, output);
    }
  }
}

struct saber_output *
saber_display_find_output(struct saber_display *display, const char *name)
{
  struct saber_output *output;

  wl_list_for_each(output, &display->outputs, link) {
    if (name == NULL) {
      return output;
    }

    if (output->name != NULL && strcmp(output->name, name) == 0) {
      return output;
    }
  }

  return NULL;
}

static void
foreign_toplevels_bind(struct saber_display *display)
{
  if (display->foreign_toplevel_manager != NULL ||
      !display->foreign_toplevel_advertised) {
    return;
  }

  display->foreign_toplevel_manager = wl_registry_bind(display->registry,
      display->foreign_toplevel_global,
      &zwlr_foreign_toplevel_manager_v1_interface,
      display->foreign_toplevel_version);
}

struct zwlr_foreign_toplevel_manager_v1 *
saber_display_take_foreign_toplevels(struct saber_display *display)
{
  foreign_toplevels_bind(display);

  struct zwlr_foreign_toplevel_manager_v1 *manager =
      display->foreign_toplevel_manager;

  display->foreign_toplevel_manager = NULL;

  return manager;
}

void
saber_display_flush(struct saber_display *display)
{
  while (wl_display_flush(display->wl_display) < 0) {
    if (errno != EINTR) {
      break;
    }
  }
}

struct saber_display *
saber_display_create(const char *name)
{
  struct saber_display *display = g_new0(struct saber_display, 1);

  wl_list_init(&display->outputs);

  display->wl_display = wl_display_connect(name);

  if (display->wl_display == NULL) {
    g_free(display);
    return NULL;
  }

  display->registry = wl_display_get_registry(display->wl_display);
  wl_registry_add_listener(display->registry, &registry_listener, display);

  /* Action purpose: Two round trips, not one. The first delivers the globals;
  the second delivers the events those globals then emit -- wl_output's mode,
  scale and name, and wl_seat's capabilities -- so the caller sees fully
  described outputs and a seat with its devices already bound. */
  if (wl_display_roundtrip(display->wl_display) < 0 ||
      wl_display_roundtrip(display->wl_display) < 0) {
    saber_display_destroy(display);
    return NULL;
  }

  if (display->compositor == NULL || display->shm == NULL ||
      display->layer_shell == NULL) {
    saber_display_destroy(display);
    return NULL;
  }

  cursor_init(display);

  /* Action purpose: LAST, and it must stay last. The compositor answers this
  bind with one `toplevel` event per window that is already open; binding it
  above, with a round trip still to come, dispatches that burst into nothing.
  Bound here it cannot be delivered until the caller pumps the connection,
  which is after saber_display_take_foreign_toplevels has handed the proxy to
  a listener. Nothing may dispatch, round-trip or run the main loop below. */
  foreign_toplevels_bind(display);

  return display;
}

void
saber_display_destroy(struct saber_display *display)
{
  if (display == NULL) {
    return;
  }

  saber_display_detach(display);

  struct saber_output *output, *tmp;

  wl_list_for_each_safe(output, tmp, &display->outputs, link) {
    output_destroy(output);
  }

  if (display->cursor_surface != NULL) {
    wl_surface_destroy(display->cursor_surface);
  }

  if (display->cursor_theme != NULL) {
    wl_cursor_theme_destroy(display->cursor_theme);
  }

  g_free(display->keymap_data);
  g_free(display->cursor_theme_name);
  g_free(display->cursor_name);

  if (display->pointer != NULL) {
    wl_pointer_destroy(display->pointer);
  }

  if (display->keyboard != NULL) {
    wl_keyboard_destroy(display->keyboard);
  }

  if (display->seat != NULL) {
    wl_seat_destroy(display->seat);
  }

  if (display->data_device_manager != NULL) {
    wl_data_device_manager_destroy(display->data_device_manager);
  }

  /* Only ever non-NULL when nothing took it; the taker owns it otherwise. */
  if (display->foreign_toplevel_manager != NULL) {
    zwlr_foreign_toplevel_manager_v1_destroy(display->foreign_toplevel_manager);
  }

  if (display->activation != NULL) {
    xdg_activation_v1_destroy(display->activation);
  }

  if (display->wm_base != NULL) {
    xdg_wm_base_destroy(display->wm_base);
  }

  if (display->fractional_scale_manager != NULL) {
    wp_fractional_scale_manager_v1_destroy(display->fractional_scale_manager);
  }

  if (display->viewporter != NULL) {
    wp_viewporter_destroy(display->viewporter);
  }

  if (display->layer_shell != NULL) {
    zwlr_layer_shell_v1_destroy(display->layer_shell);
  }

  if (display->shm != NULL) {
    wl_shm_destroy(display->shm);
  }

  if (display->compositor != NULL) {
    wl_compositor_destroy(display->compositor);
  }

  if (display->registry != NULL) {
    wl_registry_destroy(display->registry);
  }

  wl_display_disconnect(display->wl_display);
  g_free(display);
}
