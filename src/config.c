/* Script function and purpose: Parse saber.conf (UCL, via libucl), apply a
default for every absent key, import the compositor's palette, and read and
write the favourites state file. Nothing here is fatal: a user with no
configuration, or a broken one, still gets a panel. */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <ucl.h>

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

/* Action purpose: The documented keys are hyphenated (`icon-size`), but UCL
accepts either and users copy from both the manual and hikari.conf, so the
underscore spelling resolves to the same key rather than silently doing
nothing. */
static const ucl_object_t *
config_lookup(const ucl_object_t *obj, const char *key)
{
  if (obj == NULL) {
    return NULL;
  }

  const ucl_object_t *found = ucl_object_lookup(obj, key);

  if (found != NULL || strchr(key, '-') == NULL) {
    return found;
  }

  char *alternative = g_strdup(key);

  for (char *c = alternative; *c != '\0'; c++) {
    if (*c == '-') {
      *c = '_';
    }
  }

  found = ucl_object_lookup(obj, alternative);
  g_free(alternative);

  return found;
}

static void
config_apply_string(const ucl_object_t *obj, const char *key, char **out)
{
  const ucl_object_t *entry = config_lookup(obj, key);
  const char *value = entry == NULL ? NULL : ucl_object_tostring(entry);

  if (value == NULL) {
    return;
  }

  g_free(*out);
  *out = g_strdup(value);
}

static void
config_apply_bool(const ucl_object_t *obj, const char *key, bool *out)
{
  const ucl_object_t *entry = config_lookup(obj, key);
  bool value;

  if (entry != NULL && ucl_object_toboolean_safe(entry, &value)) {
    *out = value;
  }
}

static void
config_apply_int(
    const ucl_object_t *obj, const char *key, int *out, int low, int high)
{
  const ucl_object_t *entry = config_lookup(obj, key);
  int64_t value;

  if (entry == NULL || !ucl_object_toint_safe(entry, &value)) {
    return;
  }

  *out = (int)CLAMP(value, (int64_t)low, (int64_t)high);
}

static void
config_apply_double(
    const ucl_object_t *obj, const char *key, double *out, double low,
    double high)
{
  const ucl_object_t *entry = config_lookup(obj, key);
  double value;

  if (entry == NULL || !ucl_object_todouble_safe(entry, &value)) {
    return;
  }

  *out = CLAMP(value, low, high);
}

/* Function purpose: Returns `fallback` for an absent or unrecognised key, and
takes the value by return rather than through a pointer so no caller has to
alias an enum through an int *. */
static int
config_enum(const ucl_object_t *obj, const char *key,
    const struct saber_enum_name *names, int fallback, const char *where)
{
  const ucl_object_t *entry = config_lookup(obj, key);
  const char *value = entry == NULL ? NULL : ucl_object_tostring(entry);

  if (value == NULL) {
    return fallback;
  }

  for (const struct saber_enum_name *n = names; n->name != NULL; n++) {
    if (g_ascii_strcasecmp(value, n->name) == 0) {
      return n->value;
    }
  }

  fprintf(stderr, "saber: %s: %s: unknown value \"%s\"; keeping default\n",
      where, key, value);

  return fallback;
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

/* Function purpose: Resolve which saber.conf to read. An explicit path is
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
    candidate = g_build_filename(xdg, "saber", "saber.conf", NULL);

    if (g_file_test(candidate, G_FILE_TEST_IS_REGULAR)) {
      return candidate;
    }

    g_free(candidate);
  }

  if (home != NULL && *home != '\0') {
    candidate = g_build_filename(home, ".config", "saber", "saber.conf", NULL);

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
config_apply_favourites(struct saber_config *config, const ucl_object_t *list)
{
  if (list == NULL) {
    return;
  }

  GPtrArray *items = g_ptr_array_new();
  ucl_object_iter_t iterator = NULL;
  const ucl_object_t *entry;

  /* Action purpose: Iterating works for a one-element list written without
  brackets as well as for an array, so `favourites = "sofi.desktop"` is not a
  silent no-op. */
  while ((entry = ucl_object_iterate(list, &iterator, true)) != NULL) {
    const char *id = ucl_object_tostring(entry);

    if (id != NULL && *id != '\0') {
      g_ptr_array_add(items, g_strdup(id));
    }
  }

  g_strfreev(config->launcher.favourites);
  config->launcher.favourites_len = items->len;
  g_ptr_array_add(items, NULL);
  config->launcher.favourites = (char **)g_ptr_array_free(items, FALSE);
}

