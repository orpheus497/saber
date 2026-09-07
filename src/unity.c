/* Script function and purpose: The com.canonical.Unity.LauncherEntry receiver
-- owns the bus name applications wait for, matches Update on any object path,
and accumulates per-application badge state. Written fresh for Saber; sofi has
no launcher-entry code to port from. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <gio/gio.h>
#include <glib.h>

#include <saber/unity.h>

#define UNITY_BUS_NAME "com.canonical.Unity"
#define LAUNCHER_ENTRY_INTERFACE "com.canonical.Unity.LauncherEntry"
#define LAUNCHER_ENTRY_SIGNAL "Update"

/* app_uri is "application://<desktop-file-id>". */
#define APPLICATION_SCHEME "application://"

/* Action purpose: LauncherEntry.Update carries no authentication of any kind --
the desktop ID is a string in the payload and the protocol offers no way to
prove the sender owns it. That is a property of the protocol, not something this
module can repair, and every implementation of it trusts the sender. What can be
bounded is the damage: an unbounded insert-only table let any session peer grow
the panel's memory without limit by emitting one Update per made-up ID. These
two caps plus the name watch below turn that into a fixed ceiling that empties
itself when publishers exit. Both are far above any real desktop. */
#define UNITY_MAX_ENTRIES 256
#define UNITY_MAX_ID_LEN 256

struct saber_unity {
  GDBusConnection *connection;
  guint owner_id;
  guint signal_sub;
  bool owned;

  GHashTable *entries; /* desktop-file ID -> struct saber_unity_entry * */

  /* desktop-file ID -> g_bus_watch_name id for the peer that published it.
  Without this a badge outlived the application that set it for the whole
  session, because nothing ever noticed the publisher going away. */
  GHashTable *watches;

  saber_unity_changed_func cb;
  void *user;
};

static void
entry_destroy(gpointer data)
{
  struct saber_unity_entry *entry = data;

  if (entry == NULL) {
    return;
  }

  g_free(entry->quicklist);
  g_free(entry->bus_name);
  g_free(entry);
}

/* Function purpose: Reduce app_uri to the desktop-file ID the launcher model is
keyed on. Returns NULL for a URI that names no entry. */
static const char *
desktop_id_of(const char *app_uri)
{
  if (app_uri == NULL) {
    return NULL;
  }

  if (g_str_has_prefix(app_uri, APPLICATION_SCHEME)) {
    app_uri += strlen(APPLICATION_SCHEME);
  }

  /* Action purpose: Not every sender writes the scheme -- some emit the bare
  desktop-file ID -- so a URI that survives the strip unchanged is accepted as
  one rather than dropped. A path is not: it would key the model on something
  no desktop entry can ever match. */
  if (app_uri[0] == '\0' || strchr(app_uri, '/') != NULL) {
    return NULL;
  }

  /* A desktop-file ID is a filename; anything this long is not one, and it is
  the hash key, so it is the peer choosing how much memory to spend. */
  if (strlen(app_uri) > UNITY_MAX_ID_LEN) {
    return NULL;
  }

  return app_uri;
}

static bool
lookup_bool(GVariant *props, const char *key, bool *out)
{
  gboolean value = FALSE;

  if (!g_variant_lookup(props, key, "b", &value)) {
    return false;
  }

  *out = value;

  return true;
}

/* Function purpose: Merge one Update into an application's accumulated state.
Only the keys present are touched: an application that sends `count` alone must
not have its progress or urgency reset by the omission. */
static void
apply_update(struct saber_unity_entry *entry,
    const char *sender,
    GVariant *props)
{
  gint64 count = 0;

  if (g_variant_lookup(props, "count", "x", &count)) {
    entry->count = count;
  }

  gdouble progress = 0.0;

  if (g_variant_lookup(props, "progress", "d", &progress)) {
    entry->progress = CLAMP(progress, 0.0, 1.0);
  }

  lookup_bool(props, "count-visible", &entry->count_visible);
  lookup_bool(props, "progress-visible", &entry->progress_visible);
  lookup_bool(props, "urgent", &entry->urgent);
  lookup_bool(props, "updating", &entry->updating);

  /* Action purpose: The specification types quicklist as `s`, but libunity
  itself has always sent the menu's object path, and several bindings send it
  as an `o` variant. Both are the same string and both are accepted; an empty
  one unsets the menu. */
  GVariant *quicklist = g_variant_lookup_value(props, "quicklist", NULL);

  if (quicklist != NULL) {
    if (g_variant_is_of_type(quicklist, G_VARIANT_TYPE_STRING) ||
        g_variant_is_of_type(quicklist, G_VARIANT_TYPE_OBJECT_PATH)) {
      g_free(entry->quicklist);
      entry->quicklist = g_variant_dup_string(quicklist, NULL);
    }

    g_variant_unref(quicklist);
  }

  /* The quicklist path is only openable against the bus name that published
  it, and that name is the signal's sender rather than anything in the
  payload. */
  if (sender != NULL && g_strcmp0(entry->bus_name, sender) != 0) {
    g_free(entry->bus_name);
    entry->bus_name = g_strdup(sender);
  }
}

