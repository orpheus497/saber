/* Script function and purpose: Parse saber.lua, apply a default for every
absent key, import the compositor's palette, and read and write the favourites
state file. Nothing here is fatal: a user with no configuration, or a broken
one, still gets a panel.

The file is a Lua chunk that returns a table, which means it is code, and it is
run in a restricted environment rather than in Lua's own: the chunk is handed
arithmetic, strings and tables and nothing that reaches the machine. A panel's
settings have no reason to open a file or start a process. */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <saber/config.h>
#include <saber/saber.h>

#define SABER_ICON_SIZE_MIN 24
#define SABER_ICON_SIZE_MAX 64

/* Zero is a legitimate choice -- an icon-sized column with no gutter at all --
and the ceiling only stops a typo turning the strip into half the desktop. */
#define SABER_PANEL_PADDING_MIN 0
#define SABER_PANEL_PADDING_MAX 64

struct saber_enum_name {
  const char *name;
  int value;
};

static const struct saber_enum_name saber_edge_names[] = {
  { "left", SABER_EDGE_LEFT },
  { "right", SABER_EDGE_RIGHT },
  { NULL, 0 },
};

static const struct saber_enum_name saber_autohide_names[] = {
  { "never", SABER_AUTOHIDE_NEVER },
  { "auto", SABER_AUTOHIDE_AUTO },
  { NULL, 0 },
};

static const struct saber_enum_name saber_backlight_names[] = {
  { "palette", SABER_BACKLIGHT_PALETTE },
  { "dominant", SABER_BACKLIGHT_DOMINANT },
  { "off", SABER_BACKLIGHT_OFF },
  { NULL, 0 },
};

/* The ten names `theme { roles }` accepts, in the order theme.h declares them.
The hyphenated spellings also answer to an underscore, as every other key
does. */
static const struct saber_enum_name saber_role_names[] = {
  { "background", SABER_ROLE_BACKGROUND },
  { "foreground", SABER_ROLE_FOREGROUND },
  { "accent", SABER_ROLE_ACCENT },
  { "backlight", SABER_ROLE_BACKLIGHT },
  { "urgent", SABER_ROLE_URGENT },
  { "badge-bg", SABER_ROLE_BADGE_BG },
  { "badge-fg", SABER_ROLE_BADGE_FG },
  { "progress", SABER_ROLE_PROGRESS },
  { "dim", SABER_ROLE_DIM },
  { "overlay", SABER_ROLE_OVERLAY },
  { NULL, 0 },
};

static const struct saber_enum_name saber_portion_names[] = {
  { "bfb", SABER_PORTION_BFB },
  { "apps", SABER_PORTION_APPS },
  { "sheets", SABER_PORTION_SHEETS },
  { "devices", SABER_PORTION_DEVICES },
  { "trash", SABER_PORTION_TRASH },
  { "tray", SABER_PORTION_TRAY },
  { "session", SABER_PORTION_SESSION },
  { NULL, 0 },
};

/* The order the column carried before `items { order }` existed, and what an
absent key still means. */
static const enum saber_portion saber_portion_default[] = {
  SABER_PORTION_BFB,
  SABER_PORTION_APPS,
  SABER_PORTION_SHEETS,
  SABER_PORTION_DEVICES,
  SABER_PORTION_TRASH,
  SABER_PORTION_TRAY,
  SABER_PORTION_SESSION,
};

/* Action purpose: The documented keys are hyphenated (`icon-size`), which Lua
can only write as ["icon-size"], while the underscore spelling is a bare
identifier and is what a Lua table wants to be written with. Both resolve to the
same key -- the underscore first -- so a file copied from the manual and a file
written the idiomatic way say the same thing, and neither spelling is a silent
no-op.

Every helper below is written to leave the stack exactly as it found it, which
is what lets the block readers be a flat run of calls with no bookkeeping of
their own. This one pushes the value and returns true, or pushes nothing. */
static bool
config_push_field(lua_State *L, const char *key)
{
  if (strchr(key, '-') != NULL) {
    char *alternative = g_strdup(key);

    for (char *c = alternative; *c != '\0'; c++) {
      if (*c == '-') {
        *c = '_';
      }
    }

    lua_getfield(L, -1, alternative);
    g_free(alternative);

    if (!lua_isnil(L, -1)) {
      return true;
    }

    lua_pop(L, 1);
  }

  lua_getfield(L, -1, key);

  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);

    return false;
  }

  return true;
}

