/* Script function and purpose: The launcher item list -- what the column shows,
in what order. Favourites in their stored order, then running-but-unpinned
applications in launch order, with the special tiles placed around them by
config->items.order (BLUEPRINT.md 5.1). */

#if !defined(SABER_MODEL_H)
#define SABER_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <glib.h>

#include <saber/appinfo.h>
#include <saber/config.h>
#include <saber/match.h>

enum saber_item_type {
  SABER_ITEM_BFB,
  SABER_ITEM_APP,
  SABER_ITEM_SHEETS,
  SABER_ITEM_DEVICES,
  SABER_ITEM_TRASH,
  SABER_ITEM_TRAY,
  SABER_ITEM_SESSION,
};

/* Fed by unity.c from com.canonical.Unity.LauncherEntry (BLUEPRINT.md 5.5).
The model only stores it; nothing here interprets or ages it. */
struct saber_badge {
  int64_t count;
  bool count_visible;
  double progress;
  bool progress_visible;
  bool urgent;
};

struct saber_item {
  enum saber_item_type type;

  /* Desktop-file ID for a resolved application; the raw app_id for a running
  window with no entry; a fixed literal ("bfb", "trash", ...) for a special. */
  char *id;
  struct saber_appinfo *app; /* held ref; NULL for specials and unresolved */

  bool pinned;
  bool focused;
  bool launching;

  /* Opaque toplevel handles owned by the Wayland side; the model stores them
  in the order they appeared and never dereferences one. */
  GPtrArray *windows;

  struct saber_badge badge;
  int64_t launched_at; /* monotonic microseconds; orders running-unpinned */
};

static inline size_t
saber_item_window_count(const struct saber_item *item)
{
  return item->windows != NULL ? item->windows->len : 0;
}

struct saber_model;

typedef void (*saber_model_changed_func)(void *user_data);

/* Function purpose: Build the list from the favourites actually in force and
the portions config->items.order names, in the sequence it names them. Borrows
`config`, `index` and `match`; all three must outlive the model. */
struct saber_model *
saber_model_create(const struct saber_config *config,
    struct saber_appinfo_index *index,
    struct saber_match *match);

void
saber_model_destroy(struct saber_model *model);

void
saber_model_set_changed(struct saber_model *model,
    saber_model_changed_func func,
    void *user_data);

size_t
saber_model_size(const struct saber_model *model);

struct saber_item *
saber_model_nth(const struct saber_model *model, size_t n);

/* Function purpose: Where the application band sits in the list. The portions
the configuration placed before SABER_PORTION_APPS lead it and the ones placed
after it trail it, and panel.c pins the first group to the top of the column
and anchors the second to the bottom -- so it asks these rather than testing an
item's type, which would make the split this file's opinion instead of the
user's. The band itself is [begin, end) and may be empty. */
size_t
saber_model_apps_begin(const struct saber_model *model);

size_t
saber_model_apps_end(const struct saber_model *model);

struct saber_item *
saber_model_find(const struct saber_model *model, const char *id);

struct saber_item *
saber_model_find_by_window(const struct saber_model *model, void *handle);

bool
saber_model_index_of(const struct saber_model *model,
    const struct saber_item *item,
    size_t *out);

/* Function purpose: Bind a new toplevel to a tile, resolving its app_id
through match.c and creating a running-unpinned item when nothing matches an
existing one. Returns the item the window landed on. */
struct saber_item *
saber_model_window_added(struct saber_model *model,
    void *handle,
    const char *app_id);

void
saber_model_window_removed(struct saber_model *model, void *handle);

/* `handle` NULL clears focus everywhere. */
void
saber_model_set_focus(struct saber_model *model, void *handle);

/* Function purpose: Record that this entry is being started, so its tile can
throb and so the next unmatched toplevel binds to it rather than opening a tile
of its own. */
void
saber_model_note_launch(struct saber_model *model, const char *desktop_id);

/* Function purpose: Clear `launching` on any tile whose launch window has
closed, so an application that never opened a window -- or whose window never
resolved to the tile that started it -- stops being marked as starting. Returns
whether anything changed, so a caller on a repaint path can skip a redraw that
would produce an identical frame. */
bool
saber_model_expire_launches(struct saber_model *model);

bool
saber_model_pin(struct saber_model *model, const char *desktop_id);

bool
saber_model_unpin(struct saber_model *model, const char *desktop_id);

/* Function purpose: Drag-to-reorder. Indices are into the whole list; both
must land on application tiles, since the special tiles are positional. An
unpinned tile dropped among the favourites is pinned by the move, which is the
Unity gesture for "keep in launcher".

`to` is the index the item OCCUPIES AFTERWARDS, not an index into the list as
it stands before the call. The distinction is the whole of whether a downward
move lands one place late, so it is stated rather than left to be inferred:
moving index 0 to 2 in [A,B,C,D] yields [B,C,A,D], with A at 2.

Note for anyone auditing this: it has no caller. The drag gesture that would
supply one is not implemented, so nothing here is exercised. */
bool
saber_model_move(struct saber_model *model, size_t from, size_t to);

void
saber_model_set_badge(struct saber_model *model,
    const char *desktop_id,
    const struct saber_badge *badge);

/* Function purpose: Re-resolve every application tile against the index after
it has been re-scanned, so an entry that was installed, edited or removed while
the panel was running is reflected without restarting it. */
void
saber_model_refresh(struct saber_model *model);

bool
saber_model_save(const struct saber_model *model);

#endif
