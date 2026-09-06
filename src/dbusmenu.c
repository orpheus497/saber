/* Script function and purpose: com.canonical.dbusmenu spoken directly over
GDBus -- GetLayout into a tree, Event to activate a row, AboutToShow before a
submenu opens. Ported from sofi's source/dbusmenu.c (MIT, same author), widened
from one flat level to the recursive layout Saber renders itself. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <gio/gio.h>
#include <glib.h>

#include <saber/dbusmenu.h>

#define MENU_INTERFACE "com.canonical.dbusmenu"

/* Action purpose: Bound every call, because the peer is an arbitrary
application and these are synchronous. Shorter than the tray item property
timeout: that one runs with nothing waiting on it, this one runs with a menu
half-open in front of the user. Two seconds is long enough for an application
that is merely busy and short enough that a wedged one reads as an empty menu
rather than a frozen panel. */
#define MENU_CALL_TIMEOUT_MS 2000

/* Action purpose: The peer decides how many rows to send, and how deeply
nested. A menu past these bounds is either a defect or hostile, and rendering
it is not useful either way. Both are far above any real menu. */
#define MENU_MAX_ENTRIES 512
#define MENU_MAX_DEPTH 16

/* GetLayout's recursionDepth: -1 is the whole tree. Saber draws the submenus
itself rather than asking the application to open them, so it wants the tree in
one round trip; AboutToShow still runs before a submenu opens, for applications
that populate lazily. */
#define MENU_LAYOUT_DEPTH (-1)

/* Action purpose: A layout signal arrives on the same main loop the re-read
would block, so refetching straight from the handler would reenter the bus
dispatch. Deferring also collapses the burst an application emits while
rebuilding a menu row by row. */
#define MENU_REFRESH_COALESCE_MS 40

/* The properties worth asking for. Anything else is ignored on arrival. */
static const char *const wanted_properties[] = {
  "type",
  "label",
  "enabled",
  "visible",
  "icon-name",
  "icon-data",
  "toggle-type",
  "toggle-state",
  "children-display",
  NULL,
};

struct saber_dbusmenu {
  GDBusConnection *connection;
  char *bus_name;
  char *object_path;

  guint layout_sub;
  guint properties_sub;
  guint refresh_source;

  uint32_t revision;
  struct saber_dbusmenu_item *root;

  saber_dbusmenu_updated_func cb;
  void *user;
};

static void item_destroy(gpointer data);

/* Function purpose: Strip dbusmenu's GTK-style mnemonic markers -- a lone `_`
marks the accelerator, `__` is a literal underscore. Saber has no accelerator
row, and a raw label renders as `_Quit`, which looks broken. */
static char *
strip_mnemonics(const char *raw)
{
  if (raw == NULL) {
    return g_strdup("");
  }

  GString *out = g_string_sized_new(strlen(raw));

  for (const char *p = raw; *p != '\0'; p++) {
    if (*p != '_') {
      g_string_append_c(out, *p);
      continue;
    }

    if (*(p + 1) == '_') {
      g_string_append_c(out, '_');
      p++;
    }
  }

  return g_string_free(out, FALSE);
}