/* Function purpose: Push the table `name` holds -- one of the five top-level
blocks, or the palette and roles blocks nested in the theme. Returns false for
an absent key and for a key that is not a table, so each caller is a single
guarded section and no path can leave a value behind. */
static bool
config_push_block(lua_State *L, const char *name)
{
  lua_getfield(L, -1, name);

  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);

    return false;
  }

  return true;
}

/* Function purpose: Whether a key is written at all, in either spelling. One
setting turns on this rather than on its value, because 180 written out by hand
and 180 left to the default are the same number and must not mean the same
thing. */
static bool
config_written(lua_State *L, const char *key)
{
  if (!config_push_field(L, key)) {
    return false;
  }

  lua_pop(L, 1);

  return true;
}

/* Function purpose: A copy of the string a key holds, or NULL. It is copied
rather than handed back as the pointer Lua owns, because that pointer lives only
as long as the value stays on the stack -- so every call site would otherwise
have to unwind the stack in step with its own use of the string, which is the
one piece of bookkeeping this file keeps out of them. */
static char *
config_dup_string(lua_State *L, const char *key)
{
  if (!config_push_field(L, key)) {
    return NULL;
  }

  char *value = lua_type(L, -1) == LUA_TSTRING
      ? g_strdup(lua_tostring(L, -1))
      : NULL;

  lua_pop(L, 1);

  return value;
}

static void
config_apply_string(lua_State *L, const char *key, char **out)
{
  char *value = config_dup_string(L, key);

  if (value == NULL) {
    return;
  }

  g_free(*out);
  *out = value;
}

static void
config_apply_bool(lua_State *L, const char *key, bool *out)
{
  if (!config_push_field(L, key)) {
    return;
  }

  if (lua_isboolean(L, -1)) {
    *out = lua_toboolean(L, -1) != 0;
  }

  lua_pop(L, 1);
}

static void
config_apply_int(lua_State *L, const char *key, int *out, int low, int high)
{
  if (!config_push_field(L, key)) {
    return;
  }

  int usable = 0;
  lua_Integer value = lua_tointegerx(L, -1, &usable);

  lua_pop(L, 1);

  if (usable) {
    *out = (int)CLAMP(value, (lua_Integer)low, (lua_Integer)high);
  }
}

static void
config_apply_double(
    lua_State *L, const char *key, double *out, double low, double high)
{
  if (!config_push_field(L, key)) {
    return;
  }

  int usable = 0;
  double value = (double)lua_tonumberx(L, -1, &usable);

  lua_pop(L, 1);

  if (usable) {
    *out = CLAMP(value, low, high);
  }
}

/* Function purpose: Returns `fallback` for an absent or unrecognised key, and
takes the value by return rather than through a pointer so no caller has to
alias an enum through an int *. */
static int
config_enum(lua_State *L, const char *key,
    const struct saber_enum_name *names, int fallback, const char *where)
{
  char *value = config_dup_string(L, key);

  if (value == NULL) {
    return fallback;
  }

  int resolved = fallback;
  bool known = false;

  for (const struct saber_enum_name *n = names; n->name != NULL; n++) {
    if (g_ascii_strcasecmp(value, n->name) == 0) {
      resolved = n->value;
      known = true;

      break;
    }
  }

  if (!known) {
    fprintf(stderr, "saber: %s: %s: unknown value \"%s\"; keeping default\n",
        where, key, value);
  }

  g_free(value);

  return resolved;
}

/* Function purpose: How many entries a list key holds, with a bare string
counting as one. `favourites = "sofi.desktop"` written without braces has always
been read as the list it plainly is, and still is: a one-element list is the
common case, and a silent no-op is the worst available answer to it. */
static lua_Integer
config_list_len(lua_State *L)
{
  if (lua_type(L, -1) == LUA_TSTRING) {
    return 1;
  }

  return lua_istable(L, -1) ? luaL_len(L, -1) : 0;
}

/* Function purpose: Entry `index` of the list at the top of the stack, copied
for the same reason config_dup_string copies, and leaving the stack as it found
it so the list stays on top across a whole loop. */
static char *
config_list_dup(lua_State *L, lua_Integer index)
{
  if (lua_type(L, -1) == LUA_TSTRING) {
    return index == 1 ? g_strdup(lua_tostring(L, -1)) : NULL;
  }

  lua_geti(L, -1, index);

  char *item = lua_type(L, -1) == LUA_TSTRING
      ? g_strdup(lua_tostring(L, -1))
      : NULL;

  lua_pop(L, 1);

  return item;
}

