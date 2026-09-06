/* Script function and purpose: The Dash -- the full-screen application grid.
Layout, cairo/pango drawing, the substring filter and its ranking, and the
xkbcommon keyboard handling that makes the search field type.

The surface is an OVERLAY layer surface anchored to all four edges with
EXCLUSIVE keyboard interactivity, and it is CREATED on show and DESTROYED on
hide rather than merely being unmapped: a layer surface holding the seat's
keyboard exclusively goes on holding it for as long as it exists. */

#include <math.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>

#include <glib.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon.h>

#include <saber/dash.h>
#include <saber/surface.h>

/* linux/input-event-codes.h is a Linux header; the button codes the protocol
carries are stable and are spelled out rather than pulled in through a shim. */
#define DASH_BTN_LEFT 0x110

#define DASH_ICON 48
#define DASH_CELL_WIDTH 148.0
#define DASH_CELL_HEIGHT 118.0
#define DASH_MARGIN 64.0
#define DASH_SEARCH_HEIGHT 46.0
#define DASH_SEARCH_MAX_WIDTH 560.0
#define DASH_SEARCH_GAP 40.0
#define DASH_RADIUS 8.0
#define DASH_SCROLLBAR 4.0

/* Weight of the second backdrop pass; see dash_render. */
#define DASH_BACKDROP_PASS 0.7

/* Action purpose: A grid is only readable while the eye can find the start of
the next row. Uncapped, a 3440px output lays twenty entries out as one line. */
#define DASH_MAX_COLUMNS 7

/* Saber's configuration carries no font key and the theme is colours only, so
the dash asks fontconfig for the system sans at the sizes it wants. */
#define DASH_FONT "Sans 9.5"
#define DASH_SEARCH_FONT "Sans 13"

/* Rank ladder: a prefix hit on any field beats an interior hit on every field.
Within each half the fields are ordered by how much the user meant them. */
enum dash_rank {
  DASH_RANK_NAME_PREFIX,
  DASH_RANK_GENERIC_PREFIX,
  DASH_RANK_EXEC_PREFIX,
  DASH_RANK_NAME_INSIDE,
  DASH_RANK_GENERIC_INSIDE,
  DASH_RANK_EXEC_INSIDE,
  DASH_RANK_NONE,
};

struct dash_entry {
  struct saber_appinfo *app; /* held ref: a re-scan replaces the index */
  const char *label;

  /* Case-folded once when the dash opens, so a keystroke costs three strstr
  per entry rather than three casefold allocations. */
  char *fold_name;
  char *fold_generic;
  char *fold_exec;
  char *sort_key;

  int rank;
};

struct saber_dash {
  struct saber_dash_deps deps;
  struct saber_surface *surface;

  int width, height;
  double scale;

  GString *query;
  GPtrArray *entries; /* struct dash_entry *, owned */
  GPtrArray *results; /* struct dash_entry *, borrowed from entries */

  int selected;
  int hovered;
  int pressed;
  int scroll; /* first visible row */

  int columns, rows, visible_rows;
  double cell_width, cell_height;
  double grid_x, grid_y;
  double search_x, search_y, search_width;

  PangoFontDescription *font;
  PangoFontDescription *search_font;

  struct xkb_context *xkb;
  struct xkb_keymap *keymap;
  struct xkb_state *xkb_state;

  const struct saber_pointer_listener *prev_pointer;
  void *prev_pointer_data;
  const struct saber_keyboard_listener *prev_keyboard;
  void *prev_keyboard_data;

  bool listening;
  bool hiding;
};

/* ------------------------------------------------------------- primitives */

static void
rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
  if (r > w / 2.0) {
    r = w / 2.0;
  }

  if (r > h / 2.0) {
    r = h / 2.0;
  }

  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2.0, 0.0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0.0, G_PI / 2.0);
  cairo_arc(cr, x + r, y + h - r, r, G_PI / 2.0, G_PI);
  cairo_arc(cr, x + r, y + r, r, G_PI, 3.0 * G_PI / 2.0);
  cairo_close_path(cr);
}

