/* Script function and purpose: The drawing half of the panel -- the XDG icon
theme resolver and its cache, and the tile decoration rules of BLUEPRINT.md
5.2. Colours are read from struct saber_theme roles only; the one number that
is a literal here is a proportion of the tile, never a pixel value tied to a
particular icon size. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <librsvg/rsvg.h>
#include <pango/pangocairo.h>

#include <saber/render.h>
#include <saber/surface.h>

/* Tile decoration proportions, all relative to the tile box so that every
icon-size from 24 to 64 stays visually consistent. */
#define SABER_BACKLIGHT_INSET 0.04
#define SABER_BACKLIGHT_RADIUS 0.10
#define SABER_PIP_WIDTH 0.055
#define SABER_PIP_LENGTH 0.13
#define SABER_PIP_GAP 0.05
#define SABER_ARROW_DEPTH 0.10
#define SABER_ARROW_SPAN 0.20
#define SABER_BADGE_RADIUS 0.20
#define SABER_PROGRESS_HEIGHT 0.09
#define SABER_PROGRESS_INSET 0.12
#define SABER_SEPARATOR_INSET 0.20

#define SABER_HOVER_ALPHA 0.22
#define SABER_PRESSED_SCALE 0.92
#define SABER_URGENT_TINT 0.45
#define SABER_DIM_ALPHA 0.45

/* ------------------------------------------------------------------ icons */

/* One "Directories" entry of an index.theme, reduced to what the size-matching
rule in the icon theme specification actually consults. */
struct icon_dir {
  char *path; /* absolute: <base>/<theme>/<subdir> */
  int size, min, max, threshold;
  int type; /* 0 fixed, 1 scalable, 2 threshold */
};

struct icon_candidate {
  const struct icon_dir *dir;
  int distance;
  int order; /* theme search position, to keep the sort stable and meaningful */
};

struct saber_icons {
  GPtrArray *dirs;      /* struct icon_dir *, in theme search order */
  GPtrArray *fallbacks; /* char * flat directories: pixmaps */
  GHashTable *cache;    /* "name@size" -> cairo_surface_t *, NULL for a miss */
  bool scanned;
};

static void
icon_dir_free(gpointer data)
{
  struct icon_dir *dir = data;

  g_free(dir->path);
  g_free(dir);
}

static void
icon_cache_value_free(gpointer data)
{
  if (data != NULL) {
    cairo_surface_destroy(data);
  }
}

/* Function purpose: The base directories the specification searches, in its
order. /usr/local/share is where FreeBSD's ports install, and is normally
already in XDG_DATA_DIRS -- it is appended anyway because a session started
without a full environment would otherwise find no icons at all. */
static GPtrArray *
icon_base_dirs(void)
{
  GPtrArray *bases = g_ptr_array_new_with_free_func(g_free);

  const char *home = g_get_home_dir();

  if (home != NULL) {
    g_ptr_array_add(bases, g_build_filename(home, ".icons", NULL));
  }

  g_ptr_array_add(bases, g_build_filename(g_get_user_data_dir(), "icons", NULL));

  const gchar *const *system = g_get_system_data_dirs();

  for (int i = 0; system != NULL && system[i] != NULL; i++) {
    g_ptr_array_add(bases, g_build_filename(system[i], "icons", NULL));
  }

  g_ptr_array_add(bases, g_strdup("/usr/local/share/icons"));
  g_ptr_array_add(bases, g_strdup("/usr/share/icons"));

  for (guint i = bases->len; i-- > 0;) {
    for (guint j = 0; j < i; j++) {
      if (g_strcmp0(g_ptr_array_index(bases, i), g_ptr_array_index(bases, j)) ==
          0) {
        g_ptr_array_remove_index(bases, i);
        break;
      }
    }
  }

  return bases;
}