static void
config_defaults(struct saber_config *config)
{
  memset(config, 0, sizeof(*config));

  config->panel.output = g_strdup("all");
  config->panel.edge = SABER_EDGE_LEFT;
  /* 32 is a size every icon theme ships natively, so raster icons are used at
  1:1 rather than resampled to an in-between number. With SABER_PANEL_PADDING
  that gives a 44px column. */
  config->panel.icon_size = 32;
  config->panel.padding = SABER_PANEL_PADDING;
  config->panel.autohide = SABER_AUTOHIDE_NEVER;
  config->panel.reveal_pressure = 240;
  config->panel.animation_ms = 180;

  config->theme.inherit_hikari = true;
  config->theme.backlight = SABER_BACKLIGHT_PALETTE;
  config->theme.opacity = 0.92;
  config->theme.overlay_opacity = 1.0;
  config->theme.palette_valid = false;

  config->items.bfb = true;
  config->items.sheets = true;
  config->items.devices = true;
  config->items.trash = true;
  config->items.tray = true;
  config->items.session = true;
  config->items.order =
      g_memdup2(saber_portion_default, sizeof(saber_portion_default));
  config->items.order_len = G_N_ELEMENTS(saber_portion_default);

  config->session.suspend = g_strdup("");
  config->session.reboot = g_strdup("");
  config->session.poweroff = g_strdup("");
  config->session.lock = g_strdup("");
  config->session.logout = g_strdup("");
}

/* Function purpose: Resolve which saber.lua to read. An explicit path is
returned whether or not it exists, so the caller reports it as missing rather
than silently reading somebody else's file. */
static char *
config_find(const char *path)
{
  if (path != NULL) {
    return g_strdup(path);
  }

  const char *xdg = g_getenv("XDG_CONFIG_HOME");
  const char *home = g_getenv("HOME");
  char *candidate;

  if (xdg != NULL && *xdg != '\0') {
    candidate = g_build_filename(xdg, "saber", "saber.lua", NULL);

    if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR)) {
      return candidate;
    }

    g_free(candidate);
  }

  if (home != NULL && *home != '\0') {
    candidate = g_build_filename(home, ".config", "saber", "saber.lua", NULL);

    if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR)) {
      return candidate;
    }

    g_free(candidate);
  }

  if (g_file_test(SABER_SYSTEM_CONFIG, G_FILE_TEST_IS_REGULAR)) {
    return g_strdup(SABER_SYSTEM_CONFIG);
  }

  return NULL;
}

static void
config_apply_favourites(struct saber_config *config, lua_State *L)
{
  if (!config_push_field(L, "favourites")) {
    return;
  }

  GPtrArray *items = g_ptr_array_new();
  lua_Integer len = config_list_len(L);

  for (lua_Integer i = 1; i <= len; i++) {
    char *id = config_list_dup(L, i);

    if (id != NULL && *id != '\0') {
      g_ptr_array_add(items, id);
    } else {
      g_free(id);
    }
  }

  lua_pop(L, 1);

  g_strfreev(config->launcher.favourites);
  config->launcher.favourites_len = items->len;
  g_ptr_array_add(items, NULL);
  config->launcher.favourites = (char **)g_ptr_array_free(items, FALSE);
}

/* Function purpose: Read a `theme.palette` block, with the theme table on top
of the stack. Returns true only for a complete sixteen-slot palette, which is
what lets an explicit block override the hikari import without a half-written
one doing so. */
static bool
config_apply_palette(struct saber_config *config, lua_State *L)
{
  if (!config_push_block(L, "palette")) {
    return false;
  }

  struct saber_color palette[SABER_PALETTE_SLOTS];
  int count = 0;

  for (int slot = 0; slot < SABER_PALETTE_SLOTS; slot++) {
    char key[16];

    snprintf(key, sizeof(key), "color%d", slot);

    char *spec = config_dup_string(L, key);

    if (spec != NULL && saber_color_parse(spec, &palette[slot])) {
      count++;
    }

    g_free(spec);
  }

  lua_pop(L, 1);

  if (count != SABER_PALETTE_SLOTS) {
    fprintf(stderr,
        "saber: theme.palette: %d of %d colours usable; ignoring it\n",
        count, SABER_PALETTE_SLOTS);

    return false;
  }

  memcpy(config->theme.palette, palette, sizeof(palette));
  config->theme.palette_valid = true;

  return true;
}

