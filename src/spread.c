/* Script function and purpose: The window spread -- layout, cairo/pango drawing
and input for the full-screen window grid.

There are NO THUMBNAILS here and none are coming. hikari creates an OUTPUT
image-capture source and never a foreign-toplevel one, and wlr-screencopy has no
per-window request, so a window's contents are not obtainable by any client on
this compositor. An icon-and-title grid is the design, not a placeholder for one
(BLUEPRINT.md 4.1). */

#include <math.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>

#include <glib.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon.h>

#include <saber/anim.h>
#include <saber/spread.h>
#include <saber/surface.h>

/* linux/input-event-codes.h is a Linux header; the button codes the protocol
carries are stable and are spelled out rather than pulled in through a shim. */
#define SPREAD_BTN_LEFT 0x110
#define SPREAD_BTN_MIDDLE 0x112

#define SPREAD_ICON 56
#define SPREAD_CELL_WIDTH 236.0
#define SPREAD_CELL_HEIGHT 158.0
#define SPREAD_MARGIN 64.0
#define SPREAD_HEADER 44.0
#define SPREAD_RADIUS 10.0
#define SPREAD_CLOSE 11.0
#define SPREAD_SCROLLBAR 4.0

/* Padding either side of the painted bar. Four logical pixels is a legible
width and an unhittable target; this is what makes it a pointer target. */
#define SPREAD_SCROLLBAR_GRAB 6.0

/* How far the grid rises into place on open, in logical pixels. */
#define SPREAD_REVEAL_RISE 24.0

/* Uncapped, a wide output lays a dozen windows out as one unreadable line. */
#define SPREAD_MAX_COLUMNS 5

#define SPREAD_FONT "Sans 9.5"
#define SPREAD_HEADER_FONT "Sans 12"

struct spread_cell {
  struct saber_toplevel *toplevel;

  /* The window's identity when the cell was built. Compared as well as the
  pointer, because the pointer alone cannot tell a live window from a different
  one allocated at the same address after the first closed. */
  uint64_t toplevel_id;
  char *title;
  char *subtitle;  /* the application, so two same-named windows still differ */
  char *icon_name; /* resolved once per rebuild; looked up per frame */
  bool minimized;
  bool activated;

  double x, y; /* filled in by spread_place, logical pixels */
  bool placed;
};

struct saber_spread {
  struct saber_spread_deps deps;
  struct saber_surface *surface;

  int width, height;
  double scale;

  char *filter; /* app_id, case-folded; NULL for every window */
  GPtrArray *cells;

  int selected;
  int hovered;
  int fading;

  /* The spread's clock. Both tweens are one-shot -- `reveal` fades the grid up
  once on open, `hover_in`/`hover_out` cross-fade a cell -- so the loop settles
  by itself and cannot pin a core the way an unbounded repeat did (D-036). */
  struct saber_clock *clock;
  bool revealing;
  struct saber_tween reveal;
  struct saber_tween hover_in, hover_out;
  int pressed;
  bool on_close; /* the pointer is over the hovered cell's close disc */
  int scroll;
  struct saber_scroll_accum scroll_accum;

  /* Action purpose: The scrollbar track sits in the margin beside the grid, so
  it falls inside no cell -- spread_cell_at answers -1 there, and until Phase 12
  that made a press on the bar indistinguishable from a press on the backdrop,
  which dismisses the spread. These say the press was the bar's, so the
  dismissal path never sees it. `bar_grab` keeps the same point of the thumb
  under the pointer for the length of the drag. */
  double pointer_x, pointer_y;
  bool bar_pressed;
  bool bar_dragging;
  double bar_grab;

  int columns, rows, visible_rows;
  double cell_width, cell_height;
  double grid_x, grid_y;

  PangoFontDescription *font;
  PangoFontDescription *header_font;

  struct xkb_context *xkb;
  struct xkb_keymap *keymap;
  struct xkb_state *xkb_state;

  const struct saber_keyboard_listener *prev_keyboard;
  void *prev_keyboard_data;

  bool listening;
  bool hiding;
};

/* ------------------------------------------------------------- primitives */

/* Function purpose: Casefold a filter that did not come from the compositor.
The panel passes a desktop file ID here, which is derived from a filename and so
is an arbitrary byte string, and g_utf8_casefold requires valid UTF-8 and walks
off the end of a truncated sequence without it. Toplevel app_ids need no such
care -- toplevel.c makes those valid as they enter the process. */
static char *
fold_filter(const char *app_id)
{
  if (app_id == NULL || *app_id == '\0') {
    return NULL;
  }

  if (g_utf8_validate(app_id, -1, NULL)) {
    return g_utf8_casefold(app_id, -1);
  }

  char *valid = g_utf8_make_valid(app_id, -1);
  char *folded = g_utf8_casefold(valid, -1);

  g_free(valid);
  return folded;
}

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

/* The cache hands back a surface measured in DEVICE pixels, so the scale is
recovered from the surface itself rather than assumed. */
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

/* ------------------------------------------------------------------- cells */

static void
cell_free(gpointer data)
{
  struct spread_cell *cell = data;

  g_free(cell->title);
  g_free(cell->subtitle);
  g_free(cell->icon_name);
  g_free(cell);
}

/* Function purpose: The desktop entry a window belongs to. The model is asked
first because it already holds the resolved entry for every window it knows and
asking it costs nothing; saber_match_resolve is the fallback, and CONSUMES a
launch-window hit, so it must not be called once per frame. */
static struct saber_appinfo *
spread_resolve(struct saber_spread *spread, struct saber_toplevel *toplevel)
{
  if (spread->deps.model != NULL) {
    struct saber_item *item =
        saber_model_find_by_window(spread->deps.model, toplevel);

    if (item != NULL && item->app != NULL) {
      return item->app;
    }
  }

  if (spread->deps.match != NULL && toplevel->app_id != NULL) {
    return saber_match_resolve(spread->deps.match, toplevel->app_id);
  }

  return NULL;
}

