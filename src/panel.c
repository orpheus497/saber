/* Script function and purpose: One column per output -- layout, painting,
hit testing and the pointer semantics of BLUEPRINT.md 5.3.

The panel set owns the display's single pointer and output listeners and routes
each event to the column whose surface received it, because the display layer
deliberately has room for only one of each. */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <sys/wait.h>

#include <dev/evdev/input-event-codes.h>

#include <glib.h>
#include <pango/pangocairo.h>

#include "xdg-activation-v1-protocol.h"

#include <saber/anim.h>
#include <saber/appinfo.h>
#include <saber/panel.h>
#include <saber/quicklist.h>
#include <saber/session.h>
#include <saber/surface.h>

/* linux/input-event-codes.h is a Linux header; the three button codes the
protocol actually carries are stable and are spelled out rather than pulled in
through a compatibility shim. */
#define SABER_BTN_LEFT 0x110
#define SABER_BTN_RIGHT 0x111
#define SABER_BTN_MIDDLE 0x112

#define SABER_HOVER_MS 120
#define SABER_THROB_MS 640
#define SABER_WIGGLE_MS 360
#define SABER_LAUNCH_TIMEOUT_MS 5000
#define SABER_THROB_DEPTH 0.12
#define SABER_WIGGLE_PIXELS 3.0
#define SABER_SEPARATOR_GAP 8.0
#define SABER_TOKEN_TIMEOUT_MS 1000

/* The sheet grid, in logical pixels: one wide cell for sheet 0 over a three by
three block for 1-9. */
#define SABER_GRID_PAD 10
#define SABER_GRID_GAP 6
#define SABER_GRID_CELL_W 44
#define SABER_GRID_CELL_H 40
#define SABER_GRID_ZERO_H 32
#define SABER_GRID_COLUMNS 3
#define SABER_GRID_FONT "Sans Bold 13"
#define SABER_GRID_SMALL_FONT "Sans 8"

/* `sub` selects one device or one tray item within the single model entry that
stands for the whole zone; -1 means the entry itself. */
struct saber_slot {
  size_t item;
  int sub;
  double y, height;
};

struct saber_anim_state {
  struct saber_tween throb;
  struct saber_tween wiggle;
  int64_t launch_started;
  bool throbbing, wiggling, seen;
};

struct saber_panel {
  struct saber_panels *set;
  struct saber_output *output;
  struct saber_surface *surface;
  struct wl_callback *frame;

  struct saber_render render; /* per panel: each output has its own scale */
  struct saber_clock *clock;
  GHashTable *anim; /* char *item id -> struct saber_anim_state * */

  GArray *slots; /* struct saber_slot */
  double separator_y;
  int width, height;

  /* Action purpose: The application band scrolls. Until Phase 12 panel_layout
  simply stopped pushing head tiles once they ran past the tail, so on a short
  output the applications beyond the fold were not drawn, not hit-testable and
  not reachable by any gesture at all. `head_scroll` counts application tiles
  hidden above the fold -- the BFB is not among them, see panel_layout -- and
  the rest is what that function worked out about the band, kept so the clamp
  and the overflow arrows agree with the layout rather than re-deriving it. */
  int head_scroll;
  int head_count;    /* scrollable head tiles the model holds */
  int head_visible;  /* scrollable head tiles the band has room for */
  double head_top;   /* where the scrolling part of the band starts */
  double head_limit; /* and where it ends */

  int hover;  /* slot index, or -1 */
  int fading; /* slot index fading out, or -1 */
  int pressed;
  struct saber_tween hover_in, hover_out;

  double pointer_x, pointer_y;
};

/* Which tile a quicklist was opened from. The menu's callbacks run after the
menu itself is gone, so they cannot read anything off it. */
enum saber_menu_kind {
  SABER_MENU_APP,
  SABER_MENU_SESSION,
  SABER_MENU_TRASH,
  SABER_MENU_TRAY,
};

struct saber_menu {
  struct saber_panels *set;
  enum saber_menu_kind kind;
  char *id; /* the model item's id, for an application menu */
};

/* Resolved on first use and kept. `warned` is what keeps a system with no way
to open a directory from logging on every click. */
struct saber_filemanager {
  char **argv;               /* NULL-terminated; the path is appended */
  struct saber_appinfo *app; /* held ref, when the index answered */
  bool resolved, warned;
};

/* A launch parked until its xdg_activation token arrives. */
struct saber_launch {
  struct saber_panels *set;
  struct xdg_activation_token_v1 *token;
  struct saber_appinfo *app;
  char *id;
  guint timeout;
};

struct saber_sheet_grid;

struct saber_panels {
  struct saber_panel_deps deps;
  struct saber_render render;
  GPtrArray *list;
  struct saber_panel *pointer_panel;

  /* Action purpose: One popup at a time. A second would strand the first's
  grab and its listener swap, and stacking popups out of order is what
  xdg_wm_base.not_the_topmost_popup disconnects a client for. */
  struct saber_quicklist *menu;
  struct saber_menu *menu_ctx;
  struct saber_sheet_grid *grid;

  struct saber_filemanager filemanager;
  GPtrArray *launches; /* struct saber_launch * */

  saber_panel_spread_func spread;
  void *spread_user;
  saber_panel_dash_func dash;
  void *dash_user;

  /* Action purpose: One accumulator, keyed to what is being scrolled rather
  than to the slot under the pointer -- the head band keeps its sub-notch
  remainder while its own tiles slide past, and moving onto a different tile
  drops it instead of spending it there. SABER_SCROLL_HEAD is the band itself,
  which is what everything with no scroll of its own falls back to. */
  struct saber_scroll_accum scroll;
  size_t scroll_item;
  int scroll_sub;
};

#define SABER_SCROLL_HEAD G_MAXSIZE

static void
panel_schedule_frame(struct saber_panel *panel);

static void
panel_layout(struct saber_panel *panel);

static void
panel_sync_anim(struct saber_panel *panel);

static void
panel_set_hover(struct saber_panel *panel, int slot);

static int
panel_slot_at(const struct saber_panel *panel, double x, double y);

static void
sheet_grid_close(struct saber_sheet_grid *grid);

/* ------------------------------------------------------------- animation */

static void
anim_state_free(gpointer data)
{
  g_free(data);
}

static struct saber_anim_state *
panel_anim(struct saber_panel *panel, const char *id)
{
  struct saber_anim_state *state = g_hash_table_lookup(panel->anim, id);

  if (state != NULL) {
    return state;
  }

  state = g_new0(struct saber_anim_state, 1);

  saber_tween_init(&state->throb, 0.0);
  saber_tween_init(&state->wiggle, 0.0);
  saber_clock_add(panel->clock, &state->throb);
  saber_clock_add(panel->clock, &state->wiggle);
  g_hash_table_insert(panel->anim, g_strdup(id), state);

  return state;
}

/* Phase tweens are stopped back at zero rather than at their target, so a tile
that stops throbbing settles at its nominal size instead of at the top of the
swing. */
static void
anim_phase_stop(struct saber_tween *tween)
{
  saber_tween_stop(tween);
  tween->value = 0.0;
}

static void
panel_sync_anim(struct saber_panel *panel)
{
  struct saber_model *model = panel->set->deps.model;
  int64_t now = saber_clock_now(panel->clock);
  size_t count = saber_model_size(model);

  GHashTableIter iter;
  gpointer value;

  g_hash_table_iter_init(&iter, panel->anim);

  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    ((struct saber_anim_state *)value)->seen = false;
  }

  for (size_t i = 0; i < count; i++) {
    struct saber_item *item = saber_model_nth(model, i);

    if (item->type != SABER_ITEM_APP || item->id == NULL) {
      continue;
    }

    struct saber_anim_state *state = panel_anim(panel, item->id);

    state->seen = true;

    /* Action purpose: The throb has a five second ceiling (BLUEPRINT.md 5.2).
    An application that never opens a window -- or one whose app_id never
    matches -- must not leave a tile pulsing for the rest of the session. */
    if (item->launching) {
      if (!state->throbbing) {
        state->throbbing = true;
        state->launch_started = now;
        saber_tween_start_repeating(&state->throb, 0.0, 1.0, SABER_THROB_MS,
            SABER_EASE_LINEAR, now);
      } else if (now - state->launch_started > SABER_LAUNCH_TIMEOUT_MS) {
        state->throbbing = false;
        anim_phase_stop(&state->throb);
      }
    } else if (state->throbbing) {
      state->throbbing = false;
      anim_phase_stop(&state->throb);
    }

    if (item->badge.urgent) {
      if (!state->wiggling) {
        state->wiggling = true;
        saber_tween_start_repeating(&state->wiggle, 0.0, 1.0, SABER_WIGGLE_MS,
            SABER_EASE_LINEAR, now);
      }
    } else if (state->wiggling) {
      state->wiggling = false;
      anim_phase_stop(&state->wiggle);
    }
  }

  g_hash_table_iter_init(&iter, panel->anim);

  while (g_hash_table_iter_next(&iter, NULL, &value)) {
    struct saber_anim_state *state = value;

    if (!state->seen) {
      saber_clock_remove(panel->clock, &state->throb);
      saber_clock_remove(panel->clock, &state->wiggle);
      g_hash_table_iter_remove(&iter);
    }
  }
}

static void
panel_damage(struct saber_panel *panel)
{
  if (panel->surface == NULL || panel->surface->closed) {
    return;
  }

  if (saber_clock_busy(panel->clock)) {
    panel_schedule_frame(panel);
  } else {
    saber_surface_damage(panel->surface);
  }
}

static void
panel_frame_done(void *data, struct wl_callback *callback, uint32_t time)
{
  struct saber_panel *panel = data;

  wl_callback_destroy(callback);
  panel->frame = NULL;

  if (panel->surface == NULL || panel->surface->closed) {
    return;
  }

  int64_t now = saber_clock_stamp(panel->clock, time);

  if (saber_clock_advance(panel->clock, now)) {
    panel_schedule_frame(panel);
  } else {
    /* One last paint so the settled values reach the screen. */
    saber_surface_damage(panel->surface);
  }
}

static const struct wl_callback_listener panel_frame_listener = {
  .done = panel_frame_done,
};

/* Action purpose: The surface layer owns its own frame callback and discards
the timestamp, so the panel asks for one of its own -- Wayland allows any
number per surface and delivers them all on the same frame. The request is made
BEFORE the damage that triggers the commit, because a frame callback is only
registered by the commit that follows it. */
static void
panel_schedule_frame(struct saber_panel *panel)
{
  if (panel->frame != NULL || panel->surface == NULL ||
      panel->surface->closed || !panel->surface->configured) {
    return;
  }

  panel->frame = wl_surface_frame(panel->surface->wl_surface);
  wl_callback_add_listener(panel->frame, &panel_frame_listener, panel);

  saber_surface_damage(panel->surface);
}

