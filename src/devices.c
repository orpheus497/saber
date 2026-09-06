/* Script function and purpose: Mounted volumes for the launcher, read through
GUnixMountMonitor -- getfsstat(2)-backed on FreeBSD, so no udev and no /proc --
filtered down to the things a user would actually click, and unmounted by
fork/exec of umount(8). */

#include <stdbool.h>
#include <string.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixmounts.h>
#include <glib.h>

#include <saber/devices.h>

#define UMOUNT_PATH "/sbin/umount"

/* Action purpose: GLib 2.84 renamed the whole g_unix_mount_* entry family to
g_unix_mount_entry_* and deprecated the old spellings. One alias set keeps this
compiling on either side of that line. */
#if GLIB_CHECK_VERSION(2, 84, 0)
#define mount_entries_get g_unix_mount_entries_get
#define mount_entry_free g_unix_mount_entry_free
#define mount_entry_at g_unix_mount_entry_at
#define mount_entry_path g_unix_mount_entry_get_mount_path
#define mount_entry_device g_unix_mount_entry_get_device_path
#define mount_entry_fs_type g_unix_mount_entry_get_fs_type
#define mount_entry_readonly g_unix_mount_entry_is_readonly
#define mount_entry_internal g_unix_mount_entry_is_system_internal
#define mount_entry_guess_name g_unix_mount_entry_guess_name
#define mount_entry_can_eject g_unix_mount_entry_guess_can_eject
#else
#define mount_entries_get g_unix_mounts_get
#define mount_entry_free g_unix_mount_free
#define mount_entry_at g_unix_mount_at
#define mount_entry_path g_unix_mount_get_mount_path
#define mount_entry_device g_unix_mount_get_device_path
#define mount_entry_fs_type g_unix_mount_get_fs_type
#define mount_entry_readonly g_unix_mount_is_readonly
#define mount_entry_internal g_unix_mount_is_system_internal
#define mount_entry_guess_name g_unix_mount_guess_name
#define mount_entry_can_eject g_unix_mount_guess_can_eject
#endif

struct saber_devices {
  GUnixMountMonitor *monitor;
  GPtrArray *list;
  GSList *pending; /* struct unmount_job *, unmount children in flight */

  saber_devices_cb cb;
  void *user;
};

struct unmount_job {
  struct saber_devices *devices;
  char *mount_point;
  saber_devices_unmount_cb cb;
  void *user;
  guint watch;
};

/* Mount points the user has no business unmounting or browsing from a panel. */
static const char *const system_paths[] = {
  "/boot",
  "/dev",
  "/proc",
  "/sys",
  "/run",
  "/tmp",
  "/usr",
  "/var",
  "/compat",
  NULL,
};

/* Roots under which anything mounted is by convention removable media. */
static const char *const media_roots[] = {
  "/media",
  "/mnt",
  "/run/media",
  NULL,
};

/* Filesystems that are never a volume, whatever they are mounted on. */
static const char *const pseudo_fs[] = {
  "devfs",
  "fdescfs",
  "procfs",
  "linprocfs",
  "linsysfs",
  "kernfs",
  "mqueuefs",
  "pseudofs",
  "autofs",
  "unionfs",
  "fusectl",
  "cgroup",
  "cgroup2",
  "sysfs",
  "proc",
  "devpts",
  NULL,
};

/* Filesystems that mark a volume as worth showing wherever it is mounted. */
static const char *const removable_fs[] = {
  "msdosfs",
  "exfat",
  "ntfs",
  "ext2fs",
  "cd9660",
  "udf",
  NULL,
};

static bool
in_list(const char *const *list, const char *value)
{
  if (value == NULL) {
    return false;
  }

  for (int i = 0; list[i] != NULL; i++) {
    if (strcmp(list[i], value) == 0) {
      return true;
    }
  }

  return false;
}

static bool
under(const char *path, const char *root)
{
  size_t len = strlen(root);

  return strncmp(path, root, len) == 0 &&
      (path[len] == '/' || path[len] == '\0');
}