/* Function purpose: Resolve a "colorN" palette reference, 0-15. Returns -1 for
anything else, which is what tells the caller to read the value as a literal
instead -- the same two forms the compositor's own colour keys take. */
static int
config_palette_reference(const char *spec)
{
  if (spec == NULL || g_ascii_strncasecmp(spec, "color", 5) != 0) {
    return -1;
  }

  const char *digits = spec + 5;
  char *end = NULL;
  int64_t slot = g_ascii_strtoll(digits, &end, 10);

  if (end == digits || end == NULL || *end != '\0' || slot < 0
      || slot >= SABER_PALETTE_SLOTS) {
    return -1;
  }

  return (int)slot;
}

/* Function purpose: Read `theme.roles`, the per-role override block, with the
theme table on top of the stack. The palette says which colours exist; this says
which of them carries which meaning, and an override wins over the colour the
role map would otherwise derive. It is what lets a user re-point one role --
urgency back onto a palette slot, say -- without having to rebuild the whole
palette around it. An unusable value costs that one role and is reported, never
the block. */
static void
config_apply_roles(struct saber_config *config, lua_State *L)
{
  if (!config_push_block(L, "roles")) {
    return;
  }

  for (const struct saber_enum_name *n = saber_role_names; n->name != NULL;
      n++) {
    char *spec = config_dup_string(L, n->name);

    if (spec == NULL) {
      continue;
    }

    struct saber_role_override override = { .set = true, .slot = -1 };

    override.slot = config_palette_reference(spec);

    if (override.slot >= 0 || saber_color_parse(spec, &override.color)) {
      config->theme.roles.role[n->value] = override;
    } else {
      fprintf(stderr,
          "saber: theme.roles: %s: \"%s\" is neither colorN nor a #rrggbb "
          "literal; keeping the default\n",
          n->name, spec);
    }

    g_free(spec);
  }

  lua_pop(L, 1);
}

/* Function purpose: Read `items.order`, the ordered portion list, with the
items table on top of the stack. An unrecognised name is reported rather than
silently dropped -- config_enum's precedent -- and a repeat is ignored, because
a portion is one place in the column and cannot be in two. */
static void
config_apply_order(struct saber_config *config, lua_State *L)
{
  if (!config_push_field(L, "order")) {
    return;
  }

  enum saber_portion order[SABER_PORTION_COUNT];
  bool seen[SABER_PORTION_COUNT] = { false };
  size_t len = 0;
  lua_Integer count = config_list_len(L);

  for (lua_Integer i = 1; i <= count; i++) {
    char *name = config_list_dup(L, i);
    const struct saber_enum_name *match = NULL;

    for (const struct saber_enum_name *n = saber_portion_names;
        name != NULL && n->name != NULL; n++) {
      if (g_ascii_strcasecmp(name, n->name) == 0) {
        match = n;

        break;
      }
    }

    if (match == NULL) {
      fprintf(stderr,
          "saber: items.order: unknown portion \"%s\"; ignoring it\n",
          name == NULL ? "" : name);
    } else if (!seen[match->value]) {
      seen[match->value] = true;
      order[len++] = (enum saber_portion)match->value;
    }

    g_free(name);
  }

  lua_pop(L, 1);

  /* Action purpose: The application band has no key that turns it off, so an
  order that forgets to name it still gets it -- in the place the column has
  always put it, directly after the BFB. Without this a user who listed only
  the special tiles would lose every application tile with no way to tell why,
  and len is at most six here, so the insert cannot overrun. */
  if (!seen[SABER_PORTION_APPS]) {
    size_t at = 0;

    for (size_t i = 0; i < len; i++) {
      if (order[i] == SABER_PORTION_BFB) {
        at = i + 1;

        break;
      }
    }

    memmove(&order[at + 1], &order[at], (len - at) * sizeof(order[0]));
    order[at] = SABER_PORTION_APPS;
    len++;
  }

  g_free(config->items.order);
  config->items.order = g_memdup2(order, len * sizeof(order[0]));
  config->items.order_len = len;
}