/* ----------------------------------------------------------------- layout */

static bool
item_is_zone(enum saber_item_type type)
{
  return type == SABER_ITEM_DEVICES || type == SABER_ITEM_TRAY;
}

/* How many tiles this model entry contributes. Zero hides it entirely, which
is what an empty tray or an unavailable sheet socket must do -- a tile that
answers nothing is worse than no tile. */
static int
panel_tile_count(const struct saber_panels *set, const struct saber_item *item)
{
  switch (item->type) {
  case SABER_ITEM_SHEETS:
    return set->deps.sheets != NULL && saber_sheets_available(set->deps.sheets)
        ? 1
        : 0;

  case SABER_ITEM_DEVICES: {
    if (set->deps.devices == NULL) {
      return 0;
    }

    const GPtrArray *list = saber_devices_list(set->deps.devices);

    return list != NULL ? (int)list->len : 0;
  }

  case SABER_ITEM_TRASH:
    return set->deps.trash != NULL ? 1 : 0;

  case SABER_ITEM_TRAY:
    return set->deps.sni != NULL ? (int)saber_sni_count(set->deps.sni) : 0;

  default:
    return 1;
  }
}

static void
panel_push_slot(struct saber_panel *panel, size_t item, int sub, double y)
{
  struct saber_slot slot = {
    .item = item,
    .sub = sub,
    .y = y,
    .height = (double)panel->render.tile,
  };

  g_array_append_val(panel->slots, slot);
}

/* Function purpose: How far the head band can be scrolled, in whole tiles.
Reads only what panel_layout last worked out, so it is safe to ask before the
first layout -- it answers 0 there, which is the truth about a column with no
size yet. */
static int
panel_head_max_scroll(const struct saber_panel *panel)
{
  int max = panel->head_count - panel->head_visible;

  return max > 0 ? max : 0;
}

static void
panel_layout(struct saber_panel *panel)
{
  struct saber_panels *set = panel->set;
  struct saber_model *model = set->deps.model;
  double tile = (double)panel->render.tile;
  size_t count = saber_model_size(model);

  g_array_set_size(panel->slots, 0);
  panel->separator_y = -1.0;

  if (panel->height <= 0) {
    return;
  }

  /* Action purpose: Where the column splits is the configuration's answer, not
  a test on what kind of tile happens to lead the list: items { order } says
  which portions come before the application band and which come after it, and
  the model hands both boundaries over. */
  size_t apps_begin = saber_model_apps_begin(model);
  size_t tail_begin = saber_model_apps_end(model);

  if (tail_begin > count) {
    tail_begin = count;
  }

  /* Action purpose: The tail is anchored to the bottom rather than following
  the applications. The session tile has to stay reachable however many windows
  are open, and a column that scrolled its own power button off the screen
  would be the first thing anyone noticed. */
  int tail_tiles = 0;

  for (size_t i = tail_begin; i < count; i++) {
    tail_tiles += panel_tile_count(set, saber_model_nth(model, i));
  }

  double tail_top = (double)panel->height - tail_tiles * tile;
  double head_limit = tail_tiles > 0 ? tail_top - SABER_SEPARATOR_GAP
                                     : (double)panel->height;

  /* Action purpose: What does not fit is scrolled to, not thrown away. The band
  shows whole tiles only -- a half tile clipped by the separator reads as a
  rendering fault, and quantising the offset to the tile pitch is also what lets
  the arrows and the clamp be exact rather than approximate. */
  panel->head_limit = head_limit;

  double y = 0.0;
  size_t head = 0;

  /* Action purpose: The portions configured ahead of the application band --
  by default just the BFB -- are pinned and do not scroll. The BFB is the only
  route to the Dash, and the argument the tail is anchored for is the same one:
  a column able to scroll its own launcher button off the top would be the
  first thing anyone noticed. They contribute tiles the same way the tail's do,
  because items { order } may now put a zone or a hideable tile up here, and
  one of those is worth zero tiles or several rather than always exactly one. */
  for (; head < apps_begin; head++) {
    struct saber_item *item = saber_model_nth(model, head);
    int tiles = panel_tile_count(set, item);
    int sub = 0;

    for (; sub < tiles && y + tile <= head_limit; sub++) {
      panel_push_slot(panel, head, item_is_zone(item->type) ? sub : -1, y);
      y += tile;
    }

    if (sub < tiles) {
      break;
    }
  }

  panel->head_top = y;
  panel->head_count = (int)(tail_begin - head);
  panel->head_visible = tile > 0.0 ? (int)floor((head_limit - y) / tile) : 0;

  if (panel->head_visible < 0) {
    panel->head_visible = 0;
  }

  panel->head_scroll =
      CLAMP(panel->head_scroll, 0, panel_head_max_scroll(panel));

  for (head += (size_t)panel->head_scroll; head < tail_begin; head++) {
    if (y + tile > head_limit) {
      break;
    }

    panel_push_slot(panel, head, -1, y);
    y += tile;
  }

  if (tail_tiles > 0 && tail_begin > 0) {
    panel->separator_y = floor(tail_top - SABER_SEPARATOR_GAP / 2.0) + 0.5;
  }

  y = tail_top;

  for (size_t i = tail_begin; i < count; i++) {
    struct saber_item *item = saber_model_nth(model, i);
    int tiles = panel_tile_count(set, item);

    for (int sub = 0; sub < tiles; sub++) {
      panel_push_slot(panel, i, item_is_zone(item->type) ? sub : -1, y);
      y += tile;
    }
  }
}

static int
panel_slot_at(const struct saber_panel *panel, double x, double y)
{
  if (x < 0.0 || x > (double)panel->width) {
    return -1;
  }

  for (guint i = 0; i < panel->slots->len; i++) {
    const struct saber_slot *slot =
        &g_array_index(panel->slots, struct saber_slot, i);

    if (y >= slot->y && y < slot->y + slot->height) {
      return (int)i;
    }
  }

  return -1;
}

/* ---------------------------------------------------------------- painting */

/* Function purpose: First name in the chain that resolves. Icon names in
desktop entries and in SNI items are frequently absent from whichever theme is
installed, so every tile has a fallback ladder ending in something hicolor is
guaranteed to ship. */
static cairo_surface_t *
panel_icon(struct saber_panel *panel,
    const char *const *names,
    size_t count,
    struct saber_tile *tile)
{
  int pixels = (int)lround((double)panel->render.icon_size * panel->render.scale);

  for (size_t i = 0; i < count; i++) {
    if (names[i] == NULL || *names[i] == '\0') {
      continue;
    }

    cairo_surface_t *surface =
        saber_icons_lookup(panel->set->deps.icons, names[i], pixels);

    if (surface != NULL) {
      tile->symbolic = g_str_has_suffix(names[i], "-symbolic");

      return surface;
    }
  }

  return NULL;
}

static void
panel_fill_app_tile(struct saber_panel *panel,
    struct saber_item *item,
    struct saber_tile *tile,
    char *initial)
{
  char *stem = NULL;
  const char *names[3];
  size_t count = 0;

  if (item->app != NULL && item->app->icon != NULL) {
    names[count++] = item->app->icon;
  }

  if (item->id != NULL) {
    stem = g_str_has_suffix(item->id, ".desktop")
        ? g_strndup(item->id, strlen(item->id) - strlen(".desktop"))
        : g_strdup(item->id);
    names[count++] = stem;
  }

  names[count++] = "application-x-executable";

  tile->icon = panel_icon(panel, names, count, tile);

  /* The first character of the visible name, so an entry with no icon at all
  still reads as a distinct tile rather than as a blank square. */
  const char *label = item->app != NULL && item->app->name != NULL
      ? item->app->name
      : item->id;

  if (tile->icon == NULL && label != NULL && *label != '\0') {
    g_utf8_strncpy(initial, label, 1);
    *initial = (char)g_ascii_toupper(*initial);
    tile->label = initial;
  }

  tile->windows = (int)saber_item_window_count(item);
  tile->running = tile->windows > 0;
  tile->focused = item->focused;
  tile->badge = item->badge;

  struct saber_anim_state *state =
      item->id != NULL ? panel_anim(panel, item->id) : NULL;

  if (state != NULL && state->throbbing) {
    tile->throb = saber_anim_throb(saber_tween_value(&state->throb),
        SABER_THROB_DEPTH);
  }

  if (state != NULL && state->wiggling) {
    tile->wiggle = saber_anim_wiggle(saber_tween_value(&state->wiggle)) *
        SABER_WIGGLE_PIXELS;
  }

  g_free(stem);
}