static void
apply_properties(struct saber_dbusmenu_item *item, GVariant *props)
{
  const char *type = NULL;
  const char *label = NULL;
  const char *icon_name = NULL;
  const char *toggle = NULL;
  const char *children = NULL;

  g_variant_lookup(props, "type", "&s", &type);
  g_variant_lookup(props, "label", "&s", &label);
  g_variant_lookup(props, "icon-name", "&s", &icon_name);
  g_variant_lookup(props, "toggle-type", "&s", &toggle);
  g_variant_lookup(props, "children-display", "&s", &children);

  item->type = g_strcmp0(type, "separator") == 0 ? SABER_DBUSMENU_SEPARATOR
                                                 : SABER_DBUSMENU_STANDARD;
  item->submenu = g_strcmp0(children, "submenu") == 0;
  item->label = item->type == SABER_DBUSMENU_SEPARATOR
                    ? g_strdup("")
                    : strip_mnemonics(label);
  item->icon_name = g_strdup(icon_name != NULL ? icon_name : "");

  /* Action purpose: "b" writes a gboolean, which is an int. Looking these up
  straight into the struct's one-byte bool would scribble over the three bytes
  after it. */
  gboolean enabled = TRUE;
  gboolean visible = TRUE;

  g_variant_lookup(props, "enabled", "b", &enabled);
  g_variant_lookup(props, "visible", "b", &visible);

  item->enabled = enabled;
  item->visible = visible;

  item->toggle_type = SABER_DBUSMENU_TOGGLE_NONE;

  if (g_strcmp0(toggle, "checkmark") == 0) {
    item->toggle_type = SABER_DBUSMENU_TOGGLE_CHECKMARK;
  } else if (g_strcmp0(toggle, "radio") == 0) {
    item->toggle_type = SABER_DBUSMENU_TOGGLE_RADIO;
  }

  item->toggle_state = SABER_DBUSMENU_TOGGLE_INDETERMINATE;

  gint32 state = 0;

  if (g_variant_lookup(props, "toggle-state", "i", &state)) {
    item->toggle_state = (int)state;
  }

  GVariant *icon_data = g_variant_lookup_value(props,
      "icon-data",
      G_VARIANT_TYPE_BYTESTRING);

  if (icon_data != NULL) {
    gsize length = 0;
    const guchar *bytes = g_variant_get_fixed_array(icon_data, &length, 1);

    if (bytes != NULL && length > 0) {
      item->icon_data = g_memdup2(bytes, length);
      item->icon_data_len = length;
    }

    g_variant_unref(icon_data);
  }
}

/* Function purpose: Turn one `(ia{sv}av)` layout node, and everything below it,
into an item. `depth` is the remaining recursion budget: a peer chooses the
nesting, so the descent has to be bounded rather than trusted. */
static struct saber_dbusmenu_item *
parse_node(GVariant *node, int depth)
{
  gint32 id = 0;
  GVariant *props = NULL;
  GVariant *children = NULL;

  g_variant_get(node, "(i@a{sv}@av)", &id, &props, &children);

  struct saber_dbusmenu_item *item =
      g_new0(struct saber_dbusmenu_item, 1);

  item->id = id;
  item->children = g_ptr_array_new_with_free_func(item_destroy);

  apply_properties(item, props);
  g_variant_unref(props);

  gsize n = g_variant_n_children(children);

  if (n > MENU_MAX_ENTRIES) {
    g_warning("saber: a dbusmenu level reported %" G_GSIZE_FORMAT
              " rows; showing the first %d.",
        n,
        MENU_MAX_ENTRIES);
    n = MENU_MAX_ENTRIES;
  }

  if (depth > 0) {
    for (gsize i = 0; i < n; i++) {
      GVariant *wrapper = g_variant_get_child_value(children, i);
      GVariant *child = g_variant_get_variant(wrapper);

      if (g_variant_is_of_type(child, G_VARIANT_TYPE("(ia{sv}av)"))) {
        g_ptr_array_add(item->children, parse_node(child, depth - 1));
      }

      g_variant_unref(child);
      g_variant_unref(wrapper);
    }
  }

  g_variant_unref(children);

  /* An application that populates lazily sends children-display=submenu with no
  children yet; one that sends children without saying so still has a submenu.
  Either fact alone is enough. */
  if (item->children->len > 0) {
    item->submenu = true;
  }

  return item;
}

static void
item_destroy(gpointer data)
{
  struct saber_dbusmenu_item *item = data;

  if (item == NULL) {
    return;
  }

  if (item->children != NULL) {
    g_ptr_array_free(item->children, TRUE);
  }

  g_free(item->label);
  g_free(item->icon_name);
  g_free(item->icon_data);
  g_free(item);
}

