/* Script function and purpose: Inject real keyboard and pointer events into the
running compositor, so the click-driven half of Saber can be tested instead of
reasoned about.

Saber is a layer-shell client. Nothing in the tree can be exercised by calling a
function: a tile press, a click-outside dismissal, a wheel scroll over the Dash
grid are all reachable only through the seat. `wtype`, `ydotool` and `wlrctl` are
absent on this host and `xdotool` is X11, so this exists.

It is a separate binary and is deliberately NOT built by `all` -- it is a test
harness, not part of the product, and nothing Saber ships depends on it.

Two protocols carry the work, both advertised by hikari-sakura and both vendored
under protocol/ because wlr-protocols is not packaged on FreeBSD:
zwp_virtual_keyboard_manager_v1 and zwlr_virtual_pointer_manager_v1.

Usage:
  vinput outputs
  vinput [--output NAME] [--verbose] CMD [ARGS] [+ CMD [ARGS] ...]

Commands:
  move X Y        absolute position, in the target output's coordinates
  motion DX DY    relative movement
  click BUTTON    press then release (left | right | middle)
  press BUTTON    button down
  release BUTTON  button up
  scroll STEPS    vertical wheel; negative scrolls up
  hscroll STEPS   horizontal wheel; negative scrolls left
  key NAME        one keysym by name, pressed and released (e.g. Escape, Tab, a)
  type TEXT       each character of TEXT as a keystroke
  sleep MS        pause, for letting the compositor settle between steps

Example -- open the Dash on the second output and wheel the grid down:
  vinput --output HDMI-A-1 key super+space + sleep 400 + move 300 500 \
      + scroll 3 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "virtual-keyboard-unstable-v1-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-protocol.h"

/* Wire button codes. Taken as literals rather than through
<dev/evdev/input-event-codes.h> so this tool builds anywhere the rest does; the
values are protocol constants and cannot drift. */
#define VIN_BTN_LEFT 0x110
#define VIN_BTN_RIGHT 0x111
#define VIN_BTN_MIDDLE 0x112

/* One wheel detent, in the fixed-point units wl_pointer.axis carries. */
#define VIN_WHEEL_STEP 10.0

struct vin_output {
  struct wl_output *output;
  uint32_t name_id;
  char *name;
  char *description;
  int32_t width;
  int32_t height;
  int32_t x;
  int32_t y;
  struct vin_output *next;
};

struct vin {
  struct wl_display *display;
  struct wl_registry *registry;
  struct wl_seat *seat;
  struct zwp_virtual_keyboard_manager_v1 *keyboard_manager;
  struct zwlr_virtual_pointer_manager_v1 *pointer_manager;
  uint32_t pointer_manager_version;

  struct zwp_virtual_keyboard_v1 *keyboard;
  struct zwlr_virtual_pointer_v1 *pointer;

  struct vin_output *outputs;
  struct vin_output *target;
  const char *target_name;

  struct xkb_context *xkb;
  struct xkb_keymap *keymap;
  xkb_mod_index_t shift;

  bool verbose;
  uint32_t time;
};

static void
vin_log(const struct vin *vin, const char *fmt, ...)
{
  if (!vin->verbose) {
    return;
  }

  va_list ap;

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

/* Function purpose: A monotonically rising millisecond stamp. The compositor
only requires that timestamps do not go backwards within a device. */
static uint32_t
vin_now(struct vin *vin)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);

  uint32_t now = (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);

  if (now <= vin->time) {
    now = vin->time + 1;
  }

  vin->time = now;
  return now;
}