static void
panel_fill_special_tile(struct saber_panel *panel,
    const struct saber_slot *slot,
    struct saber_item *item,
    struct saber_tile *tile,
    cairo_surface_t **owned,
    char *scratch,
    size_t scratch_size)
{
  struct saber_panels *set = panel->set;

  switch (item->type) {
  case SABER_ITEM_BFB: {
    /* Action purpose: "saber" first so the shipped emblem wins over whatever
    the user's icon theme happens to supply for start-here, which is usually
    that distribution's logo. The rest stay as fallbacks for a tree installed
    without its share/icons. */
    static const char *const names[] = { "saber", "start-here",
      "distributor-logo", "applications-other", "view-app-grid-symbolic",
      "view-grid-symbolic" };

    tile->icon = panel_icon(panel, names, G_N_ELEMENTS(names), tile);
    tile->label = "S";
    break;
  }

  case SABER_ITEM_SHEETS: {
    const struct saber_sheets_state *state =
        saber_sheets_get_state(set->deps.sheets);

    g_snprintf(scratch, scratch_size, "%d", state != NULL ? state->current : 0);
    tile->label = scratch;
    tile->focused = state != NULL;
    tile->running = state != NULL && state->current > 0;
    break;
  }

  case SABER_ITEM_DEVICES: {
    const GPtrArray *list = saber_devices_list(set->deps.devices);

    if (list == NULL || slot->sub < 0 || (guint)slot->sub >= list->len) {
      break;
    }

    const struct saber_device *device = g_ptr_array_index(list, slot->sub);
    const char *names[] = { device->icon, "drive-removable-media",
      "drive-harddisk", "drive-removable-media-symbolic" };

    tile->icon = panel_icon(panel, names, G_N_ELEMENTS(names), tile);
    tile->running = true;
    break;
  }

  case SABER_ITEM_TRASH: {
    unsigned int count = saber_trash_count(set->deps.trash);
    const char *names[] = { count > 0 ? "user-trash-full" : "user-trash",
      "user-trash", "edit-delete", "user-trash-symbolic" };

    tile->icon = panel_icon(panel, names, G_N_ELEMENTS(names), tile);
    tile->badge.count = count;
    tile->badge.count_visible = count > 0;
    break;
  }

  case SABER_ITEM_TRAY: {
    struct saber_sni_item *entry =
        saber_sni_nth(set->deps.sni, (unsigned int)slot->sub);

    if (entry == NULL) {
      break;
    }

    const char *names[] = { saber_sni_item_icon_name(entry) };

    tile->icon = panel_icon(panel, names, G_N_ELEMENTS(names), tile);

    /* Action purpose: An item that ships no themable icon name still has to
    appear, so its decoded pixmap is wrapped for this frame only. sni.c owns
    the decode; nothing is cached here. */
    if (tile->icon == NULL) {
      const uint8_t *data = NULL;
      size_t length = 0;
      int width = 0, height = 0;

      if (saber_sni_item_icon_argb32(entry, &data, &length, &width, &height)) {
        *owned = saber_icon_from_argb32(data, length, width, height);
        tile->icon = *owned;
      }
    }

    tile->badge.urgent =
        saber_sni_item_status(entry) == SABER_SNI_NEEDS_ATTENTION;
    break;
  }

  case SABER_ITEM_SESSION: {
    /* Action purpose: The symbolic power glyph leads deliberately. Adwaita
    carries Inherits=AdwaitaLegacy, and AdwaitaLegacy is the only theme in that
    chain holding a plain `system-shutdown` -- the GNOME-2 light switch, which
    would otherwise win on the first name and render a skeuomorphic switch
    beside the flat icons around it. */
    static const char *const names[] = { "system-shutdown-symbolic",
      "system-shutdown", "system-log-out", "application-exit",
      "system-log-out-symbolic" };

    tile->icon = panel_icon(panel, names, G_N_ELEMENTS(names), tile);
    break;
  }

  default:
    break;
  }
}

/* Function purpose: Say that the head band has more above or below. Without it
a scrolled column is indistinguishable from one that has simply lost tiles --
which is exactly the failure this replaced, so leaving the state invisible would
have fixed nothing a user could see. Drawn over the band's own edges, in the
foreground role, because there is no gutter in a column this narrow. */
static void
panel_draw_head_arrows(struct saber_panel *panel, cairo_t *cr, double width)
{
  int max = panel_head_max_scroll(panel);

  if (max <= 0 || panel->head_limit <= panel->head_top) {
    return;
  }

  const struct saber_theme *theme = panel->render.theme;
  double cx = width / 2.0;
  double arm = 4.0;

  saber_theme_set_source(cr, &theme->foreground);
  cairo_set_line_width(cr, 1.5);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);

  if (panel->head_scroll > 0) {
    double base = panel->head_top;

    cairo_move_to(cr, cx - arm, base + 6.0);
    cairo_line_to(cr, cx, base + 2.0);
    cairo_line_to(cr, cx + arm, base + 6.0);
    cairo_stroke(cr);
  }

  if (panel->head_scroll < max) {
    double base = panel->head_limit;

    cairo_move_to(cr, cx - arm, base - 6.0);
    cairo_line_to(cr, cx, base - 2.0);
    cairo_line_to(cr, cx + arm, base - 6.0);
    cairo_stroke(cr);
  }
}

static void
panel_render_surface(void *data,
    struct saber_surface *surface,
    cairo_t *cr,
    int width,
    int height)
{
  struct saber_panel *panel = data;

  panel->render.scale = saber_surface_scale(surface);

  if (width != panel->width || height != panel->height) {
    panel->width = width;
    panel->height = height;
    panel_layout(panel);
  }

  saber_render_column(&panel->render, cr, width, height);

  if (panel->separator_y >= 0.0) {
    saber_render_separator(&panel->render, cr, panel->separator_y,
        (double)width);
  }

  struct saber_model *model = panel->set->deps.model;

  for (guint i = 0; i < panel->slots->len; i++) {
    const struct saber_slot *slot =
        &g_array_index(panel->slots, struct saber_slot, i);
    struct saber_item *item = saber_model_nth(model, slot->item);

    if (item == NULL) {
      continue;
    }

    cairo_surface_t *owned = NULL;
    char scratch[16] = { 0 };
    char initial[8] = { 0 };
    struct saber_tile tile = {
      .x = 0.0,
      .y = slot->y,
      .width = (double)width,
      .height = slot->height,
      .throb = 1.0,
    };

    if (item->type == SABER_ITEM_APP) {
      panel_fill_app_tile(panel, item, &tile, initial);
    } else {
      panel_fill_special_tile(panel, slot, item, &tile, &owned, scratch,
          sizeof(scratch));
    }

    if ((int)i == panel->hover) {
      tile.hover = saber_tween_value(&panel->hover_in);
      tile.pressed = panel->pressed == (int)i;
    } else if ((int)i == panel->fading) {
      tile.hover = saber_tween_value(&panel->hover_out);
    }

    saber_render_tile(&panel->render, cr, &tile);

    if (owned != NULL) {
      cairo_surface_destroy(owned);
    }
  }

  panel_draw_head_arrows(panel, cr, (double)width);
}

static void
panel_configure(void *data,
    struct saber_surface *surface,
    int width,
    int height)
{
  (void)surface;

  struct saber_panel *panel = data;

  panel->width = width;
  panel->height = height;
  panel_layout(panel);
  panel_sync_anim(panel);
}

static void
panel_closed(void *data, struct saber_surface *surface)
{
  (void)surface;

  struct saber_panel *panel = data;

  /* The surface cannot be reused; it is torn down when the output that owned
  it goes, which is the event that actually follows this one. */
  panel->hover = -1;
  panel->fading = -1;
  panel->pressed = -1;
}

static const struct saber_surface_listener panel_surface_listener = {
  .configure = panel_configure,
  .render = panel_render_surface,
  .closed = panel_closed,
};

/* ----------------------------------------------------------------- actions */

static struct saber_toplevel *
item_window(const struct saber_item *item, guint n)
{
  if (item->windows == NULL || n >= item->windows->len) {
    return NULL;
  }

  return g_ptr_array_index(item->windows, n);
}

static int
item_active_window(const struct saber_item *item)
{
  for (guint i = 0; item->windows != NULL && i < item->windows->len; i++) {
    const struct saber_toplevel *toplevel = g_ptr_array_index(item->windows, i);

    if (saber_toplevel_has_state(toplevel, SABER_TOPLEVEL_ACTIVATED)) {
      return (int)i;
    }
  }

  return -1;
}

static void
window_raise(struct saber_toplevel *toplevel)
{
  if (toplevel == NULL) {
    return;
  }

  /* Unminimise first: on hikari-sakura the bit usually means "on another
  sheet", and activating without clearing it would raise a hidden view. */
  saber_toplevel_unset_minimized(toplevel);
  saber_toplevel_activate(toplevel);
}

/* ------------------------------------------------------- launch with a token */

static void
launch_free(gpointer data)
{
  struct saber_launch *launch = data;

  if (launch->timeout != 0) {
    g_source_remove(launch->timeout);
  }

  if (launch->token != NULL) {
    xdg_activation_token_v1_destroy(launch->token);
  }

  saber_appinfo_unref(launch->app);
  g_free(launch->id);
  g_free(launch);
}

static void
launch_finish(struct saber_launch *launch, const char *token)
{
  struct saber_panels *set = launch->set;

  if (saber_appinfo_launch(launch->app, NULL, NULL, token)) {
    saber_model_note_launch(set->deps.model, launch->id);
  } else {
    g_warning("saber: failed to launch '%s'", launch->id);
  }

  g_ptr_array_remove_fast(set->launches, launch);
}

static void
launch_token_done(void *data,
    struct xdg_activation_token_v1 *token,
    const char *string)
{
  (void)token;

  launch_finish(data, string);
}

static const struct xdg_activation_token_v1_listener launch_token_listener = {
  .done = launch_token_done,
};

/* Action purpose: A compositor that binds xdg_activation_v1 but never answers
would otherwise swallow the launch entirely. The application then starts with
no token, which costs it the focus and nothing else. */
static gboolean
launch_token_timeout(gpointer data)
{
  struct saber_launch *launch = data;

  launch->timeout = 0;
  launch_finish(launch, NULL);

  return G_SOURCE_REMOVE;
}

static void
panel_launch(struct saber_panels *set, struct saber_item *item)
{
  if (item->app == NULL) {
    g_warning("saber: no desktop entry for '%s'; cannot launch", item->id);

    return;
  }

  struct saber_display *display = set->deps.display;

  if (display->activation == NULL) {
    if (!saber_appinfo_launch(item->app, NULL, NULL, NULL)) {
      g_warning("saber: failed to launch '%s'", item->id);

      return;
    }

    saber_model_note_launch(set->deps.model, item->id);

    return;
  }

  /* Action purpose: The token is issued asynchronously, so the launch waits
  for it: XDG_ACTIVATION_TOKEN in the child's environment is the only thing
  that lets the new window raise itself instead of arriving urgent. The click's
  serial is what the compositor validates the request against. */
  struct saber_launch *launch = g_new0(struct saber_launch, 1);

  launch->set = set;
  launch->app = saber_appinfo_ref(item->app);
  launch->id = g_strdup(item->id);
  launch->token = xdg_activation_v1_get_activation_token(display->activation);

  xdg_activation_token_v1_add_listener(launch->token, &launch_token_listener,
      launch);

  if (display->seat != NULL) {
    xdg_activation_token_v1_set_serial(launch->token,
        display->pointer_press_serial, display->seat);
  }

  if (set->pointer_panel != NULL && set->pointer_panel->surface != NULL) {
    xdg_activation_token_v1_set_surface(launch->token,
        set->pointer_panel->surface->wl_surface);
  }

  if (item->id != NULL) {
    xdg_activation_token_v1_set_app_id(launch->token, item->id);
  }

  xdg_activation_token_v1_commit(launch->token);
  launch->timeout =
      g_timeout_add(SABER_TOKEN_TIMEOUT_MS, launch_token_timeout, launch);

  g_ptr_array_add(set->launches, launch);
  saber_display_flush(display);
}

/* ------------------------------------------------------------ file manager */

/* Function purpose: Case-sensitive membership in a desktop entry's Categories,
which the spec's registered names are. */
static bool
appinfo_has_category(const struct saber_appinfo *app, const char *category)
{
  if (app == NULL || app->categories == NULL) {
    return false;
  }

  for (guint i = 0; app->categories[i] != NULL; i++) {
    if (g_strcmp0(app->categories[i], category) == 0) {
      return true;
    }
  }

  return false;
}

