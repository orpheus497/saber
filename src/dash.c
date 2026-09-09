/* Script function and purpose: The Dash -- the edge-docked application grid.
Layout, cairo/pango drawing, the substring filter and its ranking, and the
xkbcommon keyboard handling that makes the search field type.

The surface is an OVERLAY layer surface anchored to config->panel.edge plus TOP
and BOTTOM, with EXCLUSIVE keyboard interactivity, and it is CREATED on show and
DESTROYED on hide rather than merely being unmapped: a layer surface holding the
seat's keyboard exclusively goes on holding it for as long as it exists.

The surface IS the dash. Unity's dash is a panel docked to the launcher's edge
over an undimmed desktop, and it is drawn here by asking the compositor for a
surface that size rather than by taking the output and painting a third of it:
an exclusive_zone of 0 then has the compositor dock it beside the panel's own
reserved column, so the two sit side by side without either measuring the
other. Everything below works in surface-local coordinates, and panel_x and
panel_width -- which dash_layout still sets, because every measurement here is
written against them -- are now simply 0 and the surface's width.

What that costs is click-away-to-dismiss: a click on the desktop lands on the
desktop. Escape, a second click on the BFB and `saberctl dash` are the ways
out, plus dash_keyboard_leave for the case where a compositor moves the
keyboard on anyway. */

#include <math.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>

#include <glib.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon.h>

#include <saber/anim.h>
#include <saber/dash.h>
#include <saber/surface.h>

/* linux/input-event-codes.h is a Linux header; the button codes the protocol
carries are stable and are spelled out rather than pulled in through a shim. */
#define DASH_BTN_LEFT 0x110

#define DASH_ICON 44
#define DASH_CELL_MIN_WIDTH 100.0
#define DASH_CELL_HEIGHT 112.0
#define DASH_PAD 20.0
#define DASH_SEARCH_HEIGHT 40.0
#define DASH_SEARCH_GAP 18.0
#define DASH_RADIUS 8.0
#define DASH_SCROLLBAR 4.0

/* Padding either side of the painted bar. Four logical pixels is a legible
width and an unhittable target; this is what makes it a pointer target. */
#define DASH_SCROLLBAR_GRAB 6.0

/* Action purpose: The scrollbar sits in a gutter of its own rather than over
the last column, so a full row of cells is never partly hidden by the thumb. */
#define DASH_GUTTER 14.0

/* The width the dash asks the compositor for: a third of the output, held
between a width that still fits three columns on a laptop and one that stops an
ultrawide handing the dash half the desktop. */
#define DASH_PANEL_DIVISOR 3.0
#define DASH_PANEL_MIN_WIDTH 420.0
#define DASH_PANEL_MAX_WIDTH 700.0

#define DASH_STRIP_HEIGHT 46.0
#define DASH_STRIP_ICON 22.0
#define DASH_STRIP_GAP 10.0 /* grid to the category strip */

/* Action purpose: A grid is only readable while the eye can find the start of
the next row. Uncapped, a 3440px output lays twenty entries out as one line. */
#define DASH_MAX_COLUMNS 7

/* Saber's configuration carries no font key and the theme is colours only, so
the dash asks fontconfig for the system sans at the sizes it wants. */
#define DASH_FONT "Sans 9.5"
#define DASH_SEARCH_FONT "Sans 12"
#define DASH_CHIP_FONT "Sans 8"

/* The freedesktop menu main categories, in the order the filter strip shows
them. The label is the category name verbatim -- the one exception is
AudioVideo, which is unreadable run together and is nothing else anywhere.

`icon` is the freedesktop name for the category and `alt` a second name to try:
an icon theme is free to inherit from one that carries only half the set, and a
strip is not worth a hole in it. The label is the last resort. */
static const struct {
  const char *key;
  const char *label;
  const char *icon;
  const char *alt;
} dash_categories[] = {
  { "AudioVideo", "Audio & Video", "applications-multimedia",
      "multimedia-player" },
  { "Development", "Development", "applications-development",
      "applications-engineering" },
  { "Education", "Education", "applications-education",
      "accessories-dictionary" },
  { "Game", "Game", "applications-games", "input-gaming" },
  { "Graphics", "Graphics", "applications-graphics", "image-x-generic" },
  { "Network", "Network", "applications-internet", "network-workgroup" },
  { "Office", "Office", "applications-office", "x-office-document" },
  { "Science", "Science", "applications-science", "applications-engineering" },
  { "Settings", "Settings", "preferences-desktop", "preferences-system" },
  { "System", "System", "applications-system", "computer" },
  { "Utility", "Utility", "applications-utilities", "applications-accessories" },
};

#define DASH_CATEGORY_COUNT ((int)G_N_ELEMENTS(dash_categories))

/* Not a main category: the bucket for an entry that names none of them. */
#define DASH_CATEGORY_OTHER DASH_CATEGORY_COUNT

/* The pseudo-filter the row opens on, and the only one that is not a bucket. */
#define DASH_CATEGORY_ALL (-1)

/* One filter in the bottom strip. `icon` is NULL for All, which is drawn rather
than looked up: no icon theme names "every category at once". The position and
width are assigned by dash_layout, the only thing that knows how wide the dash
rectangle is. */
struct dash_chip {
  int category;
  const char *label;
  const char *icon, *alt;
  double x, y, width;
};

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
  int category;
};

struct saber_dash {
  struct saber_dash_deps deps;
  struct saber_surface *surface;

  int width, height; /* the surface's logical size, which is the dash's */

  /* The output's own logical size, remembered across hides. The surface is no
  longer the output, so its size can no longer be read back as one -- and the
  width the dash asks for is a fraction of the OUTPUT, not of itself, which
  would otherwise shrink a little further every time it opened. */
  int output_width, output_height;

  double scale;

  GString *query;
  GPtrArray *entries; /* struct dash_entry *, owned */
  GPtrArray *results; /* struct dash_entry *, borrowed from entries */

  /* The filter strip: only the categories the index actually has entries for,
  always led by All. Rebuilt whenever the entries are. */
  GArray *chips; /* struct dash_chip */
  int category;  /* DASH_CATEGORY_ALL, a category index, or _OTHER */
  int chip_hovered;
  int chip_pressed;

  int selected;
  int hovered;
  int pressed;
  int scroll; /* first visible row */

  /* The dash rectangle inside the surface. The surface is the whole output, so
  this is the painted strip: panel_x is 0 on a left-hand panel and
  `width - panel_width` on a right-hand one. Kept as a pair because every
  measurement, hit test and paint below is written against them. */
  double panel_x, panel_width;

  /* A press that landed outside the strip. Held to the release so a drag that
  starts outside and ends inside does not dismiss, matching how a press on a
  cell must be released on the same cell to launch it. */
  bool pressed_outside;

  saber_dash_dismissed_cb dismissed;
  void *dismissed_user;

  /* Action purpose: The dash's own clock, driven from the surface layer's frame
  hook. Both tweens are one-shot -- neither repeats -- so the loop settles by
  itself and cannot pin a core the way an unbounded repeat did (D-036).
  `reveal` runs once on show and slides the strip in from the panel's edge;
  `hover_in`/`hover_out` fade a cell's highlight, with `fading` naming the cell
  the pointer has just left. */
  struct saber_clock *clock;
  bool revealing; /* the open transition has been started for this mapping */
  struct saber_tween reveal;
  struct saber_tween hover_in, hover_out;
  int fading;