static void
vin_sleep_ms(int ms)
{
  struct timespec ts = {
    .tv_sec = ms / 1000,
    .tv_nsec = (long)(ms % 1000) * 1000000L,
  };

  nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------- outputs */

static void
output_geometry(void *data,
    struct wl_output *output,
    int32_t x,
    int32_t y,
    int32_t physical_width,
    int32_t physical_height,
    int32_t subpixel,
    const char *make,
    const char *model,
    int32_t transform)
{
  (void)output;
  (void)physical_width;
  (void)physical_height;
  (void)subpixel;
  (void)make;
  (void)model;
  (void)transform;

  struct vin_output *out = data;

  out->x = x;
  out->y = y;
}

static void
output_mode(void *data,
    struct wl_output *output,
    uint32_t flags,
    int32_t width,
    int32_t height,
    int32_t refresh)
{
  (void)output;
  (void)refresh;

  struct vin_output *out = data;

  if ((flags & WL_OUTPUT_MODE_CURRENT) != 0) {
    out->width = width;
    out->height = height;
  }
}

static void
output_done(void *data, struct wl_output *output)
{
  (void)data;
  (void)output;
}

static void
output_scale(void *data, struct wl_output *output, int32_t factor)
{
  (void)data;
  (void)output;
  (void)factor;
}

static void
output_name(void *data, struct wl_output *output, const char *name)
{
  (void)output;

  struct vin_output *out = data;

  free(out->name);
  out->name = name != NULL ? strdup(name) : NULL;
}

static void
output_description(void *data, struct wl_output *output, const char *description)
{
  (void)output;

  struct vin_output *out = data;

  free(out->description);
  out->description = description != NULL ? strdup(description) : NULL;
}

static const struct wl_output_listener output_listener = {
  .geometry = output_geometry,
  .mode = output_mode,
  .done = output_done,
  .scale = output_scale,
  .name = output_name,
  .description = output_description,
};

/* ------------------------------------------------------------ registry */

static void
registry_global(void *data,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version)
{
  struct vin *vin = data;

  if (strcmp(interface, wl_seat_interface.name) == 0) {
    vin->seat = wl_registry_bind(registry, name, &wl_seat_interface,
        version < 7 ? version : 7);
    return;
  }

  if (strcmp(interface, wl_output_interface.name) == 0) {
    /* Action purpose: version 4 is what carries wl_output.name, which is the
    only stable way to say "the extended screen" rather than "the second one to
    appear". Below 4 the name stays NULL and only the index can be used. */
    uint32_t bind = version < 4 ? version : 4;
    struct vin_output *out = calloc(1, sizeof(*out));

    out->name_id = name;
    out->output = wl_registry_bind(registry, name, &wl_output_interface, bind);
    out->next = vin->outputs;
    vin->outputs = out;

    wl_output_add_listener(out->output, &output_listener, out);
    return;
  }

  if (strcmp(interface, zwp_virtual_keyboard_manager_v1_interface.name) == 0) {
    vin->keyboard_manager = wl_registry_bind(registry, name,
        &zwp_virtual_keyboard_manager_v1_interface, 1);
    return;
  }

  if (strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
    vin->pointer_manager_version = version < 2 ? version : 2;
    vin->pointer_manager = wl_registry_bind(registry, name,
        &zwlr_virtual_pointer_manager_v1_interface,
        vin->pointer_manager_version);
    return;
  }
}

static void
registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
  (void)data;
  (void)registry;
  (void)name;
}

static const struct wl_registry_listener registry_listener = {
  .global = registry_global,
  .global_remove = registry_global_remove,
};

/* --------------------------------------------------------------- keymap */

/* Action purpose: FreeBSD's SHM_ANON creates an unnamed object with no window in
which another process could open it by name. Mirrors src/buffer.c, including the
named fallback that exists only so this still compiles where SHM_ANON is
absent. */
static int
shm_fd_create(size_t size)
{
  int fd = -1;

#if defined(SHM_ANON)
  fd = shm_open(SHM_ANON, O_RDWR | O_CREAT | O_EXCL, 0600);
#else
  for (int attempt = 0; attempt < 16 && fd < 0; attempt++) {
    struct timespec now;
    char name[64];

    clock_gettime(CLOCK_MONOTONIC, &now);
    snprintf(name, sizeof(name), "/vinput-%d-%ld-%d", (int)getpid(),
        (long)now.tv_nsec, attempt);

    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);

    if (fd >= 0) {
      shm_unlink(name);
    } else if (errno != EEXIST) {
      break;
    }
  }
#endif

  if (fd < 0) {
    return -1;
  }

  if (ftruncate(fd, (off_t)size) < 0) {
    close(fd);
    return -1;
  }

  return fd;
}