/* Function purpose: An entry that runs in a terminal, or is one, is never a
file manager. Both passes below need this: kitty registers inode/directory as a
URL handler, and yazi carries the FileManager category with Terminal=true, so
either preference alone would still land the user in a terminal. */
static bool
filemanager_usable(const struct saber_appinfo *app)
{
  return app != NULL && !app->terminal &&
      !appinfo_has_category(app, "TerminalEmulator");
}

/* Function purpose: The first indexed entry that calls itself a file manager
and survives the terminal test. Preferred over the inode/directory association,
because that association is exactly what goes wrong -- it is stale or absent far
more often than a FileManager entry is miscategorised. Borrowed. */
static struct saber_appinfo *
filemanager_from_categories(struct saber_panels *set)
{
  if (set->deps.index == NULL) {
    return NULL;
  }

  size_t count = saber_appinfo_index_size(set->deps.index);

  for (size_t i = 0; i < count; i++) {
    struct saber_appinfo *app = saber_appinfo_index_nth(set->deps.index, i);

    if (appinfo_has_category(app, "FileManager") && filemanager_usable(app)) {
      return app;
    }
  }

  return NULL;
}

/* Function purpose: The desktop entry registered as the default for
inode/directory, read from the mimeapps.list files in XDG order. The panel's own
index is asked rather than GIO's, so a folder opens in the same application the
launcher would show. Borrowed. */
static struct saber_appinfo *
filemanager_from_index(struct saber_panels *set)
{
  if (set->deps.index == NULL) {
    return NULL;
  }

  GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);

  g_ptr_array_add(paths,
      g_build_filename(g_get_user_config_dir(), "mimeapps.list", NULL));

  for (const char *const *dir = g_get_system_config_dirs(); *dir != NULL;
      dir++) {
    g_ptr_array_add(paths, g_build_filename(*dir, "mimeapps.list", NULL));
  }

  g_ptr_array_add(paths, g_build_filename(g_get_user_data_dir(), "applications",
                             "mimeapps.list", NULL));

  for (const char *const *dir = g_get_system_data_dirs(); *dir != NULL; dir++) {
    g_ptr_array_add(paths,
        g_build_filename(*dir, "applications", "mimeapps.list", NULL));
  }

  struct saber_appinfo *app = NULL;

  for (guint i = 0; app == NULL && i < paths->len; i++) {
    GKeyFile *file = g_key_file_new();

    if (g_key_file_load_from_file(file, g_ptr_array_index(paths, i),
            G_KEY_FILE_NONE, NULL)) {
      char **ids = g_key_file_get_string_list(file, "Default Applications",
          "inode/directory", NULL, NULL);

      for (guint n = 0; ids != NULL && ids[n] != NULL && app == NULL; n++) {
        struct saber_appinfo *candidate =
            saber_appinfo_index_lookup(set->deps.index, ids[n]);

        /* Action purpose: A registered handler that is a terminal is worse
        than none; skipping it lets a later id in the same list answer. */
        if (filemanager_usable(candidate)) {
          app = candidate;
        }
      }

      g_strfreev(ids);
    }

    g_key_file_free(file);
  }

  g_ptr_array_unref(paths);

  return app;
}

/* Function purpose: Whatever can open a directory on this system, worked out
once and kept. $FILEMANAGER is the user's explicit answer and wins; then a real
file manager from the index; then the inode/directory handler; and only then
xdg-open. xdg-open is last, not first, because it answers from the same
association chain with none of the terminal test applied -- it will happily
resolve a directory to a terminal emulator and hang there. Nothing is guessed
by name: a tile that shells out to a browser nobody installed is worse than one
that says it cannot. */
static void
filemanager_resolve(struct saber_panels *set)
{
  struct saber_filemanager *fm = &set->filemanager;

  if (fm->resolved) {
    return;
  }

  fm->resolved = true;

  const char *env = g_getenv("FILEMANAGER");
  char **argv = NULL;

  if (env != NULL && *env != '\0' &&
      g_shell_parse_argv(env, NULL, &argv, NULL)) {
    char *program = g_find_program_in_path(argv[0]);

    if (program != NULL) {
      g_free(argv[0]);
      argv[0] = program;
      fm->argv = argv;

      return;
    }

    g_warning("saber: FILEMANAGER names '%s', which is not on PATH", argv[0]);
    g_strfreev(argv);
  }

  struct saber_appinfo *app = filemanager_from_categories(set);

  if (app == NULL) {
    app = filemanager_from_index(set);
  }

  if (app != NULL) {
    fm->app = saber_appinfo_ref(app);

    return;
  }

  char *program = g_find_program_in_path("xdg-open");

  if (program != NULL) {
    fm->argv = g_new0(char *, 2);
    fm->argv[0] = program;
  }
}

/* Function purpose: fork/exec an argv the panel assembled itself -- never a
shell, so a mount point with a space in its name cannot become two arguments.
Double forked, so the grandchild reparents to init and the panel never has to
reap anything under a main loop that knows nothing about it. */
static bool
panel_spawn(char *const *argv)
{
  pid_t outer = fork();

  if (outer < 0) {
    g_warning("saber: fork: %s", g_strerror(errno));

    return false;
  }

  if (outer == 0) {
    setsid();

    if (fork() == 0) {
      int null = open("/dev/null", O_RDWR);

      if (null >= 0) {
        dup2(null, STDIN_FILENO);
        dup2(null, STDOUT_FILENO);
        dup2(null, STDERR_FILENO);

        if (null > STDERR_FILENO) {
          close(null);
        }
      }

      execv(argv[0], argv);
      _exit(127);
    }

    _exit(0);
  }

  int status = 0;

  while (waitpid(outer, &status, 0) < 0 && errno == EINTR) {
    continue;
  }

  return true;
}

static void
panel_open_path(struct saber_panels *set, const char *path)
{
  if (path == NULL || *path == '\0') {
    return;
  }

  filemanager_resolve(set);

  struct saber_filemanager *fm = &set->filemanager;

  if (fm->argv != NULL) {
    guint length = g_strv_length(fm->argv);
    char **argv = g_new0(char *, length + 2);

    /* Shallow: every string still belongs to fm->argv or to the caller. */
    for (guint i = 0; i < length; i++) {
      argv[i] = fm->argv[i];
    }

    argv[length] = (char *)path;
    panel_spawn(argv);
    g_free(argv);

    return;
  }

  if (fm->app != NULL) {
    char *uri = g_filename_to_uri(path, NULL, NULL);
    const char *uris[] = { uri != NULL ? uri : path, NULL };

    saber_appinfo_launch(fm->app, NULL, uris, NULL);
    g_free(uri);

    return;
  }

  if (!fm->warned) {
    fm->warned = true;
    g_warning("saber: nothing can open a directory here -- set FILEMANAGER, "
              "install xdg-open, or register a handler for inode/directory");
  }
}

/* -------------------------------------------------------------- quicklists */

/* Action purpose: A quicklist's window rows are a labelled list of opaque
handles reported through one callback, which is exactly the shape a menu the
panel composes itself needs. The session and trash entries ride in there rather
than growing a second menu widget; the handle is the entry's tag plus one, plus
one only so that no handle is NULL. */
#define SABER_MENU_TAG(n) ((void *)(uintptr_t)((n) + 1))
#define SABER_MENU_UNTAG(p) ((int)(uintptr_t)(p) - 1)

enum saber_trash_entry {
  SABER_TRASH_OPEN,
  SABER_TRASH_EMPTY,
};

static void
panel_close_menu(struct saber_panels *set)
{
  if (set->menu != NULL) {
    saber_quicklist_close(set->menu);
  }

  sheet_grid_close(set->grid);
}

static void
menu_closed(void *user)
{
  struct saber_menu *menu = user;
  struct saber_panels *set = menu->set;

  set->menu = NULL;

  /* Action purpose: The context is deliberately NOT freed here. quicklist.c
  reports the close before it reports the activation that caused it, and the
  handlers below still need it; the next menu to open frees it instead.

  The panel saw no pointer events while the menu held the seat, so its idea of
  what is hovered is as old as the click that opened the menu. */
  struct saber_panel *panel = set->pointer_panel;

  if (panel != NULL) {
    panel_set_hover(panel,
        panel_slot_at(panel, panel->pointer_x, panel->pointer_y));
  }
}

static void
menu_action(void *user, enum saber_quicklist_action action)
{
  struct saber_menu *menu = user;
  struct saber_panels *set = menu->set;

  if (menu->kind != SABER_MENU_APP || menu->id == NULL) {
    return;
  }

  switch (action) {
  case SABER_QUICKLIST_PIN:
    if (saber_model_pin(set->deps.model, menu->id)) {
      saber_model_save(set->deps.model);
      saber_panels_refresh(set);
    }
    break;

  case SABER_QUICKLIST_UNPIN:
    if (saber_model_unpin(set->deps.model, menu->id)) {
      saber_model_save(set->deps.model);
      saber_panels_refresh(set);
    }
    break;

  case SABER_QUICKLIST_QUIT: {
    struct saber_item *item = saber_model_find(set->deps.model, menu->id);

    /* Every window, because the row says Quit and not Close: an application
    showing three windows is not quit by closing one of them. */
    for (guint i = 0; item != NULL && i < (guint)saber_item_window_count(item);
        i++) {
      saber_toplevel_close(g_ptr_array_index(item->windows, i));
    }
    break;
  }
  }
}

static void
menu_window(void *user, void *handle)
{
  struct saber_menu *menu = user;
  struct saber_panels *set = menu->set;
  GError *error = NULL;

  switch (menu->kind) {
  case SABER_MENU_APP:
    /* Action purpose: the row's handle was snapshotted into the menu when it
    opened, and the compositor can close that window while the menu is still on
    screen -- after which toplevel_free has released it and both the raise and
    the wl_proxy behind it would be a use-after-free. handle_closed withdraws
    the window from the model before freeing it, so a handle the model still
    knows is a handle that still exists. */
    if (saber_model_find_by_window(set->deps.model, handle) != NULL) {
      window_raise(handle);
    }
    break;

  case SABER_MENU_SESSION: {
    int tag = SABER_MENU_UNTAG(handle);

    /* Anything outside the enum is the dismiss row. */
    if (tag < 0 || tag >= SABER_SESSION_ACTION_COUNT) {
      break;
    }

    if (!saber_session_run(set->deps.config, tag, &error)) {
      g_warning("saber: %s: %s", saber_session_action_id(tag),
          error != NULL ? error->message : "failed");
    }
    break;
  }

  case SABER_MENU_TRASH:
    if (SABER_MENU_UNTAG(handle) == SABER_TRASH_OPEN) {
      panel_open_path(set, saber_trash_path(set->deps.trash));
      break;
    }

    if (!saber_trash_empty(set->deps.trash, &error)) {
      g_warning("saber: cannot empty the trash: %s",
          error != NULL ? error->message : "failed");
    }
    break;

  case SABER_MENU_TRAY:
    break;
  }

  g_clear_error(&error);
}