/* Function purpose: Reconcile the two forms of the items block. The booleans
filter the order -- `trash = false` still hides the trash, whether the order
came from the user or from the default -- and the survivors then define the
booleans, so config->items.bfb and friends describe the column that will
actually be built rather than what was written. */
static void
config_resolve_order(struct saber_config *config)
{
  bool enabled[SABER_PORTION_COUNT] = { false };
  size_t len = 0;

  enabled[SABER_PORTION_BFB] = config->items.bfb;
  enabled[SABER_PORTION_APPS] = true;
  enabled[SABER_PORTION_SHEETS] = config->items.sheets;
  enabled[SABER_PORTION_DEVICES] = config->items.devices;
  enabled[SABER_PORTION_TRASH] = config->items.trash;
  enabled[SABER_PORTION_TRAY] = config->items.tray;
  enabled[SABER_PORTION_SESSION] = config->items.session;

  bool present[SABER_PORTION_COUNT] = { false };

  for (size_t i = 0; i < config->items.order_len; i++) {
    enum saber_portion portion = config->items.order[i];

    if (enabled[portion]) {
      present[portion] = true;
      config->items.order[len++] = portion;
    }
  }

  config->items.order_len = len;

  config->items.bfb = present[SABER_PORTION_BFB];
  config->items.sheets = present[SABER_PORTION_SHEETS];
  config->items.devices = present[SABER_PORTION_DEVICES];
  config->items.trash = present[SABER_PORTION_TRASH];
  config->items.tray = present[SABER_PORTION_TRAY];
  config->items.session = present[SABER_PORTION_SESSION];
}

/* Which of the settings a hikari.lua could also supply the saber.lua wrote for
itself. A key written here always wins over the compositor's, so the import has
to know what it is not allowed to answer. */
struct config_written {
  bool palette;
  bool animation_ms;
};

/* Function purpose: Apply the table the chunk returned, which is on top of the
stack. Each block is read inside its own guard, so a file that writes only one
of them pays nothing for the other four and the stack is back where it started
after each. */
static void
config_apply(struct saber_config *config,
    lua_State *L,
    struct config_written *written)
{
  if (config_push_block(L, "panel")) {
    config_apply_string(L, "output", &config->panel.output);
    config->panel.edge = config_enum(
        L, "edge", saber_edge_names, config->panel.edge, "panel");
    config_apply_int(L, "icon-size", &config->panel.icon_size,
        SABER_ICON_SIZE_MIN, SABER_ICON_SIZE_MAX);
    config_apply_int(L, "padding", &config->panel.padding,
        SABER_PANEL_PADDING_MIN, SABER_PANEL_PADDING_MAX);
    config->panel.autohide = config_enum(L, "autohide", saber_autohide_names,
        config->panel.autohide, "panel");
    config_apply_int(
        L, "reveal-pressure", &config->panel.reveal_pressure, 0, 100000);
    config_apply_int(
        L, "animation-ms", &config->panel.animation_ms, 0, 10000);

    /* Action purpose: Noted before the import runs rather than inferred from
    the value afterwards, because 180 written out by hand and 180 left to the
    default are the same number and must not mean the same thing -- one of them
    refuses the compositor's duration and the other asks for it. */
    written->animation_ms = config_written(L, "animation-ms");

    lua_pop(L, 1);
  }

  if (config_push_block(L, "theme")) {
    config_apply_bool(L, "inherit-hikari", &config->theme.inherit_hikari);
    config->theme.backlight = config_enum(
        L, "backlight", saber_backlight_names, config->theme.backlight,
        "theme");
    config_apply_double(L, "opacity", &config->theme.opacity, 0.0, 1.0);
    config_apply_double(
        L, "overlay-opacity", &config->theme.overlay_opacity, 0.0, 1.0);

    written->palette = config_apply_palette(config, L);

    config_apply_roles(config, L);

    lua_pop(L, 1);
  }

  if (config_push_block(L, "launcher")) {
    config_apply_favourites(config, L);

    lua_pop(L, 1);
  }

  if (config_push_block(L, "items")) {
    config_apply_bool(L, "bfb", &config->items.bfb);
    config_apply_bool(L, "sheets", &config->items.sheets);
    config_apply_bool(L, "devices", &config->items.devices);
    config_apply_bool(L, "trash", &config->items.trash);
    config_apply_bool(L, "tray", &config->items.tray);
    config_apply_bool(L, "session", &config->items.session);
    config_apply_order(config, L);

    lua_pop(L, 1);
  }