/* Function purpose: Read a `theme { palette }` block. Returns true only for a
complete sixteen-slot palette, which is what lets an explicit block override
the hikari import without a half-written one doing so. */
static bool
config_apply_palette(struct saber_config *config, const ucl_object_t *theme)
{
  const ucl_object_t *block = config_lookup(theme, "palette");

  if (block == NULL) {
    return false;
  }

  struct saber_color palette[SABER_PALETTE_SLOTS];
  int count = 0;

  for (int slot = 0; slot < SABER_PALETTE_SLOTS; slot++) {
    char key[16];

    snprintf(key, sizeof(key), "color%d", slot);

    const ucl_object_t *entry = ucl_object_lookup(block, key);
    const char *spec = entry == NULL ? NULL : ucl_object_tostring(entry);

    if (spec != NULL && saber_color_parse(spec, &palette[slot])) {
      count++;
    }
  }

  if (count != SABER_PALETTE_SLOTS) {
    fprintf(stderr,
        "saber: theme { palette }: %d of %d colours usable; ignoring it\n",
        count, SABER_PALETTE_SLOTS);

    return false;
  }

  memcpy(config->theme.palette, palette, sizeof(palette));
  config->theme.palette_valid = true;

  return true;
}

/* Function purpose: Read `items { order }`, the ordered portion list. An
unrecognised name is reported rather than silently dropped -- config_enum's
precedent -- and a repeat is ignored, because a portion is one place in the
column and cannot be in two. */
static void
config_apply_order(struct saber_config *config, const ucl_object_t *list)
{
  if (list == NULL) {
    return;
  }

  enum saber_portion order[SABER_PORTION_COUNT];
  bool seen[SABER_PORTION_COUNT] = { false };
  size_t len = 0;
  ucl_object_iter_t iterator = NULL;
  const ucl_object_t *entry;

  /* Same reason as the favourites list: iterating covers a one-element list
  written without brackets as well as an array. */
  while ((entry = ucl_object_iterate(list, &iterator, true)) != NULL) {
    const char *name = ucl_object_tostring(entry);
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
          "saber: items: order: unknown portion \"%s\"; ignoring it\n",
          name == NULL ? "" : name);

      continue;
    }

    if (!seen[match->value]) {
      seen[match->value] = true;
      order[len++] = (enum saber_portion)match->value;
    }
  }

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

static bool
config_apply(struct saber_config *config, const ucl_object_t *root)
{
  const ucl_object_t *panel = ucl_object_lookup(root, "panel");
  const ucl_object_t *theme = ucl_object_lookup(root, "theme");
  const ucl_object_t *launcher = ucl_object_lookup(root, "launcher");
  const ucl_object_t *items = ucl_object_lookup(root, "items");
  const ucl_object_t *session = ucl_object_lookup(root, "session");

  config_apply_string(panel, "output", &config->panel.output);
  config->panel.edge = config_enum(
      panel, "edge", saber_edge_names, config->panel.edge, "panel");
  config_apply_int(panel, "icon-size", &config->panel.icon_size,
      SABER_ICON_SIZE_MIN, SABER_ICON_SIZE_MAX);
  config_apply_int(panel, "padding", &config->panel.padding,
      SABER_PANEL_PADDING_MIN, SABER_PANEL_PADDING_MAX);
  config->panel.autohide = config_enum(panel, "autohide", saber_autohide_names,
      config->panel.autohide, "panel");
  config_apply_int(
      panel, "reveal-pressure", &config->panel.reveal_pressure, 0, 100000);
  config_apply_int(panel, "animation-ms", &config->panel.animation_ms, 0, 10000);

  config_apply_bool(theme, "inherit-hikari", &config->theme.inherit_hikari);
  config->theme.backlight = config_enum(theme, "backlight",
      saber_backlight_names, config->theme.backlight, "theme");
  config_apply_double(theme, "opacity", &config->theme.opacity, 0.0, 1.0);
  config_apply_double(
      theme, "overlay-opacity", &config->theme.overlay_opacity, 0.0, 1.0);

  config_apply_favourites(config, config_lookup(launcher, "favourites"));

  config_apply_bool(items, "bfb", &config->items.bfb);
  config_apply_bool(items, "sheets", &config->items.sheets);
  config_apply_bool(items, "devices", &config->items.devices);
  config_apply_bool(items, "trash", &config->items.trash);
  config_apply_bool(items, "tray", &config->items.tray);
  config_apply_bool(items, "session", &config->items.session);
  config_apply_order(config, config_lookup(items, "order"));

  config_apply_string(session, "suspend", &config->session.suspend);
  config_apply_string(session, "reboot", &config->session.reboot);
  config_apply_string(session, "poweroff", &config->session.poweroff);
  config_apply_string(session, "lock", &config->session.lock);
  config_apply_string(session, "logout", &config->session.logout);

  return theme != NULL && config_apply_palette(config, theme);
}

