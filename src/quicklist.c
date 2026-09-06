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

#include <saber/dbusmenu.h>
#include <saber/display.h>
#include <saber/quicklist.h>

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
  int mark_column, icon_column, arrow_column; /* 0 when the level has none */

  int hovered;  /* pointer */
  int selected; /* keyboard cursor */
  int open;     /* the row whose submenu is showing */
};

struct ql_window {
  char *title;
  void *handle;
};

struct saber_quicklist {
  struct saber_display *display;
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

  const struct saber_pointer_listener *prev_pointer;
  void *prev_pointer_data;
  const struct saber_keyboard_listener *prev_keyboard;
  void *prev_keyboard_data;

  struct saber_quicklist_handlers handlers;
  void *user;

  struct ql_level *pointer_level;
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

/* Action purpose: Only an absolute path is resolved. Icon-theme name lookup is
a panel-wide concern that belongs in the shared icon module, not in three
private copies; until that exists a themed name draws nothing rather than
something wrong. */
static cairo_surface_t *
icon_from_name(const char *name)
{
  if (name == NULL || name[0] == '\0' || !g_path_is_absolute(name)) {
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
entries_add_dbusmenu(GPtrArray *entries, const struct saber_dbusmenu_item *item)
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
    entry->icon = icon_from_name(item->icon_name);
  }
}

/* Function purpose: Build one level's rows from a DBusMenu item's children,
which is the only shape a submenu ever has. */
static GPtrArray *
entries_from_children(const struct saber_dbusmenu_item *item)
{
  GPtrArray *entries = g_ptr_array_new_with_free_func(entry_free);

  for (guint i = 0; item->children != NULL && i < item->children->len; i++) {
    entries_add_dbusmenu(entries, g_ptr_array_index(item->children, i));
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
      entries_add_dbusmenu(entries, g_ptr_array_index(root->children, i));
    }

    divide = entries->len > 0;
  }

  for (size_t i = 0; ql->app != NULL && i < ql->app->actions_len; i++) {
    const struct saber_appinfo_action *action = &ql->app->actions[i];

    entries_divide(entries, &divide);

    struct ql_entry *entry = entry_new(entries, QL_ACTION,
        action->name != NULL ? action->name : action->id);
    entry->action_id = g_strdup(action->id);
    entry->icon = icon_from_name(action->icon);
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

  level->width = CLAMP(width, QL_MIN_WIDTH, QL_MAX_WIDTH);
  level->height = y + QL_PAD_Y;
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
  const struct saber_theme *theme = &level->ql->theme;

  saber_theme_set_source(cr, &theme->background);
  cairo_rectangle(cr, 0.0, 0.0, width, height);
  cairo_fill(cr);

  saber_theme_set_source(cr, &theme->dim);
  cairo_set_line_width(cr, 1.0);
  cairo_rectangle(cr, 0.5, 0.5, width - 1.0, height - 1.0);
  cairo_stroke(cr);

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
}

/* Levels ----------------------------------------------------------------- */

static void
level_close_children(struct ql_level *level)
{
  if (level->child == NULL) {
    return;
  }

  level_destroy(level->child);
  level->child = NULL;
  level->open = -1;
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

  if (level->ql->pointer_level == level) {
    level->ql->pointer_level = NULL;
  }

  saber_popup_destroy(level->popup);
  g_ptr_array_unref(level->entries);
  g_free(level);
}

static void
level_configure(void *data, struct saber_popup *popup, int width, int height)
{
  (void)popup;
  (void)width;
  (void)height;
  (void)data;
}

static void
level_done(void *data, struct saber_popup *popup)
{
  (void)popup;

  struct ql_level *level = data;

  if (level->parent != NULL) {
    level_close_children(level->parent);
    saber_popup_damage(level->parent->popup);
    return;
  }

  saber_quicklist_close(level->ql);
}

static const struct saber_popup_listener level_popup_listener = {
  .configure = level_configure,
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
  level->selected = -1;
  level->open = -1;

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
    g_ptr_array_unref(entries);
    g_free(level);
    return NULL;
  }

  return level;
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

  /* Action purpose: AboutToShow is the one notice an application that builds
  its menu lazily ever gets, and a TRUE reply means it changed the tree -- the
  layout has already been re-read by then, so the children must be looked up
  again afterwards rather than before. */
  saber_dbusmenu_about_to_show(ql->menu, entry->dbusmenu_id);

  const struct saber_dbusmenu_item *item =
      saber_dbusmenu_find(ql->menu, entry->dbusmenu_id);

  if (item == NULL || item->children == NULL || item->children->len == 0) {
    return;
  }

  GPtrArray *entries = entries_from_children(item);

  if (entries->len == 0) {
    g_ptr_array_unref(entries);
    return;
  }

  level->child = level_create(ql, level, entries, 0, entry->y, level->width,
      entry->height);

  if (level->child != NULL) {
    level->open = index;
  }
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
    saber_appinfo_launch(ql->app, entry->action_id, NULL, NULL);
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
  for (guint i = 0; i < level->entries->len; i++) {
    const struct ql_entry *entry = g_ptr_array_index(level->entries, i);

    if (y >= entry->y && y < entry->y + entry->height) {
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

  if (ql->pointer_level != NULL) {
    level_hover(ql->pointer_level, y);
  }
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

static const struct saber_pointer_listener quicklist_pointer_listener = {
  .enter = quicklist_pointer_enter,
  .leave = quicklist_pointer_leave,
  .motion = quicklist_pointer_motion,
  .button = quicklist_pointer_button,
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
      level_close_children(level->parent);
      saber_popup_damage(level->parent->popup);
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
      level_close_children(level->parent);
      saber_popup_damage(level->parent->popup);
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

  level_layout(ql->root);

  struct saber_popup_params params;
  saber_popup_menu_params(&params, ql->edge, ql->root->width, ql->root->height,
      ql->anchor_x, ql->anchor_y, ql->anchor_width, ql->anchor_height);
  saber_popup_reposition(ql->root->popup, &params);

  saber_popup_damage(ql->root->popup);
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
    saber_theme_init(&ql->theme, NULL, 1.0);
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

  /* Action purpose: The menu is modal, and display.c holds one listener of
  each kind. Taking both over for the menu's lifetime and putting the previous
  pair back on close is the only way to share them. */
  ql->prev_pointer = ql->display->pointer_listener;
  ql->prev_pointer_data = ql->display->pointer_data;
  ql->prev_keyboard = ql->display->keyboard_listener;
  ql->prev_keyboard_data = ql->display->keyboard_data;

  saber_display_set_pointer_listener(ql->display, &quicklist_pointer_listener,
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
    saber_display_set_pointer_listener(ql->display, ql->prev_pointer,
        ql->prev_pointer_data);
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