  if (config_push_block(L, "session")) {
    config_apply_string(L, "suspend", &config->session.suspend);
    config_apply_string(L, "reboot", &config->session.reboot);
    config_apply_string(L, "poweroff", &config->session.poweroff);
    config_apply_string(L, "lock", &config->session.lock);
    config_apply_string(L, "logout", &config->session.logout);

    lua_pop(L, 1);
  }
}

/* Function purpose: Try each hikari.lua in turn, user first, for whichever of
the two imports the saber.lua left open. Only a complete sixteen-slot import
counts as a palette, so a truncated or half-commented one falls through to the
next candidate rather than tinting the panel with defaults for the missing half.
The motion duration is taken the same way and independently -- the first
candidate that carries an animation block answers it -- so a user whose own
hikari.lua sets only one of the two still inherits both from where they are
actually written. Returns whether the palette was imported, which is the one
answer the caller cannot derive for itself. */
static bool
config_import_hikari(
    struct saber_config *config, bool want_palette, bool want_animation)
{
  const char *xdg = g_getenv("XDG_CONFIG_HOME");
  const char *home = g_getenv("HOME");
  char *candidates[3];
  int count = 0;

  if (xdg != NULL && *xdg != '\0') {
    candidates[count++] = g_build_filename(xdg, "hikari", "hikari.lua", NULL);
  }

  if (home != NULL && *home != '\0') {
    candidates[count++] =
        g_build_filename(home, ".config", "hikari", "hikari.lua", NULL);
  }

  candidates[count++] = g_strdup(SABER_HIKARI_SYSTEM_CONFIG);

  struct saber_color palette[SABER_PALETTE_SLOTS];
  bool imported = false;
  bool animated = false;

  for (int i = 0; i < count; i++) {
    if (want_palette && !imported
        && saber_theme_import_hikari(candidates[i], palette)
               == SABER_PALETTE_SLOTS) {
      memcpy(config->theme.palette, palette, sizeof(palette));
      imported = true;
    }

    if (want_animation && !animated) {
      animated = saber_theme_import_hikari_animation(
          candidates[i], &config->panel.animation_ms);
    }

    g_free(candidates[i]);
  }

  return imported;
}

/* The whole of what a configuration file may reach. Settings are data, so the
chunk is handed arithmetic, strings and tables and nothing else: no io, no os,
no package, no require, no load, no dofile. Nothing named here can open a file
or start a process, and a name absent from this list is absent from the chunk's
globals, so the sandbox is this list rather than a set of removals a chunk could
work its way around. */
static const char *const config_environment[] = {
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

/* Function purpose: Give the chunk on top of the stack the restricted globals
above in place of Lua's own. _ENV is a chunk's first upvalue, which is what
makes this one substitution cover the whole file rather than a list of deletions
applying only where they were made. The chunk stays on top either way. */
static bool
config_sandbox(lua_State *L)
{
  lua_newtable(L);

  for (int i = 0; config_environment[i] != NULL; i++) {
    lua_getglobal(L, config_environment[i]);
    lua_setfield(L, -2, config_environment[i]);
  }

  /* Action purpose: A main chunk always has _ENV as its first upvalue, so this
  cannot fail in practice -- but lua_setupvalue pops nothing when it does not
  take the value, and an unnoticed failure would run the file with Lua's real
  globals, which is the one outcome the sandbox exists to prevent. */
  if (lua_setupvalue(L, -2, 1) == NULL) {
    lua_pop(L, 1);

    return false;
  }

  return true;
}

/* Function purpose: Take Lua's error off the stack as a string this file owns.
Lua reports "file:line: message", which is the whole value of passing it
through rather than answering with a bare refusal. */
static char *
config_take_error(lua_State *L)
{
  const char *message = lua_tostring(L, -1);
  char *copy = g_strdup(message == NULL ? "unreadable" : message);

  lua_pop(L, 1);

  return copy;
}

/* Function purpose: Load `path`, run it in the sandbox, and leave the table it
returned on top of the stack. A file that will not load, will not run, or does
not return a table is refused whole: `reload` reports it and the panel keeps the
settings it is already running with, because a configuration applied in part is
worse than one not applied at all. The stack is left as it was found on every
one of those paths. */
static bool
config_open(lua_State *L, const char *path, char **error)
{
  if (luaL_loadfile(L, path) != LUA_OK) {
    *error = config_take_error(L);

    return false;
  }

  if (!config_sandbox(L)) {
    lua_pop(L, 1);
    *error = g_strdup_printf("%s: not a Lua chunk", path);

    return false;
  }

  if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
    *error = config_take_error(L);

    return false;
  }

  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    *error = g_strdup_printf("%s: did not return a table", path);

    return false;
  }

  return true;
}