static const struct saber_quicklist_handlers panel_menu_handlers = {
  .action = menu_action,
  .window = menu_window,
  .closed = menu_closed,
};

/* Function purpose: Everything a menu on this tile shares -- the parent
surface, the edge it grows away from, the palette, and the tile rectangle it
hangs off -- so each caller supplies only its own contents. */
static void
panel_menu_params(struct saber_panel *panel,
    const struct saber_slot *slot,
    struct saber_quicklist_params *params)
{
  saber_quicklist_params_init(params);

  params->parent = panel->surface;
  params->edge = panel->set->deps.config->panel.edge;
  params->theme = panel->set->deps.theme;
  params->anchor_x = 0;
  params->anchor_y = (int32_t)slot->y;
  params->anchor_width = (int32_t)panel->width;
  params->anchor_height = (int32_t)slot->height;
}

static struct saber_menu *
panel_menu_context(struct saber_panels *set,
    enum saber_menu_kind kind,
    const char *id)
{
  if (set->menu_ctx != NULL) {
    g_free(set->menu_ctx->id);
    g_free(set->menu_ctx);
  }

  struct saber_menu *menu = g_new0(struct saber_menu, 1);

  menu->set = set;
  menu->kind = kind;
  menu->id = g_strdup(id);
  set->menu_ctx = menu;

  return menu;
}

static void
panel_open_app_menu(struct saber_panel *panel,
    const struct saber_slot *slot,
    struct saber_item *item)
{
  struct saber_panels *set = panel->set;
  size_t count = saber_item_window_count(item);
  struct saber_quicklist_window *windows =
      count > 0 ? g_new0(struct saber_quicklist_window, count) : NULL;

  for (size_t i = 0; i < count; i++) {
    struct saber_toplevel *toplevel = g_ptr_array_index(item->windows, i);

    windows[i].title = toplevel->title;
    windows[i].handle = toplevel;
  }

  struct saber_quicklist_params params;

  panel_menu_params(panel, slot, &params);
  params.app = item->app;
  params.windows = windows;
  params.windows_len = count;
  params.pinned = item->pinned;
  params.running = count > 0;
  params.offer_pin = true;

  panel_close_menu(set);
  set->menu = saber_quicklist_open(&params, &panel_menu_handlers,
      panel_menu_context(set, SABER_MENU_APP, item->id));

  g_free(windows);
}

static void
panel_open_tray_menu(struct saber_panel *panel,
    const struct saber_slot *slot,
    struct saber_sni_item *entry)
{
  struct saber_panels *set = panel->set;
  struct saber_quicklist_params params;

  panel_menu_params(panel, slot, &params);
  params.menu_bus_name = saber_sni_item_bus_name(entry);
  params.menu_object_path = saber_sni_item_menu_path(entry);

  panel_close_menu(set);
  set->menu = saber_quicklist_open(&params, &panel_menu_handlers,
      panel_menu_context(set, SABER_MENU_TRAY, NULL));

  /* The item published nothing drawable. ContextMenu is then the only thing
  left to ask, and most items answer it by doing nothing. */
  if (set->menu == NULL) {
    saber_sni_item_context_menu(entry, (int)panel->pointer_x,
        (int)panel->pointer_y);
  }
}

/* Function purpose: The session menu. D-013: an action this user cannot
perform is ABSENT, never greyed out -- somebody outside the operator group sees
a menu with no power entries at all rather than three dead ones. */
static void
panel_open_session_menu(struct saber_panel *panel,
    const struct saber_slot *slot)
{
  static const enum saber_session_action order[] = {
    SABER_SESSION_LOCK,
    SABER_SESSION_LOGOUT,
    SABER_SESSION_SUSPEND,
    SABER_SESSION_REBOOT,
    SABER_SESSION_POWEROFF,
  };

  struct saber_panels *set = panel->set;
  struct saber_quicklist_window entries[G_N_ELEMENTS(order) + 1];
  size_t count = 0;

  for (size_t i = 0; i < G_N_ELEMENTS(order); i++) {
    if (!saber_session_available(set->deps.config, order[i])) {
      continue;
    }

    entries[count].title = saber_session_action_label(order[i]);
    entries[count].handle = SABER_MENU_TAG(order[i]);
    count++;
  }

  if (count == 0) {
    g_message("saber: no session action is available to this user");

    return;
  }

  /* The ordinary escape from a power menu, and it is also what keeps a menu
  down to a single available action from being one the widget declines to
  draw. */
  entries[count].title = "Cancel";
  entries[count].handle = SABER_MENU_TAG(SABER_SESSION_ACTION_COUNT);
  count++;

  struct saber_quicklist_params params;

  panel_menu_params(panel, slot, &params);
  params.windows = entries;
  params.windows_len = count;

  panel_close_menu(set);
  set->menu = saber_quicklist_open(&params, &panel_menu_handlers,
      panel_menu_context(set, SABER_MENU_SESSION, NULL));
}

static void
panel_open_trash_menu(struct saber_panel *panel, const struct saber_slot *slot)
{
  struct saber_panels *set = panel->set;
  struct saber_quicklist_window entries[] = {
    { .title = "Open Trash", .handle = SABER_MENU_TAG(SABER_TRASH_OPEN) },
    { .title = "Empty Trash", .handle = SABER_MENU_TAG(SABER_TRASH_EMPTY) },
  };

  struct saber_quicklist_params params;

  panel_menu_params(panel, slot, &params);
  params.windows = entries;
  params.windows_len = G_N_ELEMENTS(entries);

  panel_close_menu(set);
  set->menu = saber_quicklist_open(&params, &panel_menu_handlers,
      panel_menu_context(set, SABER_MENU_TRASH, NULL));
}

/* -------------------------------------------------------------- sheet grid */

/* Sheet 0 is not one of ten equal cells: its views stay visible underneath
whichever sheet is being displayed. It gets a row of its own above the three by
three block that holds 1-9, so the asymmetry is what the grid shows rather than
something the user is expected to remember. */
struct saber_sheet_grid {
  struct saber_panels *set;
  struct saber_panel *panel;
  struct saber_popup *popup;

  int counts[SABER_SHEET_COUNT];
  int current;
  int width, height;
  int hovered, selected;

  PangoFontDescription *font, *small_font;

  const struct saber_keyboard_listener *prev_keyboard;
  void *prev_keyboard_data;

  struct saber_scroll_accum scroll;

  bool inside, pressed, closing, held, listening;
};

static void
grid_cell(int sheet, double *x, double *y, double *width, double *height)
{
  if (sheet == 0) {
    *x = SABER_GRID_PAD;
    *y = SABER_GRID_PAD;
    *width = SABER_GRID_COLUMNS * SABER_GRID_CELL_W +
        (SABER_GRID_COLUMNS - 1) * SABER_GRID_GAP;
    *height = SABER_GRID_ZERO_H;

    return;
  }

  int column = (sheet - 1) % SABER_GRID_COLUMNS;
  int row = (sheet - 1) / SABER_GRID_COLUMNS;

  *x = SABER_GRID_PAD + column * (SABER_GRID_CELL_W + SABER_GRID_GAP);
  *y = SABER_GRID_PAD + SABER_GRID_ZERO_H + SABER_GRID_GAP +
      row * (SABER_GRID_CELL_H + SABER_GRID_GAP);
  *width = SABER_GRID_CELL_W;
  *height = SABER_GRID_CELL_H;
}

/* `y` is the text's vertical centre, which is what every caller here has. */
static void
grid_text(cairo_t *cr,
    PangoFontDescription *font,
    const char *text,
    double x,
    double y,
    double width,
    bool centre)
{
  PangoLayout *layout = pango_cairo_create_layout(cr);
  int text_width = 0, text_height = 0;

  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, text, -1);
  pango_layout_get_pixel_size(layout, &text_width, &text_height);

  cairo_move_to(cr, centre ? x + (width - text_width) / 2.0 : x,
      y - text_height / 2.0);
  pango_cairo_show_layout(cr, layout);
  g_object_unref(layout);
}

static void
grid_render(void *data,
    struct saber_popup *popup,
    cairo_t *cr,
    int width,
    int height)
{
  (void)popup;

  struct saber_sheet_grid *grid = data;
  const struct saber_theme *theme = grid->set->deps.theme;

  saber_theme_set_source(cr, &theme->background);
  cairo_rectangle(cr, 0.0, 0.0, width, height);
  cairo_fill(cr);

  saber_theme_set_source(cr, &theme->dim);
  cairo_set_line_width(cr, 1.0);
  cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
  cairo_stroke(cr);

  for (int sheet = 0; sheet < SABER_SHEET_COUNT; sheet++) {
    double x, y, w, h;

    grid_cell(sheet, &x, &y, &w, &h);

    int count = grid->counts[sheet];
    bool marked = sheet == grid->hovered || sheet == grid->selected;
    const struct saber_color *ink = &theme->dim;

    if (sheet == grid->current) {
      saber_theme_set_source(cr, &theme->accent);
      cairo_rectangle(cr, x, y, w, h);
      cairo_fill(cr);
      ink = &theme->badge_fg;
    } else if (count > 0) {
      saber_theme_set_source(cr, &theme->backlight);
      cairo_rectangle(cr, x, y, w, h);
      cairo_fill(cr);
      ink = &theme->foreground;
    }

    /* An empty sheet keeps its outline, so the grid still reads as ten places,
    and keeps the dim ink, so it reads as an empty one. */
    saber_theme_set_source(cr, marked ? &theme->accent : &theme->dim);
    cairo_set_line_width(cr, marked ? 2.0 : 1.0);
    cairo_rectangle(cr, x + 0.5, y + 0.5, w - 1.0, h - 1.0);
    cairo_stroke(cr);

    char label[8], badge[16];

    g_snprintf(label, sizeof(label), "%d", sheet);
    g_snprintf(badge, sizeof(badge), "%d", count);
    saber_theme_set_source(cr, ink);

    if (sheet == 0) {
      grid_text(cr, grid->font, label, x + 10.0, y + h / 2.0, 0.0, false);
      grid_text(cr, grid->small_font, "always visible", x + 28.0, y + h / 2.0,
          0.0, false);

      if (count > 0) {
        grid_text(cr, grid->small_font, badge, x + w - 16.0, y + h / 2.0, 0.0,
            false);
      }

      continue;
    }

    grid_text(cr, grid->font, label, x, y + h / 2.0 - (count > 0 ? 5.0 : 0.0),
        w, true);

    if (count > 0) {
      grid_text(cr, grid->small_font, badge, x, y + h - 10.0, w, true);
    }
  }
}