/* Function purpose: Paint a cached icon centred on (cx, cy) at `size` logical
pixels. The cache hands back a surface measured in DEVICE pixels, so the scale
factor is recovered from the surface itself rather than assumed. */
static void
draw_icon(cairo_t *cr, cairo_surface_t *icon, double cx, double cy, double size)
{
  double w = cairo_image_surface_get_width(icon);
  double h = cairo_image_surface_get_height(icon);

  if (w <= 0.0 || h <= 0.0) {
    return;
  }

  double k = size / (w > h ? w : h);

  cairo_save(cr);
  cairo_translate(cr, cx - w * k / 2.0, cy - h * k / 2.0);
  cairo_scale(cr, k, k);
  cairo_set_source_surface(cr, icon, 0.0, 0.0);
  cairo_paint(cr);
  cairo_restore(cr);
}

static void
set_source_alpha(cairo_t *cr, const struct saber_color *color, double alpha)
{
  cairo_set_source_rgba(cr, color->r, color->g, color->b, color->a * alpha);
}

/* --------------------------------------------------------------- indexing */

static void
entry_free(gpointer data)
{
  struct dash_entry *entry = data;

  saber_appinfo_unref(entry->app);
  g_free(entry->fold_name);
  g_free(entry->fold_generic);
  g_free(entry->fold_exec);
  g_free(entry->sort_key);
  g_free(entry);
}

static char *
fold(const char *text)
{
  return text != NULL && *text != '\0' ? g_utf8_casefold(text, -1) : NULL;
}

/* Function purpose: The command a desktop entry actually runs, stripped of its
arguments and its path, because "gimp-2.10" is what a user types when the entry
is called "GNU Image Manipulation Program". */
static char *
exec_basename(const char *exec)
{
  if (exec == NULL || *exec == '\0') {
    return NULL;
  }

  const char *end = strchr(exec, ' ');
  char *first = end != NULL ? g_strndup(exec, (size_t)(end - exec))
                            : g_strdup(exec);
  char *base = g_path_get_basename(first);

  g_free(first);

  return base;
}

static void
dash_build_entries(struct saber_dash *dash)
{
  g_ptr_array_set_size(dash->entries, 0);

  size_t count = saber_appinfo_index_size(dash->deps.index);

  for (size_t i = 0; i < count; i++) {
    struct saber_appinfo *app = saber_appinfo_index_nth(dash->deps.index, i);

    if (app == NULL) {
      continue;
    }

    const char *label = app->name != NULL && *app->name != '\0' ? app->name
                                                                : app->id;

    if (label == NULL || *label == '\0') {
      continue;
    }

    struct dash_entry *entry = g_new0(struct dash_entry, 1);
    char *exec = exec_basename(app->exec);

    entry->app = saber_appinfo_ref(app);
    entry->label = label;
    entry->fold_name = fold(label);
    entry->fold_generic = fold(app->generic_name);
    entry->fold_exec = fold(exec);
    entry->sort_key = g_utf8_collate_key(label, -1);

    g_free(exec);
    g_ptr_array_add(dash->entries, entry);
  }
}

/* Function purpose: Where `needle` sits in `haystack`, expressed as the ladder
above: `prefix` when it starts the field, `inside` when it merely occurs in it,
and DASH_RANK_NONE when it does not occur at all. */
static int
field_rank(const char *haystack, const char *needle, int prefix, int inside)
{
  if (haystack == NULL) {
    return DASH_RANK_NONE;
  }

  const char *hit = strstr(haystack, needle);

  if (hit == NULL) {
    return DASH_RANK_NONE;
  }

  return hit == haystack ? prefix : inside;
}

static int
entry_rank(const struct dash_entry *entry, const char *needle)
{
  int rank = field_rank(entry->fold_name, needle, DASH_RANK_NAME_PREFIX,
      DASH_RANK_NAME_INSIDE);
  int generic = field_rank(entry->fold_generic, needle,
      DASH_RANK_GENERIC_PREFIX, DASH_RANK_GENERIC_INSIDE);
  int exec = field_rank(entry->fold_exec, needle, DASH_RANK_EXEC_PREFIX,
      DASH_RANK_EXEC_INSIDE);

  if (generic < rank) {
    rank = generic;
  }

  if (exec < rank) {
    rank = exec;
  }

  return rank;
}

static gint
entry_compare(gconstpointer a, gconstpointer b)
{
  const struct dash_entry *left = *(const struct dash_entry *const *)a;
  const struct dash_entry *right = *(const struct dash_entry *const *)b;

  if (left->rank != right->rank) {
    return left->rank < right->rank ? -1 : 1;
  }

  return strcmp(left->sort_key, right->sort_key);
}

static void
dash_layout(struct saber_dash *dash);