bool
saber_config_load(struct saber_config *config, const char *path)
{
  return saber_config_load_status(config, path, NULL);
}

bool
saber_config_load_status(struct saber_config *config,
    const char *path,
    enum saber_config_status *status)
{
  enum saber_config_status outcome = SABER_CONFIG_ABSENT;

  if (status != NULL) {
    *status = outcome;
  }

  if (config == NULL) {
    return false;
  }

  config_defaults(config);

  char *file = config_find(path);
  struct config_written written = { false, false };

  if (file != NULL) {
    lua_State *L = luaL_newstate();

    outcome = SABER_CONFIG_UNREADABLE;

    if (L == NULL) {
      fprintf(stderr, "saber: out of memory reading %s\n", file);
    } else {
      /* Action purpose: The standard libraries are opened so that math, string
      and table exist to be handed to the chunk. What the chunk can reach is
      decided by config_environment and not by what was opened here -- io and
      os are loaded into globals the chunk never sees. */
      luaL_openlibs(L);

      char *error = NULL;

      if (!config_open(L, file, &error)) {
        /* Action purpose: A broken configuration file costs the user their
        settings, never their panel -- report it and carry on with defaults.
        Lua's message already names the file and the line. */
        fprintf(stderr, "saber: %s\n", error == NULL ? "unreadable" : error);
        g_free(error);
      } else {
        /* The load is what the outcome turns on, not what the file contained:
        a chunk returning an empty table is a valid configuration that asks for
        the defaults. */
        outcome = SABER_CONFIG_LOADED;

        config_apply(config, L, &written);
        lua_pop(L, 1);
      }

      lua_close(L);
    }

    g_free(file);
  }

  config_resolve_order(config);

  /* Action purpose: Both imports hang off inherit-hikari, so the one key that
  turns the compositor's colours off turns its motion off with it -- the panel
  either follows the desktop or it does not. Either one already answered by the
  saber.lua is left alone. */
  bool want_palette = !written.palette && config->theme.inherit_hikari;
  bool want_animation = !written.animation_ms && config->theme.inherit_hikari;

  if (want_palette || want_animation) {
    bool imported = config_import_hikari(config, want_palette, want_animation);

    if (want_palette) {
      config->theme.palette_valid = imported;
    }
  }

  if (status != NULL) {
    *status = outcome;
  }

  return true;
}

void
saber_config_fini(struct saber_config *config)
{
  if (config == NULL) {
    return;
  }

  g_free(config->panel.output);
  g_free(config->items.order);
  g_strfreev(config->launcher.favourites);
  g_free(config->session.suspend);
  g_free(config->session.reboot);
  g_free(config->session.poweroff);
  g_free(config->session.lock);
  g_free(config->session.logout);

  memset(config, 0, sizeof(*config));
}

static char *
config_state_dir(void)
{
  const char *xdg = g_getenv("XDG_DATA_HOME");

  if (xdg != NULL && *xdg != '\0') {
    return g_build_filename(xdg, "saber", NULL);
  }

  const char *home = g_getenv("HOME");

  if (home != NULL && *home != '\0') {
    return g_build_filename(home, ".local", "share", "saber", NULL);
  }

  return NULL;
}

static char *
config_favourites_path(void)
{
  char *dir = config_state_dir();

  if (dir == NULL) {
    return NULL;
  }

  char *path = g_build_filename(dir, "favourites", NULL);

  g_free(dir);

  return path;
}

static char **
config_favourites_from_config(
    const struct saber_config *config, size_t *out_len)
{
  GPtrArray *items = g_ptr_array_new();

  for (size_t i = 0; config != NULL && i < config->launcher.favourites_len;
      i++) {
    g_ptr_array_add(items, g_strdup(config->launcher.favourites[i]));
  }

  *out_len = items->len;
  g_ptr_array_add(items, NULL);

  return (char **)g_ptr_array_free(items, FALSE);
}