static void
spread_layout(struct saber_spread *spread);

static void
spread_place(struct saber_spread *spread);

static int
spread_max_scroll(const struct saber_spread *spread);

static void
spread_rebuild(struct saber_spread *spread)
{
  /* Action purpose: The handle alone cannot carry a selection across a rebuild.
  A window that closed frees the one the selection named, and a window opened
  afterwards can be allocated at the same address -- which would silently move
  the cursor onto a window the user never selected. The identity the cell
  already carries for exactly this reason is compared with it. */
  const struct spread_cell *was_cell = spread->selected >= 0 &&
          spread->selected < (int)spread->cells->len
      ? g_ptr_array_index(spread->cells, spread->selected)
      : NULL;
  const struct saber_toplevel *was =
      was_cell != NULL ? was_cell->toplevel : NULL;
  uint64_t was_id = was_cell != NULL ? was_cell->toplevel_id : 0;

  g_ptr_array_set_size(spread->cells, 0);

  const struct wl_list *list = spread->deps.toplevels != NULL
      ? saber_toplevels_list(spread->deps.toplevels)
      : NULL;

  if (list != NULL) {
    struct saber_toplevel *toplevel;

    wl_list_for_each (toplevel, list, link) {
      if (spread->filter != NULL) {
        if (toplevel->app_id == NULL) {
          continue;
        }

        char *folded = g_utf8_casefold(toplevel->app_id, -1);
        bool keep = strcmp(folded, spread->filter) == 0;

        g_free(folded);

        if (!keep) {
          continue;
        }
      }

      struct saber_appinfo *app = spread_resolve(spread, toplevel);
      struct spread_cell *cell = g_new0(struct spread_cell, 1);

      cell->toplevel = toplevel;
      cell->toplevel_id = toplevel->id;
      cell->minimized =
          saber_toplevel_has_state(toplevel, SABER_TOPLEVEL_MINIMIZED);
      cell->activated =
          saber_toplevel_has_state(toplevel, SABER_TOPLEVEL_ACTIVATED);

      const char *title = toplevel->title;

      if (title == NULL || *title == '\0') {
        title = app != NULL && app->name != NULL ? app->name : toplevel->app_id;
      }

      cell->title = g_strdup(title != NULL ? title : "Window");
      cell->subtitle = g_strdup(app != NULL && app->name != NULL ? app->name
                                                                 : toplevel->app_id);

      if (app != NULL && app->icon != NULL) {
        cell->icon_name = g_strdup(app->icon);
      } else if (toplevel->app_id != NULL) {
        cell->icon_name = g_strdup(toplevel->app_id);
      }

      g_ptr_array_add(spread->cells, cell);
    }
  }

  /* Keep the cursor on the same window across a rebuild; a window closing must
  not silently move the selection onto its neighbour. */
  spread->selected = spread->cells->len > 0 ? 0 : -1;

  for (guint i = 0; was != NULL && i < spread->cells->len; i++) {
    const struct spread_cell *cell = g_ptr_array_index(spread->cells, i);

    if (cell->toplevel == was && cell->toplevel_id == was_id) {
      spread->selected = (int)i;
      break;
    }
  }

  spread->hovered = -1;
  spread->pressed = -1;
  spread->on_close = false;

  spread_layout(spread);
}

/* Function purpose: Whether a handle the grid is still holding is a window the
compositor still has. The protocol destroys a toplevel out from under any
pointer to it, and an owner that forgets to refresh must not be able to make the
spread act on freed memory. */
static bool
spread_alive(const struct saber_spread *spread,
    const struct saber_toplevel *toplevel,
    uint64_t id)
{
  const struct wl_list *list = spread->deps.toplevels != NULL
      ? saber_toplevels_list(spread->deps.toplevels)
      : NULL;

  if (list == NULL) {
    return false;
  }

  const struct saber_toplevel *entry;

  wl_list_for_each (entry, list, link) {
    /* Action purpose: The id is what makes this a liveness test rather than an
    address test. A closed window's block is routinely reused for the next one,
    so a stale cell could otherwise match a live entry and the spread would
    activate -- or close -- an unrelated window. */
    if (entry == toplevel && entry->id == id) {
      return true;
    }
  }

  return false;
}

/* ----------------------------------------------------------------- layout */

static void
spread_layout(struct saber_spread *spread)
{
  int count = (int)spread->cells->len;

  spread->cell_width = SPREAD_CELL_WIDTH;
  spread->cell_height = SPREAD_CELL_HEIGHT;

  double available = (double)spread->width - 2.0 * SPREAD_MARGIN;

  if (available < spread->cell_width) {
    available = spread->cell_width;
  }

  spread->columns = (int)(available / spread->cell_width);
  spread->columns = CLAMP(spread->columns, 1, SPREAD_MAX_COLUMNS);

  if (count > 0 && spread->columns > count) {
    spread->columns = count;
  }

  spread->grid_x =
      ((double)spread->width - spread->columns * spread->cell_width) / 2.0;
  spread->grid_y = SPREAD_MARGIN + SPREAD_HEADER;

  double room = (double)spread->height - spread->grid_y - SPREAD_MARGIN / 2.0;

  spread->visible_rows = (int)(room / spread->cell_height);

  if (spread->visible_rows < 1) {
    spread->visible_rows = 1;
  }

  spread->rows =
      count > 0 ? (count + spread->columns - 1) / spread->columns : 0;

  /* Action purpose: A spread that fits is centred vertically. Unlike the dash
  its contents do not change under the user's fingers, so there is nothing for
  the centring to make jump. */
  if (spread->rows > 0 && spread->rows <= spread->visible_rows) {
    spread->grid_y += (room - spread->rows * spread->cell_height) / 2.0;
  }

  /* Action purpose: The offset outlives the cell list, so it is clamped
  wherever the row count is recomputed. Re-filtering an open spread to a shorter
  application reaches saber_spread_refresh and returns from saber_spread_show
  BEFORE that function's own `scroll = 0`, so a stale offset would survive and
  place every remaining cell off the top -- a grid that draws empty with windows
  in it. spread_reveal_selection clamps too, but only when something is
  selected, which an empty result set never is. */
  spread->scroll = CLAMP(spread->scroll, 0, spread_max_scroll(spread));

  spread_place(spread);
}