static void
dash_filter(struct saber_dash *dash)
{
  char *needle = fold(dash->query->str);

  g_ptr_array_set_size(dash->results, 0);

  for (guint i = 0; i < dash->entries->len; i++) {
    struct dash_entry *entry = g_ptr_array_index(dash->entries, i);

    entry->rank = needle != NULL ? entry_rank(entry, needle)
                                 : DASH_RANK_NAME_PREFIX;

    if (entry->rank != DASH_RANK_NONE) {
      g_ptr_array_add(dash->results, entry);
    }
  }

  g_free(needle);
  g_ptr_array_sort(dash->results, entry_compare);

  dash->selected = dash->results->len > 0 ? 0 : -1;
  dash->hovered = -1;
  dash->pressed = -1;
  dash->scroll = 0;

  dash_layout(dash);
}

/* ----------------------------------------------------------------- layout */

static void
dash_layout(struct saber_dash *dash)
{
  int count = (int)dash->results->len;

  dash->cell_width = DASH_CELL_WIDTH;
  dash->cell_height = DASH_CELL_HEIGHT;

  dash->search_width = MIN((double)dash->width - 2.0 * DASH_MARGIN,
      DASH_SEARCH_MAX_WIDTH);

  if (dash->search_width < 120.0) {
    dash->search_width = 120.0;
  }

  dash->search_x = ((double)dash->width - dash->search_width) / 2.0;
  dash->search_y = DASH_MARGIN;

  double available = (double)dash->width - 2.0 * DASH_MARGIN;

  if (available < dash->cell_width) {
    available = dash->cell_width;
  }

  dash->columns = (int)(available / dash->cell_width);
  dash->columns = CLAMP(dash->columns, 1, DASH_MAX_COLUMNS);

  /* A short result set narrows the grid rather than leaving a ragged single
  row hanging off the left of a full-width one. */
  if (count > 0 && dash->columns > count) {
    dash->columns = count;
  }

  dash->grid_x =
      ((double)dash->width - dash->columns * dash->cell_width) / 2.0;
  dash->grid_y = dash->search_y + DASH_SEARCH_HEIGHT + DASH_SEARCH_GAP;

  double room = (double)dash->height - dash->grid_y - DASH_MARGIN / 2.0;

  dash->visible_rows = (int)(room / dash->cell_height);

  if (dash->visible_rows < 1) {
    dash->visible_rows = 1;
  }

  dash->rows = count > 0 ? (count + dash->columns - 1) / dash->columns : 0;
}

static int
dash_max_scroll(const struct saber_dash *dash)
{
  int max = dash->rows - dash->visible_rows;

  return max > 0 ? max : 0;
}

static void
dash_reveal_selection(struct saber_dash *dash)
{
  if (dash->selected < 0 || dash->columns <= 0) {
    return;
  }

  int row = dash->selected / dash->columns;

  if (row < dash->scroll) {
    dash->scroll = row;
  } else if (row >= dash->scroll + dash->visible_rows) {
    dash->scroll = row - dash->visible_rows + 1;
  }

  dash->scroll = CLAMP(dash->scroll, 0, dash_max_scroll(dash));
}

static bool
dash_cell_rect(const struct saber_dash *dash,
    int index,
    double *x,
    double *y)
{
  if (index < 0 || index >= (int)dash->results->len || dash->columns <= 0) {
    return false;
  }

  int row = index / dash->columns - dash->scroll;

  if (row < 0 || row >= dash->visible_rows) {
    return false;
  }

  *x = dash->grid_x + (index % dash->columns) * dash->cell_width;
  *y = dash->grid_y + row * dash->cell_height;

  return true;
}

static int
dash_cell_at(const struct saber_dash *dash, double x, double y)
{
  if (dash->columns <= 0) {
    return -1;
  }

  double local_x = x - dash->grid_x;
  double local_y = y - dash->grid_y;

  if (local_x < 0.0 || local_y < 0.0 ||
      local_x >= dash->columns * dash->cell_width ||
      local_y >= dash->visible_rows * dash->cell_height) {
    return -1;
  }

  int column = (int)(local_x / dash->cell_width);
  int row = (int)(local_y / dash->cell_height) + dash->scroll;
  int index = row * dash->columns + column;

  return index < (int)dash->results->len ? index : -1;
}

/* ---------------------------------------------------------------- drawing */