/* Records which publisher a badge belongs to, so the badge can be withdrawn
when that peer leaves the bus. */
struct unity_watch {
  struct saber_unity *unity;
  char *desktop_id;
};

static void
unity_watch_free(gpointer data)
{
  struct unity_watch *watch = data;

  g_free(watch->desktop_id);
  g_free(watch);
}

static void
watch_id_unwatch(gpointer data)
{
  guint id = GPOINTER_TO_UINT(data);

  if (id != 0) {
    g_bus_unwatch_name(id);
  }
}

/* Function purpose: Withdraw an application's badge when its process leaves the
session bus. Without this a count or progress bar set by an application that has
since exited stayed on the tile for the rest of the session, because nothing in
the protocol says "clear". */
static void
on_publisher_vanished(GDBusConnection *connection,
    const char *name,
    gpointer data)
{
  struct unity_watch *watch = data;
  struct saber_unity *unity = watch->unity;

  (void)connection;
  (void)name;

  /* Action purpose: Copy the id first. Removing from `watches` unwatches the
  name, which frees `watch` -- from inside its own callback. Nothing may read
  `watch` after the first removal below. */
  char *desktop_id = g_strdup(watch->desktop_id);

  g_hash_table_remove(unity->watches, desktop_id);
  g_hash_table_remove(unity->entries, desktop_id);

  if (unity->cb != NULL) {
    /* A zeroed entry is "no badge", which is what the tile must fall back to. */
    struct saber_unity_entry cleared = { 0 };

    unity->cb(desktop_id, &cleared, unity->user);
  }

  g_free(desktop_id);
}

/* Function purpose: Watch the peer that published this badge, replacing any
previous watch for the same tile -- an application that restarts publishes under
a new unique name, and the stale watch would otherwise never fire. */
static void
unity_watch_publisher(struct saber_unity *unity,
    const char *desktop_id,
    const char *sender)
{
  if (unity->connection == NULL || sender == NULL || !g_dbus_is_name(sender)) {
    return;
  }

  struct unity_watch *watch = g_new0(struct unity_watch, 1);

  watch->unity = unity;
  watch->desktop_id = g_strdup(desktop_id);

  guint id = g_bus_watch_name_on_connection(unity->connection,
      sender,
      G_BUS_NAME_WATCHER_FLAGS_NONE,
      NULL,
      on_publisher_vanished,
      watch,
      unity_watch_free);

  /* Replaces any existing watch for this tile; the old id is unwatched by the
  table's value destructor. */
  g_hash_table_insert(unity->watches, g_strdup(desktop_id),
      GUINT_TO_POINTER(id));
}

static void
on_update(GDBusConnection *connection,
    const char *sender,
    const char *object_path,
    const char *interface_name,
    const char *signal_name,
    GVariant *parameters,
    gpointer data)
{
  struct saber_unity *unity = data;

  (void)connection;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;

  if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sa{sv})"))) {
    return;
  }

  const char *app_uri = NULL;
  GVariant *props = NULL;

  g_variant_get(parameters, "(&s@a{sv})", &app_uri, &props);

  const char *desktop_id = desktop_id_of(app_uri);

  if (desktop_id == NULL) {
    g_variant_unref(props);
    return;
  }

  struct saber_unity_entry *entry =
      g_hash_table_lookup(unity->entries, desktop_id);

  if (entry == NULL) {
    /* Action purpose: Refuse only NEW tiles once the ceiling is reached, so a
    peer flooding made-up IDs cannot grow the table but also cannot stop the
    applications already on it from updating their own badges. */
    if (g_hash_table_size(unity->entries) >= UNITY_MAX_ENTRIES) {
      g_debug("saber: ignoring a LauncherEntry update for %s; the badge table "
              "is at its %d-entry ceiling",
          desktop_id, UNITY_MAX_ENTRIES);
      g_variant_unref(props);

      return;
    }

    entry = g_new0(struct saber_unity_entry, 1);
    entry->quicklist = g_strdup("");
    entry->bus_name = g_strdup("");
    g_hash_table_insert(unity->entries, g_strdup(desktop_id), entry);
  }

  apply_update(entry, sender, props);
  unity_watch_publisher(unity, desktop_id, sender);
  g_variant_unref(props);

  if (unity->cb != NULL) {
    unity->cb(desktop_id, entry, unity->user);
  }
}