static int
spread_max_scroll(const struct saber_spread *spread)
{
  int max = spread->rows - spread->visible_rows;

  return max > 0 ? max : 0;
}

static void
spread_reveal_selection(struct saber_spread *spread)
{
  if (spread->selected < 0 || spread->columns <= 0) {
    return;
  }

  int row = spread->selected / spread->columns;

  if (row < spread->scroll) {
    spread->scroll = row;
  } else if (row >= spread->scroll + spread->visible_rows) {
    spread->scroll = row - spread->visible_rows + 1;
  }

  spread->scroll = CLAMP(spread->scroll, 0, spread_max_scroll(spread));
  spread_place(spread);
}

static void
spread_place(struct saber_spread *spread)
{
  for (guint i = 0; i < spread->cells->len; i++) {
    struct spread_cell *cell = g_ptr_array_index(spread->cells, i);
    int row = (int)i / spread->columns - spread->scroll;

    cell->placed = row >= 0 && row < spread->visible_rows;

    if (!cell->placed) {
      continue;
    }

    cell->x = spread->grid_x + ((int)i % spread->columns) * spread->cell_width;
    cell->y = spread->grid_y + row * spread->cell_height;
  }
}

static void
spread_close_centre(const struct saber_spread *spread,
    const struct spread_cell *cell,
    double *cx,
    double *cy)
{
  *cx = cell->x + spread->cell_width - 16.0 - SPREAD_CLOSE;
  *cy = cell->y + 16.0 + SPREAD_CLOSE;
}

static int
spread_cell_at(const struct saber_spread *spread,
    double x,
    double y,
    bool *on_close)
{
  *on_close = false;

  for (guint i = 0; i < spread->cells->len; i++) {
    const struct spread_cell *cell = g_ptr_array_index(spread->cells, i);

    if (!cell->placed || x < cell->x || y < cell->y ||
        x >= cell->x + spread->cell_width ||
        y >= cell->y + spread->cell_height) {
      continue;
    }

    double cx, cy;

    spread_close_centre(spread, cell, &cx, &cy);
    *on_close = hypot(x - cx, y - cy) <= SPREAD_CLOSE + 2.0;

    return (int)i;
  }

  return -1;
}

/* Function purpose: Where the scrollbar and its thumb are, in one place, so the
painter and the hit test cannot disagree about it. False when there is nothing
to scroll, which is also when no bar is drawn. */
static bool
spread_scrollbar_geometry(const struct saber_spread *spread,
    double *x,
    double *track_y,
    double *track_h,
    double *thumb_y,
    double *thumb_h)
{
  int max = spread_max_scroll(spread);

  if (max <= 0 || spread->rows <= 0) {
    return false;
  }

  double track = spread->visible_rows * spread->cell_height;
  double thumb = track * spread->visible_rows / (double)spread->rows;

  *x = spread->grid_x + spread->columns * spread->cell_width + 10.0;
  *track_y = spread->grid_y;
  *track_h = track;
  *thumb_h = thumb;
  *thumb_y = spread->grid_y + (track - thumb) * spread->scroll / (double)max;

  return true;
}

static bool
spread_on_scrollbar(const struct saber_spread *spread, double x, double y)
{
  double bar_x, track_y, track_h, thumb_y, thumb_h;

  if (!spread_scrollbar_geometry(spread, &bar_x, &track_y, &track_h, &thumb_y,
          &thumb_h)) {
    return false;
  }

  return x >= bar_x - SPREAD_SCROLLBAR_GRAB &&
      x < bar_x + SPREAD_SCROLLBAR + SPREAD_SCROLLBAR_GRAB && y >= track_y &&
      y < track_y + track_h;
}

/* ---------------------------------------------------------------- drawing */

static void
spread_draw_close(struct saber_spread *spread,
    cairo_t *cr,
    const struct spread_cell *cell,
    bool armed)
{
  const struct saber_theme *theme = spread->deps.theme;
  double cx, cy;

  spread_close_centre(spread, cell, &cx, &cy);

  saber_theme_set_source(cr, armed ? &theme->urgent : &theme->badge_bg);
  cairo_arc(cr, cx, cy, SPREAD_CLOSE, 0.0, 2.0 * G_PI);
  cairo_fill(cr);

  double arm = SPREAD_CLOSE * 0.45;

  saber_theme_set_source(cr, &theme->badge_fg);
  cairo_set_line_width(cr, 1.8);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_move_to(cr, cx - arm, cy - arm);
  cairo_line_to(cr, cx + arm, cy + arm);
  cairo_move_to(cr, cx + arm, cy - arm);
  cairo_line_to(cr, cx - arm, cy + arm);
  cairo_stroke(cr);
}