bool
saber_config_load_favourites(
    const struct saber_config *config, char ***out, size_t *out_len)
{
  return saber_config_load_favourites_status(config, out, out_len, NULL);
}

bool
saber_config_load_favourites_status(const struct saber_config *config,
    char ***out,
    size_t *out_len,
    enum saber_config_favourites *status)
{
  enum saber_config_favourites outcome = SABER_FAVOURITES_SEEDED;

  if (status != NULL) {
    *status = outcome;
  }

  if (out == NULL || out_len == NULL) {
    return false;
  }

  char *path = config_favourites_path();
  char *contents = NULL;
  GError *error = NULL;

  if (path != NULL && !g_file_get_contents(path, &contents, NULL, &error)) {
    /* Action purpose: A file that is THERE and unreadable is not the same
    answer as no file at all, and the difference is a destructive one. Both
    paths below hand back the seed list, so a caller told only "true" would
    write that list straight back over a state file still holding the real
    order. The outcome is what lets it decline instead.

    The read attempt is what decides which case this is -- ONLY ENOENT means
    "no state file, seed the run". A g_file_test() ahead of it reopened the
    same hole this outcome exists to close: it stats, so every stat failure
    (a directory that cannot be traversed, an I/O error, a symlink loop) and
    everything that is not a regular file read as absence, and the seed list
    was then cleared to overwrite whatever is really there. Testing first also
    raced the read it guarded. */
    if (!g_error_matches(error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
      outcome = SABER_FAVOURITES_FAILED;
      fprintf(stderr, "saber: %s: %s\n", path, error->message);
    }

    g_clear_error(&error);
  }

  g_free(path);

  /* Action purpose: The state file wins whenever it exists, because it holds
  the live drag-to-reorder order; the config list only seeds the first run. */
  if (contents == NULL) {
    *out = config_favourites_from_config(config, out_len);

    if (status != NULL) {
      *status = outcome;
    }

    return true;
  }

  char **lines = g_strsplit(contents, "\n", -1);
  GPtrArray *items = g_ptr_array_new();

  g_free(contents);

  for (int i = 0; lines[i] != NULL; i++) {
    char *id = g_strstrip(lines[i]);

    if (*id != '\0' && *id != '#') {
      g_ptr_array_add(items, g_strdup(id));
    }
  }

  g_strfreev(lines);

  *out_len = items->len;
  g_ptr_array_add(items, NULL);
  *out = (char **)g_ptr_array_free(items, FALSE);

  if (status != NULL) {
    *status = SABER_FAVOURITES_LOADED;
  }

  return true;
}

bool
saber_config_save_favourites(char *const *favourites, size_t len)
{
  if (favourites == NULL && len > 0) {
    return false;
  }

  char *dir = config_state_dir();

  if (dir == NULL) {
    fprintf(stderr, "saber: no HOME or XDG_DATA_HOME; favourites not saved\n");

    return false;
  }

  bool saved = false;

  if (g_mkdir_with_parents(dir, 0700) != 0) {
    fprintf(stderr, "saber: %s: %s\n", dir, g_strerror(errno));
    g_free(dir);

    return false;
  }

  char *path = g_build_filename(dir, "favourites", NULL);
  char *temporary = g_strconcat(path, ".tmp", NULL);

  g_free(dir);

  /* Action purpose: Write beside the target and rename, so an interrupted save
  leaves the previous list intact rather than a truncated one. Same directory,
  because rename(2) is only atomic within a filesystem. */
  FILE *stream = g_fopen(temporary, "w");

  if (stream == NULL) {
    fprintf(stderr, "saber: %s: %s\n", temporary, g_strerror(errno));
  } else {
    bool written = true;

    for (size_t i = 0; written && i < len; i++) {
      if (favourites[i] != NULL && fprintf(stream, "%s\n", favourites[i]) < 0) {
        written = false;
      }
    }

    if (fflush(stream) != 0 || fsync(fileno(stream)) != 0) {
      written = false;
    }

    if (fclose(stream) != 0) {
      written = false;
    }

    if (!written) {
      fprintf(stderr, "saber: %s: %s\n", temporary, g_strerror(errno));
      g_unlink(temporary);
    } else if (g_rename(temporary, path) != 0) {
      fprintf(stderr, "saber: %s: %s\n", path, g_strerror(errno));
      g_unlink(temporary);
    } else {
      saved = true;
    }
  }

  g_free(temporary);
  g_free(path);

  return saved;
}