static void
dash_draw_search(struct saber_dash *dash, cairo_t *cr)
{
  const struct saber_theme *theme = dash->deps.theme;
  double x = dash->search_x;
  double y = dash->search_y;
  double w = dash->search_width;
  double h = DASH_SEARCH_HEIGHT;

  /* Filled twice for the same reason the backdrop is: the background role
  carries the user's panel translucency, and a see-through search field over a
  see-through backdrop leaves nothing for the query to sit on. */
  rounded_rect(cr, x, y, w, h, DASH_RADIUS);
  saber_theme_set_source(cr, &theme->background);
  cairo_fill_preserve(cr);
  cairo_fill_preserve(cr);

  saber_theme_set_source(cr, &theme->accent);
  cairo_set_line_width(cr, 1.5);
  cairo_stroke(cr);

  bool empty = dash->query->len == 0;
  PangoLayout *layout = pango_cairo_create_layout(cr);

  pango_layout_set_font_description(layout, dash->search_font);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_START);
  pango_layout_set_width(layout, (int)((w - 32.0) * PANGO_SCALE));
  pango_layout_set_text(layout,
      empty ? "Search applications\xe2\x80\xa6" : dash->query->str, -1);

  int text_width, text_height;

  pango_layout_get_pixel_size(layout, &text_width, &text_height);
  saber_theme_set_source(cr, empty ? &theme->dim : &theme->foreground);
  cairo_move_to(cr, x + 16.0, y + (h - text_height) / 2.0);
  pango_cairo_show_layout(cr, layout);

  if (!empty) {
    double caret = x + 16.0 + MIN((double)text_width, w - 32.0) + 2.0;

    saber_theme_set_source(cr, &theme->accent);
    cairo_rectangle(cr, caret, y + (h - text_height) / 2.0, 1.5, text_height);
    cairo_fill(cr);
  }

  g_object_unref(layout);
}

static void
dash_draw_cell(struct saber_dash *dash,
    cairo_t *cr,
    PangoLayout *layout,
    int index,
    double x,
    double y)
{
  const struct dash_entry *entry = g_ptr_array_index(dash->results, index);
  const struct saber_theme *theme = dash->deps.theme;
  double w = dash->cell_width;
  double h = dash->cell_height;

  if (index == dash->selected || index == dash->hovered) {
    rounded_rect(cr, x + 4.0, y + 4.0, w - 8.0, h - 8.0, DASH_RADIUS);
    set_source_alpha(cr, &theme->accent,
        index == dash->pressed ? 0.55 : (index == dash->selected ? 0.38
                                                                 : 0.18));
    cairo_fill_preserve(cr);

    set_source_alpha(cr, &theme->accent, index == dash->selected ? 1.0 : 0.5);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
  }

  int pixels = (int)lround(DASH_ICON * dash->scale);
  cairo_surface_t *icon = NULL;

  if (entry->app->icon != NULL) {
    icon = saber_icons_lookup(dash->deps.icons, entry->app->icon, pixels);
  }

  if (icon == NULL) {
    icon = saber_icons_lookup(dash->deps.icons, "application-x-executable",
        pixels);
  }

  double icon_cy = y + 14.0 + DASH_ICON / 2.0;

  if (icon != NULL) {
    draw_icon(cr, icon, x + w / 2.0, icon_cy, DASH_ICON);
  } else {
    /* The initial, so an entry whose icon resolves to nothing still reads as a
    distinct cell rather than as a gap in the grid. */
    char initial[8] = { 0 };

    g_utf8_strncpy(initial, entry->label, 1);
    *initial = (char)g_ascii_toupper(*initial);

    set_source_alpha(cr, &theme->dim, 0.6);
    rounded_rect(cr, x + w / 2.0 - DASH_ICON / 2.0, y + 14.0, DASH_ICON,
        DASH_ICON, DASH_RADIUS);
    cairo_fill(cr);

    /* The layout is already centred across the cell, so only the baseline has
    to be placed; the font is swapped for the one line and swapped back. */
    pango_layout_set_font_description(layout, dash->search_font);
    pango_layout_set_text(layout, initial, -1);

    int initial_height;

    pango_layout_get_pixel_size(layout, NULL, &initial_height);
    saber_theme_set_source(cr, &theme->foreground);
    cairo_move_to(cr, x + 8.0, icon_cy - initial_height / 2.0);
    pango_cairo_show_layout(cr, layout);
    pango_layout_set_font_description(layout, dash->font);
  }

  pango_layout_set_text(layout, entry->label, -1);
  saber_theme_set_source(cr, &theme->foreground);
  cairo_move_to(cr, x + 8.0, y + 14.0 + DASH_ICON + 10.0);
  pango_cairo_show_layout(cr, layout);
}

