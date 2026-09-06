/* Script function and purpose: com.canonical.Unity.LauncherEntry -- the count
badges, progress bars, urgency and dynamic quicklists applications publish for
their launcher tile.

The whole protocol turns on one fact: an application emits its LauncherEntry
state when the bus name `com.canonical.Unity` becomes owned, and says nothing
at all otherwise. Saber owns that name. See the comment in unity.c. */

#if !defined(SABER_UNITY_H)
#define SABER_UNITY_H

#include <stdbool.h>
#include <stdint.h>

#include <gio/gio.h>

/* The accumulated state of one desktop entry. An Update carries only the keys
that changed, so this is built up across signals rather than replaced. */
struct saber_unity_entry {
  int64_t count;
  bool count_visible;

  double progress; /* 0.0 - 1.0 */
  bool progress_visible;

  bool urgent;
  bool updating;

  /* A com.canonical.dbusmenu object path, empty when unset, together with the
  bus name that published it -- the path alone cannot be opened. */
  char *quicklist;
  char *bus_name;
};

struct saber_unity;

typedef void (*saber_unity_changed_func)(const char *desktop_id,
    const struct saber_unity_entry *entry,
    void *user);

/* Function purpose: Claim `com.canonical.Unity` and start listening for
LauncherEntry updates. Returns NULL only when the session bus is unreachable;
losing the name to another launcher is warned about and leaves badges dead,
which is the failure this whole module exists to avoid. */
struct saber_unity *
saber_unity_create(saber_unity_changed_func cb, void *user);

void
saber_unity_destroy(struct saber_unity *unity);

/* `desktop_id` is the desktop-file ID with its suffix, as it appears in
app_uri: "firefox.desktop". NULL when that application has never published. */
const struct saber_unity_entry *
saber_unity_get(const struct saber_unity *unity, const char *desktop_id);

/* True while Saber holds com.canonical.Unity. False means no application will
emit, so every badge is stale. */
bool
saber_unity_is_owner(const struct saber_unity *unity);

/* The session bus connection, so a caller can open a quicklist path without
opening a second one. */
GDBusConnection *
saber_unity_connection(const struct saber_unity *unity);

#endif
