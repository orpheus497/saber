/* Script function and purpose: The launcher item list -- favourites, running
applications and the special tiles, in the order the column draws them. Pin,
unpin and reorder live here; the live order is persisted through config.h's
favourites pair (D-009). */

#include <string.h>

#include <glib.h>

#include <saber/model.h>

struct saber_model {
  const struct saber_config *config;
  struct saber_appinfo_index *index;
  struct saber_match *match;

  GPtrArray *items; /* struct saber_item *, display order */
  size_t head; /* leading special tiles */
  size_t tail; /* trailing special tiles */

  bool loading; /* suppress persistence while the list is being built */
  saber_model_changed_func changed;
  void *changed_data;
};

static void
notify(struct saber_model *model)
{
  if (!model->loading && model->changed != NULL) {
    model->changed(model->changed_data);
  }
}

static struct saber_item *
item_new(enum saber_item_type type, const char *id)
{
  struct saber_item *item = g_new0(struct saber_item, 1);
  item->type = type;
  item->id = g_strdup(id);
  item->windows = g_ptr_array_new(); /* handles belong to the Wayland side */
  return item;
}

static void
item_free(struct saber_item *item)
{
  saber_appinfo_unref(item->app);
  g_ptr_array_free(item->windows, TRUE);
  g_free(item->id);
  g_free(item);
}

/* The application tiles occupy [head, len - tail); everything outside is a
special tile whose position is fixed by BLUEPRINT.md 5.1. */
static size_t
apps_end(const struct saber_model *model)
{
  return model->items->len - model->tail;
}

static bool
in_app_region(const struct saber_model *model, size_t at)
{
  return at >= model->head && at < apps_end(model) &&
      ((struct saber_item *)g_ptr_array_index(model->items, at))->type ==
      SABER_ITEM_APP;
}

/* Index just past the last favourite: where a newly pinned tile lands. */
static size_t
pinned_end(const struct saber_model *model)
{
  size_t at = model->head;
  size_t end = apps_end(model);
  while (at < end) {
    const struct saber_item *item = g_ptr_array_index(model->items, at);
    if (item->type != SABER_ITEM_APP || !item->pinned) {
      break;
    }
    at++;
  }
  return at;
}

static struct saber_item *
add_app(struct saber_model *model, const char *id, bool pinned, size_t at)
{
  struct saber_item *item = item_new(SABER_ITEM_APP, id);
  item->pinned = pinned;
  item->launched_at = g_get_monotonic_time();
  item->app = saber_appinfo_ref(saber_appinfo_index_lookup(model->index, id));
  g_ptr_array_insert(model->items, (gint)at, item);
  return item;
}

/* Function purpose: Restore "all favourites, then all running-unpinned"
without disturbing the relative order inside either group, which is what a
reorder that crossed the boundary has to be followed by. */
static void
normalise(struct saber_model *model)
{
  size_t begin = model->head;
  size_t end = apps_end(model);

  GPtrArray *favourites = g_ptr_array_new();
  GPtrArray *running = g_ptr_array_new();
  for (size_t i = begin; i < end; i++) {
    struct saber_item *item = g_ptr_array_index(model->items, i);
    g_ptr_array_add(item->pinned ? favourites : running, item);
  }

  size_t at = begin;
  for (guint i = 0; i < favourites->len; i++) {
    model->items->pdata[at++] = g_ptr_array_index(favourites, i);
  }
  for (guint i = 0; i < running->len; i++) {
    model->items->pdata[at++] = g_ptr_array_index(running, i);
  }

  g_ptr_array_free(favourites, TRUE);
  g_ptr_array_free(running, TRUE);
}

bool
saber_model_save(const struct saber_model *model)
{
  GPtrArray *ids = g_ptr_array_new();
  size_t end = apps_end(model);
  for (size_t i = model->head; i < end; i++) {
    struct saber_item *item = g_ptr_array_index(model->items, i);
    if (item->type == SABER_ITEM_APP && item->pinned) {
      g_ptr_array_add(ids, item->id);
    }
  }

  bool ok = saber_config_save_favourites((char *const *)ids->pdata, ids->len);
  g_ptr_array_free(ids, TRUE);
  return ok;
}

static void
persist(struct saber_model *model)
{
  if (!model->loading) {
    saber_model_save(model);
  }
}