/* Function purpose: Build the default keymap, hand it to the compositor, and
keep it here too -- the same keymap has to answer "which keycode produces this
keysym" when a key command is resolved. */
static bool
vin_keyboard_init(struct vin *vin)
{
  if (vin->keyboard != NULL) {
    return true;
  }

  if (vin->keyboard_manager == NULL) {
    fprintf(stderr,
        "vinput: the compositor does not advertise "
        "zwp_virtual_keyboard_manager_v1\n");
    return false;
  }

  vin->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

  if (vin->xkb == NULL) {
    fprintf(stderr, "vinput: could not create an xkb context\n");
    return false;
  }

  struct xkb_rule_names names = { 0 };

  vin->keymap =
      xkb_keymap_new_from_names(vin->xkb, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);

  if (vin->keymap == NULL) {
    fprintf(stderr, "vinput: could not compile the default keymap\n");
    return false;
  }

  vin->shift = xkb_keymap_mod_get_index(vin->keymap, XKB_MOD_NAME_SHIFT);

  char *text = xkb_keymap_get_as_string(vin->keymap, XKB_KEYMAP_FORMAT_TEXT_V1);

  if (text == NULL) {
    fprintf(stderr, "vinput: could not serialise the keymap\n");
    return false;
  }

  size_t size = strlen(text) + 1;
  int fd = shm_fd_create(size);

  if (fd < 0) {
    fprintf(stderr, "vinput: could not create the keymap shm: %s\n",
        strerror(errno));
    free(text);
    return false;
  }

  void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

  if (map == MAP_FAILED) {
    fprintf(stderr, "vinput: could not map the keymap: %s\n", strerror(errno));
    close(fd);
    free(text);
    return false;
  }

  memcpy(map, text, size);
  munmap(map, size);
  free(text);

  vin->keyboard = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(
      vin->keyboard_manager, vin->seat);

  zwp_virtual_keyboard_v1_keymap(vin->keyboard, XKB_KEYMAP_FORMAT_TEXT_V1, fd,
      (uint32_t)size);

  close(fd);
  wl_display_roundtrip(vin->display);

  /* The compositor attaches the new keyboard to the seat, which makes every
  client on it re-read the keymap. Give that a moment to land before the first
  key, or the first keystroke can arrive at a surface that has not finished
  rebuilding its xkb state. */
  vin_sleep_ms(120);

  return true;
}

/* Function purpose: Find a keycode and shift level that produce `sym` under the
active keymap. Returns false when the symbol is not reachable, which is a real
outcome for a layout that simply has no key for it. */
static bool
vin_keycode_for(struct vin *vin,
    xkb_keysym_t sym,
    uint32_t *keycode,
    bool *needs_shift)
{
  xkb_keycode_t min = xkb_keymap_min_keycode(vin->keymap);
  xkb_keycode_t max = xkb_keymap_max_keycode(vin->keymap);

  for (xkb_keycode_t code = min; code <= max; code++) {
    xkb_layout_index_t layouts = xkb_keymap_num_layouts_for_key(vin->keymap, code);

    for (xkb_layout_index_t layout = 0; layout < layouts; layout++) {
      xkb_level_index_t levels =
          xkb_keymap_num_levels_for_key(vin->keymap, code, layout);

      /* Only the unshifted and shifted levels are reachable here: anything
      above needs a modifier mask this tool does not model, and silently
      pressing the wrong key would be worse than refusing. */
      for (xkb_level_index_t level = 0; level < levels && level < 2; level++) {
        const xkb_keysym_t *syms = NULL;
        int n = xkb_keymap_key_get_syms_by_level(vin->keymap, code, layout,
            level, &syms);

        for (int i = 0; i < n; i++) {
          if (syms[i] == sym) {
            /* xkb keycodes are evdev codes offset by 8; the wire wants evdev. */
            *keycode = (uint32_t)code - 8;
            *needs_shift = level == 1;
            return true;
          }
        }
      }
    }
  }

  return false;
}

static void
vin_set_shift(struct vin *vin, bool down)
{
  uint32_t mask = down && vin->shift != XKB_MOD_INVALID
      ? (uint32_t)1 << vin->shift
      : 0;

  zwp_virtual_keyboard_v1_modifiers(vin->keyboard, mask, 0, 0, 0);
}

static bool
vin_tap_keysym(struct vin *vin, xkb_keysym_t sym, const char *what)
{
  uint32_t keycode = 0;
  bool shift = false;

  if (!vin_keycode_for(vin, sym, &keycode, &shift)) {
    fprintf(stderr, "vinput: no key on this layout produces \"%s\"\n", what);
    return false;
  }

  if (shift) {
    vin_set_shift(vin, true);
  }

  zwp_virtual_keyboard_v1_key(vin->keyboard, vin_now(vin), keycode,
      WL_KEYBOARD_KEY_STATE_PRESSED);
  wl_display_flush(vin->display);
  vin_sleep_ms(12);

  zwp_virtual_keyboard_v1_key(vin->keyboard, vin_now(vin), keycode,
      WL_KEYBOARD_KEY_STATE_RELEASED);

  if (shift) {
    vin_set_shift(vin, false);
  }

  wl_display_roundtrip(vin->display);
  vin_sleep_ms(12);

  vin_log(vin, "key %s -> keycode %u%s", what, keycode, shift ? " +shift" : "");
  return true;
}

