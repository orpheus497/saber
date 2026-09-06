/* Script function and purpose: The XDG desktop-entry index -- which
applications exist, what they are called, and what actually launches them.
Ported from sofi's source/modes/drun.c (MIT, same author). */

#if !defined(SABER_APPINFO_H)
#define SABER_APPINFO_H

#include <stdbool.h>
#include <stddef.h>

/* One "Desktop Action <id>" group: the static half of a tile's quicklist. */
struct saber_appinfo_action {
  char *id;
  char *name;
  char *icon;
  char *exec;
};

struct saber_appinfo {
  /* Desktop-file ID: the path below the applications directory with '/'
  replaced by '-', so kde/foo.desktop indexes as "kde-foo.desktop". This is
  the identifier every other module keys on -- favourites, LauncherEntry
  app_uris and window matching all use it. */
  char *id;
  char *path;

  char *name;
  char *generic_name;
  char *comment;
  char *icon;
  char *exec;
  char *startup_wm_class;
  char *work_dir; /* Path= */
  char **categories; /* NULL-terminated, may be NULL */

  bool terminal;
  bool dbus_activatable;

  struct saber_appinfo_action *actions;
  size_t actions_len;

  int refs;
};

/* Entries outlive the index that produced them: a re-scan replaces the index
contents wholesale, and a tile holding an entry must not be left dangling. */
struct saber_appinfo *
saber_appinfo_ref(struct saber_appinfo *app);

void
saber_appinfo_unref(struct saber_appinfo *app);

const struct saber_appinfo_action *
saber_appinfo_find_action(const struct saber_appinfo *app, const char *id);

struct saber_appinfo_index;

typedef void (*saber_appinfo_changed_func)(void *user_data);

/* Function purpose: Build the index and start watching for changes. Scans
$XDG_DATA_HOME/applications then each $XDG_DATA_DIRS/applications in order;
the first directory to supply a given ID wins, which is what lets a user
override or suppress a system entry. */
struct saber_appinfo_index *
saber_appinfo_index_create(void);

void
saber_appinfo_index_destroy(struct saber_appinfo_index *index);

void
saber_appinfo_index_set_changed(struct saber_appinfo_index *index,
    saber_appinfo_changed_func func,
    void *user_data);

void
saber_appinfo_index_rescan(struct saber_appinfo_index *index);

/* Function purpose: Bumped by every re-scan, so a module holding derived
lookup tables can rebuild them lazily instead of subscribing to the change
callback that the application owns. */
unsigned int
saber_appinfo_index_generation(const struct saber_appinfo_index *index);

/* Borrowed; ref it to hold it across a re-scan. */
struct saber_appinfo *
saber_appinfo_index_lookup(const struct saber_appinfo_index *index,
    const char *id);

size_t
saber_appinfo_index_size(const struct saber_appinfo_index *index);

struct saber_appinfo *
saber_appinfo_index_nth(const struct saber_appinfo_index *index, size_t n);

/* Function purpose: Expand an Exec line into an argv, exposed separately from
the launch so the field-code rules can be exercised without spawning anything.
`action_id` NULL means the entry's own Exec. `uris` is NULL-terminated and may
be NULL. Returns NULL on an Exec that the spec makes invalid; free with
saber_appinfo_free_argv. */
char **
saber_appinfo_build_argv(const struct saber_appinfo *app,
    const char *action_id,
    const char *const *uris);

void
saber_appinfo_free_argv(char **argv);

/* Function purpose: fork/exec the entry -- never a shell, so a hostile Exec
line or file name cannot become a command. `activation_token` may be NULL; when
given it reaches the child as XDG_ACTIVATION_TOKEN so the compositor can hand
the new window focus. */
bool
saber_appinfo_launch(const struct saber_appinfo *app,
    const char *action_id,
    const char *const *uris,
    const char *activation_token);

#endif