static char *
icon_theme_index(const GPtrArray *bases, const char *theme)
{
  for (guint i = 0; i < bases->len; i++) {
    char *path = g_build_filename(g_ptr_array_index(bases, i), theme,
        "index.theme", NULL);

    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
      return path;
    }

    g_free(path);
  }

  return NULL;
}

/* Function purpose: The user's theme name. Read from the environment first so
a session can override it, then from the GTK settings file that every FreeBSD
desktop writes, and finally nothing -- hicolor is appended unconditionally by
the caller and is the specification's guaranteed floor. */
static char *
icon_user_theme(void)
{
  const char *env = g_getenv("XDG_ICON_THEME");

  if (env != NULL && *env != '\0') {
    return g_strdup(env);
  }

  char *path =
      g_build_filename(g_get_user_config_dir(), "gtk-3.0", "settings.ini", NULL);
  GKeyFile *keys = g_key_file_new();
  char *name = NULL;

  if (g_key_file_load_from_file(keys, path, G_KEY_FILE_NONE, NULL)) {
    name = g_key_file_get_string(keys, "Settings", "gtk-icon-theme-name", NULL);
  }

  g_key_file_free(keys);
  g_free(path);

  return name;
}

static void
icon_collect_theme(struct saber_icons *icons,
    const GPtrArray *bases,
    const char *theme,
    GHashTable *seen,
    int depth);

/* Action purpose: Inheritance is followed depth-first and deduplicated, so a
theme that names its parent twice, or a cycle, costs nothing. Depth is bounded
because a hand-written index.theme is not a trusted input. */
static void
icon_collect_inherits(struct saber_icons *icons,
    const GPtrArray *bases,
    GKeyFile *keys,
    GHashTable *seen,
    int depth)
{
  gsize count = 0;
  char **inherits =
      g_key_file_get_string_list(keys, "Icon Theme", "Inherits", &count, NULL);

  for (gsize i = 0; inherits != NULL && i < count; i++) {
    icon_collect_theme(icons, bases, g_strstrip(inherits[i]), seen, depth + 1);
  }

  g_strfreev(inherits);
}

static void
icon_collect_theme(struct saber_icons *icons,
    const GPtrArray *bases,
    const char *theme,
    GHashTable *seen,
    int depth)
{
  if (theme == NULL || *theme == '\0' || depth > 8 ||
      g_hash_table_contains(seen, theme)) {
    return;
  }

  g_hash_table_add(seen, g_strdup(theme));

  char *index = icon_theme_index(bases, theme);

  if (index == NULL) {
    return;
  }

  GKeyFile *keys = g_key_file_new();

  /* Action purpose: An index.theme separates its lists with commas. GKeyFile
  defaults to the semicolon of the desktop-entry spec, and silently returns the
  entire Directories line as one unusable element if this is not set -- a whole
  icon theme lost with no error anywhere. */
  g_key_file_set_list_separator(keys, ',');

  if (!g_key_file_load_from_file(keys, index, G_KEY_FILE_NONE, NULL)) {
    g_key_file_free(keys);
    g_free(index);

    return;
  }

  gsize count = 0;
  char **subdirs =
      g_key_file_get_string_list(keys, "Icon Theme", "Directories", &count, NULL);

  for (gsize i = 0; subdirs != NULL && i < count; i++) {
    const char *sub = g_strstrip(subdirs[i]);
    int size = g_key_file_get_integer(keys, sub, "Size", NULL);

    if (size <= 0) {
      continue;
    }

    char *type = g_key_file_get_string(keys, sub, "Type", NULL);
    int threshold = g_key_file_get_integer(keys, sub, "Threshold", NULL);
    int min = g_key_file_get_integer(keys, sub, "MinSize", NULL);
    int max = g_key_file_get_integer(keys, sub, "MaxSize", NULL);

    for (guint b = 0; b < bases->len; b++) {
      char *path =
          g_build_filename(g_ptr_array_index(bases, b), theme, sub, NULL);

      if (!g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_free(path);
        continue;
      }

      struct icon_dir *dir = g_new0(struct icon_dir, 1);

      dir->path = path;
      dir->size = size;
      dir->min = min > 0 ? min : size;
      dir->max = max > 0 ? max : size;
      dir->threshold = threshold > 0 ? threshold : 2;
      dir->type = g_strcmp0(type, "Scalable") == 0 ? 1
          : g_strcmp0(type, "Fixed") == 0          ? 0
                                                   : 2;

      g_ptr_array_add(icons->dirs, dir);
    }

    g_free(type);
  }

  g_strfreev(subdirs);
  icon_collect_inherits(icons, bases, keys, seen, depth);
  g_key_file_free(keys);
  g_free(index);
}