static int
grid_at(double px, double py)
{
  for (int sheet = 0; sheet < SABER_SHEET_COUNT; sheet++) {
    double x, y, w, h;

    grid_cell(sheet, &x, &y, &w, &h);

    if (px >= x && px < x + w && py >= y && py < y + h) {
      return sheet;
    }
  }

  return -1;
}

static void
grid_hover(struct saber_sheet_grid *grid, double x, double y)
{
  int sheet = grid_at(x, y);

  if (sheet == grid->hovered) {
    return;
  }

  grid->hovered = sheet;
  grid->selected = sheet;
  saber_popup_damage(grid->popup);
}

/* The grid is torn down before the switch is asked for: closing frees it, and
the socket answers on its own schedule. */
static void
grid_activate(struct saber_sheet_grid *grid, int sheet)
{
  struct saber_sheets *sheets = grid->set->deps.sheets;

  if (sheet < 0 || sheet >= SABER_SHEET_COUNT) {
    return;
  }

  sheet_grid_close(grid);
  saber_sheets_switch(sheets, sheet, NULL, NULL);
}

static void
grid_pointer_enter(void *data, struct wl_surface *surface, double x, double y)
{
  struct saber_sheet_grid *grid = data;

  grid->inside = grid->popup != NULL && surface == grid->popup->wl_surface;

  if (grid->inside) {
    grid_hover(grid, x, y);
  }
}

static void
grid_pointer_leave(void *data, struct wl_surface *surface)
{
  struct saber_sheet_grid *grid = data;

  if (grid->popup == NULL || surface != grid->popup->wl_surface) {
    return;
  }

  grid->inside = false;
  grid->hovered = -1;
  saber_popup_damage(grid->popup);
}

static void
grid_pointer_motion(void *data, uint32_t time, double x, double y)
{
  (void)time;

  struct saber_sheet_grid *grid = data;

  if (grid->inside) {
    grid_hover(grid, x, y);
  }
}

static void
grid_pointer_button(void *data, uint32_t time, uint32_t button, uint32_t state)
{
  (void)time;

  struct saber_sheet_grid *grid = data;

  if (button != BTN_LEFT) {
    return;
  }

  /* Action purpose: The press that opened the grid is often still in flight
  when the popup maps, so a bare release cannot be trusted -- a pick needs a
  press inside the grid first. */
  if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
    if (!grid->inside) {
      sheet_grid_close(grid);

      return;
    }

    grid->pressed = true;

    return;
  }

  if (!grid->pressed) {
    return;
  }

  grid->pressed = false;

  if (!grid->inside) {
    sheet_grid_close(grid);

    return;
  }

  grid_activate(grid, grid->hovered);
}

static void
grid_move(struct saber_sheet_grid *grid, int delta)
{
  int next = grid->selected < 0 ? (delta > 0 ? 0 : SABER_SHEET_COUNT - 1)
                                : grid->selected + delta;

  if (next < 0 || next >= SABER_SHEET_COUNT || next == grid->selected) {
    return;
  }

  grid->selected = next;
  grid->hovered = -1;
  saber_popup_damage(grid->popup);
}

/* Action purpose: The grid had no axis handler at all, so a wheel over an open
grid did nothing while the same wheel over the tile behind it stepped sheets --
the one gesture the grid exists to replace. Scroll moves the cursor rather than
switching, because the grid is a picker: the switch is the click. */
static void
grid_pointer_axis(void *data, uint32_t time, uint32_t axis, double value)
{
  (void)time;

  struct saber_sheet_grid *grid = data;

  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
    return;
  }

  int steps = saber_scroll_steps(&grid->scroll,
      saber_scroll_delta(&grid->scroll, axis, value), 120);

  if (steps != 0) {
    grid_move(grid, steps);
  }
}

static void
grid_pointer_axis_value120(void *data, uint32_t axis, int32_t value120)
{
  struct saber_sheet_grid *grid = data;

  saber_scroll_detail(&grid->scroll, axis, value120);
}

static void
grid_pointer_axis_stop(void *data, uint32_t time, uint32_t axis)
{
  (void)time;
  (void)axis;

  struct saber_sheet_grid *grid = data;

  saber_scroll_reset(&grid->scroll);
}

static void
grid_key(void *data, uint32_t time, uint32_t key, uint32_t state)
{
  (void)time;

  struct saber_sheet_grid *grid = data;

  if (state != WL_KEYBOARD_KEY_STATE_PRESSED) {
    return;
  }

  /* Action purpose: Wayland carries evdev keycodes, and every key that drives
  a grid of numbers sits at a fixed physical position -- so these need no xkb
  keymap, and none of them can be missing at the moment the grid opens. */
  switch (key) {
  case KEY_ESC:
    sheet_grid_close(grid);
    break;

  case KEY_LEFT:
    grid_move(grid, -1);
    break;

  case KEY_RIGHT:
    grid_move(grid, 1);
    break;

  case KEY_UP:
    grid_move(grid,
        grid->selected > SABER_GRID_COLUMNS ? -SABER_GRID_COLUMNS : -1);
    break;

  case KEY_DOWN:
    grid_move(grid, grid->selected == 0 ? 1 : SABER_GRID_COLUMNS);
    break;

  case KEY_ENTER:
  case KEY_KPENTER:
  case KEY_SPACE:
    grid_activate(grid, grid->selected);
    break;

  case KEY_0:
    grid_activate(grid, 0);
    break;

  default:
    if (key >= KEY_1 && key <= KEY_9) {
      grid_activate(grid, (int)(key - KEY_1) + 1);
    }
    break;
  }
}

static void
grid_configure(void *data, struct saber_popup *popup, int width, int height)
{
  (void)data;
  (void)popup;
  (void)width;
  (void)height;
}

static void
grid_done(void *data, struct saber_popup *popup)
{
  (void)popup;

  sheet_grid_close(data);
}

static const struct saber_popup_listener grid_popup_listener = {
  .configure = grid_configure,
  .render = grid_render,
  .done = grid_done,
};

/* The sheet grid is one popup surface; the same test grid_pointer_enter makes
to set `inside`. */
static bool
grid_pointer_owns(void *data, struct wl_surface *surface)
{
  struct saber_sheet_grid *grid = data;

  return grid->popup != NULL && grid->popup->wl_surface == surface;
}

static const struct saber_pointer_listener grid_pointer_listener = {
  .owns = grid_pointer_owns,
  .enter = grid_pointer_enter,
  .leave = grid_pointer_leave,
  .motion = grid_pointer_motion,
  .button = grid_pointer_button,
  .axis = grid_pointer_axis,
  .axis_value120 = grid_pointer_axis_value120,
  .axis_stop = grid_pointer_axis_stop,
};

static const struct saber_keyboard_listener grid_keyboard_listener = {
  .key = grid_key,
};

static void
sheet_grid_close(struct saber_sheet_grid *grid)
{
  if (grid == NULL || grid->closing) {
    return;
  }

  struct saber_panels *set = grid->set;

  grid->closing = true;
  set->grid = NULL;

  saber_popup_destroy(grid->popup);

  if (grid->held) {
    saber_surface_hold_keyboard(grid->panel->surface, false);
  }

  if (grid->listening) {
    saber_display_remove_pointer_listener(set->deps.display,
        &grid_pointer_listener, grid);
    saber_display_set_keyboard_listener(set->deps.display, grid->prev_keyboard,
        grid->prev_keyboard_data);
  }

  saber_display_flush(set->deps.display);

  pango_font_description_free(grid->font);
  pango_font_description_free(grid->small_font);

  struct saber_panel *panel = set->pointer_panel;

  if (panel != NULL) {
    panel_set_hover(panel,
        panel_slot_at(panel, panel->pointer_x, panel->pointer_y));
  }

  g_free(grid);
}

/* Function purpose: Pick a sheet rather than step to the next one. Stepping is
what scroll already does, and nine of the ten sheets are unreachable that way
without counting clicks. */
static void
panel_open_sheet_grid(struct saber_panel *panel, const struct saber_slot *slot)
{
  struct saber_panels *set = panel->set;
  const struct saber_sheets_state *state =
      saber_sheets_get_state(set->deps.sheets);

  if (state == NULL) {
    return;
  }

  panel_close_menu(set);

  struct saber_sheet_grid *grid = g_new0(struct saber_sheet_grid, 1);

  grid->set = set;
  grid->panel = panel;
  grid->current = state->current;
  grid->hovered = -1;
  grid->selected = state->current;
  grid->font = pango_font_description_from_string(SABER_GRID_FONT);
  grid->small_font = pango_font_description_from_string(SABER_GRID_SMALL_FONT);
  grid->width = SABER_GRID_PAD * 2 + SABER_GRID_COLUMNS * SABER_GRID_CELL_W +
      (SABER_GRID_COLUMNS - 1) * SABER_GRID_GAP;
  grid->height = SABER_GRID_PAD * 2 + SABER_GRID_ZERO_H + SABER_GRID_GAP +
      3 * SABER_GRID_CELL_H + 2 * SABER_GRID_GAP;

  memcpy(grid->counts, state->counts, sizeof(grid->counts));
  set->grid = grid;

  struct saber_popup_params params;

  saber_popup_menu_params(&params, set->deps.config->panel.edge, grid->width,
      grid->height, 0, (int32_t)slot->y, (int32_t)panel->width,
      (int32_t)slot->height);
  params.grab = true;

  /* Action purpose: Before the popup maps, not after -- a popup inherits the
  keyboard interactivity its parent layer surface had at the time, so raising
  it afterwards leaves a grid that is already on screen deaf. */
  saber_surface_hold_keyboard(panel->surface, true);
  grid->held = true;

  grid->popup = saber_popup_create(panel->surface, &params,
      &grid_popup_listener, grid);

  if (grid->popup == NULL) {
    sheet_grid_close(grid);

    return;
  }

  /* Action purpose: Pointer input is registered, not seized -- display.c routes
  by surface, so the column keeps its own clicks while the grid is up. The
  keyboard stays a single slot and the grid is modal for it. */
  grid->prev_keyboard = set->deps.display->keyboard_listener;
  grid->prev_keyboard_data = set->deps.display->keyboard_data;

  saber_display_add_pointer_listener(set->deps.display, &grid_pointer_listener,
      grid);
  saber_display_set_keyboard_listener(set->deps.display,
      &grid_keyboard_listener, grid);
  grid->listening = true;

  saber_display_flush(set->deps.display);
}

