/* Script function and purpose: Colour parsing, the hikari.conf `ui { palette }`
and `ui { animation }` imports, and the mapping of those sixteen slots onto the
semantic roles every drawing module reads. */

#include <math.h>
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

/* Action purpose: Urgency is a literal red held outside the palette, and there
are two of them because there cannot be one: the band of reds that stays legible
against the darkest ground the desktop ships does not overlap the band that
stays legible against the lightest, so a single value is a choice of which
theme to fail on. The dark pick is byte-identical to the shipped palette's
bright red, which is what urgency already resolved to there, so the default
theme is unchanged by the split. Both are overridable by name through
`theme { roles { urgent } }`, which is also the only way a dropped-in terminal
colour scheme can take this one role back. */
#define SABER_URGENT_ON_DARK "#df8787"
#define SABER_URGENT_ON_LIGHT "#9b2226"

/* A ground at or above this relative luminance is light. It sits far from every
palette the desktop ships -- the two dark grounds measure 0.008 and 0.018 and
the light one 0.694 -- so an unfamiliar palette is not being judged on a knife
edge either. */
#define SABER_LIGHT_GROUND 0.18

/* How far the backlight travels from the column towards its accent. Deriving
it is the point: slot 8 is the dim role, and reusing it for the backlight made
"running" and "unavailable" the same colour on every theme, so the running-tile
fill now sits a fixed distance from the column whatever the palette holds. The
fraction is set so the fill is no weaker than it has always been on the default
theme and is plainly visible on the light one. */
#define SABER_BACKLIGHT_MIX 0.45

/* The compositor validates its own duration over this range, and a value
outside it is a typo rather than an intention worth inheriting. */
#define SABER_HIKARI_DURATION_MAX 1000

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

/* Function purpose: WCAG 2.1 relative luminance. The semantic layer turns on
it three times over -- the light/dark discriminator, the accent pick and the
badge foreground pick are all settled by measuring the palette in hand rather
than by naming a slot that happened to suit one theme. */
static double
relative_luminance(const struct saber_color *color)
{
  const double channel[3] = { color->r, color->g, color->b };
  const double weight[3] = { 0.2126, 0.7152, 0.0722 };
  double luminance = 0.0;

  for (int i = 0; i < 3; i++) {
    double c = channel[i];

    c = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
    luminance += weight[i] * c;
  }

  return luminance;
}

/* Function purpose: WCAG contrast ratio, 1.0 to 21.0. Alpha is deliberately
ignored: every pair the role map compares is opaque at the point it compares
them, and the column's own translucency is applied afterwards. */
static double
contrast_ratio(const struct saber_color *a, const struct saber_color *b)
{
  double la = relative_luminance(a);
  double lb = relative_luminance(b);

  return la > lb ? (la + 0.05) / (lb + 0.05) : (lb + 0.05) / (la + 0.05);
}

/* Function purpose: Whichever candidate reads better against `ground`. Three
lines here remove a whole class of defect permanently -- a role picked by
measurement cannot be legible on the palette it was chosen for and invisible on
the next one. `first` wins a tie, so the pick is stable. */
static struct saber_color
better_against(const struct saber_color *ground,
    const struct saber_color *first,
    const struct saber_color *second)
{
  return contrast_ratio(ground, second) > contrast_ratio(ground, first)
      ? *second
      : *first;
}

/* Function purpose: `base` moved `mix` of the way towards `towards`, keeping
base's alpha -- a derived tone is a tint of the surface it sits on, not a second
translucency stacked over it. */
static struct saber_color
mix_towards(const struct saber_color *base,
    const struct saber_color *towards,
    double mix)
{
  struct saber_color out = *base;

  out.r += (towards->r - base->r) * mix;
  out.g += (towards->g - base->g) * mix;
  out.b += (towards->b - base->b) * mix;

  return out;
}

/* Function purpose: Hand back the override for a role if the configuration
wrote one, otherwise the colour the map derived. A palette reference resolves
here rather than at parse time, so a role pointed at a slot follows whichever
palette ended up in force. */
static struct saber_color
role_value(const struct saber_theme *theme,
    const struct saber_theme_roles *roles,
    enum saber_role role,
    struct saber_color derived)
{
  if (roles == NULL || !roles->role[role].set) {
    return derived;
  }

  int slot = roles->role[role].slot;

  /* A slot past the end of the palette cannot come from the parser, which
  bounds it, but falling back to the derived colour rather than to a zeroed one
  keeps a role that is somehow out of range painted rather than black. */
  if (slot >= 0) {
    return slot < SABER_PALETTE_SLOTS ? theme->palette[slot] : derived;
  }

