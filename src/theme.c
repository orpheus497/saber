/* Script function and purpose: Colour parsing, the hikari.lua `ui.palette` and
`ui.animation` imports, and the mapping of those sixteen slots onto the semantic
roles every drawing module reads.

The two imports live here together deliberately: they are the only places in
the tree that open the compositor's own configuration, so there is one answer to
where hikari's file is read and one sandbox it is read under. */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#include <saber/luaguard.h>
#include <saber/theme.h>

/* Action purpose: The compiled-in default is hikari-sakura's shipped palette,
so a panel started with no hikari.lua and no configuration still comes up in
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
`theme.roles.urgent`, which is also the only way a dropped-in terminal
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

/* What the hint role is settled at against the column. It is deliberately
under body AA: this is content that has to read as recessed -- a placeholder
behind what the user is typing, the number of a sheet holding nothing -- so
holding it to 4.5:1 would put it level with the labels it sits beneath. It is
equally deliberately above the 3:1 graphical-object floor, because every one of
those things is still text or a glyph somebody has to make out.

It is the figure sofi's own `hint` is settled at, and by the same walk, so a
placeholder in the dash and one in a sofi panel recede by the same amount
rather than by two numbers that happen to look similar. The two land on the
same literal on the two derived palettes and one step apart on Sakura, because
sofi rounds each step of the walk to bytes before measuring it and this works
in doubles throughout -- a difference of 1/255 in a tone nobody is meant to
read closely. */
#define SABER_HINT_TARGET 3.6

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

/* Function purpose: A tone that sits AT `target` against `ground` rather than
above it, walked out from the ground towards `slot` and stopped at the first
whole percent that reaches it.

This is what a recession needs, and both of the obvious alternatives get it
wrong on one of the three shipped palettes. Taking the grey slot outright gives
2.29:1 on Sakura, 1.34:1 on Sakura Dark and 1.16:1 on Sakura Light -- a
placeholder and a sheet number nobody can read on two of the three. Lifting the
slot until it clears instead leaves Sakura's own grey untouched at 5.97:1,
brighter than the body text it is supposed to sit behind. Walking out from the
ground caps it from below, and only where the walk cannot reach the target at
all is the slot carried past itself, towards white on a dark ground and black on
a light one.

Towards the extreme rather than towards the text colour, for the reason the
accent family already takes that route: a blend towards a near-grey text colour
reaches the ratio by draining the hue out of the tone. */
static struct saber_color
settle_to(const struct saber_color *ground,
    const struct saber_color *slot,
    double target)
{
  for (int step = 1; step <= 100; step++) {
    struct saber_color out = mix_towards(ground, slot, step / 100.0);

    if (contrast_ratio(ground, &out) >= target) {
      return out;
    }
  }

  struct saber_color extreme = *slot;

  extreme.r = extreme.g = extreme.b =
      relative_luminance(ground) >= SABER_LIGHT_GROUND ? 0.0 : 1.0;

  for (int step = 1; step <= 100; step++) {
    struct saber_color out = mix_towards(slot, &extreme, step / 100.0);

    if (contrast_ratio(ground, &out) >= target) {
      return out;
    }
  }

  return extreme;
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

/* What a hikari.lua may reach while saber is the one running it. It is somebody
else's file, read for two blocks out of it, so the sandbox is tighter than the
compositor's own would be rather than looser: arithmetic, strings and tables,
and nothing that opens a file or starts a process. A name absent from this list
is absent from the chunk's globals, and the libraries the names are not in are
never opened at all -- see guard_prepare in src/luaguard.c.

No metatable goes on this environment, so an undefined name here reads as nil
rather than being refused. That is deliberate and it is the one place this
sandbox is looser than saber.lua's: a hikari.lua is written for the compositor
and will mention names the compositor defines and saber does not, and refusing
it for that would mean a desktop whose palette stops being imported the moment
hikari gains a configuration function. */
static const char *const hikari_environment[] = {
  "assert",
  "error",
  "ipairs",
  "math",
  "pairs",
  "select",
  "string",
  "table",
  "tonumber",
  "tostring",
  "type",
  NULL,
};

/* How many VM instructions the compositor's configuration gets before it is
taken to have hung. Reading somebody else's file must not be able to stop the
panel starting, and the two blocks wanted from it cost a few hundred.

It counts VM instructions and so cannot interrupt a loop that runs inside one C
function. A hikari.lua carrying `string.rep("", math.maxinteger)` hung the
panel at startup for exactly that reason, which is what the ceilings in
src/luaguard.c are for. */
#define SABER_HIKARI_STEPS 2000000

/* Function purpose: End a chunk that has run too long, through Lua's own error
path, so a hikari.lua that does not finish simply fails the import like any
other unusable file. */
static void
hikari_runaway(lua_State *L, lua_Debug *activation)
{
  (void)activation;

  luaL_error(L, "the file did not finish");
}

/* Function purpose: The one place the compositor's configuration is opened.
Hands back a state with the table the chunk returned on top, or NULL -- and
NULL silently, because a missing or malformed hikari.lua is the ordinary case
for a first run and must cost nothing but the import. Every failure closes the
state, so neither caller has a cleanup path of its own. */
static lua_State *
hikari_open(const char *path)
{
  lua_State *L = saber_lua_open(hikari_environment, NULL, true);

  if (L == NULL) {
    return NULL;
  }

  /* Action purpose: "t" loads TEXT only, for the reason config_open() gives --
  and with more force here, because this file belongs to another program. A
  precompiled chunk left where hikari.lua goes would be read by saber with the
  compositor's authority and none of the compositor's intent. */
  if (luaL_loadfilex(L, path, "t") != LUA_OK || !saber_lua_enter(L)) {
    saber_lua_close(L);

    return NULL;
  }

  lua_sethook(L, hikari_runaway, LUA_MASKCOUNT, SABER_HIKARI_STEPS);

  int status = lua_pcall(L, 0, 1, 0);

  /* Action purpose: The memory ceiling is lifted as soon as the chunk stops
  running. Both importers below read the table with no protected frame under
  them, so an allocation failure there would abort the panel instead of failing
  the import -- and a hikari.lua that parked the whole budget in live data
  could arrange it. The ceiling is for the file, not for the reading of it. */
  saber_lua_unbound(L);

  if (status != LUA_OK || !lua_istable(L, -1)) {
    saber_lua_close(L);

    return NULL;
  }

  return L;
}

/* Function purpose: Push hikari's `ui.<name>` block, or a top-level block of
the same name. hikari-sakura's own move to Lua is still settling the shape of
its file, so both are read rather than one being guessed at, and a shape that
carries neither simply falls through to saber's fallback. Returns false with the
stack as it found it; on success exactly the block is left on top. */
static bool
hikari_push_block(lua_State *L, const char *name)
{
  lua_getfield(L, -1, "ui");

  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, name);

    if (lua_istable(L, -1)) {
      /* Drop the enclosing ui table from under the block, so the caller sees
      one value pushed whichever of the two shapes answered. */
      lua_remove(L, -2);

      return true;
    }

    lua_pop(L, 2);
  } else {
    lua_pop(L, 1);
  }

  lua_getfield(L, -1, name);

  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);

    return false;
  }

  return true;
}