static void
dash_draw_scrollbar(struct saber_dash *dash, cairo_t *cr)
{
  int max = dash_max_scroll(dash);

  if (max <= 0) {
    return;
  }

  const struct saber_theme *theme = dash->deps.theme;
  double track_h = dash->visible_rows * dash->cell_height;
  double x = dash->grid_x + dash->columns * dash->cell_width + 10.0;
  double thumb_h = track_h * dash->visible_rows / (double)dash->rows;
  double thumb_y = dash->grid_y +
      (track_h - thumb_h) * dash->scroll / (double)max;

  set_source_alpha(cr, &theme->dim, 0.5);
  rounded_rect(cr, x, dash->grid_y, DASH_SCROLLBAR, track_h,
      DASH_SCROLLBAR / 2.0);
  cairo_fill(cr);

  saber_theme_set_source(cr, &theme->accent);
  rounded_rect(cr, x, thumb_y, DASH_SCROLLBAR, thumb_h, DASH_SCROLLBAR / 2.0);
  cairo_fill(cr);
}

static void
dash_render(void *data,
    struct saber_surface *surface,
    cairo_t *cr,
    int width,
    int height)
{
  struct saber_dash *dash = data;
  const struct saber_theme *theme = dash->deps.theme;

  dash->scale = saber_surface_scale(surface);

  if (width != dash->width || height != dash->height) {
    dash->width = width;
    dash->height = height;
    dash_layout(dash);
    dash_reveal_selection(dash);
  }

  /* Action purpose: A translucent palette fill, never a blur -- hikari does not
  advertise ext-background-effect and a client cannot read the screen behind
  itself (BLUEPRINT.md 5.7). SOURCE, not OVER: the buffer is recycled, so a
  translucent paint over a stale frame would accumulate. */
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  saber_theme_set_source(cr, &theme->overlay);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  /* Action purpose: The same role composited a second time. One pass of the
  theme's overlay alpha is not enough to read fifty names over a bright desktop,
  and inventing a colour here would break the rule that every colour in the
  panel comes from a theme role. Two passes still leave the desktop showing. */
  set_source_alpha(cr, &theme->overlay, DASH_BACKDROP_PASS);
  cairo_paint(cr);

  dash_draw_search(dash, cr);

  PangoLayout *layout = pango_cairo_create_layout(cr);

  pango_layout_set_font_description(layout, dash->font);
  pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_width(layout, (int)((dash->cell_width - 16.0) * PANGO_SCALE));

  /* A negative height is pango's "at most this many lines"; two is what a name
  as long as "Application Finder" needs and no more. */
  pango_layout_set_height(layout, -2);

  if (dash->results->len == 0) {
    pango_layout_set_width(layout, (int)(dash->width * PANGO_SCALE));
    pango_layout_set_text(layout, "No matching applications", -1);
    saber_theme_set_source(cr, &theme->dim);
    cairo_move_to(cr, 0.0, dash->grid_y + 24.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);

    return;
  }

  for (int i = 0; i < (int)dash->results->len; i++) {
    double x, y;

    if (dash_cell_rect(dash, i, &x, &y)) {
      dash_draw_cell(dash, cr, layout, i, x, y);
    }
  }

  g_object_unref(layout);
  dash_draw_scrollbar(dash, cr);
}

static void
dash_configure(void *data,
    struct saber_surface *surface,
    int width,
    int height)
{
  (void)surface;

  struct saber_dash *dash = data;

  dash->width = width;
  dash->height = height;
  dash_layout(dash);
  dash_reveal_selection(dash);
}

static void
dash_surface_closed(void *data, struct saber_surface *surface)
{
  (void)surface;

  saber_dash_hide(data);
}

static const struct saber_surface_listener dash_surface_listener = {
  .configure = dash_configure,
  .render = dash_render,
  .closed = dash_surface_closed,
};

static void
dash_damage(struct saber_dash *dash)
{
  if (dash->surface != NULL && !dash->surface->closed) {
    saber_surface_damage(dash->surface);
  }
}

/* -------------------------------------------------------------- activation */

