/* Script function and purpose: app_id to desktop entry -- the BAMF replacement
(BLUEPRINT.md 5.6). A five-rung ladder over the desktop-entry index, plus a
launch window for the applications Saber started itself. */

#include <string.h>

#include <glib.h>

#include <saber/match.h>

struct saber_match_launch {
  char *desktop_id;
  gint64 at;
};

struct saber_match {
  struct saber_appinfo_index *index;

  /* Derived lookup tables. They hold borrowed entries and are therefore only
  valid for the index generation they were built from. */
  unsigned int generation;
  bool built;
  GHashTable *by_id_ci;
  GHashTable *by_wmclass;
  GHashTable *by_exec;
  GHashTable *by_tail;

  GQueue *launches; /* struct saber_match_launch *, oldest first */
};

static void
launch_free(struct saber_match_launch *launch)
{
  g_free(launch->desktop_id);
  g_free(launch);
}

/* Only the first entry to claim a key keeps it: the scan order already
expresses precedence, so a later duplicate must not displace it. */
static void
claim(GHashTable *table, char *key, struct saber_appinfo *app)
{
  if (key == NULL || *key == '\0' || g_hash_table_contains(table, key)) {
    g_free(key);
    return;
  }
  g_hash_table_insert(table, key, app);
}

/* Function purpose: The name a user would recognise as "the program", pulled
out of an Exec line. Wrapper prefixes matter here -- "env LC_ALL=C foo %U" and
"flatpak run org.x.Y" both name something other than the application in their
first token, and taking that token verbatim would index every such entry under
the wrapper. */
static char *
exec_basename(const char *exec)
{
  int count = 0;
  char **argv = NULL;
  if (!g_shell_parse_argv(exec, &count, &argv, NULL) || count == 0) {
    g_strfreev(argv);
    return NULL;
  }

  int i = 0;
  char *lead = g_path_get_basename(argv[0]);
  if (g_strcmp0(lead, "env") == 0) {
    for (i = 1; i < count && strchr(argv[i], '=') != NULL; i++) {
      continue;
    }
  }
  g_free(lead);

  char *name = i < count ? g_path_get_basename(argv[i]) : NULL;
  g_strfreev(argv);

  if (name == NULL) {
    return NULL;
  }
  char *folded = g_ascii_strdown(name, -1);
  g_free(name);
  return folded;
}

/* Function purpose: The tail of a reverse-DNS desktop-file ID, which is what a
toplevel usually reports: org.mozilla.firefox.desktop yields "firefox". */
static char *
reverse_dns_tail(const char *id)
{
  char *base = g_strdup(id);
  if (g_str_has_suffix(base, ".desktop")) {
    base[strlen(base) - strlen(".desktop")] = '\0';
  }

  const char *dot = strrchr(base, '.');
  char *tail = dot != NULL && dot[1] != '\0' ? g_ascii_strdown(dot + 1, -1)
                                             : NULL;
  g_free(base);
  return tail;
}

static void
rebuild(struct saber_match *match)
{
  g_hash_table_remove_all(match->by_id_ci);
  g_hash_table_remove_all(match->by_wmclass);
  g_hash_table_remove_all(match->by_exec);
  g_hash_table_remove_all(match->by_tail);

  size_t count = saber_appinfo_index_size(match->index);
  for (size_t i = 0; i < count; i++) {
    struct saber_appinfo *app = saber_appinfo_index_nth(match->index, i);

    claim(match->by_id_ci, g_ascii_strdown(app->id, -1), app);
    if (app->startup_wm_class != NULL) {
      claim(match->by_wmclass, g_ascii_strdown(app->startup_wm_class, -1),
          app);
    }
    if (app->exec != NULL) {
      claim(match->by_exec, exec_basename(app->exec), app);
    }
    claim(match->by_tail, reverse_dns_tail(app->id), app);
  }

  match->generation = saber_appinfo_index_generation(match->index);
  match->built = true;
}