int
saber_theme_import_hikari(const char *path,
    struct saber_color palette[SABER_PALETTE_SLOTS])
{
  if (path == NULL || palette == NULL) {
    return 0;
  }

  lua_State *L = hikari_open(path);

  if (L == NULL) {
    return 0;
  }

  int count = 0;

  if (hikari_push_block(L, "palette")) {
    for (int slot = 0; slot < SABER_PALETTE_SLOTS; slot++) {
      char key[16];

      snprintf(key, sizeof(key), "color%d", slot);
      lua_getfield(L, -1, key);

      const char *spec =
          lua_type(L, -1) == LUA_TSTRING ? lua_tostring(L, -1) : NULL;

      if (spec != NULL && saber_color_parse(spec, &palette[slot])) {
        count++;
      }

      lua_pop(L, 1);
    }
  }

  saber_lua_close(L);

  return count;
}

bool
saber_theme_import_hikari_animation(const char *path, int *duration_ms)
{
  if (path == NULL || duration_ms == NULL) {
    return false;
  }

  lua_State *L = hikari_open(path);

  if (L == NULL) {
    return false;
  }

  bool imported = false;

  if (hikari_push_block(L, "animation")) {
    lua_getfield(L, -1, "duration");

    int usable = 0;
    lua_Integer duration = lua_tointegerx(L, -1, &usable);

    lua_pop(L, 1);

    if (usable && duration >= 0 && duration <= SABER_HIKARI_DURATION_MAX) {
      *duration_ms = (int)duration;
      imported = true;
    }

    /* Action purpose: `enabled = false` is the compositor saying it does not
    animate, and it is read after the duration because it overrides it -- the
    two keys arrive as one number, and zero is already what the tween code
    reads as "settle immediately". */
    lua_getfield(L, -1, "enabled");

    if (lua_isboolean(L, -1) && !lua_toboolean(L, -1)) {
      *duration_ms = 0;
      imported = true;
    }

    lua_pop(L, 1);
  }

  saber_lua_close(L);

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
  the same fixed indices read correctly on one palette and wrongly on the next.

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

  /* Action purpose: `hint` is settled against the column from the grey slot,
  not taken from it. The defect this repairs is the same shape as the
  backlight/dim collision above and was left behind by it: `dim` was carrying
  both the hairlines and the recessed text, and palette[8] reads at 2.29:1,
  1.34:1 and 1.16:1 against the three shipped grounds -- so the dash's "Search
  applications", its "No matching applications" line and the numbers in the
  sheet grid were invisible on two of the three themes. Separating them lets
  each sit where it belongs: see settle_to() for why neither taking the slot nor
  lifting it works, and SABER_HINT_TARGET for the figure. */
  theme->hint = role_value(theme, roles, SABER_ROLE_HINT,
      settle_to(&theme->background, &theme->palette[6], SABER_HINT_TARGET));

  /* `dim` keeps palette[8] and keeps the structural work: hairlines, frame
  outlines, tracks, and a menu item that is genuinely unavailable. */
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