  int columns, rows, visible_rows;
  double cell_width, cell_height;
  double grid_x, grid_y;
  double search_x, search_y, search_width;
  double strip_y;

  /* Where the pointer last was across the dash, kept so the hover can be
  re-resolved when the grid scrolls under a pointer that did not move. The
  vertical half is what a scrollbar drag is measured against. */
  double pointer_x;
  double pointer_y;

  /* The scrollbar was decoration until Phase 12: nothing outside
  dash_draw_scrollbar knew where it was, so there was no thumb to grab.
  `bar_grab` is where on the thumb it was picked up, which is what keeps the
  same point of the thumb under the pointer for the whole drag. */
  bool bar_dragging;
  double bar_grab;

  struct saber_scroll_accum scroll_accum;

  PangoFontDescription *font;
  PangoFontDescription *search_font;
  PangoFontDescription *chip_font;

  struct xkb_context *xkb;
  struct xkb_keymap *keymap;
  struct xkb_state *xkb_state;

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
factor is recovered from the surface itself rather than assumed. `alpha` is what
holds an unselected category icon back from a selected one -- a themed pixmap
cannot be retinted, so it is dimmed instead. */
static void
draw_icon(cairo_t *cr,
    cairo_surface_t *icon,
    double cx,
    double cy,
    double size,
    double alpha)
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
  cairo_paint_with_alpha(cr, alpha);
  cairo_restore(cr);
}

static void
set_source_alpha(cairo_t *cr, const struct saber_color *color, double alpha)
{
  cairo_set_source_rgba(cr, color->r, color->g, color->b, color->a * alpha);
}

/* Function purpose: The search field's magnifier. Stroked rather than looked up
so the search row reads the same whatever icon theme is installed; (cx, cy) is
the centre of the lens, not of the whole glyph. */
static void
draw_magnifier(cairo_t *cr, double cx, double cy, double radius)
{
  cairo_new_path(cr);
  cairo_arc(cr, cx, cy, radius, 0.0, 2.0 * G_PI);
  cairo_move_to(cr, cx + radius * 0.72, cy + radius * 0.72);
  cairo_line_to(cr, cx + radius * 1.7, cy + radius * 1.7);
  cairo_set_line_width(cr, 1.6);
  cairo_stroke(cr);
}

/* Three stacked rules of decreasing width -- the filter affordance at the right
of the search row, naming whichever category the bottom strip has active. */
static void
draw_filter_glyph(cairo_t *cr, double x, double y, double width)
{
  for (int i = 0; i < 3; i++) {
    double inset = i * width / 6.0;

    cairo_move_to(cr, x + inset, y + i * 4.5);
    cairo_line_to(cr, x + width - inset, y + i * 4.5);
  }

  cairo_set_line_width(cr, 1.4);
  cairo_stroke(cr);
}