static void
icon_scan(struct saber_icons *icons)
{
  if (icons->scanned) {
    return;
  }

  icons->scanned = true;

  GPtrArray *bases = icon_base_dirs();
  GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
  char *user = icon_user_theme();

  icon_collect_theme(icons, bases, user, seen, 0);
  /* Action purpose: Adwaita before hicolor. hicolor is the specification's
  floor but ships almost no named icons, so a session that has never chosen a
  theme -- no XDG_ICON_THEME and no GTK settings file, which is the ordinary
  state of a fresh hikari install -- would otherwise render a column of
  fallback letters. Adwaita is what GTK itself defaults to. */
  icon_collect_theme(icons, bases, "Adwaita", seen, 0);
  icon_collect_theme(icons, bases, "hicolor", seen, 0);

  /* Themes that ship no index.theme still resolve if their subdirectories are
  named the conventional way; and pixmaps is a flat directory with no theme at
  all. Both are probed last. */
  for (guint i = 0; i < bases->len; i++) {
    const char *base = g_ptr_array_index(bases, i);

    if (user != NULL) {
      g_ptr_array_add(icons->fallbacks, g_build_filename(base, user, NULL));
    }

    g_ptr_array_add(icons->fallbacks, g_build_filename(base, "hicolor", NULL));
  }

  const gchar *const *system = g_get_system_data_dirs();

  for (int i = 0; system != NULL && system[i] != NULL; i++) {
    g_ptr_array_add(icons->fallbacks, g_build_filename(system[i], "pixmaps",
        NULL));
  }

  g_ptr_array_add(icons->fallbacks, g_strdup("/usr/local/share/pixmaps"));
  g_ptr_array_add(icons->fallbacks, g_strdup("/usr/share/pixmaps"));

  g_free(user);
  g_hash_table_destroy(seen);
  g_ptr_array_free(bases, TRUE);
}

struct saber_icons *
saber_icons_create(void)
{
  struct saber_icons *icons = g_new0(struct saber_icons, 1);

  icons->dirs = g_ptr_array_new_with_free_func(icon_dir_free);
  icons->fallbacks = g_ptr_array_new_with_free_func(g_free);
  icons->cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
      icon_cache_value_free);

  return icons;
}

void
saber_icons_destroy(struct saber_icons *icons)
{
  if (icons == NULL) {
    return;
  }

  g_hash_table_destroy(icons->cache);
  g_ptr_array_free(icons->fallbacks, TRUE);
  g_ptr_array_free(icons->dirs, TRUE);
  g_free(icons);
}

/* The specification's DirectorySizeDistance, verbatim. */
static int
icon_dir_distance(const struct icon_dir *dir, int want)
{
  switch (dir->type) {
  case 1:
    if (want < dir->min) {
      return dir->min - want;
    }
    if (want > dir->max) {
      return want - dir->max;
    }
    return 0;

  case 2:
    if (want < dir->size - dir->threshold) {
      return dir->size - dir->threshold - want;
    }
    if (want > dir->size + dir->threshold) {
      return want - dir->size - dir->threshold;
    }
    return 0;

  default:
    return abs(dir->size - want);
  }
}

static int
icon_candidate_compare(gconstpointer a, gconstpointer b)
{
  const struct icon_candidate *left = a;
  const struct icon_candidate *right = b;

  if (left->distance != right->distance) {
    return left->distance - right->distance;
  }

  return left->order - right->order;
}

