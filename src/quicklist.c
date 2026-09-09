/* Script function and purpose: The quicklist popup -- composition, layout,
cairo/pango drawing and input for the menu a tile or a tray icon opens.

One widget serves both cases. A tray item's context menu is a com.canonical
.dbusmenu object and nothing else; an application's quicklist is that same
object with the desktop entry's Actions, its window list and the pin and quit
rows composed underneath (BLUEPRINT.md 5.4). Nothing below asks the peer to
show a menu, because no such method exists in DBusMenu: an application
publishes a DESCRIPTION and the host draws it. That is what lets this file
paint somebody else's menu in Saber's own theme. */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>

#include <dev/evdev/input-event-codes.h>

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <pango/pangocairo.h>
#include <xkbcommon/xkbcommon.h>

#include <saber/anim.h>
#include <saber/dbusmenu.h>
#include <saber/display.h>
#include <saber/quicklist.h>
#include <saber/render.h>

#define QL_PAD_X 10
#define QL_PAD_Y 4
#define QL_GAP 6
#define QL_ROW_HEIGHT 26
#define QL_SEPARATOR_HEIGHT 9
#define QL_ICON 16
#define QL_MARK 12  /* checkmark and radio column */
#define QL_ARROW 8  /* submenu indicator column */
#define QL_MIN_WIDTH 160
#define QL_MAX_WIDTH 460
#define QL_TEXT_MAX 340

/* Breathing room kept between a full-height menu and the edges of the output.
A menu flush against both is indistinguishable from one the compositor has
clipped. */
#define QL_SCREEN_MARGIN 8

/* Rows per wheel notch, the desktop's usual figure for a menu. */
#define QL_SCROLL_ROWS 3

/* Saber's configuration carries no font key and the theme is colours only, so
the menu asks fontconfig for the system sans at the size a menu wants. */
#define QL_FONT "Sans 10"

enum ql_kind {
  QL_SEPARATOR,
  QL_DBUSMENU,
  QL_ACTION,
  QL_WINDOW,
  QL_PIN,
  QL_UNPIN,
  QL_QUIT,
};

struct ql_entry {
  enum ql_kind kind;
  char *label;
  bool enabled;
  bool submenu;

  enum saber_dbusmenu_toggle toggle_type;
  int toggle_state;

  int32_t dbusmenu_id;
  char *action_id;
  void *window;
  cairo_surface_t *icon;

  int y, height; /* filled in by level_layout, logical pixels */
};

struct ql_level {
  struct saber_quicklist *ql;
  struct ql_level *parent;
  struct ql_level *child;
  struct saber_popup *popup;

  GPtrArray *entries; /* struct ql_entry * */

  int width, height;

  /* Action purpose: Three heights, because a menu can be taller than the
  screen. `content_height` is what the rows need; `height` is what was ASKED of
  the compositor, clamped to the output because level_layout used to clamp the
  width and not this, so a long tray menu asked for a popup taller than the
  display; `view_height` is what the compositor actually gave, which its
  RESIZE_Y correction can make shorter still. `scroll` is how far the rows are
  slid up inside that view -- applied in level_render AND in level_at, or the
  highlight lands on a different row from the one under the pointer. */
  int content_height;
  int view_height;
  int scroll;

  int mark_column, icon_column, arrow_column; /* 0 when the level has none */

  /* Kept so the level can be moved when its rows change size under an open
  popup; a nested level's anchor is a row of its parent. */
  int32_t anchor_x, anchor_y, anchor_width, anchor_height;

  int hovered;  /* pointer */
  int selected; /* keyboard cursor */
  int open;     /* the row whose submenu is showing */

  /* Action purpose: AboutToShow is answered off the main loop now, so a reply
  can arrive for a submenu the pointer has already left. This counts the times
  this level has been asked to open something; a reply quoting an older value
  is answering a question nobody is asking any more and is dropped. */
  guint open_serial;

  /* Cancelled by level_destroy before anything is freed, so a reply in flight
  has something safe to ask about a level that may no longer exist. */
  GCancellable *cancellable;

  /* Action purpose: One tween, one-shot: the level's contents fade up as it
  opens, so a submenu appearing under the pointer reads as arriving rather than
  as the screen changing between two frames. The ROW highlight deliberately does
  not fade -- a menu highlight that lags the pointer reads as the menu being
  slow, and every desktop menu snaps it. */
  struct saber_clock *clock;
  struct saber_tween reveal;
};

struct ql_window {
  char *title;
  void *handle;
};

struct saber_quicklist {
  struct saber_display *display;
  /* Borrowed. NULL means a themed icon name does not resolve. */
  struct saber_icons *icons;
  struct saber_surface *parent;
  enum saber_edge edge;
  struct saber_theme theme;

  struct saber_dbusmenu *menu;
  struct saber_appinfo *app;

  /* Kept so the menu can be recomposed in place when the application
  republishes its layout under an open popup. */
  GPtrArray *windows; /* struct ql_window * */
  bool pinned, running, offer_pin;
  int32_t anchor_x, anchor_y, anchor_width, anchor_height;

  struct ql_level *root;

  cairo_surface_t *measure_surface;
  cairo_t *measure_cr;
  PangoFontDescription *font;

  struct xkb_context *xkb;
  struct xkb_keymap *keymap;
  struct xkb_state *xkb_state;

  const struct saber_keyboard_listener *prev_keyboard;
  void *prev_keyboard_data;

  struct saber_quicklist_handlers handlers;
  void *user;

  /* The configured animation duration, copied because the menu outlives the
  params it was opened from. 0 means animation is switched off. */
  int config_anim_ms;

  struct ql_level *pointer_level;
  /* The pointer's last row-local position, kept so a scroll can re-resolve the
  hover without waiting for the pointer to move. */
  double pointer_y;
  struct saber_scroll_accum scroll;
  uint32_t serial;
  bool pressed;
  bool closing;
  bool held;     /* the panel's keyboard hold is ours to release */
  bool listening; /* the display's listeners are ours to put back */
};

static void
level_destroy(struct ql_level *level);

static struct ql_level *
level_create(struct saber_quicklist *ql,
    struct ql_level *parent,
    GPtrArray *entries,
    int32_t anchor_x,
    int32_t anchor_y,
    int32_t anchor_width,
    int32_t anchor_height);

/* A desktop action waiting on its activation token. Everything it needs is
copied, because the menu that asked for it is closed before the reply lands. */
struct ql_action_launch {
  struct saber_appinfo *app; /* owned */
  char *action_id;           /* owned */
};

static void
ql_action_with_token(const char *token, void *user)
{
  struct ql_action_launch *pending = user;

  if (!saber_appinfo_launch(pending->app, pending->action_id, NULL, token)) {
    g_warning("saber: failed to run action '%s'", pending->action_id);
  }

  saber_appinfo_unref(pending->app);
  g_free(pending->action_id);
  g_free(pending);
}

/* Icons ------------------------------------------------------------------ */

