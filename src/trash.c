/* Script function and purpose: XDG trash spec -- $XDG_DATA_HOME/Trash with its
files/ and info/ pair -- kept live by a GFileMonitor, which GLib backs with
kqueue on FreeBSD, so this needs no polling loop. */

#include <stdbool.h>

#include <gio/gio.h>
#include <glib.h>

#include <saber/trash.h>

/* Action purpose: A single delete writes both files/ and info/ and a
drag-and-drop of a selection writes many; one recount per burst is enough. */
#define TRASH_COALESCE_MS 200
#define TRASH_RETRY_S 5

struct saber_trash {
  char *root;
  char *files;
  char *info;

  GFileMonitor *files_monitor;
  GFileMonitor *root_monitor;

  guint recount_source;
  unsigned int count;

  saber_trash_cb cb;
  void *user;
};

static void retune(struct saber_trash *trash);

static unsigned int
count_entries(const char *path)
{
  GFile *dir = g_file_new_for_path(path);
  GFileEnumerator *entries = g_file_enumerate_children(dir,
      G_FILE_ATTRIBUTE_STANDARD_NAME,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
      NULL,
      NULL);

  g_object_unref(dir);

  if (entries == NULL) {
    return 0;
  }

  unsigned int count = 0;
  GFileInfo *info = NULL;

  while ((info = g_file_enumerator_next_file(entries, NULL, NULL)) != NULL) {
    count++;
    g_object_unref(info);
  }

  g_file_enumerator_close(entries, NULL, NULL);
  g_object_unref(entries);

  return count;
}

static gboolean
on_recount(gpointer data)
{
  struct saber_trash *trash = data;

  trash->recount_source = 0;
  retune(trash);

  unsigned int count = count_entries(trash->files);

  if (count != trash->count) {
    trash->count = count;
    if (trash->cb != NULL) {
      trash->cb(count, trash->user);
    }
  }

  /* Action purpose: With no Trash/ there is nothing a monitor can attach to, so
  retry slowly rather than stay deaf until the panel restarts. Once Trash/ is
  watched its own events cover files/ appearing. */
  if (trash->root_monitor == NULL) {
    trash->recount_source =
        g_timeout_add_seconds(TRASH_RETRY_S, on_recount, trash);
  }

  return G_SOURCE_REMOVE;
}

static void
schedule_recount(struct saber_trash *trash)
{
  if (trash->recount_source == 0) {
    trash->recount_source =
        g_timeout_add(TRASH_COALESCE_MS, on_recount, trash);
  }
}

static void
on_change(GFileMonitor *monitor,
    GFile *file,
    GFile *other,
    GFileMonitorEvent event,
    gpointer data)
{
  (void)monitor;
  (void)file;
  (void)other;
  (void)event;

  schedule_recount(data);
}

static GFileMonitor *
watch(const char *path, struct saber_trash *trash)
{
  GFile *dir = g_file_new_for_path(path);
  GFileMonitor *monitor =
      g_file_monitor_directory(dir, G_FILE_MONITOR_NONE, NULL, NULL);

  g_object_unref(dir);

  if (monitor != NULL) {
    g_signal_connect(monitor, "changed", G_CALLBACK(on_change), trash);
  }

  return monitor;
}

/* Function purpose: Keep the monitors attached to whichever of the two
directories currently exists. The trash tree is created lazily by whatever first
deletes a file, so the watch on Trash/ is what notices files/ appearing and
lets the watch on files/ be established then. */
static void
retune(struct saber_trash *trash)
{
  if (trash->root_monitor == NULL && g_file_test(trash->root, G_FILE_TEST_IS_DIR)) {
    trash->root_monitor = watch(trash->root, trash);
  }

  if (trash->files_monitor == NULL &&
      g_file_test(trash->files, G_FILE_TEST_IS_DIR)) {
    trash->files_monitor = watch(trash->files, trash);
  }
}

struct saber_trash *
saber_trash_create(saber_trash_cb cb, void *user)
{
  struct saber_trash *trash = g_new0(struct saber_trash, 1);

  trash->root = g_build_filename(g_get_user_data_dir(), "Trash", NULL);
  trash->files = g_build_filename(trash->root, "files", NULL);
  trash->info = g_build_filename(trash->root, "info", NULL);
  trash->cb = cb;
  trash->user = user;

  retune(trash);
  trash->count = count_entries(trash->files);

  if (trash->root_monitor == NULL) {
    schedule_recount(trash);
  }

  return trash;
}

void
saber_trash_destroy(struct saber_trash *trash)
{
  if (trash == NULL) {
    return;
  }

  g_clear_handle_id(&trash->recount_source, g_source_remove);

  if (trash->files_monitor != NULL) {
    g_file_monitor_cancel(trash->files_monitor);
    g_object_unref(trash->files_monitor);
  }

  if (trash->root_monitor != NULL) {
    g_file_monitor_cancel(trash->root_monitor);
    g_object_unref(trash->root_monitor);
  }

  g_free(trash->root);
  g_free(trash->files);
  g_free(trash->info);
  g_free(trash);
}

unsigned int
saber_trash_count(const struct saber_trash *trash)
{
  return trash == NULL ? 0 : trash->count;
}

const char *
saber_trash_path(const struct saber_trash *trash)
{
  return trash == NULL ? NULL : trash->root;
}

static bool
delete_recursive(GFile *file, GError **error)
{
  GFileEnumerator *entries = g_file_enumerate_children(file,
      G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
      NULL,
      NULL);

  if (entries != NULL) {
    GFileInfo *info = NULL;

    while ((info = g_file_enumerator_next_file(entries, NULL, NULL)) != NULL) {
      GFile *child = g_file_get_child(file, g_file_info_get_name(info));
      bool is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
      g_object_unref(info);

      bool ok = is_dir ? delete_recursive(child, error)
                       : g_file_delete(child, NULL, error);
      g_object_unref(child);

      if (!ok) {
        g_file_enumerator_close(entries, NULL, NULL);
        g_object_unref(entries);
        return false;
      }
    }

    g_file_enumerator_close(entries, NULL, NULL);
    g_object_unref(entries);
  }

  return g_file_delete(file, NULL, error);
}

static bool
empty_directory(const char *path, GError **error)
{
  GFile *dir = g_file_new_for_path(path);
  GFileEnumerator *entries = g_file_enumerate_children(dir,
      G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE,
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
      NULL,
      NULL);

  /* Action purpose: An absent directory is an already-empty trash, not a
  failure -- nothing has ever been deleted in this session's data directory. */
  if (entries == NULL) {
    g_object_unref(dir);
    return true;
  }

  bool ok = true;
  GFileInfo *info = NULL;

  while (ok && (info = g_file_enumerator_next_file(entries, NULL, NULL)) != NULL) {
    GFile *child = g_file_get_child(dir, g_file_info_get_name(info));
    bool is_dir = g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY;
    g_object_unref(info);

    ok = is_dir ? delete_recursive(child, error)
                : g_file_delete(child, NULL, error);
    g_object_unref(child);
  }

  g_file_enumerator_close(entries, NULL, NULL);
  g_object_unref(entries);
  g_object_unref(dir);

  return ok;
}

bool
saber_trash_empty(struct saber_trash *trash, GError **error)
{
  if (trash == NULL) {
    return true;
  }

  bool ok = empty_directory(trash->files, error);

  if (ok) {
    ok = empty_directory(trash->info, error);
  }

  schedule_recount(trash);

  return ok;
}