static char *
icon_probe(const char *dir, const char *name)
{
  static const char *const extensions[] = { ".svg", ".png", ".xpm" };

  for (size_t i = 0; i < G_N_ELEMENTS(extensions); i++) {
    char *file = g_strconcat(name, extensions[i], NULL);
    char *path = g_build_filename(dir, file, NULL);

    g_free(file);

    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
      return path;
    }

    g_free(path);
  }

  return NULL;
}

static char *
icon_resolve(struct saber_icons *icons, const char *name, int want)
{
  if (g_path_is_absolute(name)) {
    return g_file_test(name, G_FILE_TEST_IS_REGULAR) ? g_strdup(name) : NULL;
  }

  icon_scan(icons);

  GArray *candidates =
      g_array_sized_new(FALSE, FALSE, sizeof(struct icon_candidate),
          icons->dirs->len);

  for (guint i = 0; i < icons->dirs->len; i++) {
    const struct icon_dir *dir = g_ptr_array_index(icons->dirs, i);
    struct icon_candidate candidate = {
      .dir = dir,
      .distance = icon_dir_distance(dir, want),
      .order = (int)i,
    };

    g_array_append_val(candidates, candidate);
  }

  g_array_sort(candidates, icon_candidate_compare);

  char *found = NULL;

  for (guint i = 0; found == NULL && i < candidates->len; i++) {
    const struct icon_candidate *candidate =
        &g_array_index(candidates, struct icon_candidate, i);

    found = icon_probe(candidate->dir->path, name);
  }

  g_array_free(candidates, TRUE);

  for (guint i = 0; found == NULL && i < icons->fallbacks->len; i++) {
    found = icon_probe(g_ptr_array_index(icons->fallbacks, i), name);
  }

  return found;
}

cairo_surface_t *
saber_icon_from_argb32(const uint8_t *data,
    size_t length,
    int width,
    int height)
{
  if (data == NULL || width <= 0 || height <= 0) {
    return NULL;
  }

  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);

  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);

    return NULL;
  }

  int stride = cairo_image_surface_get_stride(surface);
  unsigned char *pixels = cairo_image_surface_get_data(surface);
  size_t needed = (size_t)width * (size_t)height * 4;

  if (length < needed) {
    cairo_surface_destroy(surface);

    return NULL;
  }

  for (int y = 0; y < height; y++) {
    memcpy(pixels + (size_t)y * (size_t)stride,
        data + (size_t)y * (size_t)width * 4, (size_t)width * 4);
  }

  cairo_surface_mark_dirty(surface);

  return surface;
}

/* Action purpose: gdk-pixbuf hands back straight (non-premultiplied) RGBA in
byte order; cairo wants premultiplied ARGB32 in native word order. Doing the
conversion here rather than linking gdk means no toolkit dependency for the
sake of one helper. */
static cairo_surface_t *
icon_from_pixbuf(GdkPixbuf *pixbuf, int size)
{
  int width = gdk_pixbuf_get_width(pixbuf);
  int height = gdk_pixbuf_get_height(pixbuf);
  int channels = gdk_pixbuf_get_n_channels(pixbuf);
  int rowstride = gdk_pixbuf_get_rowstride(pixbuf);
  const guchar *source = gdk_pixbuf_get_pixels(pixbuf);

  if (width <= 0 || height <= 0 || (channels != 3 && channels != 4)) {
    return NULL;
  }

  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);

  if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(surface);

    return NULL;
  }

  int stride = cairo_image_surface_get_stride(surface);
  unsigned char *pixels = cairo_image_surface_get_data(surface);
  int offset_x = (size - width) / 2;
  int offset_y = (size - height) / 2;

  for (int y = 0; y < height; y++) {
    int target_y = y + offset_y;

    if (target_y < 0 || target_y >= size) {
      continue;
    }

    const guchar *in = source + (size_t)y * (size_t)rowstride;
    uint32_t *out = (uint32_t *)(pixels + (size_t)target_y * (size_t)stride);

    for (int x = 0; x < width; x++) {
      int target_x = x + offset_x;

      if (target_x < 0 || target_x >= size) {
        continue;
      }

      const guchar *pixel = in + (size_t)x * (size_t)channels;
      uint32_t alpha = channels == 4 ? pixel[3] : 0xffu;
      uint32_t red = (pixel[0] * alpha + 127) / 255;
      uint32_t green = (pixel[1] * alpha + 127) / 255;
      uint32_t blue = (pixel[2] * alpha + 127) / 255;

      out[target_x] = (alpha << 24) | (red << 16) | (green << 8) | blue;
    }
  }

  cairo_surface_mark_dirty(surface);

  return surface;
}