static const struct saber_dbusmenu_item *
find_in(const struct saber_dbusmenu_item *item, int32_t id)
{
  if (item == NULL) {
    return NULL;
  }

  if (item->id == id) {
    return item;
  }

  for (guint i = 0; i < item->children->len; i++) {
    const struct saber_dbusmenu_item *found =
        find_in(g_ptr_array_index(item->children, i), id);

    if (found != NULL) {
      return found;
    }
  }

  return NULL;
}

bool
saber_dbusmenu_refresh(struct saber_dbusmenu *menu)
{
  if (menu == NULL) {
    return false;
  }

  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(menu->connection,
      menu->bus_name,
      menu->object_path,
      MENU_INTERFACE,
      "GetLayout",
      g_variant_new("(ii^as)",
          0,
          MENU_LAYOUT_DEPTH,
          (char **)wanted_properties),
      G_VARIANT_TYPE("(u(ia{sv}av))"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START,
      MENU_CALL_TIMEOUT_MS,
      NULL,
      &error);

  if (reply == NULL) {
    g_warning("saber: cannot read the menu at %s: %s",
        menu->object_path,
        error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    return false;
  }

  guint32 revision = 0;
  GVariant *layout = NULL;

  g_variant_get(reply, "(u@(ia{sv}av))", &revision, &layout);

  struct saber_dbusmenu_item *root = parse_node(layout, MENU_MAX_DEPTH);

  g_variant_unref(layout);
  g_variant_unref(reply);

  item_destroy(menu->root);
  menu->root = root;
  menu->revision = revision;

  return true;
}

static gboolean
on_refresh_due(gpointer data)
{
  struct saber_dbusmenu *menu = data;

  menu->refresh_source = 0;

  if (saber_dbusmenu_refresh(menu) && menu->cb != NULL) {
    menu->cb(menu, menu->user);
  }

  return G_SOURCE_REMOVE;
}

/* Function purpose: Handle LayoutUpdated and ItemsPropertiesUpdated the same
way -- re-read the layout. ItemsPropertiesUpdated does carry the changed
properties, but merging them means reimplementing the property model twice over
for a tree that is at most a few dozen rows and only exists while a menu is
open. */
static void
on_menu_signal(GDBusConnection *connection,
    const char *sender,
    const char *object_path,
    const char *interface_name,
    const char *signal_name,
    GVariant *parameters,
    gpointer data)
{
  struct saber_dbusmenu *menu = data;

  (void)connection;
  (void)sender;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  (void)parameters;

  if (menu->refresh_source == 0) {
    menu->refresh_source =
        g_timeout_add(MENU_REFRESH_COALESCE_MS, on_refresh_due, menu);
  }
}

struct saber_dbusmenu *
saber_dbusmenu_open(GDBusConnection *connection,
    const char *bus_name,
    const char *object_path,
    saber_dbusmenu_updated_func cb,
    void *user)
{
  if (bus_name == NULL || bus_name[0] == '\0' || object_path == NULL ||
      object_path[0] == '\0') {
    return NULL;
  }

  GDBusConnection *owned = NULL;

  if (connection == NULL) {
    GError *error = NULL;

    owned = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);

    if (owned == NULL) {
      g_warning("saber: cannot reach the session bus for a menu: %s",
          error != NULL ? error->message : "unknown error");
      g_clear_error(&error);
      return NULL;
    }

    connection = owned;
  }

  struct saber_dbusmenu *menu = g_new0(struct saber_dbusmenu, 1);

  menu->connection = owned != NULL ? owned : g_object_ref(connection);
  menu->bus_name = g_strdup(bus_name);
  menu->object_path = g_strdup(object_path);
  menu->cb = cb;
  menu->user = user;

  menu->layout_sub = g_dbus_connection_signal_subscribe(menu->connection,
      menu->bus_name,
      MENU_INTERFACE,
      "LayoutUpdated",
      menu->object_path,
      NULL,
      G_DBUS_SIGNAL_FLAGS_NONE,
      on_menu_signal,
      menu,
      NULL);

  menu->properties_sub = g_dbus_connection_signal_subscribe(menu->connection,
      menu->bus_name,
      MENU_INTERFACE,
      "ItemsPropertiesUpdated",
      menu->object_path,
      NULL,
      G_DBUS_SIGNAL_FLAGS_NONE,
      on_menu_signal,
      menu,
      NULL);

  saber_dbusmenu_refresh(menu);

  return menu;
}

void
saber_dbusmenu_close(struct saber_dbusmenu *menu)
{
  if (menu == NULL) {
    return;
  }

  g_clear_handle_id(&menu->refresh_source, g_source_remove);

  if (menu->layout_sub != 0) {
    g_dbus_connection_signal_unsubscribe(menu->connection, menu->layout_sub);
  }

  if (menu->properties_sub != 0) {
    g_dbus_connection_signal_unsubscribe(menu->connection,
        menu->properties_sub);
  }

  item_destroy(menu->root);

  g_object_unref(menu->connection);
  g_free(menu->bus_name);
  g_free(menu->object_path);
  g_free(menu);
}

const struct saber_dbusmenu_item *
saber_dbusmenu_root(const struct saber_dbusmenu *menu)
{
  return menu == NULL ? NULL : menu->root;
}

const struct saber_dbusmenu_item *
saber_dbusmenu_find(const struct saber_dbusmenu *menu, int32_t id)
{
  return menu == NULL ? NULL : find_in(menu->root, id);
}

bool
saber_dbusmenu_about_to_show(struct saber_dbusmenu *menu, int32_t id)
{
  if (menu == NULL) {
    return false;
  }

  /* Many applications do not implement AboutToShow at all, so a failure here is
  ordinary and not worth a diagnostic: the layout already in hand is what gets
  drawn. */
  GVariant *reply = g_dbus_connection_call_sync(menu->connection,
      menu->bus_name,
      menu->object_path,
      MENU_INTERFACE,
      "AboutToShow",
      g_variant_new("(i)", id),
      G_VARIANT_TYPE("(b)"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START,
      MENU_CALL_TIMEOUT_MS,
      NULL,
      NULL);

  if (reply == NULL) {
    return false;
  }

  gboolean changed = FALSE;

  g_variant_get(reply, "(b)", &changed);
  g_variant_unref(reply);

  /* TRUE is the application saying it just rebuilt the tree -- drawing the
  layout fetched before the call would show the menu it had a moment ago. */
  return changed ? saber_dbusmenu_refresh(menu) : false;
}

void
saber_dbusmenu_event(struct saber_dbusmenu *menu, int32_t id)
{
  if (menu == NULL) {
    return;
  }

  /* `clicked` is the event the specification defines for activation and its
  data argument is unused. The timestamp is advisory -- applications use it for
  focus-stealing decisions -- and 0 means "none available", which is honest:
  the click happened in a compositor with no X-style server time to quote. */
  g_dbus_connection_call(menu->connection,
      menu->bus_name,
      menu->object_path,
      MENU_INTERFACE,
      "Event",
      g_variant_new("(isvu)",
          id,
          "clicked",
          g_variant_new_int32(0),
          (guint32)0),
      NULL,
      G_DBUS_CALL_FLAGS_NO_AUTO_START,
      MENU_CALL_TIMEOUT_MS,
      NULL,
      NULL,
      NULL);
}

const char *
saber_dbusmenu_bus_name(const struct saber_dbusmenu *menu)
{
  return menu == NULL ? "" : menu->bus_name;
}

const char *
saber_dbusmenu_object_path(const struct saber_dbusmenu *menu)
{
  return menu == NULL ? "" : menu->object_path;
}