static void
dash_launch(struct saber_dash *dash, int index)
{
  if (index < 0 || index >= (int)dash->results->len) {
    return;
  }

  const struct dash_entry *entry = g_ptr_array_index(dash->results, index);
  struct saber_appinfo *app = saber_appinfo_ref(entry->app);

  /* Action purpose: The dash is dismissed BEFORE the launch is recorded and
  after the fork, so the desktop is uncovered the moment the click lands rather
  than when the child finishes starting. */
  if (!saber_appinfo_launch(app, NULL, NULL, NULL)) {
    g_warning("saber: failed to launch '%s'", app->id);
    saber_appinfo_unref(app);

    return;
  }

  saber_dash_hide(dash);

  /* Action purpose: The launch window is what binds the new window to its tile
  -- neither foreign-toplevel protocol carries a pid, so a launch this process
  did not record is a window with no application. The model's form notes it
  through match.c and throbs the tile; match.c alone is the fallback when the
  dash was built without one. */
  if (dash->deps.model != NULL) {
    saber_model_note_launch(dash->deps.model, app->id);
  } else if (dash->deps.match != NULL) {
    saber_match_note_launch(dash->deps.match, app->id);
  }

  saber_appinfo_unref(app);
}

/* ----------------------------------------------------------------- pointer */

static void
dash_set_hover(struct saber_dash *dash, int index)
{
  if (dash->hovered == index) {
    return;
  }

  dash->hovered = index;
  dash_damage(dash);
}

static void
dash_pointer_enter(void *data, struct wl_surface *surface, double x, double y)
{
  struct saber_dash *dash = data;

  if (dash->surface == NULL || dash->surface->wl_surface != surface) {
    return;
  }

  saber_display_set_cursor(dash->deps.display, "left_ptr");
  dash_set_hover(dash, dash_cell_at(dash, x, y));
}

static void
dash_pointer_leave(void *data, struct wl_surface *surface)
{
  (void)surface;

  dash_set_hover(data, -1);
}

static void
dash_pointer_motion(void *data, uint32_t time, double x, double y)
{
  (void)time;

  struct saber_dash *dash = data;

  dash_set_hover(dash, dash_cell_at(dash, x, y));
}

static void
dash_pointer_button(void *data,
    uint32_t time,
    uint32_t button,
    uint32_t state)
{
  (void)time;

  struct saber_dash *dash = data;

  if (button != DASH_BTN_LEFT) {
    return;
  }

  if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
    dash->pressed = dash->hovered;
    dash->selected = dash->hovered >= 0 ? dash->hovered : dash->selected;
    dash_damage(dash);

    return;
  }

  int index = dash->pressed;

  dash->pressed = -1;

  /* A press on the backdrop is a dismissal: the dash covers the whole output,
  so there is nowhere else for "click away to close" to happen. */
  if (index < 0) {
    if (dash->hovered < 0) {
      saber_dash_hide(dash);
    }

    return;
  }

  if (index == dash->hovered) {
    dash_launch(dash, index);
  } else {
    dash_damage(dash);
  }
}

static void
dash_pointer_axis(void *data, uint32_t time, uint32_t axis, double value)
{
  (void)time;

  struct saber_dash *dash = data;

  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || value == 0.0) {
    return;
  }

  int scroll = CLAMP(dash->scroll + (value > 0.0 ? 1 : -1), 0,
      dash_max_scroll(dash));

  if (scroll == dash->scroll) {
    return;
  }

  dash->scroll = scroll;
  dash_damage(dash);
}

static const struct saber_pointer_listener dash_pointer_listener = {
  .enter = dash_pointer_enter,
  .leave = dash_pointer_leave,
  .motion = dash_pointer_motion,
  .button = dash_pointer_button,
  .axis = dash_pointer_axis,
};

/* ---------------------------------------------------------------- keyboard */

static void
dash_keymap(void *data, uint32_t format, int fd, uint32_t size)
{
  struct saber_dash *dash = data;

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
    close(fd);

    return;
  }

  char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

  if (map == MAP_FAILED) {
    close(fd);

    return;
  }

  struct xkb_keymap *keymap = xkb_keymap_new_from_string(dash->xkb, map,
      XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);

  munmap(map, size);
  close(fd);

  if (keymap == NULL) {
    return;
  }

  xkb_state_unref(dash->xkb_state);
  xkb_keymap_unref(dash->keymap);

  dash->keymap = keymap;
  dash->xkb_state = xkb_state_new(keymap);
}