static void
ensure_tables(struct saber_match *match)
{
  if (!match->built ||
      match->generation != saber_appinfo_index_generation(match->index)) {
    rebuild(match);
  }
}

static void
prune_launches(struct saber_match *match)
{
  gint64 cutoff =
      g_get_monotonic_time() - (gint64)SABER_MATCH_LAUNCH_WINDOW_MS * 1000;

  for (;;) {
    struct saber_match_launch *head = g_queue_peek_head(match->launches);
    if (head == NULL || head->at >= cutoff) {
      return;
    }
    launch_free(g_queue_pop_head(match->launches));
  }
}

static struct saber_appinfo *
take_launch(struct saber_match *match)
{
  prune_launches(match);

  while (!g_queue_is_empty(match->launches)) {
    struct saber_match_launch *launch = g_queue_pop_head(match->launches);
    struct saber_appinfo *app =
        saber_appinfo_index_lookup(match->index, launch->desktop_id);
    launch_free(launch);
    if (app != NULL) {
      return app;
    }
  }
  return NULL;
}

struct saber_match *
saber_match_create(struct saber_appinfo_index *index)
{
  struct saber_match *match = g_new0(struct saber_match, 1);
  match->index = index;
  match->by_id_ci = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
      NULL);
  match->by_wmclass = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
      NULL);
  match->by_exec = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
      NULL);
  match->by_tail = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
      NULL);
  match->launches = g_queue_new();
  return match;
}

void
saber_match_destroy(struct saber_match *match)
{
  if (match == NULL) {
    return;
  }

  g_queue_free_full(match->launches, (GDestroyNotify)launch_free);
  g_hash_table_destroy(match->by_id_ci);
  g_hash_table_destroy(match->by_wmclass);
  g_hash_table_destroy(match->by_exec);
  g_hash_table_destroy(match->by_tail);
  g_free(match);
}

struct saber_appinfo *
saber_match_resolve(struct saber_match *match, const char *app_id)
{
  if (match == NULL || app_id == NULL || *app_id == '\0') {
    return NULL;
  }
  ensure_tables(match);

  char *id = g_str_has_suffix(app_id, ".desktop")
      ? g_strdup(app_id)
      : g_strconcat(app_id, ".desktop", NULL);

  struct saber_appinfo *app = saber_appinfo_index_lookup(match->index, id);
  if (app == NULL) {
    char *folded = g_ascii_strdown(id, -1);
    app = g_hash_table_lookup(match->by_id_ci, folded);
    g_free(folded);
  }
  g_free(id);

  if (app == NULL) {
    char *key = g_ascii_strdown(app_id, -1);
    app = g_hash_table_lookup(match->by_wmclass, key);
    if (app == NULL) {
      app = g_hash_table_lookup(match->by_exec, key);
    }
    if (app == NULL) {
      app = g_hash_table_lookup(match->by_tail, key);
    }
    g_free(key);
  }

  if (app == NULL) {
    app = take_launch(match);
  }
  return app;
}

void
saber_match_note_launch(struct saber_match *match, const char *desktop_id)
{
  if (match == NULL || desktop_id == NULL) {
    return;
  }

  prune_launches(match);

  struct saber_match_launch *launch = g_new0(struct saber_match_launch, 1);
  launch->desktop_id = g_strdup(desktop_id);
  launch->at = g_get_monotonic_time();
  g_queue_push_tail(match->launches, launch);
}

bool
saber_match_is_launching(const struct saber_match *match,
    const char *desktop_id)
{
  if (match == NULL || desktop_id == NULL) {
    return false;
  }

  gint64 cutoff =
      g_get_monotonic_time() - (gint64)SABER_MATCH_LAUNCH_WINDOW_MS * 1000;

  for (GList *node = match->launches->head; node != NULL; node = node->next) {
    const struct saber_match_launch *launch = node->data;
    if (launch->at >= cutoff &&
        g_strcmp0(launch->desktop_id, desktop_id) == 0) {
      return true;
    }
  }
  return false;
}