static cairo_surface_t *
icon_from_pixbuf(GdkPixbuf *pixbuf)
{
  int width = gdk_pixbuf_get_width(pixbuf);
  int height = gdk_pixbuf_get_height(pixbuf);
  int channels = gdk_pixbuf_get_n_channels(pixbuf);
  int stride = gdk_pixbuf_get_rowstride(pixbuf);
  const guchar *pixels = gdk_pixbuf_get_pixels(pixbuf);

  if (width <= 0 || height <= 0 || (channels != 3 && channels != 4)) {
    return NULL;
  }

  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);

  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);
    return NULL;
  }

  unsigned char *data = cairo_image_surface_get_data(surface);
  int data_stride = cairo_image_surface_get_stride(surface);

  /* Action purpose: gdk-pixbuf hands back straight RGBA; cairo wants
  premultiplied native-endian ARGB. Skipping the multiply leaves every
  antialiased icon edge outlined in white. */
  for (int y = 0; y < height; y++) {
    const guchar *row = pixels + (size_t)y * (size_t)stride;
    uint32_t *out = (uint32_t *)(data + (size_t)y * (size_t)data_stride);

    for (int x = 0; x < width; x++) {
      const guchar *pixel = row + (size_t)x * (size_t)channels;
      uint32_t a = channels == 4 ? pixel[3] : 0xffu;

      out[x] = a << 24 | (uint32_t)pixel[0] * a / 255u << 16 |
          (uint32_t)pixel[1] * a / 255u << 8 | (uint32_t)pixel[2] * a / 255u;
    }
  }

  cairo_surface_mark_dirty(surface);

  return surface;
}

static cairo_surface_t *
icon_from_data(const uint8_t *data, size_t length)
{
  if (data == NULL || length == 0) {
    return NULL;
  }

  GInputStream *stream =
      g_memory_input_stream_new_from_data(data, (gssize)length, NULL);
  GdkPixbuf *pixbuf = gdk_pixbuf_new_from_stream(stream, NULL, NULL);

  g_object_unref(stream);

  if (pixbuf == NULL) {
    return NULL;
  }

  cairo_surface_t *surface = icon_from_pixbuf(pixbuf);
  g_object_unref(pixbuf);

  return surface;
}

/* Function purpose: Resolve a menu row's icon, whether it is named by theme or
given as an absolute path.

Themed names used to be dropped outright, on the reasoning that theme lookup
belonged in a shared module rather than in a third private copy. That module
exists and is passed in now. The distinction matters because the DBusMenu
specification names icons by THEME, so a tray application's menu is exactly the
case that drew none of its icons.

The shared resolver handles absolute paths too, so it is tried first and the
local loader is the fallback for when no resolver was supplied. */
static cairo_surface_t *
icon_from_name(struct saber_icons *icons, const char *name)
{
  if (name == NULL || name[0] == '\0') {
    return NULL;
  }

  if (icons != NULL) {
    /* Owned by the cache, so it is referenced rather than adopted -- entry_free
    unconditionally destroys what it holds. */
    cairo_surface_t *shared = saber_icons_lookup(icons, name, QL_ICON * 2);

    if (shared != NULL) {
      return cairo_surface_reference(shared);
    }
  }

  if (!g_path_is_absolute(name)) {
    return NULL;
  }

  GdkPixbuf *pixbuf =
      gdk_pixbuf_new_from_file_at_size(name, QL_ICON * 2, QL_ICON * 2, NULL);

  if (pixbuf == NULL) {
    return NULL;
  }

  cairo_surface_t *surface = icon_from_pixbuf(pixbuf);
  g_object_unref(pixbuf);

  return surface;
}

/* Composition ------------------------------------------------------------ */

static void
entry_free(gpointer data)
{
  struct ql_entry *entry = data;

  if (entry->icon != NULL) {
    cairo_surface_destroy(entry->icon);
  }

  g_free(entry->label);
  g_free(entry->action_id);
  g_free(entry);
}

static struct ql_entry *
entry_new(GPtrArray *entries, enum ql_kind kind, const char *label)
{
  struct ql_entry *entry = g_new0(struct ql_entry, 1);

  entry->kind = kind;
  entry->label = g_strdup(label != NULL ? label : "");
  entry->enabled = kind != QL_SEPARATOR;
  entry->dbusmenu_id = -1;
  entry->toggle_state = SABER_DBUSMENU_TOGGLE_INDETERMINATE;

  g_ptr_array_add(entries, entry);

  return entry;
}

/* Function purpose: Emit the separator that divides two composed groups, but
only between groups that both produced rows -- a menu with no dynamic half must
not open with a rule across the top. */
static void
entries_divide(GPtrArray *entries, bool *pending)
{
  if (*pending && entries->len > 0) {
    entry_new(entries, QL_SEPARATOR, NULL);
  }

  *pending = false;
}

static void
entries_add_dbusmenu(GPtrArray *entries,
    const struct saber_dbusmenu_item *item,
    struct saber_icons *icons)
{
  if (!item->visible) {
    return;
  }

  if (item->type == SABER_DBUSMENU_SEPARATOR) {
    entry_new(entries, QL_SEPARATOR, NULL);
    return;
  }

  struct ql_entry *entry = entry_new(entries, QL_DBUSMENU, item->label);

  entry->enabled = item->enabled;
  entry->submenu = item->submenu;
  entry->dbusmenu_id = item->id;
  entry->toggle_type = item->toggle_type;
  entry->toggle_state = item->toggle_state;

  entry->icon = icon_from_data(item->icon_data, item->icon_data_len);

  if (entry->icon == NULL) {
    entry->icon = icon_from_name(icons, item->icon_name);
  }
}

/* Function purpose: Build one level's rows from a DBusMenu item's children,
which is the only shape a submenu ever has. */
static GPtrArray *
entries_from_children(const struct saber_dbusmenu_item *item,
    struct saber_icons *icons)
{
  GPtrArray *entries = g_ptr_array_new_with_free_func(entry_free);

  for (guint i = 0; item->children != NULL && i < item->children->len; i++) {
    entries_add_dbusmenu(entries, g_ptr_array_index(item->children, i), icons);
  }

  return entries;
}

static GPtrArray *
quicklist_compose(struct saber_quicklist *ql)
{
  GPtrArray *entries = g_ptr_array_new_with_free_func(entry_free);
  bool divide = false;

  const struct saber_dbusmenu_item *root =
      ql->menu != NULL ? saber_dbusmenu_root(ql->menu) : NULL;

  if (root != NULL && root->children != NULL) {
    for (guint i = 0; i < root->children->len; i++) {
      entries_add_dbusmenu(entries, g_ptr_array_index(root->children, i),
          ql->icons);
    }

    divide = entries->len > 0;
  }

  for (size_t i = 0; ql->app != NULL && i < ql->app->actions_len; i++) {
    const struct saber_appinfo_action *action = &ql->app->actions[i];

    entries_divide(entries, &divide);

    struct ql_entry *entry = entry_new(entries, QL_ACTION,
        action->name != NULL ? action->name : action->id);
    entry->action_id = g_strdup(action->id);
    entry->icon = icon_from_name(ql->icons, action->icon);
  }

  divide = divide || entries->len > 0;

  /* A single window is the tile itself; only a choice is worth listing. */
  if (ql->windows != NULL && ql->windows->len > 1) {
    for (guint i = 0; i < ql->windows->len; i++) {
      const struct ql_window *window = g_ptr_array_index(ql->windows, i);

      entries_divide(entries, &divide);

      struct ql_entry *entry = entry_new(entries, QL_WINDOW,
          window->title != NULL && window->title[0] != '\0' ? window->title
                                                            : "Window");
      entry->window = window->handle;
    }

    divide = true;
  }

  if (ql->offer_pin) {
    entries_divide(entries, &divide);

    if (ql->pinned) {
      entry_new(entries, QL_UNPIN, "Unpin from launcher");
    } else {
      entry_new(entries, QL_PIN, "Keep in launcher");
    }

    divide = false;
  }

  if (ql->running) {
    entries_divide(entries, &divide);
    entry_new(entries, QL_QUIT, "Quit");
  }

  return entries;
}