static bool
under_media_root(const char *path)
{
  for (int i = 0; media_roots[i] != NULL; i++) {
    if (under(path, media_roots[i]) && strcmp(path, media_roots[i]) != 0) {
      return true;
    }
  }

  return false;
}

static bool
is_system_path(const char *path)
{
  if (strcmp(path, "/") == 0) {
    return true;
  }

  for (int i = 0; system_paths[i] != NULL; i++) {
    if (under(path, system_paths[i])) {
      return true;
    }
  }

  return false;
}

/* Function purpose: Decide whether one mount belongs on the panel. The two
rules that matter are that a pseudo-filesystem is never a volume, and that
tmpfs and nullfs are volumes only where a user put them -- under a media root --
and never as the system's own scratch and bind mounts. */
static bool
is_interesting(const char *path, const char *fs_type)
{
  if (path == NULL || is_system_path(path)) {
    return false;
  }

  if (in_list(pseudo_fs, fs_type)) {
    return false;
  }

  bool media = under_media_root(path);

  if (fs_type != NULL &&
      (strcmp(fs_type, "tmpfs") == 0 || strcmp(fs_type, "nullfs") == 0)) {
    return media;
  }

  return media || in_list(removable_fs, fs_type);
}

static const char *
icon_for(const char *path, const char *fs_type)
{
  if (fs_type != NULL &&
      (strcmp(fs_type, "cd9660") == 0 || strcmp(fs_type, "udf") == 0)) {
    return "media-optical";
  }

  if (in_list(removable_fs, fs_type)) {
    return "drive-removable-media-usb";
  }

  if (under_media_root(path)) {
    return "drive-removable-media";
  }

  return "drive-harddisk";
}

static void
device_free(gpointer data)
{
  struct saber_device *device = data;

  g_free(device->device);
  g_free(device->mount_point);
  g_free(device->label);
  g_free(device->fs_type);
  g_free(device);
}

/* Function purpose: fstab's user-mountable entries, keyed by mount point. A
noauto/-u entry is the administrator saying "this one is the user's to mount",
which is the strongest available signal that a volume is removable. */
static GHashTable *
user_mountable_points(void)
{
  GHashTable *set = g_hash_table_new_full(
      g_str_hash, g_str_equal, g_free, NULL);
  GList *points = g_unix_mount_points_get(NULL);

  for (GList *item = points; item != NULL; item = item->next) {
    GUnixMountPoint *point = item->data;

    if (g_unix_mount_point_is_user_mountable(point)) {
      g_hash_table_add(
          set, g_strdup(g_unix_mount_point_get_mount_path(point)));
    }

    g_unix_mount_point_free(point);
  }

  g_list_free(points);

  return set;
}

static void
rebuild(struct saber_devices *devices)
{
  GPtrArray *list = g_ptr_array_new_with_free_func(device_free);
  GHashTable *mountable = user_mountable_points();
  GList *mounts = mount_entries_get(NULL);

  for (GList *item = mounts; item != NULL; item = item->next) {
    GUnixMountEntry *entry = item->data;
    const char *path = mount_entry_path(entry);
    const char *fs_type = mount_entry_fs_type(entry);

    if (is_interesting(path, fs_type) &&
        !(mount_entry_internal(entry) && !under_media_root(path))) {
      struct saber_device *device = g_new0(struct saber_device, 1);

      device->mount_point = g_strdup(path);
      device->device = g_strdup(mount_entry_device(entry));
      device->fs_type = g_strdup(fs_type);
      device->label = mount_entry_guess_name(entry);
      device->icon = icon_for(path, fs_type);
      device->read_only = mount_entry_readonly(entry);
      device->removable = mount_entry_can_eject(entry) ||
          under_media_root(path) || in_list(removable_fs, fs_type) ||
          g_hash_table_contains(mountable, path);

      if (device->label == NULL) {
        device->label = g_path_get_basename(path);
      }

      g_ptr_array_add(list, device);
    }

    mount_entry_free(entry);
  }

  g_list_free(mounts);
  g_hash_table_unref(mountable);

  if (devices->list != NULL) {
    g_ptr_array_unref(devices->list);
  }

  devices->list = list;
}