/* -------------------------------------------------------------- pointer */

static bool
vin_pointer_init(struct vin *vin)
{
  if (vin->pointer != NULL) {
    return true;
  }

  if (vin->pointer_manager == NULL) {
    fprintf(stderr,
        "vinput: the compositor does not advertise "
        "zwlr_virtual_pointer_manager_v1\n");
    return false;
  }

  /* Action purpose: Binding the pointer to an output is what keeps a test off
  the screen someone is working on. hikari confines only this device to the
  suggested output -- it does not map the whole cursor -- so the physical mouse
  keeps the full layout. Needs manager version 2. */
  if (vin->target != NULL && vin->pointer_manager_version >= 2) {
    vin->pointer =
        zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(
            vin->pointer_manager, vin->seat, vin->target->output);
    vin_log(vin, "pointer confined to output %s",
        vin->target->name != NULL ? vin->target->name : "(unnamed)");
  } else {
    if (vin->target != NULL) {
      fprintf(stderr,
          "vinput: manager version %u has no create_virtual_pointer_with_output;"
          " the pointer will not be confined to --output\n",
          vin->pointer_manager_version);
    }

    vin->pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(
        vin->pointer_manager, vin->seat);
  }

  wl_display_roundtrip(vin->display);
  return true;
}

static uint32_t
vin_button_code(const char *name)
{
  if (strcmp(name, "left") == 0) {
    return VIN_BTN_LEFT;
  }

  if (strcmp(name, "right") == 0) {
    return VIN_BTN_RIGHT;
  }

  if (strcmp(name, "middle") == 0) {
    return VIN_BTN_MIDDLE;
  }

  return 0;
}

/* Function purpose: One wheel notch, sent the way a real mouse sends it. The
source and the discrete count are what a client needs to tell a wheel from a
touchpad, and Saber's own axis handling reads them -- an axis event without a
frame is not delivered at all. */
static void
vin_axis(struct vin *vin, uint32_t axis, int steps)
{
  int direction = steps < 0 ? -1 : 1;
  int count = steps < 0 ? -steps : steps;

  for (int i = 0; i < count; i++) {
    uint32_t time = vin_now(vin);

    /* Action purpose: axis AND axis_discrete, in that order. The protocol
    describes axis_discrete as extending "data normally sent using the axis
    event" -- it supplements the axis event, it does not stand in for one, and a
    client that reads wl_pointer.axis sees nothing at all from a discrete-only
    sequence. This was found the hard way: discrete alone produced a wheel that
    the compositor accepted without error and no client ever felt. */
    zwlr_virtual_pointer_v1_axis_source(vin->pointer,
        WL_POINTER_AXIS_SOURCE_WHEEL);
    zwlr_virtual_pointer_v1_axis(vin->pointer, time, axis,
        wl_fixed_from_double(VIN_WHEEL_STEP * direction));
    zwlr_virtual_pointer_v1_axis_discrete(vin->pointer, time, axis,
        wl_fixed_from_double(VIN_WHEEL_STEP * direction), direction);
    zwlr_virtual_pointer_v1_frame(vin->pointer);

    wl_display_flush(vin->display);
    vin_sleep_ms(30);
  }

  wl_display_roundtrip(vin->display);
  vin_log(vin, "axis %u x%d", axis, steps);
}

/* ------------------------------------------------------------- commands */

static void
vin_list_outputs(struct vin *vin)
{
  int index = 0;

  for (struct vin_output *out = vin->outputs; out != NULL; out = out->next) {
    printf("%d\t%s\t%dx%d+%d+%d\t%s\n", index++,
        out->name != NULL ? out->name : "(unnamed)", out->width, out->height,
        out->x, out->y,
        out->description != NULL ? out->description : "");
  }

  if (index == 0) {
    printf("no outputs\n");
  }
}