/* Layout ----------------------------------------------------------------- */

static int
level_max_scroll(const struct ql_level *level)
{
  int max = level->content_height - level->view_height;

  return max > 0 ? max : 0;
}

/* Function purpose: The tallest popup worth asking the compositor for. The
panel's layer surface spans the output top to bottom, so its own logical height
is the screen's; the output is the fallback for the window between creation and
the first configure. 0 means "no usable reading" -- better an over-tall request
the compositor corrects than a menu clamped to a number that was never a
height. */
static int
level_height_limit(const struct ql_level *level)
{
  const struct saber_surface *parent = level->ql->parent;
  int limit = 0;

  if (parent == NULL) {
    return 0;
  }

  if (parent->height > 0) {
    limit = parent->height;
  } else if (parent->output != NULL && parent->output->scale > 0) {
    limit = parent->output->height / parent->output->scale;
  }

  limit -= QL_SCREEN_MARGIN * 2;

  return limit >= QL_ROW_HEIGHT * 3 ? limit : 0;
}

static void
level_layout(struct ql_level *level)
{
  struct saber_quicklist *ql = level->ql;
  PangoLayout *layout = pango_cairo_create_layout(ql->measure_cr);

  pango_layout_set_font_description(layout, ql->font);

  bool marks = false, icons = false, arrows = false;
  int text = 0;
  int y = QL_PAD_Y;

  for (guint i = 0; i < level->entries->len; i++) {
    struct ql_entry *entry = g_ptr_array_index(level->entries, i);

    entry->y = y;
    entry->height =
        entry->kind == QL_SEPARATOR ? QL_SEPARATOR_HEIGHT : QL_ROW_HEIGHT;
    y += entry->height;

    if (entry->kind == QL_SEPARATOR) {
      continue;
    }

    marks = marks || entry->toggle_type != SABER_DBUSMENU_TOGGLE_NONE;
    icons = icons || entry->icon != NULL;
    arrows = arrows || entry->submenu;

    int width, height;
    pango_layout_set_text(layout, entry->label, -1);
    pango_layout_get_pixel_size(layout, &width, &height);

    if (width > text) {
      text = width;
    }
  }

  g_object_unref(layout);

  level->mark_column = marks ? QL_MARK + QL_GAP : 0;
  level->icon_column = icons ? QL_ICON + QL_GAP : 0;
  level->arrow_column = arrows ? QL_GAP + QL_ARROW : 0;

  if (text > QL_TEXT_MAX) {
    text = QL_TEXT_MAX;
  }

  int width = QL_PAD_X * 2 + level->mark_column + level->icon_column + text +
      level->arrow_column;

  int content = y + QL_PAD_Y;
  int limit = level_height_limit(level);

  level->width = CLAMP(width, QL_MIN_WIDTH, QL_MAX_WIDTH);
  level->content_height = content;
  level->height = limit > 0 && content > limit ? limit : content;

  /* Action purpose: Before any configure, what was asked for is the best guess
  at what will be shown. After one, a correction the compositor already made is
  kept -- saber_popup_reposition answers false below xdg_popup v3, so a reflow
  that overwrote it there would restore the belief that the whole menu fits and
  put the bottom rows back out of reach. */
  if (level->view_height <= 0 || level->view_height > level->height) {
    level->view_height = level->height;
  }

  level->scroll = CLAMP(level->scroll, 0, level_max_scroll(level));
}

/* Drawing ---------------------------------------------------------------- */

static void
draw_icon(cairo_t *cr, cairo_surface_t *icon, double x, double y, double size)
{
  double width = cairo_image_surface_get_width(icon);
  double height = cairo_image_surface_get_height(icon);

  if (width <= 0.0 || height <= 0.0) {
    return;
  }

  double scale = size / (width > height ? width : height);

  cairo_save(cr);
  cairo_translate(cr, x + (size - width * scale) / 2.0,
      y + (size - height * scale) / 2.0);
  cairo_scale(cr, scale, scale);
  cairo_set_source_surface(cr, icon, 0.0, 0.0);
  cairo_paint(cr);
  cairo_restore(cr);
}

static void
draw_mark(cairo_t *cr, const struct ql_entry *entry, double x, double y)
{
  double size = QL_MARK;

  if (entry->toggle_state != 1) {
    /* An indeterminate toggle is a real state an application can publish, and
    is drawn as such rather than as "off". */
    if (entry->toggle_state != SABER_DBUSMENU_TOGGLE_INDETERMINATE) {
      return;
    }

    cairo_move_to(cr, x + 2.0, y + size / 2.0);
    cairo_line_to(cr, x + size - 2.0, y + size / 2.0);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    return;
  }

  if (entry->toggle_type == SABER_DBUSMENU_TOGGLE_RADIO) {
    cairo_arc(cr, x + size / 2.0, y + size / 2.0, size / 4.0, 0.0, 2.0 * G_PI);
    cairo_fill(cr);
    return;
  }

  cairo_move_to(cr, x + 2.0, y + size / 2.0);
  cairo_line_to(cr, x + size / 2.0 - 1.0, y + size - 3.0);
  cairo_line_to(cr, x + size - 2.0, y + 3.0);
  cairo_set_line_width(cr, 1.8);
  cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
  cairo_stroke(cr);
}

/* The arrow points the way the submenu opens, which is away from the panel. */
static void
draw_arrow(cairo_t *cr, enum saber_edge edge, double x, double y)
{
  double size = QL_ARROW;

  if (edge == SABER_EDGE_RIGHT) {
    cairo_move_to(cr, x + size, y);
    cairo_line_to(cr, x, y + size / 2.0);
    cairo_line_to(cr, x + size, y + size);
  } else {
    cairo_move_to(cr, x, y);
    cairo_line_to(cr, x + size, y + size / 2.0);
    cairo_line_to(cr, x, y + size);
  }

  cairo_close_path(cr);
  cairo_fill(cr);
}