static void
on_name_acquired(GDBusConnection *connection, const char *name, gpointer data)
{
  struct saber_unity *unity = data;

  (void)connection;
  (void)name;

  unity->owned = true;
}

static void
on_name_lost(GDBusConnection *connection, const char *name, gpointer data)
{
  struct saber_unity *unity = data;

  (void)connection;

  /* Action purpose: Say it, because nothing else will. Applications watch this
  name and publish only while somebody owns it; if another launcher holds it,
  every badge Saber draws is dead and there is no error anywhere to explain
  why. */
  unity->owned = false;
  g_warning("saber: %s is owned by another launcher; count badges, progress "
            "and quicklists will not update.",
      name);
}

struct saber_unity *
saber_unity_create(saber_unity_changed_func cb, void *user)
{
  GError *error = NULL;
  GDBusConnection *connection =
      g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);

  if (connection == NULL) {
    g_warning("saber: cannot reach the session bus for launcher badges: %s",
        error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    return NULL;
  }

  struct saber_unity *unity = g_new0(struct saber_unity, 1);

  unity->connection = connection;
  unity->cb = cb;
  unity->user = user;
  unity->entries = g_hash_table_new_full(g_str_hash,
      g_str_equal,
      g_free,
      entry_destroy);

  unity->watches = g_hash_table_new_full(g_str_hash,
      g_str_equal,
      g_free,
      watch_id_unwatch);

  /* Action purpose: Subscribe with a NULL object path. Applications emit
  LauncherEntry.Update on a path of their own choosing -- libunity uses one
  derived from the desktop-file ID -- so matching a path would match almost
  nothing. The interface and member are the whole match. */
  unity->signal_sub = g_dbus_connection_signal_subscribe(connection,
      NULL,
      LAUNCHER_ENTRY_INTERFACE,
      LAUNCHER_ENTRY_SIGNAL,
      NULL,
      NULL,
      G_DBUS_SIGNAL_FLAGS_NONE,
      on_update,
      unity,
      NULL);

  /* Action purpose: And only now claim the name -- subscribing afterwards would
  race the burst of Updates applications send the instant they see it owned.
  Owning `com.canonical.Unity` is the single thing that makes this protocol
  work: an application checks for the name, publishes its full LauncherEntry
  state when it appears, and stays completely silent while it is unowned. A
  panel that merely matches the signal without claiming the name shows no
  badges and produces no error anywhere. */
  unity->owner_id = g_bus_own_name_on_connection(connection,
      UNITY_BUS_NAME,
      G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
      on_name_acquired,
      on_name_lost,
      unity,
      NULL);

  return unity;
}

void
saber_unity_destroy(struct saber_unity *unity)
{
  if (unity == NULL) {
    return;
  }

  if (unity->signal_sub != 0) {
    g_dbus_connection_signal_unsubscribe(unity->connection,
        unity->signal_sub);
  }

  if (unity->owner_id != 0) {
    g_bus_unown_name(unity->owner_id);
  }

  /* Before the entries: every watch holds a pointer back into `unity`, and its
  vanish callback removes from both tables. */
  if (unity->watches != NULL) {
    g_hash_table_destroy(unity->watches);
  }

  if (unity->entries != NULL) {
    g_hash_table_destroy(unity->entries);
  }

  g_object_unref(unity->connection);
  g_free(unity);
}

const struct saber_unity_entry *
saber_unity_get(const struct saber_unity *unity, const char *desktop_id)
{
  if (unity == NULL || desktop_id == NULL) {
    return NULL;
  }

  return g_hash_table_lookup(unity->entries, desktop_id);
}

bool
saber_unity_is_owner(const struct saber_unity *unity)
{
  return unity != NULL && unity->owned;
}

GDBusConnection *
saber_unity_connection(const struct saber_unity *unity)
{
  return unity == NULL ? NULL : unity->connection;
}