/* Action purpose: The SVG is rendered into a viewport of exactly the device
pixel size rather than being rasterised once and scaled. On a 1.25 or 1.5 scale
output that is the difference between crisp glyph edges and a resampled blur,
which is the whole reason D-010 chose fractional scale plus viewporter. */
static cairo_surface_t *
icon_render_svg(const char *path, int size)
{
  GError *error = NULL;
  RsvgHandle *handle = rsvg_handle_new_from_file(path, &error);

  if (handle == NULL) {
    g_clear_error(&error);

    return NULL;
  }

  cairo_surface_t *surface =
      cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  cairo_t *cr = cairo_create(surface);
  RsvgRectangle viewport = {
    .x = 0.0,
    .y = 0.0,
    .width = (double)size,
    .height = (double)size,
  };

  bool ok = rsvg_handle_render_document(handle, cr, &viewport, &error);

  cairo_destroy(cr);
  g_object_unref(handle);

  if (!ok) {
    g_clear_error(&error);
    cairo_surface_destroy(surface);

    return NULL;
  }

  cairo_surface_mark_dirty(surface);

  return surface;
}

static cairo_surface_t *
icon_render(const char *path, int size)
{
  if (g_str_has_suffix(path, ".svg") || g_str_has_suffix(path, ".svgz")) {
    return icon_render_svg(path, size);
  }

  GError *error = NULL;
  GdkPixbuf *pixbuf =
      gdk_pixbuf_new_from_file_at_scale(path, size, size, TRUE, &error);

  if (pixbuf == NULL) {
    g_clear_error(&error);

    return NULL;
  }

  cairo_surface_t *surface = icon_from_pixbuf(pixbuf, size);

  g_object_unref(pixbuf);

  return surface;
}

cairo_surface_t *
saber_icons_lookup(struct saber_icons *icons, const char *name, int pixel_size)
{
  if (icons == NULL || name == NULL || *name == '\0' || pixel_size <= 0) {
    return NULL;
  }

  char *key = g_strdup_printf("%s@%d", name, pixel_size);
  gpointer cached = NULL;

  if (g_hash_table_lookup_extended(icons->cache, key, NULL, &cached)) {
    g_free(key);

    return cached;
  }

  char *path = icon_resolve(icons, name, pixel_size);
  cairo_surface_t *surface = path != NULL ? icon_render(path, pixel_size) : NULL;

  g_free(path);
  /* A miss is cached as NULL: an icon name that resolves to nothing must cost
  one directory walk, not one per frame. */
  g_hash_table_insert(icons->cache, key, surface);

  return surface;
}

/* ----------------------------------------------------------------- drawing */

void
saber_render_init(struct saber_render *render,
    const struct saber_config *config,
    const struct saber_theme *theme,
    struct saber_icons *icons)
{
  render->config = config;
  render->theme = theme;
  render->icons = icons;
  render->scale = 1.0;
  render->edge = config->panel.edge;
  render->icon_size = config->panel.icon_size;
  render->tile = saber_surface_panel_width_for(config);
}