/* The tile each portion contributes. SABER_PORTION_APPS is deliberately absent:
it is the application band, not a tile, and saber_model_create fills it from the
favourites instead. */
static const struct {
  enum saber_portion portion;
  enum saber_item_type type;
  const char *id;
} portion_tiles[] = {
  { SABER_PORTION_BFB, SABER_ITEM_BFB, "bfb" },
  { SABER_PORTION_SHEETS, SABER_ITEM_SHEETS, "sheets" },
  { SABER_PORTION_DEVICES, SABER_ITEM_DEVICES, "devices" },
  { SABER_PORTION_TRASH, SABER_ITEM_TRASH, "trash" },
  { SABER_PORTION_TRAY, SABER_ITEM_TRAY, "tray" },
  { SABER_PORTION_SESSION, SABER_ITEM_SESSION, "session" },
};

static void
add_portion(struct saber_model *model, enum saber_portion portion)
{
  for (size_t i = 0; i < G_N_ELEMENTS(portion_tiles); i++) {
    if (portion_tiles[i].portion == portion) {
      g_ptr_array_add(model->items,
          item_new(portion_tiles[i].type, portion_tiles[i].id));

      return;
    }
  }
}

struct saber_model *
saber_model_create(const struct saber_config *config,
    struct saber_appinfo_index *index,
    struct saber_match *match)
{
  struct saber_model *model = g_new0(struct saber_model, 1);
  model->config = config;
  model->index = index;
  model->match = match;
  model->items = g_ptr_array_new();
  model->loading = true;

  const enum saber_portion *order = config->items.order;
  size_t order_len = order != NULL ? config->items.order_len : 0;
  size_t at = 0;

  /* Action purpose: Everything the configuration listed ahead of the
  application band leads the column and is pinned there by panel.c. config.c
  guarantees the band is in the list, so this loop terminates on it rather than
  swallowing the whole order -- and if a caller hands over a config that was
  never loaded, the band simply starts at index 0, which is still a column. */
  for (; at < order_len && order[at] != SABER_PORTION_APPS; at++) {
    add_portion(model, order[at]);
  }

  model->head = model->items->len;

  char **favourites = NULL;
  size_t count = 0;
  if (saber_config_load_favourites(config, &favourites, &count)) {
    for (size_t i = 0; i < count; i++) {
      if (favourites[i] != NULL && *favourites[i] != '\0' &&
          saber_model_find(model, favourites[i]) == NULL) {
        add_app(model, favourites[i], true, model->items->len);
      }
    }
    /* Allocated by config.c, which is GLib throughout. */
    g_strfreev(favourites);
  }

  size_t before_tail = model->items->len;

  for (at = at < order_len ? at + 1 : at; at < order_len; at++) {
    add_portion(model, order[at]);
  }

  model->tail = model->items->len - before_tail;

  model->loading = false;
  return model;
}

size_t
saber_model_apps_begin(const struct saber_model *model)
{
  return model->head;
}

size_t
saber_model_apps_end(const struct saber_model *model)
{
  return apps_end(model);
}

void
saber_model_destroy(struct saber_model *model)
{
  if (model == NULL) {
    return;
  }

  for (guint i = 0; i < model->items->len; i++) {
    item_free(g_ptr_array_index(model->items, i));
  }
  g_ptr_array_free(model->items, TRUE);
  g_free(model);
}

void
saber_model_set_changed(struct saber_model *model,
    saber_model_changed_func func,
    void *user_data)
{
  model->changed = func;
  model->changed_data = user_data;
}

size_t
saber_model_size(const struct saber_model *model)
{
  return model->items->len;
}

struct saber_item *
saber_model_nth(const struct saber_model *model, size_t n)
{
  if (n >= model->items->len) {
    return NULL;
  }
  return g_ptr_array_index(model->items, n);
}

struct saber_item *
saber_model_find(const struct saber_model *model, const char *id)
{
  if (id == NULL) {
    return NULL;
  }
  for (guint i = 0; i < model->items->len; i++) {
    struct saber_item *item = g_ptr_array_index(model->items, i);
    if (g_strcmp0(item->id, id) == 0) {
      return item;
    }
  }
  return NULL;
}

struct saber_item *
saber_model_find_by_window(const struct saber_model *model, void *handle)
{
  if (handle == NULL) {
    return NULL;
  }
  for (guint i = 0; i < model->items->len; i++) {
    struct saber_item *item = g_ptr_array_index(model->items, i);
    for (guint w = 0; w < item->windows->len; w++) {
      if (g_ptr_array_index(item->windows, w) == handle) {
        return item;
      }
    }
  }
  return NULL;
}

bool
saber_model_index_of(const struct saber_model *model,
    const struct saber_item *item,
    size_t *out)
{
  for (guint i = 0; i < model->items->len; i++) {
    if (g_ptr_array_index(model->items, i) == item) {
      *out = i;
      return true;
    }
  }
  return false;
}