static void
spread_draw_cell(struct saber_spread *spread,
    cairo_t *cr,
    PangoLayout *layout,
    int index)
{
  const struct spread_cell *cell = g_ptr_array_index(spread->cells, index);
  const struct saber_theme *theme = spread->deps.theme;
  double x = cell->x;
  double y = cell->y;
  double w = spread->cell_width;
  double h = spread->cell_height;
  bool current = index == spread->selected;

  /* Action purpose: The pointer highlight is a fade between the resting
  backlight and the accent; the keyboard cursor stays a switch, because it moves
  one cell per keypress and a trail behind it would read as lag. */
  double hover = 0.0;

  if (index == spread->hovered) {
    hover = saber_tween_value(&spread->hover_in);
  } else if (index == spread->fading) {
    hover = saber_tween_value(&spread->hover_out);
  }

  rounded_rect(cr, x + 5.0, y + 5.0, w - 10.0, h - 10.0, SPREAD_RADIUS);

  if (current) {
    set_source_alpha(cr, &theme->accent, 0.34);
  } else if (hover > 0.0) {
    /* Both roles are painted, the resting one first, so the accent arrives over
    a cell that already has a fill rather than over the backdrop. */
    set_source_alpha(cr, &theme->backlight, 0.28);
    cairo_fill_preserve(cr);
    set_source_alpha(cr, &theme->accent, 0.16 * hover);
  } else {
    set_source_alpha(cr, &theme->backlight, 0.28);
  }

  cairo_fill_preserve(cr);

  if (cell->activated) {
    saber_theme_set_source(cr, &theme->accent);
    cairo_set_line_width(cr, 2.0);
  } else {
    set_source_alpha(cr, &theme->dim, current ? 1.0 : 0.55);
    cairo_set_line_width(cr, 1.0);
  }

  cairo_stroke(cr);

  int pixels = (int)lround(SPREAD_ICON * spread->scale);
  cairo_surface_t *icon = NULL;

  if (cell->icon_name != NULL) {
    icon = saber_icons_lookup(spread->deps.icons, cell->icon_name, pixels);
  }

  if (icon == NULL) {
    icon = saber_icons_lookup(spread->deps.icons, "application-x-executable",
        pixels);
  }

  double icon_cy = y + 20.0 + SPREAD_ICON / 2.0;

  if (icon != NULL) {
    /* A window on a sheet the user is not looking at is published as
    `minimized` and is drawn faded, not hidden -- reaching it is the point of
    the spread. */
    if (cell->minimized) {
      cairo_push_group(cr);
      draw_icon(cr, icon, x + w / 2.0, icon_cy, SPREAD_ICON);
      cairo_pop_group_to_source(cr);
      cairo_paint_with_alpha(cr, 0.55);
    } else {
      draw_icon(cr, icon, x + w / 2.0, icon_cy, SPREAD_ICON);
    }
  }

  pango_layout_set_height(layout, -2);
  pango_layout_set_text(layout, cell->title, -1);
  saber_theme_set_source(cr, &theme->foreground);
  cairo_move_to(cr, x + 10.0, y + 20.0 + SPREAD_ICON + 10.0);
  pango_cairo_show_layout(cr, layout);

  int title_height;

  pango_layout_get_pixel_size(layout, NULL, &title_height);

  if (cell->subtitle != NULL) {
    pango_layout_set_height(layout, -1);
    pango_layout_set_text(layout, cell->subtitle, -1);
    saber_theme_set_source(cr, &theme->dim);
    cairo_move_to(cr, x + 10.0,
        y + 20.0 + SPREAD_ICON + 12.0 + title_height);
    pango_cairo_show_layout(cr, layout);
  }

  /* The close affordance appears for the keyboard cursor and for the cell the
  pointer is actually on -- not for the one fading out behind it, which is on
  its way to having no affordance at all. */
  if (current || index == spread->hovered) {
    spread_draw_close(spread, cr, cell,
        index == spread->hovered && spread->on_close);
  }
}

static void
spread_draw_header(struct saber_spread *spread, cairo_t *cr)
{
  const struct saber_theme *theme = spread->deps.theme;
  guint count = spread->cells->len;
  char *text;

  if (count == 0) {
    text = g_strdup(spread->filter != NULL ? "No windows for this application"
                                           : "No open windows");
  } else {
    const struct spread_cell *first = g_ptr_array_index(spread->cells, 0);

    text = spread->filter != NULL && first->subtitle != NULL
        ? g_strdup_printf("%s \xe2\x80\x94 %u window%s", first->subtitle, count,
              count == 1 ? "" : "s")
        : g_strdup_printf("%u window%s", count, count == 1 ? "" : "s");
  }

  PangoLayout *layout = pango_cairo_create_layout(cr);

  pango_layout_set_font_description(layout, spread->header_font);
  pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_width(layout, (int)(spread->width * PANGO_SCALE));
  pango_layout_set_text(layout, text, -1);

  saber_theme_set_source(cr, count == 0 ? &theme->dim : &theme->foreground);
  cairo_move_to(cr, 0.0, SPREAD_MARGIN / 2.0);
  pango_cairo_show_layout(cr, layout);

  g_object_unref(layout);
  g_free(text);
}

static void
spread_draw_scrollbar(struct saber_spread *spread, cairo_t *cr)
{
  double x, track_y, track_h, thumb_y, thumb_h;

  if (!spread_scrollbar_geometry(spread, &x, &track_y, &track_h, &thumb_y,
          &thumb_h)) {
    return;
  }

  const struct saber_theme *theme = spread->deps.theme;

  set_source_alpha(cr, &theme->dim, 0.5);
  rounded_rect(cr, x, track_y, SPREAD_SCROLLBAR, track_h,
      SPREAD_SCROLLBAR / 2.0);
  cairo_fill(cr);

  saber_theme_set_source(cr, &theme->accent);
  rounded_rect(cr, x, thumb_y, SPREAD_SCROLLBAR, thumb_h,
      SPREAD_SCROLLBAR / 2.0);
  cairo_fill(cr);
}