static void
level_render(void *data,
    struct saber_popup *popup,
    cairo_t *cr,
    int width,
    int height)
{
  (void)popup;

  struct ql_level *level = data;

  /* Action purpose: The whole level is drawn into a group and composited at the
  reveal tween's alpha, so it fades up as one thing. Compositing once at the end
  rather than scaling every colour keeps the rows, icons and text in step, and
  keeps every other draw below ignorant of the fact that the menu animates.

  Balanced by the pop at the end of the function, which is safe because this
  function has no early return. */
  double reveal = saber_tween_value(&level->reveal);
  bool grouped = reveal < 1.0;

  if (grouped) {
    cairo_push_group(cr);
  }
  const struct saber_theme *theme = &level->ql->theme;

  saber_theme_set_source(cr, &theme->background);
  cairo_rectangle(cr, 0.0, 0.0, width, height);
  cairo_fill(cr);

  saber_theme_set_source(cr, &theme->dim);
  cairo_set_line_width(cr, 1.0);
  cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
  cairo_stroke(cr);

  /* Action purpose: Rows are laid out in content coordinates and slid up by the
  offset here, clipped to the inside of the frame so a half-scrolled row cannot
  paint over the border. level_at applies the SAME offset -- the two disagreeing
  is what makes a menu whose highlight sits on a different row from the pointer,
  and it is why the offset must never be applied in only one of them. */
  cairo_save(cr);
  cairo_rectangle(cr, 1.0, 1.0, width - 2.0, height - 2.0);
  cairo_clip(cr);
  cairo_translate(cr, 0.0, -(double)level->scroll);

  PangoLayout *layout = pango_cairo_create_layout(cr);
  pango_layout_set_font_description(layout, level->ql->font);
  pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);

  double text_x = QL_PAD_X + level->mark_column + level->icon_column;
  double text_width =
      width - text_x - QL_PAD_X - level->arrow_column;

  if (text_width < 1.0) {
    text_width = 1.0;
  }

  pango_layout_set_width(layout, (int)(text_width * PANGO_SCALE));

  for (guint i = 0; i < level->entries->len; i++) {
    const struct ql_entry *entry = g_ptr_array_index(level->entries, i);

    if (entry->kind == QL_SEPARATOR) {
      double y = entry->y + entry->height / 2.0 + 0.5;

      saber_theme_set_source(cr, &theme->dim);
      cairo_set_line_width(cr, 1.0);
      cairo_move_to(cr, QL_PAD_X, y);
      cairo_line_to(cr, width - QL_PAD_X, y);
      cairo_stroke(cr);
      continue;
    }

    bool highlighted =
        entry->enabled && ((int)i == level->hovered || (int)i == level->selected);
    const struct saber_color *ink = &theme->foreground;

    if (highlighted) {
      saber_theme_set_source(cr, &theme->accent);
      cairo_rectangle(cr, 1.0, entry->y, width - 2.0, entry->height);
      cairo_fill(cr);
      ink = &theme->badge_fg;
    } else if (!entry->enabled) {
      ink = &theme->dim;
    }

    saber_theme_set_source(cr, ink);

    if (level->mark_column > 0 &&
        entry->toggle_type != SABER_DBUSMENU_TOGGLE_NONE) {
      draw_mark(cr, entry, QL_PAD_X,
          entry->y + (entry->height - QL_MARK) / 2.0);
      saber_theme_set_source(cr, ink);
    }

    if (entry->icon != NULL) {
      draw_icon(cr, entry->icon, QL_PAD_X + level->mark_column,
          entry->y + (entry->height - QL_ICON) / 2.0, QL_ICON);
      saber_theme_set_source(cr, ink);
    }

    int label_width, label_height;
    pango_layout_set_text(layout, entry->label, -1);
    pango_layout_get_pixel_size(layout, &label_width, &label_height);

    cairo_move_to(cr, text_x, entry->y + (entry->height - label_height) / 2.0);
    pango_cairo_show_layout(cr, layout);

    if (entry->submenu) {
      draw_arrow(cr, level->ql->edge, width - QL_PAD_X - QL_ARROW,
          entry->y + (entry->height - QL_ARROW) / 2.0);
    }
  }

  g_object_unref(layout);
  cairo_restore(cr);
  if (grouped) {
    cairo_pop_group_to_source(cr);
    cairo_paint_with_alpha(cr, reveal);
  }
}

/* Levels ----------------------------------------------------------------- */

static void
level_close_children(struct ql_level *level)
{
  /* Action purpose: Bumped even when there is no child. A row whose AboutToShow
  is still in flight has no child yet, and its reply must be dropped just the
  same once the pointer has moved on or the rows have been recomposed
  underneath it -- the index it recorded would otherwise name a different row.
  Every path that closes a submenu or replaces a level's rows comes through
  here, which is what makes the serial sufficient on its own. */
  level->open_serial++;

  if (level->child == NULL) {
    return;
  }

  level_destroy(level->child);
  level->child = NULL;
  level->open = -1;
}

/* Function purpose: The one way a level's offset changes. Everything that can
scroll -- the wheel, the keyboard cursor leaving the view -- comes through here
so the clamp and the submenu teardown cannot be forgotten at one of them. */
static void
level_scroll_to(struct ql_level *level, int scroll)
{
  int next = CLAMP(scroll, 0, level_max_scroll(level));

  if (next == level->scroll) {
    return;
  }

  /* Action purpose: A submenu is anchored to the parent row it hangs off, and
  that row has just moved. Repositioning it would need the anchor recomputed on
  every scroll step; closing it is both honest and what leaving the row would
  have done anyway. */
  level_close_children(level);

  level->scroll = next;
  saber_popup_damage(level->popup);
}

/* Function purpose: Bring a row inside the view. The keyboard cursor can now
walk off the bottom of a clamped menu, and a selection that cannot be seen is a
menu that looks frozen. */
static void
level_reveal(struct ql_level *level, int index)
{
  if (index < 0 || index >= (int)level->entries->len) {
    return;
  }

  const struct ql_entry *entry = g_ptr_array_index(level->entries, index);
  int scroll = level->scroll;

  if (entry->y < scroll) {
    scroll = entry->y;
  } else if (entry->y + entry->height > scroll + level->view_height) {
    scroll = entry->y + entry->height - level->view_height;
  }

  level_scroll_to(level, scroll);
}

/* Action purpose: Nested popups must be destroyed innermost first, or the
compositor raises xdg_wm_base.not_the_topmost_popup and disconnects us. */
static void
level_destroy(struct ql_level *level)
{
  if (level == NULL) {
    return;
  }

  level_destroy(level->child);

  /* Action purpose: Cancel before anything is freed. An AboutToShow issued for
  this level holds it as its user data and can still complete afterwards, so
  this is the only thing standing between a slow application and a write into
  freed memory. */
  g_cancellable_cancel(level->cancellable);
  g_object_unref(level->cancellable);

  if (level->ql->pointer_level == level) {
    level->ql->pointer_level = NULL;
  }

  saber_popup_destroy(level->popup);
  saber_clock_destroy(level->clock);
  g_ptr_array_unref(level->entries);
  g_free(level);
}

/* Function purpose: Take the size the compositor settled on. The positioner
asks for SLIDE_Y then RESIZE_Y, so a menu that would run off the bottom comes
back SHORTER than it asked for -- and this discarded that answer entirely until
Phase 12, leaving the level convinced it had its full height and every row past
the real bottom edge drawn outside the buffer, invisible and unclickable. */
static void
level_configure(void *data, struct saber_popup *popup, int width, int height)
{
  (void)popup;
  (void)width;

  struct ql_level *level = data;

  if (height <= 0 || height == level->view_height) {
    return;
  }

  level->view_height = height;
  level->scroll = CLAMP(level->scroll, 0, level_max_scroll(level));

  if (level->popup != NULL) {
    saber_popup_damage(level->popup);
  }
}

