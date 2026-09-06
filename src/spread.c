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

/* Weight of the second backdrop pass; see spread_render. */
#define SPREAD_BACKDROP_PASS 0.7

/* Uncapped, a wide output lays a dozen windows out as one unreadable line. */
#define SPREAD_MAX_COLUMNS 5

#define SPREAD_FONT "Sans 9.5"
#define SPREAD_HEADER_FONT "Sans 12"

struct spread_cell {
  struct saber_toplevel *toplevel;
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
  int pressed;
  bool on_close; /* the pointer is over the hovered cell's close disc */
  int scroll;

  int columns, rows, visible_rows;
  double cell_width, cell_height;
  double grid_x, grid_y;

  PangoFontDescription *font;
  PangoFontDescription *header_font;

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

static void
spread_rebuild(struct saber_spread *spread)
{
  struct saber_toplevel *was = spread->selected >= 0 &&
          spread->selected < (int)spread->cells->len
      ? ((struct spread_cell *)g_ptr_array_index(spread->cells,
             spread->selected))
            ->toplevel
      : NULL;

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

    if (cell->toplevel == was) {
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
    const struct saber_toplevel *toplevel)
{
  const struct wl_list *list = spread->deps.toplevels != NULL
      ? saber_toplevels_list(spread->deps.toplevels)
      : NULL;

  if (list == NULL) {
    return false;
  }

  const struct saber_toplevel *entry;

  wl_list_for_each (entry, list, link) {
    if (entry == toplevel) {
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
  bool hot = current || index == spread->hovered;

  rounded_rect(cr, x + 5.0, y + 5.0, w - 10.0, h - 10.0, SPREAD_RADIUS);

  if (hot) {
    set_source_alpha(cr, &theme->accent, current ? 0.34 : 0.16);
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

  if (hot) {
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
  int max = spread_max_scroll(spread);

  if (max <= 0) {
    return;
  }

  const struct saber_theme *theme = spread->deps.theme;
  double track_h = spread->visible_rows * spread->cell_height;
  double x = spread->grid_x + spread->columns * spread->cell_width + 10.0;
  double thumb_h = track_h * spread->visible_rows / (double)spread->rows;
  double thumb_y =
      spread->grid_y + (track_h - thumb_h) * spread->scroll / (double)max;

  set_source_alpha(cr, &theme->dim, 0.5);
  rounded_rect(cr, x, spread->grid_y, SPREAD_SCROLLBAR, track_h,
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

  /* A translucent palette fill, never a blur (BLUEPRINT.md 5.7). SOURCE, not
  OVER: the buffer is recycled, so a translucent paint over a stale frame would
  accumulate. */
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  saber_theme_set_source(cr, &theme->overlay);
  cairo_paint(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

  /* Action purpose: The same role composited a second time. One pass of the
  theme's overlay alpha does not carry window titles over a bright desktop, and
  inventing a colour here would break the rule that every colour in the panel
  comes from a theme role. Two passes still leave the desktop showing. */
  set_source_alpha(cr, &theme->overlay, SPREAD_BACKDROP_PASS);
  cairo_paint(cr);

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
}

static void
spread_surface_closed(void *data, struct saber_surface *surface)
{
  (void)surface;

  saber_spread_hide(data);
}

static const struct saber_surface_listener spread_surface_listener = {
  .configure = spread_configure,
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

  if (!spread_alive(spread, toplevel)) {
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

  if (!spread_alive(spread, cell->toplevel)) {
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

  spread->hovered = index;
  spread->on_close = on_close;
  spread_damage(spread);
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

  saber_display_set_cursor(spread->deps.display, "left_ptr");
  spread_set_hover(spread, index, on_close);
}

static void
spread_pointer_leave(void *data, struct wl_surface *surface)
{
  (void)surface;

  spread_set_hover(data, -1, false);
}

static void
spread_pointer_motion(void *data, uint32_t time, double x, double y)
{
  (void)time;

  struct saber_spread *spread = data;
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
    spread->pressed = spread->hovered;

    if (spread->hovered >= 0) {
      spread->selected = spread->hovered;
    }

    spread_damage(spread);

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

static void
spread_pointer_axis(void *data, uint32_t time, uint32_t axis, double value)
{
  (void)time;

  struct saber_spread *spread = data;

  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL || value == 0.0) {
    return;
  }

  int scroll = CLAMP(spread->scroll + (value > 0.0 ? 1 : -1), 0,
      spread_max_scroll(spread));

  if (scroll == spread->scroll) {
    return;
  }

  spread->scroll = scroll;
  spread_place(spread);
  spread_damage(spread);
}

static const struct saber_pointer_listener spread_pointer_listener = {
  .enter = spread_pointer_enter,
  .leave = spread_pointer_leave,
  .motion = spread_pointer_motion,
  .button = spread_pointer_button,
  .axis = spread_pointer_axis,
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
  spread->pressed = -1;
  spread->scale = 1.0;
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

  /* Action purpose: The spread is modal, and display.c holds one listener of
  each kind. Taking both over for its lifetime and putting the previous pair
  back on hide is the only way to share them with the panel. */
  spread->prev_pointer = spread->deps.display->pointer_listener;
  spread->prev_pointer_data = spread->deps.display->pointer_data;
  spread->prev_keyboard = spread->deps.display->keyboard_listener;
  spread->prev_keyboard_data = spread->deps.display->keyboard_data;

  saber_display_set_pointer_listener(spread->deps.display,
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

  saber_surface_destroy(spread->surface);
  spread->surface = NULL;

  if (spread->listening) {
    saber_display_set_pointer_listener(spread->deps.display,
        spread->prev_pointer, spread->prev_pointer_data);
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