static void
spread_render(void *data,
    struct saber_surface *surface,
    cairo_t *cr,
    int width,
    int height)
{
  struct saber_spread *spread = data;
  const struct saber_theme *theme = spread->deps.theme;

  spread->scale = saber_surface_scale(surface);

  if (width != spread->width || height != spread->height) {
    spread->width = width;
    spread->height = height;
    spread_layout(spread);
    spread_reveal_selection(spread);
  }

  spread_place(spread);

  /* Action purpose: The open transition. The backdrop fades up and the grid
  rises slightly into place, so a full-screen surface appearing over the desktop
  reads as arriving rather than as the screen being replaced between two frames.
  The backdrop is scaled by it, and the grid is translated after it so the fade
  sits behind the movement. */
  double reveal = saber_tween_value(&spread->reveal);

  /* Action purpose: A palette fill, never a blur -- hikari advertises no blur
  protocol and a client cannot read the screen behind itself. Opacity is the
  substitute: ONE paint at exactly the configured alpha, scaled by the reveal so
  the backdrop still fades up on open and lands on the configured value.

  SOURCE, not OVER, for two reasons. The buffer is recycled, so a translucent
  paint over a stale frame would accumulate; and it is what makes the alpha
  exact. The two compounded passes this replaces reached 0.82 and left a sixth
  of the desktop legible through the grid -- and because one of them multiplied
  the role's alpha by itself, no value in the configuration could correct it. */
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  cairo_set_source_rgba(cr, theme->overlay.r, theme->overlay.g,
      theme->overlay.b, theme->overlay_opacity * reveal);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  /* No save/restore: the context is created fresh for every frame, and this
  function has early returns that a save here would leave unbalanced. */
  if (reveal < 1.0) {
    cairo_translate(cr, 0.0, (1.0 - reveal) * SPREAD_REVEAL_RISE);
  }

  spread_draw_header(spread, cr);

  if (spread->cells->len == 0) {
    return;
  }

  PangoLayout *layout = pango_cairo_create_layout(cr);

  pango_layout_set_font_description(layout, spread->font);
  pango_layout_set_alignment(layout, PANGO_ALIGN_CENTER);
  pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
  pango_layout_set_width(layout,
      (int)((spread->cell_width - 20.0) * PANGO_SCALE));

  for (int i = 0; i < (int)spread->cells->len; i++) {
    const struct spread_cell *cell = g_ptr_array_index(spread->cells, i);

    if (cell->placed) {
      spread_draw_cell(spread, cr, layout, i);
    }
  }

  g_object_unref(layout);
  spread_draw_scrollbar(spread, cr);
}

static int
spread_anim_ms(const struct saber_spread *spread);

static void
spread_damage(struct saber_spread *spread);

static void
spread_configure(void *data,
    struct saber_surface *surface,
    int width,
    int height)
{
  (void)surface;

  struct saber_spread *spread = data;

  spread->width = width;
  spread->height = height;
  spread_layout(spread);
  spread_reveal_selection(spread);

  /* The open transition starts on the FIRST configure -- there is no geometry
  to animate against before it, and a later resize must not replay it. */
  if (!spread->revealing) {
    spread->revealing = true;
    saber_tween_start(&spread->reveal, 0.0, 1.0, spread_anim_ms(spread),
        SABER_EASE_OUT_CUBIC, saber_clock_now(spread->clock));
    spread_damage(spread);
  }
}

static void
spread_surface_closed(void *data, struct saber_surface *surface)
{
  (void)surface;

  saber_spread_hide(data);
}

static int
spread_anim_ms(const struct saber_spread *spread)
{
  return spread->deps.config != NULL ? spread->deps.config->panel.animation_ms
                                     : 0;
}

/* Function purpose: Advance the spread's clock on the compositor's frame
timing. Damaging from here asks for the next frame; returning without damaging
lets the loop stop. Every tween on this clock is one-shot. */
static void
spread_frame(void *data, uint32_t time)
{
  struct saber_spread *spread = data;

  if (spread->surface == NULL || spread->surface->closed) {
    return;
  }

  if (saber_clock_advance(spread->clock,
          saber_clock_stamp(spread->clock, time))) {
    saber_surface_damage(spread->surface);
  }
}

static const struct saber_surface_listener spread_surface_listener = {
  .configure = spread_configure,
  .frame = spread_frame,
  .render = spread_render,
  .closed = spread_surface_closed,
};

static void
spread_damage(struct saber_spread *spread)
{
  if (spread->surface != NULL && !spread->surface->closed) {
    saber_surface_damage(spread->surface);
  }
}

/* ------------------------------------------------------------- activation */

static void
spread_activate(struct saber_spread *spread, int index)
{
  if (index < 0 || index >= (int)spread->cells->len) {
    return;
  }

  const struct spread_cell *cell = g_ptr_array_index(spread->cells, index);
  struct saber_toplevel *toplevel = cell->toplevel;

  if (!spread_alive(spread, toplevel, cell->toplevel_id)) {
    saber_spread_refresh(spread);

    return;
  }

  saber_spread_hide(spread);

  /* Action purpose: Unminimise FIRST. On hikari-sakura the minimized bit means
  "on a sheet you are not looking at", so activating without clearing it raises
  a view that stays invisible -- which is exactly the case the spread exists to
  reach. */
  saber_toplevel_unset_minimized(toplevel);
  saber_toplevel_activate(toplevel);
  saber_display_flush(spread->deps.display);
}

static void
spread_close(struct saber_spread *spread, int index)
{
  if (index < 0 || index >= (int)spread->cells->len) {
    return;
  }

  const struct spread_cell *cell = g_ptr_array_index(spread->cells, index);

  if (!spread_alive(spread, cell->toplevel, cell->toplevel_id)) {
    saber_spread_refresh(spread);

    return;
  }

  saber_toplevel_close(cell->toplevel);
  saber_display_flush(spread->deps.display);

  /* The grid stays up: closing several windows in a row is the gesture, and the
  compositor's `closed` event is what actually removes the cell. */
  spread_damage(spread);
}

/* ----------------------------------------------------------------- pointer */