/* Function purpose: Try each hikari.conf in turn, user first. Only a complete
sixteen-slot import counts, so a truncated or half-commented palette falls
through to the next candidate rather than tinting the panel with defaults for
the missing half. */
static bool
config_import_hikari(struct saber_config *config)
{
  const char *xdg = g_getenv("XDG_CONFIG_HOME");
  const char *home = g_getenv("HOME");
  char *candidates[3];
  int count = 0;

  if (xdg != NULL && *xdg != '\0') {
    candidates[count++] = g_build_filename(xdg, "hikari", "hikari.conf", NULL);
  }

  if (home != NULL && *home != '\0') {
    candidates[count++] =
        g_build_filename(home, ".config", "hikari", "hikari.conf", NULL);
  }

  candidates[count++] = g_strdup(SABER_HIKARI_SYSTEM_CONFIG);

  struct saber_color palette[SABER_PALETTE_SLOTS];
  bool imported = false;

  for (int i = 0; i < count; i++) {
    if (!imported
        && saber_theme_import_hikari(candidates[i], palette)
               == SABER_PALETTE_SLOTS) {
      memcpy(config->theme.palette, palette, sizeof(palette));
      imported = true;
    }

    g_free(candidates[i]);
  }

  return imported;
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
  bool explicit_palette = false;

  if (file != NULL) {
    struct ucl_parser *parser = ucl_parser_new(UCL_PARSER_NO_IMPLICIT_ARRAYS);

    outcome = SABER_CONFIG_UNREADABLE;

    if (parser == NULL) {
      fprintf(stderr, "saber: out of memory reading %s\n", file);
    } else if (!ucl_parser_add_file(parser, file)) {
      const char *error = ucl_parser_get_error(parser);

      /* Action purpose: A broken configuration file costs the user their
      settings, never their panel -- report it and carry on with defaults. */
      fprintf(stderr, "saber: %s: %s\n", file,
          error == NULL ? "unreadable" : error);
    } else {
      /* The parse is what the outcome turns on, not what the file contained:
      an empty file is a valid configuration that asks for the defaults. */
      outcome = SABER_CONFIG_LOADED;

      ucl_object_t *root = ucl_parser_get_object(parser);

      if (root != NULL) {
        explicit_palette = config_apply(config, root);
        ucl_object_unref(root);
      }
    }

    if (parser != NULL) {
      ucl_parser_free(parser);
    }

    g_free(file);
  }

  config_resolve_order(config);

  if (!explicit_palette && config->theme.inherit_hikari) {
    config->theme.palette_valid = config_import_hikari(config);
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

  if (path != NULL && g_file_test(path, G_FILE_TEST_IS_REGULAR)
      && !g_file_get_contents(path, &contents, NULL, &error)) {
    /* Action purpose: A file that is THERE and unreadable is not the same
    answer as no file at all, and the difference is a destructive one. Both
    paths below hand back the seed list, so a caller told only "true" would
    write that list straight back over a state file still holding the real
    order. The outcome is what lets it decline instead. */
    outcome = SABER_FAVOURITES_FAILED;
    fprintf(stderr, "saber: %s: %s\n", path, error->message);
    g_error_free(error);
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