static void
panel_cycle_windows(struct saber_item *item, int direction)
{
  guint count = (guint)saber_item_window_count(item);

  if (count == 0) {
    return;
  }

  int active = item_active_window(item);
  int next = active < 0 ? 0 : active + direction;

  next = ((next % (int)count) + (int)count) % (int)count;

  window_raise(item_window(item, (guint)next));
}

static void
panel_click_app(struct saber_panel *panel, struct saber_item *item)
{
  struct saber_panels *set = panel->set;
  guint count = (guint)saber_item_window_count(item);

  if (count == 0) {
    panel_launch(set, item);

    return;
  }

  if (count == 1) {
    struct saber_toplevel *toplevel = item_window(item, 0);

    if (saber_toplevel_has_state(toplevel, SABER_TOPLEVEL_ACTIVATED)) {
      saber_toplevel_set_minimized(toplevel);
    } else {
      window_raise(toplevel);
    }

    return;
  }

  /* The spread, filtered to this application (BLUEPRINT.md 5.3). Until it is
  wired in, this cycles forward through the windows -- the same thing a second
  click on an already-focused tile would do in Unity once the spread has been
  dismissed. */
  if (set->spread != NULL) {
    set->spread(item->id, panel->output, set->spread_user);

    return;
  }

  panel_cycle_windows(item, 1);
}

static void
panel_click_device(struct saber_panels *set, int sub, bool unmount)
{
  const GPtrArray *list = saber_devices_list(set->deps.devices);

  if (list == NULL || sub < 0 || (guint)sub >= list->len) {
    return;
  }

  const struct saber_device *device = g_ptr_array_index(list, sub);

  if (!unmount) {
    panel_open_path(set, device->mount_point);

    return;
  }

  GError *error = NULL;

  if (!saber_devices_unmount(set->deps.devices, device->mount_point, NULL, NULL,
          &error)) {
    g_warning("saber: unmount %s: %s", device->mount_point,
        error != NULL ? error->message : "failed");
  }

  g_clear_error(&error);
}

static void
panel_click_tray(struct saber_panel *panel,
    const struct saber_slot *slot,
    uint32_t button)
{
  struct saber_panels *set = panel->set;
  struct saber_sni_item *entry =
      saber_sni_nth(set->deps.sni, (unsigned int)slot->sub);
  int x = (int)panel->pointer_x;
  int y = (int)panel->pointer_y;

  if (entry == NULL) {
    return;
  }

  switch (button) {
  case SABER_BTN_MIDDLE:
    saber_sni_item_secondary_activate(entry, x, y);
    break;

  /* Action purpose: The item's DBusMenu is drawn by the quicklist, in Saber's
  own palette (BLUEPRINT.md 5.4). ItemIsMenu is advisory and is not the test;
  saber_sni_item_has_menu is. */
  case SABER_BTN_RIGHT:
    panel_open_tray_menu(panel, slot, entry);
    break;

  default:
    if (saber_sni_item_is_menu(entry) && saber_sni_item_has_menu(entry)) {
      panel_open_tray_menu(panel, slot, entry);
      break;
    }

    saber_sni_item_activate(entry, x, y);
    break;
  }
}

static void
panel_activate_slot(struct saber_panel *panel, int index, uint32_t button)
{
  if (index < 0 || (guint)index >= panel->slots->len) {
    return;
  }

  struct saber_panels *set = panel->set;
  const struct saber_slot *slot =
      &g_array_index(panel->slots, struct saber_slot, index);
  struct saber_item *item = saber_model_nth(set->deps.model, slot->item);

  if (item == NULL) {
    return;
  }

  switch (item->type) {
  case SABER_ITEM_APP:
    if (button == SABER_BTN_MIDDLE) {
      panel_launch(set, item);
    } else if (button == SABER_BTN_LEFT) {
      panel_click_app(panel, item);
    } else if (button == SABER_BTN_RIGHT) {
      panel_open_app_menu(panel, slot, item);
    }
    break;

  case SABER_ITEM_SHEETS:
    if (button != SABER_BTN_MIDDLE) {
      panel_open_sheet_grid(panel, slot);
    }
    break;

  case SABER_ITEM_DEVICES:
    if (button == SABER_BTN_LEFT || button == SABER_BTN_MIDDLE) {
      panel_click_device(set, slot->sub, button == SABER_BTN_MIDDLE);
    }
    break;

  case SABER_ITEM_TRASH:
    if (button == SABER_BTN_RIGHT) {
      panel_open_trash_menu(panel, slot);
    } else if (button == SABER_BTN_LEFT) {
      panel_open_path(set, saber_trash_path(set->deps.trash));
    }
    break;

  case SABER_ITEM_TRAY:
    panel_click_tray(panel, slot, button);
    break;

  case SABER_ITEM_SESSION:
    if (button != SABER_BTN_MIDDLE) {
      panel_open_session_menu(panel, slot);
    }
    break;

  case SABER_ITEM_BFB:
    if (set->dash != NULL) {
      set->dash(panel->output, set->dash_user);
    }
    break;

  default:
    break;
  }
}

/* Function purpose: Point the accumulator at a target, dropping any remainder
that belonged to the last one. Called on every scroll event rather than only on
a change, because a target is identified by what it is and not by when it was
last seen. */
static void
panel_scroll_arm(struct saber_panels *set, size_t item, int sub)
{
  if (set->scroll_item == item && set->scroll_sub == sub) {
    return;
  }

  set->scroll_item = item;
  set->scroll_sub = sub;
  saber_scroll_reset(&set->scroll);
}

static void
panel_scroll_head(struct saber_panel *panel, int steps)
{
  int next = CLAMP(panel->head_scroll + steps, 0,
      panel_head_max_scroll(panel));

  if (next == panel->head_scroll) {
    return;
  }

  panel->head_scroll = next;
  panel_layout(panel);

  /* The tiles moved under a pointer that did not, so the hover has to be
  re-resolved or the highlight stays on a tile that is now somewhere else. */
  panel_set_hover(panel,
      panel_slot_at(panel, panel->pointer_x, panel->pointer_y));
  panel_damage(panel);
}

/* Function purpose: Everything a scroll over the column can mean. `value120`
is the event's delta in v120 units, where 120 is one wheel notch -- NOT a bare
direction: a touchpad emits dozens of fractional events per gesture, and the
sign alone fired a sheet switch or a window cycle for every one of them. */
static void
panel_scroll_slot(struct saber_panel *panel, int index, int32_t value120)
{
  if (value120 == 0) {
    return;
  }

  struct saber_panels *set = panel->set;
  const struct saber_slot *slot =
      index >= 0 && (guint)index < panel->slots->len
      ? &g_array_index(panel->slots, struct saber_slot, index)
      : NULL;
  struct saber_item *item =
      slot != NULL ? saber_model_nth(set->deps.model, slot->item) : NULL;

  if (item != NULL && item->type == SABER_ITEM_TRAY) {
    struct saber_sni_item *entry =
        saber_sni_nth(set->deps.sni, (unsigned int)slot->sub);

    panel_scroll_arm(set, slot->item, slot->sub);

    /* Action purpose: Forwarded whole and negated, not truncated to an int and
    not stepped. StatusNotifierItem inherits Qt's wheel units, so 120 is one
    notch and the applet on the other end divides by 120 itself -- casting the
    raw axis value to int threw every sub-notch touchpad delta away as zero.
    The sign is Qt's too: positive is away from the user, the opposite of
    wl_pointer.axis, so an uninverted delta scrolled every tray applet the
    wrong way relative to every other client on the desktop. */
    if (entry != NULL) {
      saber_sni_item_scroll(entry, -value120, SABER_SNI_VERTICAL);
    }

    return;
  }

  /* Action purpose: An application tile only claims the wheel while it has
  something to cycle THROUGH. With one window cycling re-raises the same window
  and with none it does nothing at all, and either way the gesture is dead --
  so those tiles fall through to the band below, which is what makes scrolling
  the launcher work over most of its own length instead of only over the BFB. */
  if (item != NULL && item->type == SABER_ITEM_APP &&
      saber_item_window_count(item) > 1) {
    panel_scroll_arm(set, slot->item, -1);

    int steps = saber_scroll_steps(&set->scroll, value120, 120);

    if (steps != 0) {
      panel_cycle_windows(item, steps);
    }

    return;
  }

  if (item != NULL && item->type == SABER_ITEM_SHEETS) {
    panel_scroll_arm(set, slot->item, -1);

    int steps = saber_scroll_steps(&set->scroll, value120, 120);

    if (steps == 0) {
      return;
    }

    const struct saber_sheets_state *state =
        saber_sheets_get_state(set->deps.sheets);
    int current = state != NULL ? state->current : 0;
    int next = ((current + steps) % SABER_SHEET_COUNT + SABER_SHEET_COUNT) %
        SABER_SHEET_COUNT;

    saber_sheets_switch(set->deps.sheets, next, NULL, NULL);

    return;
  }

  /* Action purpose: Everything else scrolls the column. The BFB, the devices,
  the trash and the session tile have no per-tile scroll that is both useful
  and safe -- stepping a power action under the pointer is not something a
  stray wheel event should be able to do -- and the launcher's own band is the
  one thing a scroll anywhere on the strip can usefully mean. The empty gap
  between the band and the tail arrives here too, with no slot at all. */
  panel_scroll_arm(set, SABER_SCROLL_HEAD, -1);

  int steps = saber_scroll_steps(&set->scroll, value120, 120);

  if (steps != 0) {
    panel_scroll_head(panel, steps);
  }
}

/* ----------------------------------------------------------------- pointer */

static void
panel_set_hover(struct saber_panel *panel, int slot)
{
  if (panel->hover == slot) {
    return;
  }

  int64_t now = saber_clock_now(panel->clock);
  int animation_ms = panel->set->deps.config->panel.animation_ms;
  int duration = animation_ms > 0 ? MIN(animation_ms, SABER_HOVER_MS)
                                  : SABER_HOVER_MS;

  panel->fading = panel->hover;

  if (panel->fading >= 0) {
    saber_tween_start(&panel->hover_out, saber_tween_value(&panel->hover_in),
        0.0, duration, SABER_EASE_OUT_CUBIC, now);
  }

  panel->hover = slot;
  panel->pressed = -1;

  if (slot >= 0) {
    saber_tween_start(&panel->hover_in, 0.0, 1.0, duration,
        SABER_EASE_OUT_CUBIC, now);
  } else {
    saber_tween_stop(&panel->hover_in);
    panel->hover_in.value = 0.0;
  }

  panel_damage(panel);
}