static void
spread_set_hover(struct saber_spread *spread, int index, bool on_close)
{
  if (spread->hovered == index && spread->on_close == on_close) {
    return;
  }

  int64_t now = saber_clock_now(spread->clock);
  int duration = spread_anim_ms(spread);

  spread->fading = spread->hovered;

  if (spread->fading >= 0) {
    saber_tween_start(&spread->hover_out,
        saber_tween_value(&spread->hover_in), 0.0, duration,
        SABER_EASE_OUT_CUBIC, now);
  }

  if (index >= 0) {
    saber_tween_start(&spread->hover_in, 0.0, 1.0, duration,
        SABER_EASE_OUT_CUBIC, now);
  } else {
    saber_tween_stop(&spread->hover_in);
    spread->hover_in.value = 0.0;
  }

  spread->hovered = index;
  spread->on_close = on_close;
  spread_damage(spread);
}

/* Function purpose: The one way the grid's offset changes. Clamps, re-places
the cells, re-resolves the hover -- the cells moved under a pointer that did
not -- and repaints. */
static void
spread_scroll_to(struct saber_spread *spread, int scroll)
{
  int next = CLAMP(scroll, 0, spread_max_scroll(spread));

  if (next == spread->scroll) {
    return;
  }

  spread->scroll = next;
  spread_place(spread);

  bool on_close = false;
  int index =
      spread_cell_at(spread, spread->pointer_x, spread->pointer_y, &on_close);

  spread_set_hover(spread, index, on_close);
  spread_damage(spread);
}

/* Function purpose: Take a press on the scrollbar. Returns true when the press
belonged to the bar, which is what keeps it out of the backdrop path below --
the track is outside every cell, so an unclaimed press there dismissed the whole
spread. */
static bool
spread_scrollbar_press(struct saber_spread *spread, double x, double y)
{
  double bar_x, track_y, track_h, thumb_y, thumb_h;

  if (!spread_scrollbar_geometry(spread, &bar_x, &track_y, &track_h, &thumb_y,
          &thumb_h) ||
      !spread_on_scrollbar(spread, x, y)) {
    return false;
  }

  spread->bar_pressed = true;

  if (y >= thumb_y && y < thumb_y + thumb_h) {
    spread->bar_dragging = true;
    spread->bar_grab = y - thumb_y;

    return true;
  }

  /* Bare track: page toward the click, the way every other scrollbar does. No
  drag is started, because the thumb has just moved out from under the pointer
  and there is no grab point left that would not make it jump. */
  spread_scroll_to(spread,
      spread->scroll +
          (y < thumb_y ? -spread->visible_rows : spread->visible_rows));

  return true;
}

static void
spread_scrollbar_drag(struct saber_spread *spread, double y)
{
  double bar_x, track_y, track_h, thumb_y, thumb_h;

  if (!spread_scrollbar_geometry(spread, &bar_x, &track_y, &track_h, &thumb_y,
          &thumb_h)) {
    spread->bar_dragging = false;

    return;
  }

  double span = track_h - thumb_h;

  if (span <= 0.0) {
    return;
  }

  /* The point of the thumb the drag started on stays under the pointer, so the
  thumb does not snap its centre to the cursor on the first motion event. */
  double top = y - spread->bar_grab - track_y;

  spread_scroll_to(spread, (int)lround(top * spread_max_scroll(spread) / span));
}

/* The spread draws one surface; ownership is an identity test. */
static bool
spread_pointer_owns(void *data, struct wl_surface *surface)
{
  struct saber_spread *spread = data;

  return spread->surface != NULL && spread->surface->wl_surface == surface;
}

static void
spread_pointer_enter(void *data, struct wl_surface *surface, double x, double y)
{
  struct saber_spread *spread = data;

  if (spread->surface == NULL || spread->surface->wl_surface != surface) {
    return;
  }

  bool on_close = false;
  int index = spread_cell_at(spread, x, y, &on_close);

  spread->pointer_x = x;
  spread->pointer_y = y;

  saber_display_set_cursor(spread->deps.display, "left_ptr");
  spread_set_hover(spread, index, on_close);
}

static void
spread_pointer_leave(void *data, struct wl_surface *surface)
{
  (void)surface;

  struct saber_spread *spread = data;

  spread->bar_pressed = false;
  spread->bar_dragging = false;
  saber_scroll_reset(&spread->scroll_accum);
  spread_set_hover(spread, -1, false);
}

static void
spread_pointer_motion(void *data, uint32_t time, double x, double y)
{
  (void)time;

  struct saber_spread *spread = data;

  spread->pointer_x = x;
  spread->pointer_y = y;

  if (spread->bar_dragging) {
    spread_scrollbar_drag(spread, y);

    return;
  }

  bool on_close = false;
  int index = spread_cell_at(spread, x, y, &on_close);

  spread_set_hover(spread, index, on_close);
}

static void
spread_pointer_button(void *data,
    uint32_t time,
    uint32_t button,
    uint32_t state)
{
  (void)time;

  struct saber_spread *spread = data;

  if (button == SPREAD_BTN_MIDDLE) {
    if (state == WL_POINTER_BUTTON_STATE_RELEASED && spread->hovered >= 0) {
      spread_close(spread, spread->hovered);
    }

    return;
  }

  if (button != SPREAD_BTN_LEFT) {
    return;
  }

  if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
    if (spread_scrollbar_press(spread, spread->pointer_x,
            spread->pointer_y)) {
      return;
    }

    spread->pressed = spread->hovered;

    if (spread->hovered >= 0) {
      spread->selected = spread->hovered;
    }

    spread_damage(spread);

    return;
  }

  /* Action purpose: The release that ends a scrollbar interaction, thumb or
  track, stops here. Below, a release with no pressed cell IS the dismissal --
  which is exactly how grabbing the scrollbar used to close the spread. */
  if (spread->bar_pressed) {
    spread->bar_pressed = false;
    spread->bar_dragging = false;

    return;
  }

  int index = spread->pressed;

  spread->pressed = -1;

  /* A press on the backdrop is a dismissal: the spread covers the whole
  output, so there is nowhere else for "click away to close" to happen. */
  if (index < 0) {
    if (spread->hovered < 0) {
      saber_spread_hide(spread);
    }

    return;
  }

  if (index != spread->hovered) {
    spread_damage(spread);

    return;
  }

  if (spread->on_close) {
    spread_close(spread, index);
  } else {
    spread_activate(spread, index);
  }
}