static void
dash_modifiers(void *data,
    uint32_t depressed,
    uint32_t latched,
    uint32_t locked,
    uint32_t group)
{
  struct saber_dash *dash = data;

  if (dash->xkb_state != NULL) {
    xkb_state_update_mask(dash->xkb_state, depressed, latched, locked, 0, 0,
        group);
  }
}

static void
dash_move(struct saber_dash *dash, int delta)
{
  int count = (int)dash->results->len;

  if (count == 0) {
    return;
  }

  int index = CLAMP(dash->selected + delta, 0, count - 1);

  if (index == dash->selected) {
    return;
  }

  dash->selected = index;
  dash->hovered = -1;
  dash_reveal_selection(dash);
  dash_damage(dash);
}

static void
dash_backspace(struct saber_dash *dash)
{
  if (dash->query->len == 0) {
    return;
  }

  const char *last = g_utf8_find_prev_char(dash->query->str,
      dash->query->str + dash->query->len);

  g_string_truncate(dash->query,
      last != NULL ? (gsize)(last - dash->query->str) : 0);
  dash_filter(dash);
  dash_damage(dash);
}

/* Function purpose: Append whatever the key produces under the current layout
and modifiers, rejecting the control characters that xkb also reports through
this call -- Return and Backspace both have a UTF-8 form. */
static bool
dash_type(struct saber_dash *dash, uint32_t key)
{
  char buffer[16];
  int length = xkb_state_key_get_utf8(dash->xkb_state, key + 8, buffer,
      sizeof(buffer));

  if (length <= 0 || (size_t)length >= sizeof(buffer)) {
    return false;
  }

  buffer[length] = '\0';

  if ((unsigned char)buffer[0] < 0x20 || buffer[0] == 0x7f) {
    return false;
  }

  g_string_append_len(dash->query, buffer, length);
  dash_filter(dash);

  return true;
}

static void
dash_key(void *data, uint32_t time, uint32_t key, uint32_t state)
{
  (void)time;

  struct saber_dash *dash = data;

  if (state != WL_KEYBOARD_KEY_STATE_PRESSED || dash->xkb_state == NULL) {
    return;
  }

  /* Wayland keycodes are evdev; xkb wants the X11 numbering. */
  xkb_keysym_t sym = xkb_state_key_get_one_sym(dash->xkb_state, key + 8);

  switch (sym) {
  case XKB_KEY_Escape:
    saber_dash_hide(dash);

    return;

  case XKB_KEY_BackSpace:
    dash_backspace(dash);

    return;

  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
    dash_launch(dash, dash->selected);

    return;

  case XKB_KEY_Left:
    dash_move(dash, -1);
    break;

  case XKB_KEY_Right:
    dash_move(dash, 1);
    break;

  case XKB_KEY_Up:
    dash_move(dash, -dash->columns);
    break;

  case XKB_KEY_Down:
    dash_move(dash, dash->columns);
    break;

  case XKB_KEY_Page_Up:
    dash_move(dash, -dash->columns * dash->visible_rows);
    break;

  case XKB_KEY_Page_Down:
    dash_move(dash, dash->columns * dash->visible_rows);
    break;

  case XKB_KEY_Home:
    dash_move(dash, -(int)dash->results->len);
    break;

  case XKB_KEY_End:
    dash_move(dash, (int)dash->results->len);
    break;

  default:
    if (dash_type(dash, key)) {
      dash_damage(dash);
    }

    return;
  }
}

static const struct saber_keyboard_listener dash_keyboard_listener = {
  .keymap = dash_keymap,
  .key = dash_key,
  .modifiers = dash_modifiers,
};

/* --------------------------------------------------------------- lifecycle */

struct saber_dash *
saber_dash_create(const struct saber_dash_deps *deps)
{
  if (deps == NULL || deps->display == NULL || deps->theme == NULL ||
      deps->index == NULL) {
    return NULL;
  }

  struct saber_dash *dash = g_new0(struct saber_dash, 1);

  dash->deps = *deps;
  dash->query = g_string_new(NULL);
  dash->entries = g_ptr_array_new_with_free_func(entry_free);
  dash->results = g_ptr_array_new();
  dash->selected = -1;
  dash->hovered = -1;
  dash->pressed = -1;
  dash->scale = 1.0;
  dash->columns = 1;
  dash->visible_rows = 1;

  dash->font = pango_font_description_from_string(DASH_FONT);
  dash->search_font = pango_font_description_from_string(DASH_SEARCH_FONT);

  dash->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

  /* Action purpose: wl_keyboard.keymap fires once, when the seat's keyboard is
  bound -- long before any dash exists -- and display.c does not cache it, so a
  listener installed later never sees one. The compiled default layout is what
  makes the search field type at all; a real keymap replaces it if the event
  does arrive. */
  if (dash->xkb != NULL) {
    dash->keymap = xkb_keymap_new_from_names(dash->xkb, NULL,
        XKB_KEYMAP_COMPILE_NO_FLAGS);

    if (dash->keymap != NULL) {
      dash->xkb_state = xkb_state_new(dash->keymap);
    }
  }

  return dash;
}