static void
rounded_rect(cairo_t *cr,
    double x,
    double y,
    double width,
    double height,
    double radius)
{
  double limit = MIN(width, height) / 2.0;

  if (radius > limit) {
    radius = limit;
  }

  cairo_new_sub_path(cr);
  cairo_arc(cr, x + width - radius, y + radius, radius, -G_PI / 2.0, 0.0);
  cairo_arc(cr, x + width - radius, y + height - radius, radius, 0.0, G_PI / 2.0);
  cairo_arc(cr, x + radius, y + height - radius, radius, G_PI / 2.0, G_PI);
  cairo_arc(cr, x + radius, y + radius, radius, G_PI, 3.0 * G_PI / 2.0);
  cairo_close_path(cr);
}

static void
set_source(cairo_t *cr, const struct saber_color *color, double alpha)
{
  cairo_set_source_rgba(cr, color->r, color->g, color->b, color->a * alpha);
}

void
saber_render_column(const struct saber_render *render,
    cairo_t *cr,
    int width,
    int height)
{
  cairo_save(cr);
  cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
  set_source(cr, &render->theme->background, 1.0);
  cairo_rectangle(cr, 0.0, 0.0, (double)width, (double)height);
  cairo_fill(cr);
  cairo_restore(cr);
}

void
saber_render_separator(const struct saber_render *render,
    cairo_t *cr,
    double y,
    double width)
{
  double inset = width * SABER_SEPARATOR_INSET;

  set_source(cr, &render->theme->foreground, 0.35);
  cairo_rectangle(cr, inset, y, width - inset * 2.0, 1.0);
  cairo_fill(cr);
}

/* Function purpose: One pango layout, drawn centred on a point. The only two
places the panel draws text are the count badge and the sheet number, so this
stays private and takes its size in logical pixels like everything else. */
static void
draw_text(cairo_t *cr,
    double cx,
    double cy,
    double size,
    const char *text,
    const struct saber_color *color,
    bool bold)
{
  PangoLayout *layout = pango_cairo_create_layout(cr);
  PangoFontDescription *font = pango_font_description_new();

  pango_font_description_set_family(font, "Sans");
  pango_font_description_set_weight(font,
      bold ? PANGO_WEIGHT_BOLD : PANGO_WEIGHT_NORMAL);
  pango_font_description_set_absolute_size(font, size * PANGO_SCALE);

  pango_layout_set_font_description(layout, font);
  pango_layout_set_text(layout, text, -1);
  pango_font_description_free(font);

  int width = 0, height = 0;
  pango_layout_get_pixel_size(layout, &width, &height);

  set_source(cr, color, 1.0);
  cairo_move_to(cr, cx - width / 2.0, cy - height / 2.0);
  pango_cairo_show_layout(cr, layout);

  g_object_unref(layout);
}

/* True when the given side of the tile is the one nearest the screen edge.
Pips live there; the focus arrow lives opposite (BLUEPRINT.md 5.2). */
static bool
edge_is_left(const struct saber_render *render)
{
  return render->edge == SABER_EDGE_LEFT;
}

static void
draw_backlight(const struct saber_render *render,
    cairo_t *cr,
    const struct saber_tile *tile,
    double alpha)
{
  if (alpha <= 0.0) {
    return;
  }

  double inset = tile->width * SABER_BACKLIGHT_INSET;
  double radius = tile->width * SABER_BACKLIGHT_RADIUS;

  /* Action purpose: `dominant` would tint the fill to the icon's own dominant
  colour. Until that analysis exists it renders as `palette`, which is a
  correct-looking panel rather than a missing signifier. */
  const struct saber_color *color =
      render->config->theme.backlight == SABER_BACKLIGHT_OFF
      ? &render->theme->dim
      : &render->theme->backlight;

  rounded_rect(cr, tile->x + inset, tile->y + inset, tile->width - inset * 2.0,
      tile->height - inset * 2.0, radius);
  set_source(cr, color, alpha);
  cairo_fill(cr);
}