static void
level_done(void *data, struct saber_popup *popup)
{
  (void)popup;

  struct ql_level *level = data;

  /* Action purpose: read the parent before closing it. level_close_children
  reaches level_destroy on the parent's current child -- which is this level --
  and ends in g_free, so every later dereference of `level` would be a read of
  freed memory. The compositor delivers popup_done innermost first, so this is
  the ordinary path, not an edge case. */
  struct ql_level *parent = level->parent;
  struct saber_quicklist *ql = level->ql;

  if (parent != NULL) {
    level_close_children(parent);
    saber_popup_damage(parent->popup);
    return;
  }

  saber_quicklist_close(ql);
}

static int
ql_anim_ms(const struct saber_quicklist *ql)
{
  return ql != NULL && ql->config_anim_ms > 0 ? ql->config_anim_ms : 0;
}

/* Function purpose: Advance a level's clock on the compositor's frame timing.
Damaging from here asks for the next frame; returning without damaging lets the
loop stop. The one tween on this clock is one-shot, so it always stops. */
static void
level_frame(void *data, uint32_t time)
{
  struct ql_level *level = data;

  if (level->popup == NULL) {
    return;
  }

  if (saber_clock_advance(level->clock,
          saber_clock_stamp(level->clock, time))) {
    saber_popup_damage(level->popup);
  }
}

static const struct saber_popup_listener level_popup_listener = {
  .configure = level_configure,
  .frame = level_frame,
  .render = level_render,
  .done = level_done,
};

static struct ql_level *
level_create(struct saber_quicklist *ql,
    struct ql_level *parent,
    GPtrArray *entries,
    int32_t anchor_x,
    int32_t anchor_y,
    int32_t anchor_width,
    int32_t anchor_height)
{
  struct ql_level *level = g_new0(struct ql_level, 1);

  level->ql = ql;
  level->parent = parent;
  level->entries = entries;
  level->hovered = -1;
  level->clock = saber_clock_create();
  saber_tween_init(&level->reveal, 1.0);
  saber_clock_add(level->clock, &level->reveal);
  saber_tween_start(&level->reveal, 0.0, 1.0, ql_anim_ms(ql),
      SABER_EASE_OUT_CUBIC, 0);
  level->selected = -1;
  level->open = -1;
  level->cancellable = g_cancellable_new();
  level->anchor_x = anchor_x;
  level->anchor_y = anchor_y;
  level->anchor_width = anchor_width;
  level->anchor_height = anchor_height;

  level_layout(level);

  struct saber_popup_params params;
  saber_popup_menu_params(&params, ql->edge, level->width, level->height,
      anchor_x, anchor_y, anchor_width, anchor_height);

  /* Action purpose: Only the root takes the grab. It already routes every
  event of ours to us and dismisses the whole chain on an outside click, and a
  nested grab would have to quote a serial that a hover never produced -- which
  a compositor is entitled to refuse, dismissing the submenu on sight. */
  params.grab = parent == NULL;
  params.grab_serial = ql->serial;

  level->popup = parent == NULL
      ? saber_popup_create(ql->parent, &params, &level_popup_listener, level)
      : saber_popup_create_nested(parent->popup, &params,
            &level_popup_listener, level);

  if (level->popup == NULL) {
    g_object_unref(level->cancellable);
    /* The clock was created and had the reveal tween registered on it before
    the popup was asked for, so this path owns it exactly as level_destroy
    does. Without it the clock and its tween array leak on every menu the
    compositor declines to map. */
    saber_clock_destroy(level->clock);
    g_ptr_array_unref(entries);
    g_free(level);
    return NULL;
  }

  return level;
}

/* Function purpose: Re-measure a level whose rows have just been replaced and
move its popup to match. A level that grew or shrank and was left at its old
size is either clipped or padded with empty space, and a nested one has to be
re-anchored against the parent row it hangs off. */
static void
level_reflow(struct ql_level *level)
{
  struct saber_quicklist *ql = level->ql;

  level_layout(level);

  struct saber_popup_params params;
  saber_popup_menu_params(&params, ql->edge, level->width, level->height,
      level->anchor_x, level->anchor_y, level->anchor_width,
      level->anchor_height);
  saber_popup_reposition(level->popup, &params);

  saber_popup_damage(level->popup);
}

/* Action purpose: One AboutToShow in flight. The cancellable is the level's
own, held by reference so this can still be asked after the level is gone --
the shape sni.c's prop_fetch uses, for the same reason. `serial` and `index`
answer the softer question the cancellable cannot: the level is alive, but is
it still opening the row this call was issued for? */
struct ql_submenu_request {
  struct ql_level *level;
  GCancellable *cancellable;

  guint serial;
  int index;
  int32_t dbusmenu_id;
};

static struct ql_submenu_request *
submenu_request_new(struct ql_level *level, int index, int32_t dbusmenu_id)
{
  struct ql_submenu_request *request = g_new0(struct ql_submenu_request, 1);

  request->level = level;
  request->cancellable = g_object_ref(level->cancellable);
  request->serial = level->open_serial;
  request->index = index;
  request->dbusmenu_id = dbusmenu_id;

  return request;
}

static void
submenu_request_free(gpointer data)
{
  struct ql_submenu_request *request = data;

  g_object_unref(request->cancellable);
  g_free(request);
}

/* Function purpose: Apply an AboutToShow reply that re-read the layout. The
submenu is usually already on screen with the children the old tree held, so
the new ones replace its rows in place rather than replacing the popup -- which
is what makes an application that answers TRUE on every open look no different
from one that answers FALSE. */
static void
submenu_apply(struct ql_submenu_request *request,
    const struct saber_dbusmenu_item *item)
{
  struct ql_level *level = request->level;
  GPtrArray *entries = item != NULL
      ? entries_from_children(item, level->ql->icons)
      : g_ptr_array_new_with_free_func(entry_free);

  if (level->child != NULL) {
    if (entries->len == 0) {
      /* The row lost its children outright; leaving the old ones up would be a
      menu that lies about what it will do. */
      g_ptr_array_unref(entries);
      level_close_children(level);
      saber_popup_damage(level->popup);

      return;
    }

    struct ql_level *child = level->child;

    /* A grandchild was opened from rows that are about to be thrown away, and
    closing it here is also what invalidates any reply still in flight for
    it. */
    level_close_children(child);

    g_ptr_array_unref(child->entries);
    child->entries = entries;
    child->hovered = -1;
    child->selected = -1;

    level_reflow(child);

    return;
  }

  /* Nothing was drawn when the call went out -- an application that populates
  lazily has only now said what the row contains. */
  if (entries->len == 0) {
    g_ptr_array_unref(entries);

    return;
  }

  const struct ql_entry *entry =
      g_ptr_array_index(level->entries, request->index);

  /* The anchor is where the row is DRAWN, not where it sits in the content, or
  a submenu opened from a scrolled menu points at empty space. */
  level->child = level_create(level->ql, level, entries, 0,
      entry->y - level->scroll, level->width, entry->height);

  if (level->child != NULL) {
    level->open = request->index;
  }
}