static struct vin_output *
vin_find_output(struct vin *vin, const char *name)
{
  int index = 0;
  char *end = NULL;
  long wanted = strtol(name, &end, 10);
  bool numeric = end != NULL && *end == '\0' && end != name;

  for (struct vin_output *out = vin->outputs; out != NULL; out = out->next) {
    if (numeric && index == (int)wanted) {
      return out;
    }

    if (!numeric && out->name != NULL && strcmp(out->name, name) == 0) {
      return out;
    }

    index++;
  }

  return NULL;
}

static bool
vin_run(struct vin *vin, int argc, char **argv)
{
  const char *cmd = argv[0];

  if (strcmp(cmd, "sleep") == 0) {
    if (argc < 2) {
      fprintf(stderr, "vinput: sleep needs a duration in ms\n");
      return false;
    }

    vin_sleep_ms(atoi(argv[1]));
    return true;
  }

  if (strcmp(cmd, "move") == 0 || strcmp(cmd, "motion") == 0) {
    if (argc < 3) {
      fprintf(stderr, "vinput: %s needs two coordinates\n", cmd);
      return false;
    }

    if (!vin_pointer_init(vin)) {
      return false;
    }

    int x = atoi(argv[1]);
    int y = atoi(argv[2]);

    if (strcmp(cmd, "move") == 0) {
      /* motion_absolute is normalised against an extent the caller supplies.
      Using the target output's own mode makes the coordinates mean what a
      reader expects: pixels on that screen. */
      int32_t ex = vin->target != NULL && vin->target->width > 0
          ? vin->target->width
          : 1920;
      int32_t ey = vin->target != NULL && vin->target->height > 0
          ? vin->target->height
          : 1080;

      zwlr_virtual_pointer_v1_motion_absolute(vin->pointer, vin_now(vin),
          (uint32_t)x, (uint32_t)y, (uint32_t)ex, (uint32_t)ey);
    } else {
      zwlr_virtual_pointer_v1_motion(vin->pointer, vin_now(vin),
          wl_fixed_from_int(x), wl_fixed_from_int(y));
    }

    zwlr_virtual_pointer_v1_frame(vin->pointer);
    wl_display_roundtrip(vin->display);
    vin_sleep_ms(30);

    vin_log(vin, "%s %d %d", cmd, x, y);
    return true;
  }

  if (strcmp(cmd, "click") == 0 || strcmp(cmd, "press") == 0 ||
      strcmp(cmd, "release") == 0) {
    if (argc < 2) {
      fprintf(stderr, "vinput: %s needs a button\n", cmd);
      return false;
    }

    if (!vin_pointer_init(vin)) {
      return false;
    }

    uint32_t button = vin_button_code(argv[1]);

    if (button == 0) {
      fprintf(stderr, "vinput: unknown button \"%s\"\n", argv[1]);
      return false;
    }

    if (strcmp(cmd, "release") != 0) {
      zwlr_virtual_pointer_v1_button(vin->pointer, vin_now(vin), button,
          WL_POINTER_BUTTON_STATE_PRESSED);
      zwlr_virtual_pointer_v1_frame(vin->pointer);
      wl_display_flush(vin->display);
      vin_sleep_ms(30);
    }

    if (strcmp(cmd, "press") != 0) {
      zwlr_virtual_pointer_v1_button(vin->pointer, vin_now(vin), button,
          WL_POINTER_BUTTON_STATE_RELEASED);
      zwlr_virtual_pointer_v1_frame(vin->pointer);
    }

    wl_display_roundtrip(vin->display);
    vin_sleep_ms(30);

    vin_log(vin, "%s %s", cmd, argv[1]);
    return true;
  }

  if (strcmp(cmd, "scroll") == 0 || strcmp(cmd, "hscroll") == 0) {
    if (argc < 2) {
      fprintf(stderr, "vinput: %s needs a step count\n", cmd);
      return false;
    }

    if (!vin_pointer_init(vin)) {
      return false;
    }

    vin_axis(vin,
        strcmp(cmd, "scroll") == 0 ? WL_POINTER_AXIS_VERTICAL_SCROLL
                                   : WL_POINTER_AXIS_HORIZONTAL_SCROLL,
        atoi(argv[1]));
    return true;
  }

  if (strcmp(cmd, "key") == 0) {
    if (argc < 2) {
      fprintf(stderr, "vinput: key needs a keysym name\n");
      return false;
    }

    if (!vin_keyboard_init(vin)) {
      return false;
    }

    xkb_keysym_t sym =
        xkb_keysym_from_name(argv[1], XKB_KEYSYM_CASE_INSENSITIVE);

    if (sym == XKB_KEY_NoSymbol) {
      fprintf(stderr, "vinput: \"%s\" is not a keysym name\n", argv[1]);
      return false;
    }

    return vin_tap_keysym(vin, sym, argv[1]);
  }

  if (strcmp(cmd, "type") == 0) {
    if (argc < 2) {
      fprintf(stderr, "vinput: type needs text\n");
      return false;
    }

    if (!vin_keyboard_init(vin)) {
      return false;
    }

    for (const char *p = argv[1]; *p != '\0'; p++) {
      char label[2] = { *p, '\0' };
      xkb_keysym_t sym = xkb_utf32_to_keysym((uint32_t)(unsigned char)*p);

      if (sym == XKB_KEY_NoSymbol || !vin_tap_keysym(vin, sym, label)) {
        return false;
      }
    }

    return true;
  }

  fprintf(stderr, "vinput: unknown command \"%s\"\n", cmd);
  return false;
}