/* The All filter's glyph: four squares, the strip's only entry with no
freedesktop category icon behind it. */
static void
draw_all_glyph(cairo_t *cr, double cx, double cy, double size)
{
  double cell = (size - 2.0) / 2.0;

  for (int i = 0; i < 4; i++) {
    int column = i % 2;
    int row = i / 2;

    cairo_rectangle(cr, cx - size / 2.0 + column * (cell + 2.0),
        cy - size / 2.0 + row * (cell + 2.0), cell, cell);
  }

  cairo_fill(cr);
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

/* Function purpose: A valid-UTF-8 copy of `text`, for the callers below that
walk it as UTF-8. Desktop-entry data carries no such guarantee: an id is derived
from a filename, which is an arbitrary byte string on this platform, and Exec
reaches us through g_key_file_get_value precisely when g_key_file_get_string
rejected it as malformed. The walkers document their input as required-valid and
run off the end of a truncated sequence. Validating here rather than at the
source keeps Exec byte-exact for the execvp path, which must not be repaired. */
static char *
utf8_dup(const char *text)
{
  return g_utf8_validate(text, -1, NULL) ? g_strdup(text)
                                         : g_utf8_make_valid(text, -1);
}

static char *
fold(const char *text)
{
  if (text == NULL || *text == '\0') {
    return NULL;
  }

  char *valid = utf8_dup(text);
  char *folded = g_utf8_casefold(valid, -1);

  g_free(valid);
  return folded;
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

/* Function purpose: The bucket an entry belongs to -- the FIRST name in its own
Categories list that is a main category, so a desktop file that leads with
"AudioVideo;Audio;Player" lands under Audio & Video rather than under whichever
of its keys happens to come first in our table. An entry naming no main category
at all, or carrying no Categories key, is Other. */
static int
entry_category(const struct saber_appinfo *app)
{
  if (app->categories == NULL) {
    return DASH_CATEGORY_OTHER;
  }

  for (char **name = app->categories; *name != NULL; name++) {
    for (int i = 0; i < DASH_CATEGORY_COUNT; i++) {
      if (strcmp(*name, dash_categories[i].key) == 0) {
        return i;
      }
    }
  }

  return DASH_CATEGORY_OTHER;
}

/* Function purpose: Rebuild the filter strip from what the entries actually
are. An empty category is not offered: a strip of twelve filters, nine of which
match nothing, is worse than no strip. */
static void
dash_rebuild_chips(struct saber_dash *dash)
{
  bool present[DASH_CATEGORY_COUNT + 1] = { false };

  g_array_set_size(dash->chips, 0);

  /* Nothing to filter: All on its own is a strip that does nothing. */
  if (dash->entries->len == 0) {
    return;
  }

  for (guint i = 0; i < dash->entries->len; i++) {
    const struct dash_entry *entry = g_ptr_array_index(dash->entries, i);

    present[entry->category] = true;
  }

  for (int i = DASH_CATEGORY_ALL; i <= DASH_CATEGORY_OTHER; i++) {
    if (i != DASH_CATEGORY_ALL && !present[i]) {
      continue;
    }

    struct dash_chip chip = {
      .category = i,
      .label = i == DASH_CATEGORY_ALL ? "All"
          : (i == DASH_CATEGORY_OTHER ? "Other" : dash_categories[i].label),
      .icon = i == DASH_CATEGORY_ALL ? NULL
          : (i == DASH_CATEGORY_OTHER ? "applications-other"
                                      : dash_categories[i].icon),
      .alt = i == DASH_CATEGORY_ALL || i == DASH_CATEGORY_OTHER
          ? NULL
          : dash_categories[i].alt,
    };

    g_array_append_val(dash->chips, chip);
  }
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
    char *sort_label = utf8_dup(label);

    entry->app = saber_appinfo_ref(app);
    entry->label = label;
    entry->fold_name = fold(label);
    entry->fold_generic = fold(app->generic_name);
    entry->fold_exec = fold(exec);
    entry->sort_key = g_utf8_collate_key(sort_label, -1);
    entry->category = entry_category(app);

    g_free(sort_label);
    g_free(exec);
    g_ptr_array_add(dash->entries, entry);
  }

  dash_rebuild_chips(dash);
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

  /* Action purpose: The two filters intersect. The category is checked first
  because it is one integer compare against three substring searches, and it is
  the one that usually rejects. */
  for (guint i = 0; i < dash->entries->len; i++) {
    struct dash_entry *entry = g_ptr_array_index(dash->entries, i);

    if (dash->category != DASH_CATEGORY_ALL &&
        entry->category != dash->category) {
      continue;
    }

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
  dash->fading = -1;
  dash->pressed = -1;
  dash->scroll = 0;

  dash_layout(dash);
}

/* ----------------------------------------------------------------- layout */

/* Function purpose: The width to ask the compositor for, from the OUTPUT's
width rather than the dash's own -- the surface is the dash now, so measuring it
against itself would shrink it a third every time it opened. */
static int
dash_panel_width(int output_width)
{
  double width = CLAMP((double)output_width / DASH_PANEL_DIVISOR,
      DASH_PANEL_MIN_WIDTH, DASH_PANEL_MAX_WIDTH);

  if (width > (double)output_width) {
    width = (double)output_width;
  }

  return (int)width;
}

/* Function purpose: Spread the filter strip evenly across the bottom edge of
the dash rectangle. The strip is a fixed set of small cells rather than measured
chips: at a third of the output there is no width to wrap into, so every filter
gets the same share and is drawn as an icon. */
static void
dash_layout_strip(struct saber_dash *dash)
{
  dash->strip_y = (double)dash->height - DASH_STRIP_HEIGHT;

  if (dash->chips->len == 0) {
    return;
  }

  double inner = dash->panel_width - 2.0 * DASH_PAD;

  if (inner < 1.0) {
    inner = 1.0;
  }

  double cell = inner / dash->chips->len;
  double x = dash->panel_x + DASH_PAD;

  for (guint i = 0; i < dash->chips->len; i++) {
    struct dash_chip *chip = &g_array_index(dash->chips, struct dash_chip, i);

    chip->x = x;
    chip->y = dash->strip_y;
    chip->width = cell;
    x += cell;
  }
}

static void
dash_layout(struct saber_dash *dash)
{
  int count = (int)dash->results->len;

  /* Action purpose: The surface is the output, so the dash rectangle has to be
  placed inside it rather than being the whole of it. The width is the same
  fraction of the output as before; the offset puts it against whichever edge
  carries the panel column, so a right-hand panel gets a right-hand dash.
  Everything below -- every measurement, hit test and paint -- is written
  against this pair, so placing it here is the only change the strip needs. */
  dash->panel_width = (double)dash_panel_width(dash->width);

  if (dash->panel_width < 1.0) {
    dash->panel_width = 1.0;
  }

  bool docked_right = dash->deps.config != NULL &&
      dash->deps.config->panel.edge == SABER_EDGE_RIGHT;

  dash->panel_x =
      docked_right ? (double)dash->width - dash->panel_width : 0.0;

  if (dash->panel_x < 0.0) {
    dash->panel_x = 0.0;
  }

  double inner = dash->panel_width - 2.0 * DASH_PAD;

  if (inner < 1.0) {
    inner = 1.0;
  }

  dash->search_x = dash->panel_x + DASH_PAD;
  dash->search_y = DASH_PAD;
  dash->search_width = inner;

  dash_layout_strip(dash);

  dash->cell_height = DASH_CELL_HEIGHT;
  dash->grid_x = dash->panel_x + DASH_PAD;
  dash->grid_y = dash->search_y + DASH_SEARCH_HEIGHT + DASH_SEARCH_GAP;

  /* Action purpose: The column count comes from the width the dash actually
  has, and the cells then stretch to fill it exactly -- a fixed cell width would
  leave a ragged margin down one side of a panel this narrow. */
  double available = inner - DASH_GUTTER;

  if (available < DASH_CELL_MIN_WIDTH) {
    available = DASH_CELL_MIN_WIDTH;
  }

  dash->columns = CLAMP((int)(available / DASH_CELL_MIN_WIDTH), 1,
      DASH_MAX_COLUMNS);
  dash->cell_width = available / dash->columns;

  double room = dash->strip_y - DASH_STRIP_GAP - dash->grid_y;

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

/* Function purpose: Whether a surface-local point is on the dash itself rather
than on the transparent remainder of the output. The one test that separates a
click on the dash from a click through it, so dismissal and hit testing cannot
disagree about where the dash ends. */
static bool
dash_inside_strip(const struct saber_dash *dash, double x, double y)
{
  return x >= dash->panel_x && x < dash->panel_x + dash->panel_width &&
      y >= 0.0 && y < (double)dash->height;
}

static int
dash_chip_at(const struct saber_dash *dash, double x, double y)
{
  for (guint i = 0; i < dash->chips->len; i++) {
    const struct dash_chip *chip = &g_array_index(dash->chips,
        struct dash_chip, i);

    if (x >= chip->x && x < chip->x + chip->width && y >= chip->y &&
        y < chip->y + DASH_STRIP_HEIGHT) {
      return (int)i;
    }
  }

  return -1;
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

/* Function purpose: Where the scrollbar and its thumb are, in one place, so the
painter and the hit test cannot disagree about it. False when there is nothing
to scroll, which is also when no bar is drawn. */
static bool
dash_scrollbar_geometry(const struct saber_dash *dash,
    double *x,
    double *track_y,
    double *track_h,
    double *thumb_y,
    double *thumb_h)
{
  int max = dash_max_scroll(dash);

  if (max <= 0 || dash->rows <= 0) {
    return false;
  }

  double track = dash->visible_rows * dash->cell_height;
  double thumb = track * dash->visible_rows / (double)dash->rows;

  *x = dash->panel_x + dash->panel_width - DASH_PAD - DASH_SCROLLBAR;
  *track_y = dash->grid_y;
  *track_h = track;
  *thumb_h = thumb;
  *thumb_y = dash->grid_y + (track - thumb) * dash->scroll / (double)max;

  return true;
}

static bool
dash_on_scrollbar(const struct saber_dash *dash, double x, double y)
{
  double bar_x, track_y, track_h, thumb_y, thumb_h;

  if (!dash_scrollbar_geometry(dash, &bar_x, &track_y, &track_h, &thumb_y,
          &thumb_h)) {
    return false;
  }

  return x >= bar_x - DASH_SCROLLBAR_GRAB &&
      x < bar_x + DASH_SCROLLBAR + DASH_SCROLLBAR_GRAB && y >= track_y &&
      y < track_y + track_h;
}

/* ---------------------------------------------------------------- drawing */

/* Function purpose: The name of the filter the bottom strip has active, shown
beside the filter glyph so a row of anonymous category icons still says which
one is on. */
static const char *
dash_category_label(const struct saber_dash *dash)
{
  for (guint i = 0; i < dash->chips->len; i++) {
    const struct dash_chip *chip = &g_array_index(dash->chips,
        struct dash_chip, i);

    if (chip->category == dash->category) {
      return chip->label;
    }
  }

  return "All";
}

/* Action purpose: The top row of the dash -- magnifier, then the query or its
placeholder, then the filter affordance hard against the right edge. Everything
is left-aligned from the magnifier rather than centred: this is a panel, and a
centred field in a 480px column reads as a mistake. */
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

  bool filtered = dash->category != DASH_CATEGORY_ALL;
  PangoLayout *layout = pango_cairo_create_layout(cr);

  pango_layout_set_font_description(layout, dash->chip_font);
  pango_layout_set_text(layout, dash_category_label(dash), -1);

  int filter_width, filter_height;

  pango_layout_get_pixel_size(layout, &filter_width, &filter_height);

  double glyph_width = 12.0;
  double filter_x = x + w - 12.0 - filter_width;

  set_source_alpha(cr, filtered ? &theme->accent : &theme->dim, 1.0);
  cairo_move_to(cr, filter_x, y + (h - filter_height) / 2.0);
  pango_cairo_show_layout(cr, layout);
  draw_filter_glyph(cr, filter_x - 7.0 - glyph_width, y + h / 2.0 - 4.5,
      glyph_width);

  set_source_alpha(cr, &theme->dim, 1.0);
  draw_magnifier(cr, x + 18.0, y + h / 2.0 - 1.5, 5.5);

  double text_x = x + 32.0;
  double text_room = filter_x - 7.0 - glyph_width - 10.0 - text_x;

  if (text_room < 24.0) {
    text_room = 24.0;
  }

  bool empty = dash->query->len == 0;

  pango_layout_set_font_description(layout, dash->search_font);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_START);
  pango_layout_set_width(layout, (int)(text_room * PANGO_SCALE));
  pango_layout_set_text(layout,
      empty ? "Search applications" : dash->query->str, -1);

  int text_width, text_height;

  pango_layout_get_pixel_size(layout, &text_width, &text_height);
  saber_theme_set_source(cr, empty ? &theme->dim : &theme->foreground);
  cairo_move_to(cr, text_x, y + (h - text_height) / 2.0);
  pango_cairo_show_layout(cr, layout);

  if (!empty) {
    double caret = text_x + MIN((double)text_width, text_room) + 2.0;

    saber_theme_set_source(cr, &theme->accent);
    cairo_rectangle(cr, caret, y + (h - text_height) / 2.0, 1.5, text_height);
    cairo_fill(cr);
  }

  g_object_unref(layout);
}

/* Action purpose: The category strip along the bottom edge. The selected cell
is the accent role at the same fill and stroke weights the selected grid cell
uses, so the two selections read as one idea. Every colour here is a theme role
-- there are no literals to retint. */
static void
dash_draw_strip(struct saber_dash *dash, cairo_t *cr)
{
  const struct saber_theme *theme = dash->deps.theme;

  set_source_alpha(cr, &theme->dim, 0.35);
  cairo_set_line_width(cr, 1.0);
  cairo_move_to(cr, dash->panel_x + DASH_PAD, dash->strip_y + 0.5);
  cairo_line_to(cr, dash->panel_x + dash->panel_width - DASH_PAD,
      dash->strip_y + 0.5);
  cairo_stroke(cr);

  if (dash->chips->len == 0) {
    return;
  }

  int pixels = (int)lround(DASH_STRIP_ICON * dash->scale);
  PangoLayout *layout = pango_cairo_create_layout(cr);

  pango_layout_set_font_description(layout, dash->chip_font);
  pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

  for (guint i = 0; i < dash->chips->len; i++) {
    const struct dash_chip *chip = &g_array_index(dash->chips,
        struct dash_chip, i);
    bool selected = chip->category == dash->category;
    bool hovered = (int)i == dash->chip_hovered;
    double cy = chip->y + DASH_STRIP_HEIGHT / 2.0 + 1.0;

    if (selected || hovered) {
      rounded_rect(cr, chip->x + 2.0, chip->y + 5.0, chip->width - 4.0,
          DASH_STRIP_HEIGHT - 9.0, DASH_RADIUS);
      set_source_alpha(cr, &theme->accent,
          (int)i == dash->chip_pressed ? 0.55 : (selected ? 0.38 : 0.18));
      cairo_fill_preserve(cr);
      set_source_alpha(cr, &theme->accent, selected ? 1.0 : 0.5);
      cairo_set_line_width(cr, 1.5);
      cairo_stroke(cr);
    }

    cairo_surface_t *icon = chip->icon != NULL
        ? saber_icons_lookup(dash->deps.icons, chip->icon, pixels)
        : NULL;

    if (icon == NULL && chip->alt != NULL) {
      icon = saber_icons_lookup(dash->deps.icons, chip->alt, pixels);
    }

    if (chip->icon == NULL) {
      saber_theme_set_source(cr, selected ? &theme->foreground : &theme->dim);
      draw_all_glyph(cr, chip->x + chip->width / 2.0, cy, DASH_STRIP_ICON - 6.0);
    } else if (icon != NULL) {
      draw_icon(cr, icon, chip->x + chip->width / 2.0, cy, DASH_STRIP_ICON,
          selected ? 1.0 : 0.62);
    } else {
      /* No such name in the icon theme: the label, cut to the cell, so the
      filter is still nameable rather than a gap in the strip. */
      int text_width, text_height;

      pango_layout_set_width(layout, (int)((chip->width - 4.0) * PANGO_SCALE));
      pango_layout_set_text(layout, chip->label, -1);
      pango_layout_get_pixel_size(layout, &text_width, &text_height);
      saber_theme_set_source(cr, selected ? &theme->foreground : &theme->dim);
      cairo_move_to(cr, chip->x + (chip->width - text_width) / 2.0,
          cy - text_height / 2.0);
      pango_cairo_show_layout(cr, layout);
    }
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

  /* Action purpose: The highlight is a fade rather than a switch. `hover` is 1
  for the cell the pointer is on, whatever the incoming tween has reached, and
  the outgoing value for the cell it has just left -- so sweeping across the
  grid leaves a trail that settles instead of a highlight that snaps from cell
  to cell. The keyboard cursor is deliberately NOT faded: it moves one cell per
  keypress and a trail behind it would read as lag. */
  double hover = 0.0;

  if (index == dash->hovered) {
    hover = saber_tween_value(&dash->hover_in);
  } else if (index == dash->fading) {
    hover = saber_tween_value(&dash->hover_out);
  }

  bool selected = index == dash->selected;

  if (selected || hover > 0.0) {
    double fill = index == dash->pressed ? 0.55
        : selected                       ? 0.38
                                         : 0.18 * hover;
    double edge = selected ? 1.0 : 0.5 * hover;

    rounded_rect(cr, x + 4.0, y + 4.0, w - 8.0, h - 8.0, DASH_RADIUS);
    set_source_alpha(cr, &theme->accent, fill);
    cairo_fill_preserve(cr);

    set_source_alpha(cr, &theme->accent, edge);
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
    draw_icon(cr, icon, x + w / 2.0, icon_cy, DASH_ICON, 1.0);
  } else {
    /* The initial, so an entry whose icon resolves to nothing still reads as a
    distinct cell rather than as a gap in the grid. */
    char initial[8] = { 0 };

    /* Action purpose: g_utf8_strncpy advances with g_utf8_next_char, which
    trusts the lead byte's declared length -- on a truncated final sequence it
    reads past the terminator and copies whatever follows. entry->label is
    app->name or the filename-derived app->id, and only the former is validated
    by glib on the way in, so the label reaching here is not guaranteed valid.
    The file's own note at the top of fold() documents this hazard; it was not
    applied on this path. */
    if (g_utf8_validate(entry->label, -1, NULL)) {
      g_utf8_strncpy(initial, entry->label, 1);
      *initial = (char)g_ascii_toupper(*initial);
    }

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
  double x, track_y, track_h, thumb_y, thumb_h;

  if (!dash_scrollbar_geometry(dash, &x, &track_y, &track_h, &thumb_y,
          &thumb_h)) {
    return;
  }

  const struct saber_theme *theme = dash->deps.theme;

  set_source_alpha(cr, &theme->dim, 0.5);
  rounded_rect(cr, x, track_y, DASH_SCROLLBAR, track_h, DASH_SCROLLBAR / 2.0);
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

  /* Action purpose: Clear the whole surface to nothing first. SOURCE, not OVER:
  the buffer is recycled, so a translucent paint over a stale frame would
  accumulate into something progressively more opaque. */
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
  cairo_paint(cr);

  /* Action purpose: The open transition -- the strip slides in from the edge it
  is docked against and fades up as it arrives. Applied as a translation of
  everything below rather than as a per-element offset, so nothing else in this
  function has to know the dash animates at all.

  The clear above happens BEFORE the translation, so the part of the surface the
  strip has not reached yet stays transparent rather than being dragged along
  with it. */
  double reveal = saber_tween_value(&dash->reveal);

  if (reveal < 1.0) {
    bool docked_right = dash->deps.config != NULL &&
        dash->deps.config->panel.edge == SABER_EDGE_RIGHT;
    double slide = (1.0 - reveal) * dash->panel_width;

    /* No save/restore: the context is created fresh for every frame and
    destroyed at the end of it, and this function has early returns that a save
    here would leave unbalanced. */
    cairo_translate(cr, docked_right ? slide : -slide, 0.0);
  }

  /* Action purpose: The dash rectangle itself: a palette fill, never a blur.
  hikari advertises no blur protocol and a client cannot read the screen behind
  itself, so opacity is the substitute -- and it is laid down as ONE fill at
  exactly the configured alpha rather than as several translucent passes.

  Passes compound. Three of them turned a configured 0.92 into 0.997 on the
  way to the screen, so the number in the file was not the number the compositor
  blended with, and no setting could reach a true 1.0 however it was written.
  A single fill under SOURCE writes the alpha as asked: 1.0 means nothing behind
  the dash bleeds through, and every value below it is exactly the fraction of
  the desktop the user chose to keep.

  The operator is still SOURCE from the clear above, which is what makes the
  write exact -- OVER here would blend against the alpha already in the buffer
  and put the compounding straight back. */
  cairo_rectangle(cr, dash->panel_x, 0.0, dash->panel_width, (double)height);
  cairo_set_source_rgba(cr, theme->overlay.r, theme->overlay.g,
      theme->overlay.b, theme->overlay_opacity);
  cairo_fill_preserve(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  /* The rectangle is the surface now, so this no longer keeps paint off a
  desktop beside it -- it keeps an over-long label or an over-wide cell inside
  the dash's own edges. */
  cairo_save(cr);
  cairo_clip(cr);

  /* The inner edge, so the dash reads as a panel with a boundary rather than as
  a darkened region of the wallpaper. Which side of the surface that is follows
  the panel's edge, not panel_x: the dash starts at 0 whichever way it is
  docked, and a right-hand dash meets the desktop on its left. */
  bool docked_right = dash->deps.config != NULL &&
      dash->deps.config->panel.edge == SABER_EDGE_RIGHT;
  double edge = docked_right ? dash->panel_x + 0.5
                             : dash->panel_x + dash->panel_width - 0.5;

  set_source_alpha(cr, &theme->dim, 0.5);
  cairo_set_line_width(cr, 1.0);
  cairo_move_to(cr, edge, 0.0);
  cairo_line_to(cr, edge, (double)height);
  cairo_stroke(cr);

  dash_draw_search(dash, cr);
  dash_draw_strip(dash, cr);

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
    pango_layout_set_width(layout,
        (int)((dash->panel_width - 2.0 * DASH_PAD) * PANGO_SCALE));
    pango_layout_set_height(layout, 0);
    pango_layout_set_text(layout, "No matching applications", -1);
    saber_theme_set_source(cr, &theme->dim);
    cairo_move_to(cr, dash->panel_x + DASH_PAD, dash->grid_y + 24.0);
    pango_cairo_show_layout(cr, layout);
    g_object_unref(layout);
    cairo_restore(cr);

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
  cairo_restore(cr);
}

static int
dash_anim_ms(const struct saber_dash *dash);

static void
dash_damage(struct saber_dash *dash);

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

  /* Action purpose: The open transition begins on the first configure, not at
  saber_dash_show: the strip has no width to slide in from until the compositor
  has answered, and starting the tween before that would run most of it against
  a zero-width rectangle. `revealing` keeps it to the FIRST configure, so a
  later resize does not replay it. */
  if (!dash->revealing) {
    dash->revealing = true;
    saber_tween_start(&dash->reveal, 0.0, 1.0, dash_anim_ms(dash),
        SABER_EASE_OUT_CUBIC, saber_clock_now(dash->clock));
    dash_damage(dash);
  }
}

static void
dash_surface_closed(void *data, struct saber_surface *surface)
{
  (void)surface;

  saber_dash_hide(data);
}

/* Function purpose: Advance the dash's clock on the compositor's frame timing.

Same contract as the panel's: damaging from here asks for the next frame,
returning without damaging lets the loop stop. Every tween on this clock is
one-shot, so it always stops. */
/* The configured duration, shared by both of the dash's tweens. 0 disables
animation, which anim.c handles by landing a zero-length tween on its target
without ever running it. */
static int
dash_anim_ms(const struct saber_dash *dash)
{
  return dash->deps.config != NULL ? dash->deps.config->panel.animation_ms : 0;
}

static void
dash_frame(void *data, uint32_t time)
{
  struct saber_dash *dash = data;

  if (dash->surface == NULL || dash->surface->closed) {
    return;
  }

  if (saber_clock_advance(dash->clock, saber_clock_stamp(dash->clock, time))) {
    saber_surface_damage(dash->surface);
  }
}

static const struct saber_surface_listener dash_surface_listener = {
  .configure = dash_configure,
  .frame = dash_frame,
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

/* A launch waiting on its activation token. The model and match are held
directly rather than the dash, because the reply is asynchronous and the dash it
came from may have been hidden -- and could in principle be destroyed -- before
it arrives; these two outlive the dash by construction, being borrowed
dependencies of the whole application. */
struct dash_launch {
  struct saber_model *model;
  struct saber_match *match;
  struct saber_appinfo *app; /* owned: one reference, released here */
};

static void
dash_launch_with_token(const char *token, void *user);

static void
dash_launch(struct saber_dash *dash, int index)
{
  if (index < 0 || index >= (int)dash->results->len) {
    return;
  }

  const struct dash_entry *entry = g_ptr_array_index(dash->results, index);
  struct saber_appinfo *app = saber_appinfo_ref(entry->app);

  /* Action purpose: Ask for an activation token first, and start the
  application from the answer. The dash is the primary launcher and used to pass
  no token at all, so everything opened from it arrived unfocused and had to be
  clicked -- which is the opposite of what a launcher is for. The request is
  answered on the compositor's reply or on a one-second timeout, and a NULL
  token still launches. */
  struct dash_launch *pending = g_new0(struct dash_launch, 1);

  pending->model = dash->deps.model;
  pending->match = dash->deps.match;
  pending->app = app;

  /* Dismissed before the token arrives, not after: the desktop is uncovered the
  moment the click lands rather than a round trip later. */
  struct wl_surface *surface =
      dash->surface != NULL ? dash->surface->wl_surface : NULL;

  saber_display_request_activation(dash->deps.display, surface, app->id,
      dash_launch_with_token, pending);

  saber_dash_hide(dash);

  return;
}

/* Function purpose: Finish a dash launch once the activation token is in hand.
Takes ownership of the reference held for it, so it runs exactly once whichever
way the request was answered. */
static void
dash_launch_with_token(const char *token, void *user)
{
  struct dash_launch *pending = user;
  struct saber_model *model = pending->model;
  struct saber_match *match = pending->match;
  struct saber_appinfo *app = pending->app;

  g_free(pending);

  if (!saber_appinfo_launch(app, NULL, NULL, token)) {
    g_warning("saber: failed to launch '%s'", app->id);
    saber_appinfo_unref(app);

    return;
  }

  /* Action purpose: The launch window is what binds the new window to its tile
  -- neither foreign-toplevel protocol carries a pid, so a launch this process
  did not record is a window with no application. The model's form notes it
  through match.c and throbs the tile; match.c alone is the fallback when the
  dash was built without one. */
  if (model != NULL) {
    saber_model_note_launch(model, app->id);
  } else if (match != NULL) {
    saber_match_note_launch(match, app->id);
  }

  saber_appinfo_unref(app);
}

/* ----------------------------------------------------------------- pointer */

/* Function purpose: One place for both hover states, because the pointer is
over at most one of the grid and the filter row and each has to clear the
other's highlight as it leaves it. */
static void
dash_set_hover(struct saber_dash *dash, double x, double y)
{
  dash->pointer_x = x;
  dash->pointer_y = y;

  int cell = dash_cell_at(dash, x, y);
  int chip = cell >= 0 ? -1 : dash_chip_at(dash, x, y);

  if (dash->hovered == cell && dash->chip_hovered == chip) {
    return;
  }

  /* Action purpose: A cross-fade, the panel's shape: the cell being left fades
  out from wherever the incoming fade had reached, so a pointer swept along the
  grid leaves a trail that settles rather than a highlight that snaps. */
  int64_t now = saber_clock_now(dash->clock);
  int duration = dash_anim_ms(dash);

  dash->fading = dash->hovered;

  if (dash->fading >= 0) {
    saber_tween_start(&dash->hover_out, saber_tween_value(&dash->hover_in), 0.0,
        duration, SABER_EASE_OUT_CUBIC, now);
  }

  if (cell >= 0) {
    saber_tween_start(&dash->hover_in, 0.0, 1.0, duration, SABER_EASE_OUT_CUBIC,
        now);
  } else {
    saber_tween_stop(&dash->hover_in);
    dash->hover_in.value = 0.0;
  }

  dash->hovered = cell;
  dash->chip_hovered = chip;
  dash_damage(dash);
}

static void
dash_clear_hover(struct saber_dash *dash)
{
  if (dash->hovered < 0 && dash->chip_hovered < 0) {
    return;
  }

  dash->hovered = -1;
  dash->chip_hovered = -1;
  dash_damage(dash);
}

/* Function purpose: The one way the grid's offset changes. Clamps, re-resolves
the hover -- the cells moved under a pointer that did not -- and repaints. */
static void
dash_scroll_to(struct saber_dash *dash, int scroll)
{
  int next = CLAMP(scroll, 0, dash_max_scroll(dash));

  if (next == dash->scroll) {
    return;
  }

  dash->scroll = next;
  dash_set_hover(dash, dash->pointer_x, dash->pointer_y);
  dash_damage(dash);
}

/* Function purpose: Take a press on the scrollbar. Returns true when the press
belonged to the bar, which is what keeps it out of the cell and dismissal paths
below. */
static bool
dash_scrollbar_press(struct saber_dash *dash, double x, double y)
{
  double bar_x, track_y, track_h, thumb_y, thumb_h;

  if (!dash_scrollbar_geometry(dash, &bar_x, &track_y, &track_h, &thumb_y,
          &thumb_h) ||
      !dash_on_scrollbar(dash, x, y)) {
    return false;
  }

  dash->pressed = -1;
  dash->chip_pressed = -1;

  if (y >= thumb_y && y < thumb_y + thumb_h) {
    dash->bar_dragging = true;
    dash->bar_grab = y - thumb_y;

    return true;
  }

  /* Bare track: page toward the click, the way every other scrollbar does. No
  drag is started, because the thumb has just moved out from under the pointer
  and there is no grab point left that would not make it jump. */
  dash_scroll_to(dash,
      dash->scroll + (y < thumb_y ? -dash->visible_rows : dash->visible_rows));

  return true;
}

static void
dash_scrollbar_drag(struct saber_dash *dash, double y)
{
  double bar_x, track_y, track_h, thumb_y, thumb_h;

  if (!dash_scrollbar_geometry(dash, &bar_x, &track_y, &track_h, &thumb_y,
          &thumb_h)) {
    dash->bar_dragging = false;

    return;
  }

  double span = track_h - thumb_h;

  if (span <= 0.0) {
    return;
  }

  /* The point of the thumb the drag started on stays under the pointer, so the
  thumb does not snap its centre to the cursor on the first motion event. */
  double top = y - dash->bar_grab - track_y;

  dash_scroll_to(dash, (int)lround(top * dash_max_scroll(dash) / span));
}

/* Function purpose: Switch the filter and re-run both halves of it. The query
is deliberately kept: a category is a narrowing of what is already on screen,
not a fresh start. */
static void
dash_set_category(struct saber_dash *dash, int category)
{
  if (dash->category == category) {
    return;
  }

  dash->category = category;
  dash_filter(dash);
  dash_damage(dash);
}

/* Function purpose: Move the filter along the row by `delta`, wrapping. This is
what Tab and Shift+Tab drive -- the row has to be reachable without a pointer,
and Left/Right belong to the grid, where they move the selection one cell. */
static void
dash_cycle_category(struct saber_dash *dash, int delta)
{
  int count = (int)dash->chips->len;

  if (count == 0) {
    return;
  }

  int current = 0;

  for (int i = 0; i < count; i++) {
    if (g_array_index(dash->chips, struct dash_chip, i).category ==
        dash->category) {
      current = i;
      break;
    }
  }

  int next = ((current + delta) % count + count) % count;

  dash_set_category(dash,
      g_array_index(dash->chips, struct dash_chip, next).category);
}

/* The dash draws exactly one surface, so ownership is an identity test. Until
display.c routed by surface this check lived only in dash_pointer_enter, and
motion and button ran on panel-local coordinates as though they were the
dash's -- which is how a click on the panel came to be read as a click inside
the dash rectangle, and why the dash would not close. */
static bool
dash_pointer_owns(void *data, struct wl_surface *surface)
{
  struct saber_dash *dash = data;

  return dash->surface != NULL && dash->surface->wl_surface == surface;
}

static void
dash_pointer_enter(void *data, struct wl_surface *surface, double x, double y)
{
  struct saber_dash *dash = data;

  if (dash->surface == NULL || dash->surface->wl_surface != surface) {
    return;
  }

  saber_display_set_cursor(dash->deps.display, "left_ptr");
  dash_set_hover(dash, x, y);
}

static void
dash_pointer_leave(void *data, struct wl_surface *surface)
{
  (void)surface;

  struct saber_dash *dash = data;

  /* Off the surface is off the dash. Parking the position out of range keeps a
  scroll or a re-layout from resolving a hover against where the pointer was
  when it left. */
  dash->pointer_x = -1.0;
  dash->bar_dragging = false;
  saber_scroll_reset(&dash->scroll_accum);
  dash_clear_hover(dash);
}

static void
dash_pointer_motion(void *data, uint32_t time, double x, double y)
{
  (void)time;

  struct saber_dash *dash = data;

  if (dash->bar_dragging) {
    dash->pointer_x = x;
    dash->pointer_y = y;
    dash_scrollbar_drag(dash, y);

    return;
  }

  dash_set_hover(dash, x, y);
}

static void
dash_pointer_button(void *data,
    uint32_t time,
    uint32_t button,
    uint32_t state)
{
  (void)time;

  struct saber_dash *dash = data;

  if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
    /* Action purpose: Outside the strip is a dismissal, latched here and acted
    on at the release. The surface covers the output, so this test is exact --
    it is the whole reason the surface is not merely the strip.

    Every button is tested here, not only the left one, and the test comes
    before any filtering by button. The surface covers the output, so a press
    this handler declines is not handed to whatever is underneath -- it is lost.
    Filtering first swallowed every right and middle click outside the dash. */
    if (!dash_inside_strip(dash, dash->pointer_x, dash->pointer_y)) {
      dash->pressed_outside = true;

      return;
    }

    if (button != DASH_BTN_LEFT) {
      return;
    }

    if (dash_scrollbar_press(dash, dash->pointer_x, dash->pointer_y)) {
      return;
    }

    dash->pressed = dash->hovered;
    dash->chip_pressed = dash->chip_hovered;
    dash->selected = dash->hovered >= 0 ? dash->hovered : dash->selected;
    dash_damage(dash);

    return;
  }

  /* Action purpose: Click away to dismiss, and hand the click on rather than
  spending it. The owner is told where it landed in output-local coordinates, so
  a click on a panel tile closes the dash AND activates that tile in one press,
  and a click on the button that opened the dash closes it -- which is what
  makes that button a toggle. A release that has wandered back inside the strip
  is not a dismissal. */
  if (dash->pressed_outside) {
    dash->pressed_outside = false;

    if (dash_inside_strip(dash, dash->pointer_x, dash->pointer_y)) {
      return;
    }

    struct saber_output *output =
        dash->surface != NULL ? dash->surface->output : NULL;
    double x = dash->pointer_x;
    double y = dash->pointer_y;
    saber_dash_dismissed_cb dismissed = dash->dismissed;
    void *user = dash->dismissed_user;

    saber_dash_hide(dash);

    if (dismissed != NULL) {
      dismissed(user, output, x, y, button);
    }

    return;
  }

  /* Past the dismissal, only the left button acts on the dash's contents. */
  if (button != DASH_BTN_LEFT) {
    return;
  }

  if (dash->bar_dragging) {
    dash->bar_dragging = false;

    return;
  }

  int index = dash->pressed;
  int chip = dash->chip_pressed;

  dash->pressed = -1;
  dash->chip_pressed = -1;

  if (chip >= 0) {
    if (chip == dash->chip_hovered) {
      dash_set_category(dash,
          g_array_index(dash->chips, struct dash_chip, chip).category);
    } else {
      dash_damage(dash);
    }

    return;
  }

  /* A miss inside the strip -- the search field, the gap between cells -- is
  not a dismissal and changes nothing. Dismissal is handled above, on the press
  that landed outside. */
  if (index < 0) {
    return;
  }

  if (index == dash->hovered) {
    dash_launch(dash, index);
  } else {
    dash_damage(dash);
  }
}

/* Action purpose: A row per notch of travel, not a row per event. The bare sign
this replaced meant one two-finger swipe -- dozens of fractional axis events --
scrolled the grid dozens of rows, straight to the bottom of the list. */
static void
dash_pointer_axis(void *data, uint32_t time, uint32_t axis, double value)
{
  (void)time;

  struct saber_dash *dash = data;

  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
    return;
  }

  int steps = saber_scroll_steps(&dash->scroll_accum,
      saber_scroll_delta(&dash->scroll_accum, axis, value), 120);

  if (steps != 0) {
    dash_scroll_to(dash, dash->scroll + steps);
  }
}

static void
dash_pointer_axis_value120(void *data, uint32_t axis, int32_t value120)
{
  struct saber_dash *dash = data;

  saber_scroll_detail(&dash->scroll_accum, axis, value120);
}

static void
dash_pointer_axis_stop(void *data, uint32_t time, uint32_t axis)
{
  (void)time;

  struct saber_dash *dash = data;

  /* Action purpose: Only the axis this listener acts on ends its gesture here.
  dash_pointer_axis returns on the horizontal axis before taking a delta, so a
  reset answering a horizontal stop discarded the vertical remainder a
  half-finished gesture had earned and made the next nudge re-earn a whole
  notch. Resetting on the axis that did stop is correct: the gesture is over. */
  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
    return;
  }

  saber_scroll_reset(&dash->scroll_accum);
}

static const struct saber_pointer_listener dash_pointer_listener = {
  .owns = dash_pointer_owns,
  .enter = dash_pointer_enter,
  .leave = dash_pointer_leave,
  .motion = dash_pointer_motion,
  .button = dash_pointer_button,
  .axis = dash_pointer_axis,
  .axis_value120 = dash_pointer_axis_value120,
  .axis_stop = dash_pointer_axis_stop,
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

  /* Action purpose: Tab walks the category row and Shift+Tab walks it back --
  the row's only keyboard reach, chosen over Left/Right at the grid edge
  because the grid wraps rows there and "one more Right" is how a user crosses
  from the end of a row to the start of the next, not how they leave the grid.
  A layout that reports Shift+Tab as ISO_Left_Tab is the common case; one that
  reports a plain Tab with Shift down is not, so both are read. */
  case XKB_KEY_ISO_Left_Tab:
    dash_cycle_category(dash, -1);

    return;

  case XKB_KEY_Tab:
    dash_cycle_category(dash,
        xkb_state_mod_name_is_active(dash->xkb_state, XKB_MOD_NAME_SHIFT,
            XKB_STATE_MODS_EFFECTIVE) > 0
            ? -1
            : 1);

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

/* Function purpose: Note that the keyboard has gone elsewhere, and do nothing
about it.

This used to dismiss the dash. That is wrong under a focus-follows-mouse
compositor, where a keyboard leave is routine rather than exceptional: moving
the pointer to another monitor produces one, and the dash would vanish for no
reason the user could connect to what they did. Focus comes back when the
pointer does, because the surface asks for keyboard interactivity and the
compositor restores it on the way in.

The case the old behaviour guarded -- a dash left on screen with a search field
that no longer types -- is covered properly now: a press anywhere outside the
strip dismisses it, and the surface covers the output, so there is always
somewhere to click. Escape, the button that opened it, and `saberctl dash`
remain. */
static void
dash_keyboard_leave(void *data, struct wl_surface *surface)
{
  (void)data;
  (void)surface;
}

static const struct saber_keyboard_listener dash_keyboard_listener = {
  .keymap = dash_keymap,
  .key = dash_key,
  .modifiers = dash_modifiers,
  .leave = dash_keyboard_leave,
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
  dash->chips = g_array_new(FALSE, FALSE, sizeof(struct dash_chip));
  dash->category = DASH_CATEGORY_ALL;
  dash->chip_hovered = -1;
  dash->chip_pressed = -1;
  dash->selected = -1;
  dash->hovered = -1;
  dash->pressed = -1;
  dash->pointer_x = -1.0;
  dash->pointer_y = -1.0;
  dash->scale = 1.0;
  dash->columns = 1;
  dash->visible_rows = 1;

  dash->clock = saber_clock_create();
  saber_tween_init(&dash->reveal, 1.0);
  saber_tween_init(&dash->hover_in, 0.0);
  saber_tween_init(&dash->hover_out, 0.0);
  saber_clock_add(dash->clock, &dash->reveal);
  saber_clock_add(dash->clock, &dash->hover_in);
  saber_clock_add(dash->clock, &dash->hover_out);

  dash->font = pango_font_description_from_string(DASH_FONT);
  dash->search_font = pango_font_description_from_string(DASH_SEARCH_FONT);
  dash->chip_font = pango_font_description_from_string(DASH_CHIP_FONT);

  dash->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

  /* Action purpose: wl_keyboard.keymap fires once, when the seat's keyboard is
  bound -- long before any dash exists. display.c caches it and replays it when
  this listener registers, but a seat that has no keyboard at that moment sends
  nothing to cache, so the compiled default layout is still what makes the
  search field type at all until a real keymap arrives. */
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
  g_array_unref(dash->chips);
  g_string_free(dash->query, TRUE);

  saber_clock_destroy(dash->clock);

  pango_font_description_free(dash->font);
  pango_font_description_free(dash->search_font);
  pango_font_description_free(dash->chip_font);

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
  dash->category = DASH_CATEGORY_ALL;
  dash_build_entries(dash);

  /* The output's own size, so the first frame is laid out correctly rather than
  being relaid on the configure that follows it. */
  if (output != NULL && output->scale > 0) {
    dash->output_width = output->width / output->scale;
    dash->output_height = output->height / output->scale;
  }

  if (dash->output_width <= 0 || dash->output_height <= 0) {
    dash->output_width = 1280;
    dash->output_height = 720;
  }

  dash->width = dash->output_width;
  dash->height = dash->output_height;

  dash_filter(dash);

  /* Action purpose: The surface is the whole output; the dash is the strip
  painted inside it, and dash_layout places that strip against the panel's edge.

  The surface has to be the output because of how this compositor routes the
  pointer. A layer surface that asks for keyboard input is handed every pointer
  coordinate on its output, but the surface-local coordinates that come with it
  are only meaningful where the hit test actually succeeded -- outside the
  surface they are indeterminate. A docked surface therefore cannot tell a click
  on itself from a click on the panel or on a window, which is what left the
  dash with no working click-away and no way to be dismissed by the button that
  opened it. Covering the output makes every coordinate a real hit, so the strip
  test in dash_pointer_button is exact.

  Everything outside the strip is left fully transparent, so this is invisible:
  dash_render clears the surface and fills only panel_x..panel_width.
  exclusive_zone stays 0 -- the dash reserves nothing and overlays the panel's
  reserved column rather than displacing it. */
  struct saber_surface_params params = {
    .output = output,
    .layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY,
    .anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT,
    /* Action purpose: EXCLUSIVE, not ON_DEMAND. The dash is opened from a
    keybinding as often as from the button -- `saberctl dash` reaches it with no
    pointer interaction at all -- and ON_DEMAND only promises focus once the
    user interacts with the surface. A compositor that honours the distinction
    would hand back a dash whose search field never types and whose Escape does
    not close it. */
    .keyboard_interactivity =
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE,
    .exclusive_zone = 0,
    .width = 0,  /* all four anchors: the compositor sizes it to the output */
    .height = 0,
    .layer_namespace = "saber-dash",
  };

  dash->surface = saber_surface_create(dash->deps.display, &params,
      &dash_surface_listener, dash);

  if (dash->surface == NULL) {
    g_warning("saber: could not create the dash surface");

    return false;
  }

  /* Action purpose: Pointer input is registered, not seized. display.c routes
  by surface, so the panel keeps receiving its own clicks while the dash is up
  -- which is what lets a second click on the BFB close it. The keyboard is
  still a single slot and is genuinely exclusive while the dash is modal, so
  that one keeps the save-and-restore. */
  dash->prev_keyboard = dash->deps.display->keyboard_listener;
  dash->prev_keyboard_data = dash->deps.display->keyboard_data;

  saber_display_add_pointer_listener(dash->deps.display, &dash_pointer_listener,
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
    saber_display_remove_pointer_listener(dash->deps.display,
        &dash_pointer_listener, dash);
    saber_display_set_keyboard_listener(dash->deps.display, dash->prev_keyboard,
        dash->prev_keyboard_data);
    dash->listening = false;
  }

  saber_display_flush(dash->deps.display);

  g_ptr_array_set_size(dash->results, 0);
  g_ptr_array_set_size(dash->entries, 0);
  g_array_set_size(dash->chips, 0);
  g_string_truncate(dash->query, 0);

  dash->category = DASH_CATEGORY_ALL;
  dash->chip_hovered = -1;
  dash->chip_pressed = -1;
  dash->selected = -1;
  dash->hovered = -1;
  dash->pressed = -1;
  dash->pointer_x = -1.0;
  dash->pointer_y = -1.0;
  dash->bar_dragging = false;
  dash->pressed_outside = false;
  dash->revealing = false;
  dash->fading = -1;
  saber_tween_stop(&dash->reveal);
  dash->reveal.value = 1.0;
  saber_tween_stop(&dash->hover_in);
  dash->hover_in.value = 0.0;
  saber_tween_stop(&dash->hover_out);
  dash->hover_out.value = 0.0;
  saber_clock_reset(dash->clock);
  dash->scroll = 0;
  saber_scroll_reset(&dash->scroll_accum);
  dash->hiding = false;
}

void
saber_dash_set_dismissed(struct saber_dash *dash,
    saber_dash_dismissed_cb func,
    void *user)
{
  if (dash == NULL) {
    return;
  }

  dash->dismissed = func;
  dash->dismissed_user = user;
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