static struct saber_panel *
panel_for_surface(struct saber_panels *panels, struct wl_surface *surface)
{
  for (guint i = 0; i < panels->list->len; i++) {
    struct saber_panel *panel = g_ptr_array_index(panels->list, i);

    if (panel->surface != NULL && panel->surface->wl_surface == surface) {
      return panel;
    }
  }

  return NULL;
}

static void
pointer_enter(void *data, struct wl_surface *surface, double x, double y)
{
  struct saber_panels *panels = data;
  struct saber_panel *panel = panel_for_surface(panels, surface);

  if (panel == NULL) {
    return;
  }

  panels->pointer_panel = panel;
  panel->pointer_x = x;
  panel->pointer_y = y;

  saber_display_set_cursor(panels->deps.display, "left_ptr");
  panel_set_hover(panel, panel_slot_at(panel, x, y));
}

static void
pointer_leave(void *data, struct wl_surface *surface)
{
  struct saber_panels *panels = data;
  struct saber_panel *panel = panel_for_surface(panels, surface);

  if (panel == NULL) {
    return;
  }

  if (panels->pointer_panel == panel) {
    panels->pointer_panel = NULL;
  }

  panel_set_hover(panel, -1);
}

static void
pointer_motion(void *data, uint32_t time, double x, double y)
{
  (void)time;

  struct saber_panels *panels = data;
  struct saber_panel *panel = panels->pointer_panel;

  if (panel == NULL) {
    return;
  }

  panel->pointer_x = x;
  panel->pointer_y = y;

  panel_set_hover(panel, panel_slot_at(panel, x, y));
}

static void
pointer_button(void *data, uint32_t time, uint32_t button, uint32_t state)
{
  (void)time;

  struct saber_panels *panels = data;
  struct saber_panel *panel = panels->pointer_panel;

  if (panel == NULL) {
    return;
  }

  /* Action purpose: Press only marks the tile, release acts. Acting on press
  would fire on a drag that was never meant to be a click, and reorder is a
  drag gesture on exactly these tiles. */
  if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
    panel->pressed = panel->hover;
    panel_damage(panel);

    return;
  }

  int slot = panel->pressed;

  panel->pressed = -1;
  panel_damage(panel);

  if (slot >= 0 && slot == panel->hover) {
    panel_activate_slot(panel, slot, button);
  }
}

static void
pointer_axis(void *data, uint32_t time, uint32_t axis, double value)
{
  (void)time;

  struct saber_panels *panels = data;
  struct saber_panel *panel = panels->pointer_panel;

  if (panel == NULL || axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
    return;
  }

  panel_scroll_slot(panel, panel->hover,
      saber_scroll_delta(&panels->scroll, axis, value));
}

static void
pointer_axis_value120(void *data, uint32_t axis, int32_t value120)
{
  struct saber_panels *panels = data;

  saber_scroll_detail(&panels->scroll, axis, value120);
}

static void
pointer_axis_stop(void *data, uint32_t time, uint32_t axis)
{
  (void)time;
  (void)axis;

  struct saber_panels *panels = data;

  saber_scroll_reset(&panels->scroll);
}

/* The set owns one surface per output, and panel_for_surface is already the
lookup every handler does. Registering it as the ownership test is what keeps
the column's own clicks reaching it while a Dash, spread or menu is open. */
static bool
panel_pointer_owns(void *data, struct wl_surface *surface)
{
  struct saber_panels *panels = data;

  return panel_for_surface(panels, surface) != NULL;
}

static const struct saber_pointer_listener panel_pointer_listener = {
  .owns = panel_pointer_owns,
  .enter = pointer_enter,
  .leave = pointer_leave,
  .motion = pointer_motion,
  .button = pointer_button,
  .axis = pointer_axis,
  .axis_value120 = pointer_axis_value120,
  .axis_stop = pointer_axis_stop,
};

/* ------------------------------------------------------------- panel set */

static struct saber_panel *
panel_create(struct saber_panels *panels, struct saber_output *output)
{
  struct saber_panel *panel = g_new0(struct saber_panel, 1);

  panel->set = panels;
  panel->output = output;
  panel->render = panels->render;
  panel->clock = saber_clock_create();
  panel->anim = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
      anim_state_free);
  panel->slots = g_array_new(FALSE, FALSE, sizeof(struct saber_slot));
  panel->separator_y = -1.0;
  panel->hover = -1;
  panel->fading = -1;
  panel->pressed = -1;

  saber_tween_init(&panel->hover_in, 0.0);
  saber_tween_init(&panel->hover_out, 0.0);
  saber_clock_add(panel->clock, &panel->hover_in);
  saber_clock_add(panel->clock, &panel->hover_out);

  struct saber_surface_params params;

  saber_surface_panel_params(&params, panels->deps.config, output);

  panel->surface = saber_surface_create(panels->deps.display, &params,
      &panel_surface_listener, panel);

  if (panel->surface == NULL) {
    saber_clock_destroy(panel->clock);
    g_hash_table_destroy(panel->anim);
    g_array_free(panel->slots, TRUE);
    g_free(panel);

    return NULL;
  }

  return panel;
}

static void
panel_destroy(struct saber_panel *panel)
{
  if (panel == NULL) {
    return;
  }

  /* A popup outlives its parent layer surface for exactly as long as it takes
  the compositor to notice, which is a protocol error. */
  if (panel->set->grid != NULL && panel->set->grid->panel == panel) {
    sheet_grid_close(panel->set->grid);
  }

  if (panel->set->menu != NULL) {
    saber_quicklist_close(panel->set->menu);
  }

  if (panel->frame != NULL) {
    wl_callback_destroy(panel->frame);
  }

  saber_surface_destroy(panel->surface);
  saber_clock_destroy(panel->clock);
  g_hash_table_destroy(panel->anim);
  g_array_free(panel->slots, TRUE);
  g_free(panel);
}

/* "all" puts a column on every output, "primary" on the first to appear --
Wayland has no notion of a primary output, so first-seen is the only honest
reading -- and anything else is an output name. */
static bool
panels_wants_output(const struct saber_panels *panels,
    const struct saber_output *output)
{
  const char *want = panels->deps.config->panel.output;

  if (want == NULL || *want == '\0' || g_strcmp0(want, "all") == 0) {
    return true;
  }

  if (g_strcmp0(want, "primary") == 0) {
    return panels->list->len == 0;
  }

  return output->name != NULL && g_strcmp0(want, output->name) == 0;
}

static void
output_added(void *data, struct saber_output *output)
{
  struct saber_panels *panels = data;

  if (!panels_wants_output(panels, output)) {
    return;
  }

  struct saber_panel *panel = panel_create(panels, output);

  if (panel == NULL) {
    g_warning("saber: could not create a panel surface on output '%s'",
        output->name != NULL ? output->name : "?");

    return;
  }

  g_ptr_array_add(panels->list, panel);
}

static void
output_removed(void *data, struct saber_output *output)
{
  struct saber_panels *panels = data;

  for (guint i = 0; i < panels->list->len; i++) {
    struct saber_panel *panel = g_ptr_array_index(panels->list, i);

    if (panel->output != output) {
      continue;
    }

    if (panels->pointer_panel == panel) {
      panels->pointer_panel = NULL;
    }

    g_ptr_array_remove_index(panels->list, i);
    panel_destroy(panel);

    return;
  }
}

static void
output_changed(void *data, struct saber_output *output)
{
  (void)output;

  saber_panels_refresh(data);
}

static const struct saber_output_listener panel_output_listener = {
  .added = output_added,
  .removed = output_removed,
  .changed = output_changed,
};

struct saber_panels *
saber_panels_create(const struct saber_panel_deps *deps)
{
  struct saber_panels *panels = g_new0(struct saber_panels, 1);

  panels->deps = *deps;
  panels->list = g_ptr_array_new();
  panels->launches = g_ptr_array_new_with_free_func(launch_free);
  panels->scroll_item = SABER_SCROLL_HEAD;
  panels->scroll_sub = -1;

  saber_render_init(&panels->render, deps->config, deps->theme, deps->icons);

  saber_display_add_pointer_listener(deps->display, &panel_pointer_listener,
      panels);
  /* Fires immediately for every output already known, so no output that
  arrived during startup is missed. */
  saber_display_set_output_listener(deps->display, &panel_output_listener,
      panels);

  return panels;
}

void
saber_panels_destroy(struct saber_panels *panels)
{
  if (panels == NULL) {
    return;
  }

  /* Before the listeners are dropped: closing a menu puts the panel's own pair
  back, and doing that after they had been cleared would resurrect them. */
  panel_close_menu(panels);

  saber_display_set_output_listener(panels->deps.display, NULL, NULL);
  saber_display_remove_pointer_listener(panels->deps.display,
      &panel_pointer_listener, panels);
  saber_display_set_keyboard_listener(panels->deps.display, NULL, NULL);

  for (guint i = 0; i < panels->list->len; i++) {
    panel_destroy(g_ptr_array_index(panels->list, i));
  }

  if (panels->menu_ctx != NULL) {
    g_free(panels->menu_ctx->id);
    g_free(panels->menu_ctx);
  }

  g_ptr_array_free(panels->launches, TRUE);
  g_strfreev(panels->filemanager.argv);
  saber_appinfo_unref(panels->filemanager.app);
  g_ptr_array_free(panels->list, TRUE);
  g_free(panels);
}

void
saber_panels_set_dash(struct saber_panels *panels,
    saber_panel_dash_func func,
    void *user)
{
  panels->dash = func;
  panels->dash_user = user;
}

void
saber_panels_set_spread(struct saber_panels *panels,
    saber_panel_spread_func func,
    void *user)
{
  panels->spread = func;
  panels->spread_user = user;
}

void
saber_panels_refresh(struct saber_panels *panels)
{
  for (guint i = 0; i < panels->list->len; i++) {
    struct saber_panel *panel = g_ptr_array_index(panels->list, i);

    panel_layout(panel);
    panel_sync_anim(panel);

    /* The list changed under the pointer; whatever was hovered may now be a
    different tile or none at all. */
    if (panels->pointer_panel == panel) {
      int slot = panel_slot_at(panel, panel->pointer_x, panel->pointer_y);

      if (slot != panel->hover) {
        panel_set_hover(panel, slot);
        continue;
      }
    }

    panel_damage(panel);
  }
}

unsigned int
saber_panels_count(const struct saber_panels *panels)
{
  return panels->list->len;
}

void
saber_panels_close_menu(struct saber_panels *panels)
{
  if (panels == NULL) {
    return;
  }

  panel_close_menu(panels);
}
