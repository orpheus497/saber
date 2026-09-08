/* Script function and purpose: Colour parsing, the hikari.conf `ui { palette }`
import, and the mapping of those sixteen slots onto the semantic roles every
drawing module reads. */

#include <stdio.h>
#include <string.h>

#include <ucl.h>

#include <saber/theme.h>

/* Action purpose: The compiled-in default is hikari-sakura's shipped palette,
so a panel started with no hikari.conf and no configuration still comes up in
the desktop's own colours rather than in something invented here. */
static const char *const saber_fallback_palette[SABER_PALETTE_SLOTS] = {
  "#2b1e3a", /* base */
  "#c96464", /* red */
  "#df9f87", /* orange */
  "#e4b382", /* yellow */
  "#8e7cc3", /* violet */
  "#b18fc7", /* mauve */
  "#9fa0a6", /* grey */
  "#d4d4d9", /* text */
  "#5e5966", /* bright base */
  "#df8787", /* bright red */
  "#f2bda8", /* bright orange */
  "#f5cf9e", /* bright yellow */
  "#aba0d9", /* bright violet */
  "#cfaedc", /* bright mauve */
  "#b8b9be", /* bright grey */
  "#f0edf2", /* bright text */
};

/* The dash and spread backdrops sit under the panel's own translucency. */
#define SABER_OVERLAY_DIM 0.85

static int
hex_value(char c)
{
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }

  return -1;
}

bool
saber_color_parse(const char *spec, struct saber_color *out)
{
  if (spec == NULL || out == NULL || spec[0] != '#') {
    return false;
  }

  const char *digits = spec + 1;
  size_t len = strlen(digits);

  if (len != 3 && len != 6 && len != 8) {
    return false;
  }

  int nibble[8];

  for (size_t i = 0; i < len; i++) {
    nibble[i] = hex_value(digits[i]);

    if (nibble[i] < 0) {
      return false;
    }
  }

  /* Action purpose: "#rgb" is the same colour as "#rrggbb", so each nibble is
  doubled (n * 17) rather than merely shifted, which would darken every value
  by up to one part in sixteen. */
  if (len == 3) {
    out->r = (nibble[0] * 17) / 255.0;
    out->g = (nibble[1] * 17) / 255.0;
    out->b = (nibble[2] * 17) / 255.0;
    out->a = 1.0;

    return true;
  }

  out->r = ((nibble[0] << 4) | nibble[1]) / 255.0;
  out->g = ((nibble[2] << 4) | nibble[3]) / 255.0;
  out->b = ((nibble[4] << 4) | nibble[5]) / 255.0;
  out->a = len == 8 ? (((nibble[6] << 4) | nibble[7]) / 255.0) : 1.0;

  return true;
}

int
saber_theme_import_hikari(const char *path,
    struct saber_color palette[SABER_PALETTE_SLOTS])
{
  if (path == NULL || palette == NULL) {
    return 0;
  }

  struct ucl_parser *parser = ucl_parser_new(UCL_PARSER_NO_IMPLICIT_ARRAYS);

  if (parser == NULL) {
    return 0;
  }

  /* Action purpose: A missing or unreadable hikari.conf is the ordinary case
  for a first run and must cost nothing but the import; the caller falls back
  on a count below SABER_PALETTE_SLOTS. */
  if (!ucl_parser_add_file(parser, path)) {
    ucl_parser_free(parser);

    return 0;
  }

  ucl_object_t *root = ucl_parser_get_object(parser);

  ucl_parser_free(parser);

  if (root == NULL) {
    return 0;
  }

  const ucl_object_t *ui = ucl_object_lookup(root, "ui");
  const ucl_object_t *block =
      ui == NULL ? NULL : ucl_object_lookup(ui, "palette");
  int count = 0;

  for (int slot = 0; block != NULL && slot < SABER_PALETTE_SLOTS; slot++) {
    char key[16];

    snprintf(key, sizeof(key), "color%d", slot);

    const ucl_object_t *entry = ucl_object_lookup(block, key);
    const char *spec = entry == NULL ? NULL : ucl_object_tostring(entry);

    if (spec != NULL && saber_color_parse(spec, &palette[slot])) {
      count++;
    }
  }

  ucl_object_unref(root);

  return count;
}

void
saber_theme_init(struct saber_theme *theme,
    const struct saber_color *palette,
    double opacity,
    double overlay_opacity)
{
  if (theme == NULL) {
    return;
  }

  memset(theme, 0, sizeof(*theme));

  if (palette != NULL) {
    memcpy(theme->palette, palette, sizeof(theme->palette));
  } else {
    for (int slot = 0; slot < SABER_PALETTE_SLOTS; slot++) {
      if (!saber_color_parse(saber_fallback_palette[slot],
              &theme->palette[slot])) {
        theme->palette[slot].a = 1.0;
      }
    }
  }

  if (opacity < 0.0) {
    opacity = 0.0;
  } else if (opacity > 1.0) {
    opacity = 1.0;
  }

  theme->opacity = opacity;

  if (overlay_opacity < 0.0) {
    overlay_opacity = 0.0;
  } else if (overlay_opacity > 1.0) {
    overlay_opacity = 1.0;
  }

  theme->overlay_opacity = overlay_opacity;

  theme->background = theme->palette[0];
  theme->foreground = theme->palette[15];
  theme->accent = theme->palette[4];
  theme->backlight = theme->palette[8];
  theme->urgent = theme->palette[1];
  theme->badge_bg = theme->palette[1];
  theme->badge_fg = theme->palette[15];
  theme->progress = theme->palette[4];
  theme->dim = theme->palette[8];
  theme->overlay = theme->palette[0];

  /* Action purpose: Only the two surfaces the compositor composites through
  take the opacity setting. Applying it to foreground or badge roles would
  wash out text drawn *onto* an already translucent background. */
  theme->background.a *= opacity;
  theme->overlay.a *= opacity * SABER_OVERLAY_DIM;
}

void
saber_theme_set_source(cairo_t *cr, const struct saber_color *color)
{
  if (cr == NULL || color == NULL) {
    return;
  }

  cairo_set_source_rgba(cr, color->r, color->g, color->b, color->a);
}
