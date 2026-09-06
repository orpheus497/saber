/* Script function and purpose: Mounted volumes worth putting on a launcher, and
the unmount action for them. */

#if !defined(SABER_DEVICES_H)
#define SABER_DEVICES_H

#include <stdbool.h>

#include <glib.h>

struct saber_device {
  char *device;      /* /dev/da0s1 */
  char *mount_point; /* /media/USB */
  char *label;       /* what to draw under the icon */
  char *fs_type;     /* msdosfs, exfat, cd9660, ... */
  const char *icon;  /* freedesktop icon name, static */
  bool removable;
  bool read_only;
};

struct saber_devices;

typedef void (*saber_devices_cb)(void *user);
typedef void (*saber_devices_unmount_cb)(const char *mount_point,
    bool ok,
    void *user);

/* Function purpose: Enumerate the interesting mounts and keep the list current
through GUnixMountMonitor, which GLib backs with getfsstat(2) on FreeBSD -- no
udev, no /proc, no polling. `cb` fires after every re-enumeration. */
struct saber_devices *
saber_devices_create(saber_devices_cb cb, void *user);

void
saber_devices_destroy(struct saber_devices *devices);

/* Elements are struct saber_device *, owned by the list; it is replaced whole
on every change, so callers must not retain pointers across the callback. */
const GPtrArray *
saber_devices_list(const struct saber_devices *devices);

/* Function purpose: Run umount(8) as the invoking user. Returns whether the
child was started, not whether it succeeded -- the result arrives at `cb`, and
the list refreshes from the mount monitor regardless. */
bool
saber_devices_unmount(struct saber_devices *devices,
    const char *mount_point,
    saber_devices_unmount_cb cb,
    void *user,
    GError **error);

#endif