void
saber_dash_destroy(struct saber_dash *dash)
{
  if (dash == NULL) {
    return;
  }

  saber_dash_hide(dash);

  g_ptr_array_free(dash->results, TRUE);
  g_ptr_array_unref(dash->entries);
  g_string_free(dash->query, TRUE);

  pango_font_description_free(dash->font);
  pango_font_description_free(dash->search_font);

  xkb_state_unref(dash->xkb_state);
  xkb_keymap_unref(dash->keymap);
  xkb_context_unref(dash->xkb);

  g_free(dash);
}

bool
saber_dash_is_visible(const struct saber_dash *dash)
{
  return dash != NULL && dash->surface != NULL;
}

bool
saber_dash_show(struct saber_dash *dash, struct saber_output *output)
{
  if (dash == NULL) {
    return false;
  }

  if (dash->surface != NULL) {
    return true;
  }

  g_string_truncate(dash->query, 0);
  dash_build_entries(dash);

  /* The output's own size, so the first frame is laid out correctly rather than
  being relaid on the configure that follows it. */
  if (output != NULL && output->scale > 0) {
    dash->width = output->width / output->scale;
    dash->height = output->height / output->scale;
  }

  if (dash->width <= 0 || dash->height <= 0) {
    dash->width = 1280;
    dash->height = 720;
  }

  dash_filter(dash);

  struct saber_surface_params params = {
    .output = output,
    .layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
    .anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
    .keyboard_interactivity =
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE,
    .exclusive_zone = 0,
    .layer_namespace = "saber-dash",
  };

  dash->surface = saber_surface_create(dash->deps.display, &params,
      &dash_surface_listener, dash);

  if (dash->surface == NULL) {
    g_warning("saber: could not create the dash surface");

    return false;
  }

  /* Action purpose: The dash is modal, and display.c holds one listener of each
  kind. Taking both over for its lifetime and putting the previous pair back on
  hide is the only way to share them with the panel. */
  dash->prev_pointer = dash->deps.display->pointer_listener;
  dash->prev_pointer_data = dash->deps.display->pointer_data;
  dash->prev_keyboard = dash->deps.display->keyboard_listener;
  dash->prev_keyboard_data = dash->deps.display->keyboard_data;

  saber_display_set_pointer_listener(dash->deps.display, &dash_pointer_listener,
      dash);
  saber_display_set_keyboard_listener(dash->deps.display,
      &dash_keyboard_listener, dash);
  dash->listening = true;

  saber_display_flush(dash->deps.display);

  return true;
}

void
saber_dash_hide(struct saber_dash *dash)
{
  /* Hiding is re-entrant by design: the surface's `closed` callback lands here,
  and a launch hides the dash from inside a keyboard event. */
  if (dash == NULL || dash->surface == NULL || dash->hiding) {
    return;
  }

  dash->hiding = true;

  saber_surface_destroy(dash->surface);
  dash->surface = NULL;

  if (dash->listening) {
    saber_display_set_pointer_listener(dash->deps.display, dash->prev_pointer,
        dash->prev_pointer_data);
    saber_display_set_keyboard_listener(dash->deps.display, dash->prev_keyboard,
        dash->prev_keyboard_data);
    dash->listening = false;
  }

  saber_display_flush(dash->deps.display);

  g_ptr_array_set_size(dash->results, 0);
  g_ptr_array_set_size(dash->entries, 0);
  g_string_truncate(dash->query, 0);

  dash->selected = -1;
  dash->hovered = -1;
  dash->pressed = -1;
  dash->scroll = 0;
  dash->hiding = false;
}

bool
saber_dash_toggle(struct saber_dash *dash, struct saber_output *output)
{
  if (dash == NULL) {
    return false;
  }

  if (dash->surface != NULL) {
    saber_dash_hide(dash);

    return false;
  }

  return saber_dash_show(dash, output);
}