static void
vin_usage(void)
{
  fprintf(stderr,
      "usage: vinput outputs\n"
      "       vinput [--output NAME] [--verbose] CMD [ARGS] [+ CMD ...]\n"
      "\n"
      "commands: move X Y | motion DX DY | click BUTTON | press BUTTON |\n"
      "          release BUTTON | scroll STEPS | hscroll STEPS | key NAME |\n"
      "          type TEXT | sleep MS\n"
      "buttons:  left right middle\n");
}

int
main(int argc, char **argv)
{
  struct vin vin = { 0 };
  int i = 1;

  for (; i < argc; i++) {
    if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
      vin.target_name = argv[++i];
    } else if (strcmp(argv[i], "--verbose") == 0) {
      vin.verbose = true;
    } else if (strcmp(argv[i], "--help") == 0) {
      vin_usage();
      return 0;
    } else {
      break;
    }
  }

  if (i >= argc) {
    vin_usage();
    return 1;
  }

  vin.display = wl_display_connect(NULL);

  if (vin.display == NULL) {
    fprintf(stderr, "vinput: cannot connect to the compositor (WAYLAND_DISPLAY"
                    " unset or wrong?)\n");
    return 1;
  }

  vin.registry = wl_display_get_registry(vin.display);
  wl_registry_add_listener(vin.registry, &registry_listener, &vin);

  /* Twice: the first settles the globals, the second the wl_output events
  those globals produced. */
  wl_display_roundtrip(vin.display);
  wl_display_roundtrip(vin.display);

  int status = 0;

  if (strcmp(argv[i], "outputs") == 0) {
    vin_list_outputs(&vin);
    goto out;
  }

  if (vin.seat == NULL) {
    fprintf(stderr, "vinput: the compositor advertises no wl_seat\n");
    status = 1;
    goto out;
  }

  if (vin.target_name != NULL) {
    vin.target = vin_find_output(&vin, vin.target_name);

    if (vin.target == NULL) {
      fprintf(stderr, "vinput: no output named \"%s\"; try `vinput outputs`\n",
          vin.target_name);
      status = 1;
      goto out;
    }
  }

  /* Commands are separated by a bare "+" so a whole sequence runs against one
  connection: creating and destroying the virtual devices per step would make
  the compositor rebuild the seat's keyboard between every keystroke. */
  while (i < argc) {
    int start = i;

    while (i < argc && strcmp(argv[i], "+") != 0) {
      i++;
    }

    if (i > start && !vin_run(&vin, i - start, &argv[start])) {
      status = 1;
      break;
    }

    if (i < argc) {
      i++;
    }
  }

out:
  /* Let the last event reach the compositor before the connection drops. */
  wl_display_roundtrip(vin.display);

  if (vin.pointer != NULL) {
    zwlr_virtual_pointer_v1_destroy(vin.pointer);
  }

  if (vin.keyboard != NULL) {
    zwp_virtual_keyboard_v1_destroy(vin.keyboard);
  }

  if (vin.keymap != NULL) {
    xkb_keymap_unref(vin.keymap);
  }

  if (vin.xkb != NULL) {
    xkb_context_unref(vin.xkb);
  }

  for (struct vin_output *out = vin.outputs; out != NULL;) {
    struct vin_output *next = out->next;

    wl_output_destroy(out->output);
    free(out->name);
    free(out->description);
    free(out);
    out = next;
  }

  wl_display_roundtrip(vin.display);
  wl_display_disconnect(vin.display);

  return status;
}