static void
draw_pips(const struct saber_render *render,
    cairo_t *cr,
    const struct saber_tile *tile)
{
  int marks = tile->windows > 3 ? 3 : tile->windows;

  if (marks <= 0) {
    return;
  }

  double thickness = tile->width * SABER_PIP_WIDTH;
  double length = tile->height * SABER_PIP_LENGTH;
  double gap = tile->height * SABER_PIP_GAP;
  double span = marks * length + (marks - 1) * gap;
  double y = tile->y + (tile->height - span) / 2.0;
  double x = edge_is_left(render) ? tile->x + thickness
                                  : tile->x + tile->width - thickness * 2.0;

  set_source(cr, &render->theme->foreground, 0.9);

  for (int i = 0; i < marks; i++) {
    rounded_rect(cr, x, y + i * (length + gap), thickness, length,
        thickness / 2.0);
    cairo_fill(cr);
  }
}

static void
draw_focus_arrow(const struct saber_render *render,
    cairo_t *cr,
    const struct saber_tile *tile)
{
  double depth = tile->width * SABER_ARROW_DEPTH;
  double span = tile->height * SABER_ARROW_SPAN;
  double cy = tile->y + tile->height / 2.0;
  double base = edge_is_left(render) ? tile->x + tile->width : tile->x;
  double tip = edge_is_left(render) ? base - depth : base + depth;

  cairo_move_to(cr, base, cy - span / 2.0);
  cairo_line_to(cr, tip, cy);
  cairo_line_to(cr, base, cy + span / 2.0);
  cairo_close_path(cr);

  set_source(cr, &render->theme->accent, 1.0);
  cairo_fill(cr);
}

static void
draw_badge(const struct saber_render *render,
    cairo_t *cr,
    const struct saber_tile *tile)
{
  if (!tile->badge.count_visible || tile->badge.count <= 0) {
    return;
  }

  double radius = tile->width * SABER_BADGE_RADIUS;
  /* Top corner on the side away from the screen edge, so the disc never sits
  under the pips. */
  double cx = edge_is_left(render) ? tile->x + tile->width - radius
                                   : tile->x + radius;
  double cy = tile->y + radius;

  cairo_arc(cr, cx, cy, radius, 0.0, 2.0 * G_PI);
  set_source(cr, tile->badge.urgent ? &render->theme->urgent
                                    : &render->theme->badge_bg,
      1.0);
  cairo_fill_preserve(cr);
  set_source(cr, &render->theme->badge_fg, 0.35);
  cairo_set_line_width(cr, 1.0);
  cairo_stroke(cr);

  char text[8];

  if (tile->badge.count > 999) {
    g_strlcpy(text, "999+", sizeof(text));
  } else {
    snprintf(text, sizeof(text), "%" G_GINT64_FORMAT, tile->badge.count);
  }

  draw_text(cr, cx, cy, radius * 1.15, text, &render->theme->badge_fg, true);
}

static void
draw_progress(const struct saber_render *render,
    cairo_t *cr,
    const struct saber_tile *tile)
{
  if (!tile->badge.progress_visible) {
    return;
  }

  double fraction = CLAMP(tile->badge.progress, 0.0, 1.0);
  double inset = tile->width * SABER_PROGRESS_INSET;
  double height = tile->height * SABER_PROGRESS_HEIGHT;
  double width = tile->width - inset * 2.0;
  double x = tile->x + inset;
  double y = tile->y + tile->height - height - inset / 2.0;

  rounded_rect(cr, x, y, width, height, height / 2.0);
  set_source(cr, &render->theme->dim, 0.8);
  cairo_fill(cr);

  if (fraction > 0.0) {
    rounded_rect(cr, x, y, width * fraction, height, height / 2.0);
    set_source(cr, &render->theme->progress, 1.0);
    cairo_fill(cr);
  }
}