static void
on_about_to_show(struct saber_dbusmenu *menu, bool refreshed, void *user)
{
  struct ql_submenu_request *request = user;

  /* Action purpose: Orphan check before anything reads request->level. The
  level, or the whole quicklist under it, can be destroyed while an application
  takes its time answering, and request->level is dead memory when that has
  happened. */
  if (g_cancellable_is_cancelled(request->cancellable)) {
    return;
  }

  /* Not refreshed means the application either declined AboutToShow or said
  the tree is unchanged; the children already on screen are the right ones. */
  if (!refreshed) {
    return;
  }

  struct ql_level *level = request->level;

  if (request->serial != level->open_serial ||
      request->index >= (int)level->entries->len) {
    return;
  }

  submenu_apply(request, saber_dbusmenu_find(menu, request->dbusmenu_id));
}

static void
level_open_submenu(struct ql_level *level, int index)
{
  struct saber_quicklist *ql = level->ql;
  struct ql_entry *entry = g_ptr_array_index(level->entries, index);

  if (!entry->submenu || entry->kind != QL_DBUSMENU || ql->menu == NULL) {
    return;
  }

  if (level->open == index) {
    return;
  }

  level_close_children(level);

  /* Action purpose: Draw what the layout already holds before asking, not
  after. AboutToShow is a round trip to an arbitrary application, and a submenu
  that waited for it would take that application's reply time to appear -- on
  every hover, since this is reached from pointer motion. The reply can only
  correct what goes up here, never contradict the fact that a submenu opened.
  A row whose children the application has not published yet still draws
  nothing until they arrive, because there is nothing to draw. */
  const struct saber_dbusmenu_item *item =
      saber_dbusmenu_find(ql->menu, entry->dbusmenu_id);
  GPtrArray *entries = item != NULL
      ? entries_from_children(item, level->ql->icons)
      : g_ptr_array_new_with_free_func(entry_free);

  if (entries->len > 0) {
    level->child = level_create(ql, level, entries, 0,
        entry->y - level->scroll, level->width, entry->height);

    if (level->child != NULL) {
      level->open = index;
    }
  } else {
    g_ptr_array_unref(entries);
  }

  /* Action purpose: AboutToShow is the one notice an application that builds
  its menu lazily ever gets, and a TRUE reply means it changed the tree -- the
  layout is re-read before `refreshed` comes back true, so the children are
  looked up again in the callback rather than here. */
  saber_dbusmenu_about_to_show_async(ql->menu, entry->dbusmenu_id,
      on_about_to_show, submenu_request_new(level, index, entry->dbusmenu_id),
      submenu_request_free);
}

/* Activation ------------------------------------------------------------- */

static void
level_activate(struct ql_level *level, int index)
{
  struct saber_quicklist *ql = level->ql;
  struct ql_entry *entry = g_ptr_array_index(level->entries, index);

  if (entry->kind == QL_SEPARATOR || !entry->enabled) {
    return;
  }

  if (entry->submenu) {
    level_open_submenu(level, index);
    return;
  }

  /* Action purpose: Event() is the whole of activation under DBusMenu -- the
  application's own reaction is the only feedback the protocol offers. */
  if (entry->kind == QL_DBUSMENU) {
    saber_dbusmenu_event(ql->menu, entry->dbusmenu_id);
  } else if (entry->kind == QL_ACTION && ql->app != NULL) {
    /* Action purpose: With an activation token, so a desktop action's window
    raises itself instead of arriving urgent and needing a second click. The
    reply is asynchronous and the menu is about to be torn down, so the request
    carries its own copy of everything it needs. */
    struct ql_action_launch *pending = g_new0(struct ql_action_launch, 1);

    pending->app = saber_appinfo_ref(ql->app);
    pending->action_id = g_strdup(entry->action_id);

    saber_display_request_activation(ql->display,
        ql->parent != NULL ? ql->parent->wl_surface : NULL, ql->app->id,
        ql_action_with_token, pending);
  }

  /* Action purpose: The owner's callbacks are invoked after the menu is gone,
  from copies taken here: closing frees the entry and the quicklist, and a
  handler is entitled to open another menu straight away. */
  enum ql_kind kind = entry->kind;
  void *window = entry->window;
  struct saber_quicklist_handlers handlers = ql->handlers;
  void *user = ql->user;

  saber_quicklist_close(ql);

  switch (kind) {
  case QL_WINDOW:
    if (handlers.window != NULL) {
      handlers.window(user, window);
    }
    break;
  case QL_PIN:
    if (handlers.action != NULL) {
      handlers.action(user, SABER_QUICKLIST_PIN);
    }
    break;
  case QL_UNPIN:
    if (handlers.action != NULL) {
      handlers.action(user, SABER_QUICKLIST_UNPIN);
    }
    break;
  case QL_QUIT:
    if (handlers.action != NULL) {
      handlers.action(user, SABER_QUICKLIST_QUIT);
    }
    break;
  default:
    break;
  }
}

/* Input ------------------------------------------------------------------ */

static struct ql_level *
level_from_surface(struct saber_quicklist *ql, struct wl_surface *surface)
{
  for (struct ql_level *level = ql->root; level != NULL;
      level = level->child) {
    if (level->popup != NULL && level->popup->wl_surface == surface) {
      return level;
    }
  }

  return NULL;
}

static struct ql_level *
level_deepest(struct saber_quicklist *ql)
{
  struct ql_level *level = ql->root;

  while (level != NULL && level->child != NULL) {
    level = level->child;
  }

  return level;
}

static int
level_at(const struct ql_level *level, double y)
{
  /* The same offset level_render slides the rows by, in the other direction:
  the caller's `y` is where the pointer is on the popup, the entries are laid
  out in content coordinates. */
  double content_y = y + level->scroll;

  for (guint i = 0; i < level->entries->len; i++) {
    const struct ql_entry *entry = g_ptr_array_index(level->entries, i);

    if (content_y >= entry->y && content_y < entry->y + entry->height) {
      return entry->kind == QL_SEPARATOR ? -1 : (int)i;
    }
  }

  return -1;
}

static void
level_hover(struct ql_level *level, double y)
{
  int index = level_at(level, y);

  if (index == level->hovered) {
    return;
  }

  level->hovered = index;
  level->selected = index;
  saber_popup_damage(level->popup);

  if (index < 0) {
    return;
  }

  const struct ql_entry *entry = g_ptr_array_index(level->entries, index);

  if (entry->submenu) {
    level_open_submenu(level, index);
  } else {
    level_close_children(level);
  }
}

static void
quicklist_pointer_enter(void *data,
    struct wl_surface *surface,
    double x,
    double y)
{
  (void)x;

  struct saber_quicklist *ql = data;
  struct ql_level *level = level_from_surface(ql, surface);

  ql->pointer_level = level;
  ql->pointer_y = y;
  saber_scroll_reset(&ql->scroll);

  if (level != NULL) {
    level_hover(level, y);
  }
}