struct saber_item *
saber_model_window_added(struct saber_model *model,
    void *handle,
    const char *app_id)
{
  struct saber_appinfo *app = saber_match_resolve(model->match, app_id);
  const char *id = app != NULL ? app->id : app_id;
  if (id == NULL) {
    return NULL;
  }

  struct saber_item *item = saber_model_find(model, id);
  if (item == NULL || item->type != SABER_ITEM_APP) {
    item = add_app(model, id, false, apps_end(model));
  }

  g_ptr_array_add(item->windows, handle);
  item->launching = false;
  notify(model);
  return item;
}

void
saber_model_window_removed(struct saber_model *model, void *handle)
{
  struct saber_item *item = saber_model_find_by_window(model, handle);
  if (item == NULL) {
    return;
  }

  g_ptr_array_remove(item->windows, handle);
  if (item->windows->len == 0 && !item->pinned) {
    g_ptr_array_remove(model->items, item);
    item_free(item);
  }
  notify(model);
}

void
saber_model_set_focus(struct saber_model *model, void *handle)
{
  for (guint i = 0; i < model->items->len; i++) {
    ((struct saber_item *)g_ptr_array_index(model->items, i))->focused = false;
  }

  struct saber_item *item = saber_model_find_by_window(model, handle);
  if (item != NULL) {
    item->focused = true;
  }
  notify(model);
}

void
saber_model_note_launch(struct saber_model *model, const char *desktop_id)
{
  saber_match_note_launch(model->match, desktop_id);

  struct saber_item *item = saber_model_find(model, desktop_id);
  if (item != NULL) {
    item->launching = true;
    notify(model);
  }
}

bool
saber_model_pin(struct saber_model *model, const char *desktop_id)
{
  if (desktop_id == NULL || *desktop_id == '\0') {
    return false;
  }

  struct saber_item *item = saber_model_find(model, desktop_id);
  if (item != NULL && item->type != SABER_ITEM_APP) {
    return false;
  }

  if (item == NULL) {
    add_app(model, desktop_id, true, pinned_end(model));
  } else if (!item->pinned) {
    g_ptr_array_remove(model->items, item);
    item->pinned = true;
    g_ptr_array_insert(model->items, (gint)pinned_end(model), item);
  }

  persist(model);
  notify(model);
  return true;
}

bool
saber_model_unpin(struct saber_model *model, const char *desktop_id)
{
  struct saber_item *item = saber_model_find(model, desktop_id);
  if (item == NULL || item->type != SABER_ITEM_APP || !item->pinned) {
    return false;
  }

  g_ptr_array_remove(model->items, item);
  item->pinned = false;

  /* An unpinned tile with no windows left has nothing to show. */
  if (item->windows->len == 0) {
    item_free(item);
  } else {
    g_ptr_array_insert(model->items, (gint)apps_end(model), item);
  }

  persist(model);
  notify(model);
  return true;
}

bool
saber_model_move(struct saber_model *model, size_t from, size_t to)
{
  if (from == to || !in_app_region(model, from) || !in_app_region(model, to)) {
    return false;
  }

  struct saber_item *item = g_ptr_array_index(model->items, from);
  g_ptr_array_remove_index(model->items, (guint)from);
  g_ptr_array_insert(model->items, (gint)to, item);

  /* Action purpose: Dropping a running tile above a favourite is the Unity
  gesture for "keep in launcher" -- the drop is what pins it. Everything is
  then re-partitioned, because the move may otherwise have left a favourite
  sitting below a running tile. */
  if (!item->pinned) {
    for (size_t i = to + 1, end = apps_end(model); i < end; i++) {
      struct saber_item *below = g_ptr_array_index(model->items, i);
      if (below->pinned) {
        item->pinned = true;
        break;
      }
    }
  }
  normalise(model);

  persist(model);
  notify(model);
  return true;
}

void
saber_model_set_badge(struct saber_model *model,
    const char *desktop_id,
    const struct saber_badge *badge)
{
  struct saber_item *item = saber_model_find(model, desktop_id);
  if (item == NULL) {
    return;
  }

  item->badge = *badge;
  notify(model);
}

void
saber_model_refresh(struct saber_model *model)
{
  for (guint i = 0; i < model->items->len; i++) {
    struct saber_item *item = g_ptr_array_index(model->items, i);
    if (item->type != SABER_ITEM_APP) {
      continue;
    }

    struct saber_appinfo *app =
        saber_appinfo_index_lookup(model->index, item->id);
    if (app == item->app) {
      continue;
    }

    saber_appinfo_unref(item->app);
    item->app = saber_appinfo_ref(app);
  }
  notify(model);
}
