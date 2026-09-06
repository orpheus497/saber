/* Script function and purpose: The XDG trash directory as the panel sees it --
a live item count and an empty action. */

#if !defined(SABER_TRASH_H)
#define SABER_TRASH_H

#include <stdbool.h>

#include <glib.h>

struct saber_trash;

typedef void (*saber_trash_cb)(unsigned int count, void *user);

/* Function purpose: Watch $XDG_DATA_HOME/Trash and report the item count
whenever it changes. Neither the trash nor its subdirectories need exist: a
session that has never deleted anything reports zero and picks the directories
up when they appear. */
struct saber_trash *
saber_trash_create(saber_trash_cb cb, void *user);

void
saber_trash_destroy(struct saber_trash *trash);

unsigned int
saber_trash_count(const struct saber_trash *trash);

/* Function purpose: Delete the contents of both files/ and info/, leaving the
directories themselves. Reports the first failure and stops -- a partial empty
is still a smaller trash, and the count refreshes from the monitor either
way. */
bool
saber_trash_empty(struct saber_trash *trash, GError **error);

const char *
saber_trash_path(const struct saber_trash *trash);

#endif