/* Action purpose: A row per notch of travel, not a row per event. The bare sign
this replaced meant one two-finger swipe -- dozens of fractional axis events --
scrolled the grid dozens of rows, straight past every window in it. */
static void
spread_pointer_axis(void *data, uint32_t time, uint32_t axis, double value)
{
  (void)time;

  struct saber_spread *spread = data;

  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
    return;
  }

  int steps = saber_scroll_steps(&spread->scroll_accum,
      saber_scroll_delta(&spread->scroll_accum, axis, value), 120);

  if (steps != 0) {
    spread_scroll_to(spread, spread->scroll + steps);
  }
}

static void
spread_pointer_axis_value120(void *data, uint32_t axis, int32_t value120)
{
  struct saber_spread *spread = data;

  saber_scroll_detail(&spread->scroll_accum, axis, value120);
}

static void
spread_pointer_axis_stop(void *data, uint32_t time, uint32_t axis)
{
  (void)time;
  (void)axis;

  struct saber_spread *spread = data;

  saber_scroll_reset(&spread->scroll_accum);
}

static const struct saber_pointer_listener spread_pointer_listener = {
  .owns = spread_pointer_owns,
  .enter = spread_pointer_enter,
  .leave = spread_pointer_leave,
  .motion = spread_pointer_motion,
  .button = spread_pointer_button,
  .axis = spread_pointer_axis,
  .axis_value120 = spread_pointer_axis_value120,
  .axis_stop = spread_pointer_axis_stop,
};

/* ---------------------------------------------------------------- keyboard */

static void
spread_keymap(void *data, uint32_t format, int fd, uint32_t size)
{
  struct saber_spread *spread = data;

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
    close(fd);

    return;
  }

  char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

  if (map == MAP_FAILED) {
    close(fd);

    return;
  }

  struct xkb_keymap *keymap = xkb_keymap_new_from_string(spread->xkb, map,
      XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);

  munmap(map, size);
  close(fd);

  if (keymap == NULL) {
    return;
  }

  xkb_state_unref(spread->xkb_state);
  xkb_keymap_unref(spread->keymap);

  spread->keymap = keymap;
  spread->xkb_state = xkb_state_new(keymap);
}

static void
spread_modifiers(void *data,
    uint32_t depressed,
    uint32_t latched,
    uint32_t locked,
    uint32_t group)
{
  struct saber_spread *spread = data;

  if (spread->xkb_state != NULL) {
    xkb_state_update_mask(spread->xkb_state, depressed, latched, locked, 0, 0,
        group);
  }
}

static void
spread_move(struct saber_spread *spread, int delta)
{
  int count = (int)spread->cells->len;

  if (count == 0) {
    return;
  }

  int index = CLAMP(spread->selected + delta, 0, count - 1);

  if (index == spread->selected) {
    return;
  }

  spread->selected = index;
  spread->hovered = -1;
  spread_reveal_selection(spread);
  spread_damage(spread);
}

static void
spread_key(void *data, uint32_t time, uint32_t key, uint32_t state)
{
  (void)time;

  struct saber_spread *spread = data;

  if (state != WL_KEYBOARD_KEY_STATE_PRESSED || spread->xkb_state == NULL) {
    return;
  }

  /* Wayland keycodes are evdev; xkb wants the X11 numbering. */
  xkb_keysym_t sym = xkb_state_key_get_one_sym(spread->xkb_state, key + 8);

  switch (sym) {
  case XKB_KEY_Escape:
    saber_spread_hide(spread);
    break;

  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
  case XKB_KEY_space:
    spread_activate(spread, spread->selected);
    break;

  case XKB_KEY_Delete:
  case XKB_KEY_KP_Delete:
  case XKB_KEY_BackSpace:
    spread_close(spread, spread->selected);
    break;

  case XKB_KEY_Left:
    spread_move(spread, -1);
    break;

  case XKB_KEY_Right:
  case XKB_KEY_Tab:
    spread_move(spread, 1);
    break;

  case XKB_KEY_Up:
    spread_move(spread, -spread->columns);
    break;

  case XKB_KEY_Down:
    spread_move(spread, spread->columns);
    break;

  case XKB_KEY_Home:
    spread_move(spread, -(int)spread->cells->len);
    break;

  case XKB_KEY_End:
    spread_move(spread, (int)spread->cells->len);
    break;

  default:
    break;
  }
}

static const struct saber_keyboard_listener spread_keyboard_listener = {
  .keymap = spread_keymap,
  .key = spread_key,
  .modifiers = spread_modifiers,
};

/* --------------------------------------------------------------- lifecycle */