static void
quicklist_pointer_leave(void *data, struct wl_surface *surface)
{
  struct saber_quicklist *ql = data;
  struct ql_level *level = level_from_surface(ql, surface);

  if (level != NULL && level->hovered >= 0) {
    level->hovered = -1;
    saber_popup_damage(level->popup);
  }

  if (ql->pointer_level == level) {
    ql->pointer_level = NULL;
  }
}

static void
quicklist_pointer_motion(void *data, uint32_t time, double x, double y)
{
  (void)time;
  (void)x;

  struct saber_quicklist *ql = data;

  ql->pointer_y = y;

  if (ql->pointer_level != NULL) {
    level_hover(ql->pointer_level, y);
  }
}

/* Action purpose: The menu had no axis handler at all, so the rows a clamped
level could not show were unreachable by every route at once -- off the bottom
of the popup, out of the buffer, and with nothing bound to move them. Three rows
a notch, and the hover is re-resolved afterwards because the rows moved under a
pointer that did not. */
static void
quicklist_pointer_axis(void *data, uint32_t time, uint32_t axis, double value)
{
  (void)time;

  struct saber_quicklist *ql = data;
  struct ql_level *level = ql->pointer_level;

  if (level == NULL || axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
    return;
  }

  int steps = saber_scroll_steps(&ql->scroll,
      saber_scroll_delta(&ql->scroll, axis, value), 120);

  if (steps == 0) {
    return;
  }

  int before = level->scroll;

  level_scroll_to(level, before + steps * QL_SCROLL_ROWS * QL_ROW_HEIGHT);

  if (level->scroll != before) {
    level->hovered = -1;
    level_hover(level, ql->pointer_y);
  }
}

static void
quicklist_pointer_axis_value120(void *data, uint32_t axis, int32_t value120)
{
  struct saber_quicklist *ql = data;

  saber_scroll_detail(&ql->scroll, axis, value120);
}

static void
quicklist_pointer_axis_stop(void *data, uint32_t time, uint32_t axis)
{
  (void)time;

  struct saber_quicklist *ql = data;

  /* Action purpose: Only the axis this listener acts on ends its gesture here.
  quicklist_pointer_axis returns on the horizontal axis before taking a delta,
  so a reset answering a horizontal stop discarded the vertical remainder a
  half-finished gesture had earned. Resetting on the axis that did stop is
  correct: the gesture is over and the next one starts from zero. */
  if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL) {
    return;
  }

  saber_scroll_reset(&ql->scroll);
}

static void
quicklist_pointer_button(void *data,
    uint32_t time,
    uint32_t button,
    uint32_t state)
{
  (void)time;

  struct saber_quicklist *ql = data;

  if (button != BTN_LEFT) {
    return;
  }

  /* Action purpose: The press that opened the menu is often still in flight
  when the popup maps, so a bare release cannot be trusted -- an activation
  needs a press inside the menu first. */
  if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
    if (ql->pointer_level == NULL) {
      saber_quicklist_close(ql);
      return;
    }

    ql->pressed = true;
    return;
  }

  if (!ql->pressed) {
    return;
  }

  ql->pressed = false;

  struct ql_level *level = ql->pointer_level;

  if (level == NULL) {
    saber_quicklist_close(ql);
    return;
  }

  if (level->hovered >= 0) {
    level_activate(level, level->hovered);
  }
}

/* A menu owns one popup surface per open level, so ownership is the same walk
the event handlers already use to find which level was hit. */
static bool
quicklist_pointer_owns(void *data, struct wl_surface *surface)
{
  struct saber_quicklist *ql = data;

  return level_from_surface(ql, surface) != NULL;
}

static const struct saber_pointer_listener quicklist_pointer_listener = {
  .owns = quicklist_pointer_owns,
  .enter = quicklist_pointer_enter,
  .leave = quicklist_pointer_leave,
  .motion = quicklist_pointer_motion,
  .button = quicklist_pointer_button,
  .axis = quicklist_pointer_axis,
  .axis_value120 = quicklist_pointer_axis_value120,
  .axis_stop = quicklist_pointer_axis_stop,
};

static void
quicklist_keymap(void *data, uint32_t format, int fd, uint32_t size)
{
  struct saber_quicklist *ql = data;

  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
    close(fd);
    return;
  }

  char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);

  if (map == MAP_FAILED) {
    close(fd);
    return;
  }

  struct xkb_keymap *keymap = xkb_keymap_new_from_string(ql->xkb, map,
      XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);

  munmap(map, size);
  close(fd);

  if (keymap == NULL) {
    return;
  }

  xkb_state_unref(ql->xkb_state);
  xkb_keymap_unref(ql->keymap);

  ql->keymap = keymap;
  ql->xkb_state = xkb_state_new(keymap);
}

static void
quicklist_modifiers(void *data,
    uint32_t depressed,
    uint32_t latched,
    uint32_t locked,
    uint32_t group)
{
  struct saber_quicklist *ql = data;

  if (ql->xkb_state != NULL) {
    xkb_state_update_mask(ql->xkb_state, depressed, latched, locked, 0, 0,
        group);
  }
}

static int
level_step(const struct ql_level *level, int delta)
{
  int count = (int)level->entries->len;

  if (count == 0) {
    return -1;
  }

  int index = level->selected >= 0 ? level->selected : (delta > 0 ? -1 : 0);

  for (int i = 0; i < count; i++) {
    index = (index + delta + count) % count;

    const struct ql_entry *entry = g_ptr_array_index(level->entries, index);

    if (entry->kind != QL_SEPARATOR && entry->enabled) {
      return index;
    }
  }

  return -1;
}

static void
level_move(struct ql_level *level, int delta)
{
  int index = level_step(level, delta);

  if (index < 0 || index == level->selected) {
    return;
  }

  level->selected = index;
  level->hovered = -1;
  level_reveal(level, index);
  saber_popup_damage(level->popup);
}

static void
quicklist_key(void *data, uint32_t time, uint32_t key, uint32_t state)
{
  (void)time;

  struct saber_quicklist *ql = data;

  if (state != WL_KEYBOARD_KEY_STATE_PRESSED || ql->xkb_state == NULL) {
    return;
  }

  struct ql_level *level = level_deepest(ql);

  if (level == NULL) {
    return;
  }

  /* Wayland keycodes are evdev; xkb wants the X11 numbering. */
  xkb_keysym_t sym = xkb_state_key_get_one_sym(ql->xkb_state, key + 8);

  switch (sym) {
  case XKB_KEY_Escape:
    if (level->parent != NULL) {
      /* Action purpose: Latch the parent before closing it. level_deepest
      returned the innermost level, so this level IS parent->child --
      level_close_children reaches level_destroy on it and ends in g_free, and
      every later read of `level` would come out of the freed chunk. Same shape
      as level_done, for the same reason. */
      struct ql_level *parent = level->parent;

      level_close_children(parent);
      saber_popup_damage(parent->popup);
    } else {
      saber_quicklist_close(ql);
    }
    break;
  case XKB_KEY_Up:
    level_move(level, -1);
    break;
  case XKB_KEY_Down:
    level_move(level, 1);
    break;
  case XKB_KEY_Left:
    if (level->parent != NULL) {
      /* The same free-then-read hazard as Escape above, reached the same
      way. */
      struct ql_level *parent = level->parent;

      level_close_children(parent);
      saber_popup_damage(parent->popup);
    }
    break;
  case XKB_KEY_Right:
    if (level->selected >= 0) {
      level_open_submenu(level, level->selected);
    }
    break;
  case XKB_KEY_Return:
  case XKB_KEY_KP_Enter:
  case XKB_KEY_space:
    if (level->selected >= 0) {
      level_activate(level, level->selected);
    }
    break;
  default:
    break;
  }
}