  return roles->role[role].color;
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

bool
saber_theme_import_hikari_animation(const char *path, int *duration_ms)
{
  if (path == NULL || duration_ms == NULL) {
    return false;
  }

  struct ucl_parser *parser = ucl_parser_new(UCL_PARSER_NO_IMPLICIT_ARRAYS);

  if (parser == NULL) {
    return false;
  }

  /* Same as the palette import: no hikari.conf is a first run, not an error. */
  if (!ucl_parser_add_file(parser, path)) {
    ucl_parser_free(parser);

    return false;
  }

  ucl_object_t *root = ucl_parser_get_object(parser);

  ucl_parser_free(parser);

  if (root == NULL) {
    return false;
  }

  const ucl_object_t *ui = ucl_object_lookup(root, "ui");
  const ucl_object_t *block =
      ui == NULL ? NULL : ucl_object_lookup(ui, "animation");
  bool imported = false;

  if (block != NULL) {
    const ucl_object_t *entry = ucl_object_lookup(block, "duration");
    int64_t duration;

    if (entry != NULL && ucl_object_toint_safe(entry, &duration)
        && duration >= 0 && duration <= SABER_HIKARI_DURATION_MAX) {
      *duration_ms = (int)duration;
      imported = true;
    }

    /* Action purpose: `enabled = false` is the compositor saying it does not
    animate, and it is read after the duration because it overrides it -- the
    two keys arrive as one number, and zero is already what the tween code
    reads as "settle immediately". */
    entry = ucl_object_lookup(block, "enabled");

    bool enabled;

    if (entry != NULL && ucl_object_toboolean_safe(entry, &enabled)
        && !enabled) {
      *duration_ms = 0;
      imported = true;
    }
  }

  ucl_object_unref(root);

  return imported;
}

void
saber_theme_init(struct saber_theme *theme,
    const struct saber_color *palette,
    double opacity,
    double overlay_opacity)
{
  saber_theme_init_with_roles(theme, palette, opacity, overlay_opacity, NULL);
}

void
saber_theme_init_with_roles(struct saber_theme *theme,
    const struct saber_color *palette,
    double opacity,
    double overlay_opacity,
    const struct saber_theme_roles *roles)
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

  /* Action purpose: The roles resolve in dependency order, each one through
  role_value, so an override is picked up by everything derived from it: a new
  accent retints the backlight and the progress bar, a new urgent takes the
  count disc with it, and the badge ink is then measured against whatever the
  disc actually ended up being. A positional map cannot do that -- it is why
  the same ten indices read correctly on one palette and wrongly on the next.

  The ground comes first because the urgency literal is chosen by it. */
  theme->background =
      role_value(theme, roles, SABER_ROLE_BACKGROUND, theme->palette[0]);
  theme->light_ground =
      relative_luminance(&theme->background) >= SABER_LIGHT_GROUND;

  theme->foreground =
      role_value(theme, roles, SABER_ROLE_FOREGROUND, theme->palette[15]);

  struct saber_color accent = better_against(
      &theme->background, &theme->palette[12], &theme->palette[4]);

  theme->accent = role_value(theme, roles, SABER_ROLE_ACCENT, accent);
  theme->backlight = role_value(theme, roles, SABER_ROLE_BACKLIGHT,
      mix_towards(&theme->background, &theme->accent, SABER_BACKLIGHT_MIX));

  struct saber_color urgent = theme->palette[1];

  saber_color_parse(theme->light_ground ? SABER_URGENT_ON_LIGHT
                                        : SABER_URGENT_ON_DARK,
      &urgent);

  theme->urgent = role_value(theme, roles, SABER_ROLE_URGENT, urgent);
  theme->badge_bg =
      role_value(theme, roles, SABER_ROLE_BADGE_BG, theme->urgent);
  theme->badge_fg = role_value(theme, roles, SABER_ROLE_BADGE_FG,
      better_against(
          &theme->badge_bg, &theme->palette[0], &theme->palette[15]));

  theme->progress =
      role_value(theme, roles, SABER_ROLE_PROGRESS, theme->accent);
  theme->dim = role_value(theme, roles, SABER_ROLE_DIM, theme->palette[8]);
  theme->overlay =
      role_value(theme, roles, SABER_ROLE_OVERLAY, theme->background);

  /* Action purpose: Only the column takes the opacity setting, and it takes it
  last so the overlay role copied above carries the colour alone. Applying it
  to foreground or badge roles would wash out text drawn *onto* an already
  translucent background; applying it to the overlay would make the dash and
  the spread a product of three alphas rather than the one number
  overlay_opacity promises. */
  theme->background.a *= opacity;
}

bool
saber_theme_is_light(const struct saber_theme *theme)
{
  return theme != NULL && theme->light_ground;
}

void
saber_theme_set_source(cairo_t *cr, const struct saber_color *color)
{
  if (cr == NULL || color == NULL) {
    return;
  }

  cairo_set_source_rgba(cr, color->r, color->g, color->b, color->a);
}