struct saber_spread *
saber_spread_create(const struct saber_spread_deps *deps)
{
  if (deps == NULL || deps->display == NULL || deps->theme == NULL) {
    return NULL;
  }

  struct saber_spread *spread = g_new0(struct saber_spread, 1);

  spread->deps = *deps;
  spread->cells = g_ptr_array_new_with_free_func(cell_free);
  spread->selected = -1;
  spread->hovered = -1;
  spread->fading = -1;
  spread->pressed = -1;
  spread->scale = 1.0;

  spread->clock = saber_clock_create();
  saber_tween_init(&spread->reveal, 1.0);
  saber_tween_init(&spread->hover_in, 0.0);
  saber_tween_init(&spread->hover_out, 0.0);
  saber_clock_add(spread->clock, &spread->reveal);
  saber_clock_add(spread->clock, &spread->hover_in);
  saber_clock_add(spread->clock, &spread->hover_out);
  spread->columns = 1;
  spread->visible_rows = 1;

  spread->font = pango_font_description_from_string(SPREAD_FONT);
  spread->header_font = pango_font_description_from_string(SPREAD_HEADER_FONT);

  spread->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

  /* Action purpose: wl_keyboard.keymap fires once, when the seat's keyboard is
  bound, and display.c does not cache it -- a listener installed later never
  sees one. The compiled default layout drives the handful of keys the spread
  reads; a real keymap replaces it if the event does arrive. */
  if (spread->xkb != NULL) {
    spread->keymap = xkb_keymap_new_from_names(spread->xkb, NULL,
        XKB_KEYMAP_COMPILE_NO_FLAGS);

    if (spread->keymap != NULL) {
      spread->xkb_state = xkb_state_new(spread->keymap);
    }
  }

  return spread;
}

void
saber_spread_destroy(struct saber_spread *spread)
{
  if (spread == NULL) {
    return;
  }

  saber_spread_hide(spread);

  g_ptr_array_unref(spread->cells);
  g_free(spread->filter);

  saber_clock_destroy(spread->clock);

  pango_font_description_free(spread->font);
  pango_font_description_free(spread->header_font);

  xkb_state_unref(spread->xkb_state);
  xkb_keymap_unref(spread->keymap);
  xkb_context_unref(spread->xkb);

  g_free(spread);
}

bool
saber_spread_is_visible(const struct saber_spread *spread)
{
  return spread != NULL && spread->surface != NULL;
}

void
saber_spread_refresh(struct saber_spread *spread)
{
  if (spread == NULL || spread->surface == NULL) {
    return;
  }

  spread_rebuild(spread);
  spread_reveal_selection(spread);
  spread_damage(spread);
}

bool
saber_spread_show(struct saber_spread *spread,
    const char *app_id,
    struct saber_output *output)
{
  if (spread == NULL) {
    return false;
  }

  g_free(spread->filter);
  spread->filter = fold_filter(app_id);

  if (spread->surface != NULL) {
    saber_spread_refresh(spread);

    return true;
  }

  spread->scroll = 0;

  /* The output's own size, so the first frame is laid out correctly rather than
  being relaid on the configure that follows it. */
  if (output != NULL && output->scale > 0) {
    spread->width = output->width / output->scale;
    spread->height = output->height / output->scale;
  }

  if (spread->width <= 0 || spread->height <= 0) {
    spread->width = 1280;
    spread->height = 720;
  }

  spread_rebuild(spread);

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
    .layer_namespace = "saber-spread",
  };

  spread->surface = saber_surface_create(spread->deps.display, &params,
      &spread_surface_listener, spread);

  if (spread->surface == NULL) {
    g_warning("saber: could not create the spread surface");

    return false;
  }

  /* Action purpose: Pointer input is registered, not seized -- display.c routes
  by surface, so the panel keeps its own clicks while the spread is up. The
  keyboard is still a single slot and is genuinely exclusive here, so it keeps
  the save-and-restore. */
  spread->prev_keyboard = spread->deps.display->keyboard_listener;
  spread->prev_keyboard_data = spread->deps.display->keyboard_data;

  saber_display_add_pointer_listener(spread->deps.display,
      &spread_pointer_listener, spread);
  saber_display_set_keyboard_listener(spread->deps.display,
      &spread_keyboard_listener, spread);
  spread->listening = true;

  saber_display_flush(spread->deps.display);

  return true;
}

void
saber_spread_hide(struct saber_spread *spread)
{
  /* Hiding is re-entrant by design: the surface's `closed` callback lands here,
  and an activation hides the spread from inside an input event. */
  if (spread == NULL || spread->surface == NULL || spread->hiding) {
    return;
  }

  spread->hiding = true;
  spread->revealing = false;
  spread->fading = -1;
  saber_tween_stop(&spread->reveal);
  spread->reveal.value = 1.0;
  saber_tween_stop(&spread->hover_in);
  spread->hover_in.value = 0.0;
  saber_tween_stop(&spread->hover_out);
  spread->hover_out.value = 0.0;
  saber_clock_reset(spread->clock);

  saber_surface_destroy(spread->surface);
  spread->surface = NULL;

  if (spread->listening) {
    saber_display_remove_pointer_listener(spread->deps.display,
        &spread_pointer_listener, spread);
    saber_display_set_keyboard_listener(spread->deps.display,
        spread->prev_keyboard, spread->prev_keyboard_data);
    spread->listening = false;
  }

  saber_display_flush(spread->deps.display);

  g_ptr_array_set_size(spread->cells, 0);

  spread->selected = -1;
  spread->hovered = -1;
  spread->pressed = -1;
  spread->on_close = false;
  spread->scroll = 0;
  spread->bar_pressed = false;
  spread->bar_dragging = false;
  saber_scroll_reset(&spread->scroll_accum);
  spread->hiding = false;
}

bool
saber_spread_toggle(struct saber_spread *spread,
    const char *app_id,
    struct saber_output *output)
{
  if (spread == NULL) {
    return false;
  }

  if (spread->surface != NULL) {
    char *wanted = fold_filter(app_id);
    bool same = g_strcmp0(wanted, spread->filter) == 0;

    g_free(wanted);

    /* A second tile's spread re-filters rather than dismissing: the gesture is
    "show me that application's windows", not "close what is open". */
    if (!same) {
      return saber_spread_show(spread, app_id, output);
    }

    saber_spread_hide(spread);

    return false;
  }

  return saber_spread_show(spread, app_id, output);
}