static void
on_mounts_changed(GUnixMountMonitor *monitor, gpointer data)
{
  (void)monitor;

  struct saber_devices *devices = data;

  rebuild(devices);

  if (devices->cb != NULL) {
    devices->cb(devices->user);
  }
}

struct saber_devices *
saber_devices_create(saber_devices_cb cb, void *user)
{
  struct saber_devices *devices = g_new0(struct saber_devices, 1);

  devices->cb = cb;
  devices->user = user;
  devices->monitor = g_unix_mount_monitor_get();

  g_signal_connect(devices->monitor,
      "mounts-changed",
      G_CALLBACK(on_mounts_changed),
      devices);
  g_signal_connect(devices->monitor,
      "mountpoints-changed",
      G_CALLBACK(on_mounts_changed),
      devices);

  rebuild(devices);

  return devices;
}

static void
unmount_job_free(struct unmount_job *job)
{
  g_free(job->mount_point);
  g_free(job);
}

void
saber_devices_destroy(struct saber_devices *devices)
{
  if (devices == NULL) {
    return;
  }

  for (GSList *item = devices->pending; item != NULL; item = item->next) {
    struct unmount_job *job = item->data;

    g_source_remove(job->watch);
    unmount_job_free(job);
  }

  g_slist_free(devices->pending);

  if (devices->monitor != NULL) {
    g_signal_handlers_disconnect_by_data(devices->monitor, devices);
    g_object_unref(devices->monitor);
  }

  if (devices->list != NULL) {
    g_ptr_array_unref(devices->list);
  }

  g_free(devices);
}

const GPtrArray *
saber_devices_list(const struct saber_devices *devices)
{
  return devices == NULL ? NULL : devices->list;
}

static void
on_umount_exit(GPid pid, gint status, gpointer data)
{
  struct unmount_job *job = data;
  bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

  g_spawn_close_pid(pid);

  if (job->cb != NULL) {
    job->cb(job->mount_point, ok, job->user);
  }

  if (!ok) {
    g_warning("saber: umount %s failed", job->mount_point);
  }

  job->devices->pending = g_slist_remove(job->devices->pending, job);
  unmount_job_free(job);
}

bool
saber_devices_unmount(struct saber_devices *devices,
    const char *mount_point,
    saber_devices_unmount_cb cb,
    void *user,
    GError **error)
{
  if (devices == NULL || mount_point == NULL || mount_point[0] != '/') {
    g_set_error_literal(error,
        G_IO_ERROR,
        G_IO_ERROR_INVALID_ARGUMENT,
        "not a mount point");
    return false;
  }

  /* Action purpose: Refuse anything not currently a mount, and anything the
  filter would not have shown, so a caller cannot walk this into unmounting
  /usr by handing it a path of its own construction. */
  GUnixMountEntry *entry = mount_entry_at(mount_point, NULL);

  if (entry == NULL) {
    g_set_error(error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_MOUNTED,
        "%s is not mounted",
        mount_point);
    return false;
  }

  bool allowed = is_interesting(
      mount_entry_path(entry), mount_entry_fs_type(entry));
  mount_entry_free(entry);

  if (!allowed) {
    g_set_error(error,
        G_IO_ERROR,
        G_IO_ERROR_PERMISSION_DENIED,
        "%s is not a removable volume",
        mount_point);
    return false;
  }

  pid_t pid = fork();

  if (pid < 0) {
    g_set_error_literal(
        error, G_IO_ERROR, G_IO_ERROR_FAILED, "fork failed");
    return false;
  }

  if (pid == 0) {
    execl(UMOUNT_PATH, "umount", mount_point, (char *)NULL);
    _exit(127);
  }

  struct unmount_job *job = g_new0(struct unmount_job, 1);

  job->devices = devices;
  job->mount_point = g_strdup(mount_point);
  job->cb = cb;
  job->user = user;
  job->watch = g_child_watch_add((GPid)pid, on_umount_exit, job);

  devices->pending = g_slist_prepend(devices->pending, job);

  return true;
}