static const struct saber_keyboard_listener quicklist_keyboard_listener = {
  .keymap = quicklist_keymap,
  .key = quicklist_key,
  .modifiers = quicklist_modifiers,
};

/* Lifecycle -------------------------------------------------------------- */

/* Function purpose: Recompose the root in place when the application
republishes its layout with the menu already on screen -- the alternative is a
menu that lies about what it will do. */
static void
quicklist_menu_updated(struct saber_dbusmenu *menu, void *user)
{
  (void)menu;

  struct saber_quicklist *ql = user;

  if (ql->closing || ql->root == NULL) {
    return;
  }

  level_close_children(ql->root);

  g_ptr_array_unref(ql->root->entries);
  ql->root->entries = quicklist_compose(ql);
  ql->root->hovered = -1;
  ql->root->selected = -1;

  level_reflow(ql->root);
}

static void
window_free(gpointer data)
{
  struct ql_window *window = data;

  g_free(window->title);
  g_free(window);
}

void
saber_quicklist_params_init(struct saber_quicklist_params *params)
{
  memset(params, 0, sizeof(*params));

  params->edge = SABER_EDGE_LEFT;
}

struct saber_quicklist *
saber_quicklist_open(const struct saber_quicklist_params *params,
    const struct saber_quicklist_handlers *handlers,
    void *user)
{
  if (params->parent == NULL || params->parent->display == NULL) {
    return NULL;
  }

  struct saber_quicklist *ql = g_new0(struct saber_quicklist, 1);

  ql->display = params->parent->display;
  ql->icons = params->icons;
  ql->config_anim_ms = params->animation_ms;
  ql->parent = params->parent;
  ql->edge = params->edge;
  ql->user = user;
  ql->serial = params->serial;
  ql->pinned = params->pinned;
  ql->running = params->running;
  ql->offer_pin = params->offer_pin;
  ql->anchor_x = params->anchor_x;
  ql->anchor_y = params->anchor_y;
  ql->anchor_width = params->anchor_width;
  ql->anchor_height = params->anchor_height;

  if (handlers != NULL) {
    ql->handlers = *handlers;
  }

  if (params->theme != NULL) {
    ql->theme = *params->theme;
  } else {
    saber_theme_init(&ql->theme, NULL, 1.0, 1.0);
  }

  if (params->app != NULL) {
    ql->app = saber_appinfo_ref(params->app);
  }

  ql->windows = g_ptr_array_new_with_free_func(window_free);

  for (size_t i = 0; i < params->windows_len; i++) {
    struct ql_window *window = g_new0(struct ql_window, 1);

    window->title = g_strdup(params->windows[i].title);
    window->handle = params->windows[i].handle;
    g_ptr_array_add(ql->windows, window);
  }

  ql->measure_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  ql->measure_cr = cairo_create(ql->measure_surface);
  ql->font = pango_font_description_from_string(QL_FONT);

  if (params->menu_bus_name != NULL && params->menu_bus_name[0] != '\0' &&
      params->menu_object_path != NULL &&
      params->menu_object_path[0] != '\0') {
    ql->menu = saber_dbusmenu_open(params->connection, params->menu_bus_name,
        params->menu_object_path, quicklist_menu_updated, ql);
  }

  ql->xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

  /* Action purpose: wl_keyboard.keymap fires once, when the keyboard is bound
  -- long before a menu exists -- so a menu that waited for it would never get
  one. The compiled default layout is enough for the four keys that drive a
  menu, and a real keymap replaces it if the event does arrive. */
  if (ql->xkb != NULL) {
    ql->keymap = xkb_keymap_new_from_names(ql->xkb, NULL,
        XKB_KEYMAP_COMPILE_NO_FLAGS);

    if (ql->keymap != NULL) {
      ql->xkb_state = xkb_state_new(ql->keymap);
    }
  }

  GPtrArray *entries = quicklist_compose(ql);

  if (entries->len == 0) {
    g_ptr_array_unref(entries);
    saber_quicklist_close(ql);
    return NULL;
  }

  /* Action purpose: Before the popup maps, not after -- a popup inherits the
  keyboard interactivity its parent layer surface had at the time, so raising
  it afterwards leaves the menu that is already on screen deaf. */
  saber_surface_hold_keyboard(ql->parent, true);
  ql->held = true;

  ql->root = level_create(ql, NULL, entries, params->anchor_x, params->anchor_y,
      params->anchor_width, params->anchor_height);

  if (ql->root == NULL) {
    saber_quicklist_close(ql);
    return NULL;
  }

  /* Action purpose: Pointer input is registered, not seized. display.c routes
  by surface, so a menu no longer blinds the panel underneath it -- which is
  what used to make scrolling dead everywhere on screen while any menu was
  open. The keyboard is still one slot and the menu is genuinely modal for it,
  so that keeps the save-and-restore. */
  ql->prev_keyboard = ql->display->keyboard_listener;
  ql->prev_keyboard_data = ql->display->keyboard_data;

  saber_display_add_pointer_listener(ql->display, &quicklist_pointer_listener,
      ql);
  saber_display_set_keyboard_listener(ql->display, &quicklist_keyboard_listener,
      ql);
  ql->listening = true;

  saber_display_flush(ql->display);

  return ql;
}

void
saber_quicklist_close(struct saber_quicklist *ql)
{
  /* Closing is re-entrant by design: a `closed` handler is free to call this
  again, and popup_done can arrive while an activation is already tearing the
  menu down. */
  if (ql == NULL || ql->closing) {
    return;
  }

  bool mapped = ql->root != NULL;

  ql->closing = true;

  level_destroy(ql->root);
  ql->root = NULL;

  if (ql->held) {
    saber_surface_hold_keyboard(ql->parent, false);
  }

  if (ql->listening) {
    saber_display_remove_pointer_listener(ql->display,
        &quicklist_pointer_listener, ql);
    saber_display_set_keyboard_listener(ql->display, ql->prev_keyboard,
        ql->prev_keyboard_data);
  }

  if (mapped) {
    saber_display_flush(ql->display);
  }

  saber_dbusmenu_close(ql->menu);
  saber_appinfo_unref(ql->app);

  if (ql->windows != NULL) {
    g_ptr_array_unref(ql->windows);
  }

  if (ql->font != NULL) {
    pango_font_description_free(ql->font);
  }

  if (ql->measure_cr != NULL) {
    cairo_destroy(ql->measure_cr);
  }

  if (ql->measure_surface != NULL) {
    cairo_surface_destroy(ql->measure_surface);
  }

  xkb_state_unref(ql->xkb_state);
  xkb_keymap_unref(ql->keymap);
  xkb_context_unref(ql->xkb);

  if (mapped && ql->handlers.closed != NULL) {
    ql->handlers.closed(ql->user);
  }

  g_free(ql);
}