static void
draw_icon(const struct saber_render *render,
    cairo_t *cr,
    const struct saber_tile *tile,
    double alpha)
{
  cairo_surface_t *surface = tile->icon;

  if (surface == NULL && tile->icon_name != NULL) {
    int pixels = (int)lround((double)render->icon_size * render->scale);

    surface = saber_icons_lookup(render->icons, tile->icon_name, pixels);
  }

  double size = (double)render->icon_size;
  double cx = tile->x + tile->width / 2.0;
  double cy = tile->y + tile->height / 2.0;

  if (surface == NULL) {
    if (tile->label != NULL) {
      draw_text(cr, cx, cy, size * 0.55, tile->label,
          &render->theme->foreground, true);
    }

    return;
  }

  /* Action purpose: The surface is sized in device pixels; the context is in
  logical ones. Scaling by the inverse is what makes an icon rendered at the
  exact device size land on whole pixels instead of being resampled. */
  int pixel_width = cairo_image_surface_get_width(surface);
  double factor = pixel_width > 0 ? size / (double)pixel_width : 1.0;

  cairo_save(cr);
  cairo_translate(cr, cx - size / 2.0, cy - size / 2.0);
  cairo_scale(cr, factor, factor);

  if (tile->symbolic) {
    /* Its alpha is the stencil; the colour comes from the theme, as every
    other mark on the tile does. */
    set_source(cr, &render->theme->foreground, alpha);
    cairo_mask_surface(cr, surface, 0.0, 0.0);
  } else {
    cairo_set_source_surface(cr, surface, 0.0, 0.0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint_with_alpha(cr, alpha);
  }

  cairo_restore(cr);
}

void
saber_render_tile(const struct saber_render *render,
    cairo_t *cr,
    const struct saber_tile *tile)
{
  double alpha = tile->dim ? SABER_DIM_ALPHA : 1.0;

  cairo_save(cr);

  /* The urgent wiggle and the launch throb are the only transforms applied to
  a whole tile; both arrive already evaluated from anim.c. */
  if (tile->wiggle != 0.0) {
    cairo_translate(cr, tile->wiggle, 0.0);
  }

  double scale = tile->throb > 0.0 ? tile->throb : 1.0;

  if (tile->pressed) {
    scale *= SABER_PRESSED_SCALE;
  }

  if (scale != 1.0) {
    double cx = tile->x + tile->width / 2.0;
    double cy = tile->y + tile->height / 2.0;

    cairo_translate(cr, cx, cy);
    cairo_scale(cr, scale, scale);
    cairo_translate(cr, -cx, -cy);
  }

  if (tile->running && render->config->theme.backlight != SABER_BACKLIGHT_OFF) {
    draw_backlight(render, cr, tile, tile->focused ? 0.85 : 0.6);
  }

  if (tile->hover > 0.0) {
    double inset = tile->width * SABER_BACKLIGHT_INSET;
    double radius = tile->width * SABER_BACKLIGHT_RADIUS;

    rounded_rect(cr, tile->x + inset, tile->y + inset,
        tile->width - inset * 2.0, tile->height - inset * 2.0, radius);
    set_source(cr, &render->theme->foreground,
        SABER_HOVER_ALPHA * tile->hover);
    cairo_fill(cr);
  }

  draw_icon(render, cr, tile, alpha);

  if (tile->badge.urgent) {
    double inset = tile->width * SABER_BACKLIGHT_INSET;
    double radius = tile->width * SABER_BACKLIGHT_RADIUS;

    rounded_rect(cr, tile->x + inset, tile->y + inset,
        tile->width - inset * 2.0, tile->height - inset * 2.0, radius);
    set_source(cr, &render->theme->urgent, SABER_URGENT_TINT);
    cairo_fill(cr);
  }

  draw_pips(render, cr, tile);

  if (tile->focused) {
    draw_focus_arrow(render, cr, tile);
  }

  draw_progress(render, cr, tile);
  draw_badge(render, cr, tile);

  cairo_restore(cr);
}
