/* Script function and purpose: The system tray host -- org.kde.StatusNotifierWatcher,
an org.kde.StatusNotifierHost-<pid> registration, and one proxy per item.
Ported from sofi's source/tray-watcher.c, source/tray-item.c and
source/tray-service.c (MIT, same author), which carry the three
interoperability rules commented below. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib.h>

#include <saber/sni.h>

#define WATCHER_BUS_NAME "org.kde.StatusNotifierWatcher"
#define WATCHER_OBJECT_PATH "/StatusNotifierWatcher"
#define WATCHER_INTERFACE "org.kde.StatusNotifierWatcher"
#define ITEM_INTERFACE "org.kde.StatusNotifierItem"
#define PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"

/* Where an item is assumed to live when it registers by bus name alone; KDE's
own watcher assumes the same. */
#define ITEM_DEFAULT_OBJECT_PATH "/StatusNotifierItem"

/* 0 is what KDE's watcher has always reported and what every item in the wild
is written against. There has never been a version 1. */
#define WATCHER_PROTOCOL_VERSION 0

/* Action purpose: A call into a third-party application must not be able to pin
resources indefinitely. The bus default of 25s is a long time to hold a fetch
open for an item that has stopped answering; a tray icon three seconds stale is
not a problem, one that never resolves is. */
#define ITEM_CALL_TIMEOUT_MS 3000

/* Action purpose: Collapse a burst of change signals into one fetch. The
signals carry no payload, so each one costs a whole property batch, and volume
or network applets emit several times a second. 100ms is below the threshold at
which a changing icon reads as laggy. */
#define ITEM_REFETCH_DEBOUNCE_MS 100

/* Action purpose: Bound the decode rather than move it off the loop. A tray
pixmap is 16-64px in practice; 512 leaves room for a HiDPI asset and caps the
worst case at about a millisecond of byteswap and premultiply. Oversized
pixmaps are refused, not scaled -- an application sending a 4096px tray icon has
misunderstood something. */
#define ITEM_ICON_MAX_DIM 512

/* Items commonly ship 16/22/24/32 in one array, in no defined order. The
smallest that is at least this wins: a tile scales down cleanly and up badly. */
#define ITEM_ICON_PREFERRED_DIM 32

/* Action purpose: One wake-up per burst for a subscriber that rebuilds its
whole zone regardless of which item changed. Separate from, and shorter than,
the per-item fetch debounce above. */
#define SNI_CHANGED_COALESCE_MS 50

static const char introspection_xml[] =
    "<node>"
    "  <interface name='org.kde.StatusNotifierWatcher'>"
    "    <method name='RegisterStatusNotifierItem'>"
    "      <arg type='s' name='service' direction='in'/>"
    "    </method>"
    "    <method name='RegisterStatusNotifierHost'>"
    "      <arg type='s' name='service' direction='in'/>"
    "    </method>"
    "    <property name='RegisteredStatusNotifierItems' type='as' "
    "access='read'/>"
    "    <property name='IsStatusNotifierHostRegistered' type='b' "
    "access='read'/>"
    "    <property name='ProtocolVersion' type='i' access='read'/>"
    "    <signal name='StatusNotifierItemRegistered'>"
    "      <arg type='s' name='service'/>"
    "    </signal>"
    "    <signal name='StatusNotifierItemUnregistered'>"
    "      <arg type='s' name='service'/>"
    "    </signal>"
    "    <signal name='StatusNotifierHostRegistered'/>"
    "    <signal name='StatusNotifierHostUnregistered'/>"
    "  </interface>"
    "</node>";

struct icon_cache {
  uint8_t *argb;
  int width;
  int height;
  bool dirty;
};

struct saber_sni_item {
  struct saber_sni *sni;

  char *service; /* bus name concatenated with object path */
  char *bus_name;
  char *object_path;

  guint watch_id;
  guint signal_sub;
  guint refetch_source;
  GCancellable *cancellable;

  char *id;
  char *title;
  char *icon_name;
  char *attention_icon_name;
  char *overlay_icon_name;
  char *icon_theme_path;
  char *menu_path;

  GVariant *icon_pixmap;
  GVariant *attention_icon_pixmap;
  GVariant *overlay_icon_pixmap;

  enum saber_sni_status status;
  bool item_is_menu;

  struct icon_cache icon;
  struct icon_cache overlay;
};

struct saber_sni {
  guint watcher_owner_id;
  guint host_owner_id;
  guint registration_id;
  guint changed_source;

  GDBusConnection *connection;
  GDBusNodeInfo *introspection;

  GPtrArray *items;      /* struct saber_sni_item *, registration order */
  GHashTable *hosts;     /* foreign host bus name -> name-watch id */
  bool host_registered;  /* true once we hold the watcher name */

  saber_sni_changed_func cb;
  void *user;
};

static void item_destroy(gpointer data);
static void fetch_all(struct saber_sni_item *item);

/* ---------------------------------------------------------------- */
/* change notification                                               */
/* ---------------------------------------------------------------- */

static gboolean
on_changed_due(gpointer data)
{
  struct saber_sni *sni = data;

  sni->changed_source = 0;

  if (sni->cb != NULL) {
    sni->cb(sni->user);
  }

  return G_SOURCE_REMOVE;
}

static void
notify_changed(struct saber_sni *sni)
{
  if (sni == NULL || sni->changed_source != 0) {
    return;
  }

  sni->changed_source =
      g_timeout_add(SNI_CHANGED_COALESCE_MS, on_changed_due, sni);
}

static void
emit_signal(struct saber_sni *sni, const char *name, GVariant *params)
{
  if (sni->connection == NULL) {
    if (params != NULL) {
      g_variant_unref(g_variant_ref_sink(params));
    }
    return;
  }

  g_dbus_connection_emit_signal(sni->connection,
      NULL,
      WATCHER_OBJECT_PATH,
      WATCHER_INTERFACE,
      name,
      params,
      NULL);
}

/* ---------------------------------------------------------------- */
/* property application                                              */
/* ---------------------------------------------------------------- */

static void
set_string(char **dest, GVariant *value)
{
  /* Menu arrives as an object path rather than a string, and a sender can put
  any type it likes behind a variant. */
  if (!g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) &&
      !g_variant_is_of_type(value, G_VARIANT_TYPE_OBJECT_PATH)) {
    return;
  }

  g_free(*dest);
  *dest = g_variant_dup_string(value, NULL);
}

static void
set_pixmap(GVariant **dest, GVariant *value, struct icon_cache *cache)
{
  if (!g_variant_is_of_type(value, G_VARIANT_TYPE("a(iiay)"))) {
    return;
  }

  if (*dest != NULL) {
    g_variant_unref(*dest);
  }

  *dest = g_variant_ref(value);
  cache->dirty = true;
}

static enum saber_sni_status
parse_status(const char *status)
{
  if (g_strcmp0(status, "NeedsAttention") == 0) {
    return SABER_SNI_NEEDS_ATTENTION;
  }

  if (g_strcmp0(status, "Active") == 0) {
    return SABER_SNI_ACTIVE;
  }

  /* Passive, and anything unrecognised: an item reporting a status this version
  has never heard of is more usefully drawn than hidden. */
  return SABER_SNI_PASSIVE;
}

static void
apply_property(struct saber_sni_item *item, const char *name, GVariant *value)
{
  if (g_strcmp0(name, "Id") == 0) {
    set_string(&item->id, value);
  } else if (g_strcmp0(name, "Title") == 0) {
    set_string(&item->title, value);
  } else if (g_strcmp0(name, "IconName") == 0) {
    set_string(&item->icon_name, value);
  } else if (g_strcmp0(name, "AttentionIconName") == 0) {
    set_string(&item->attention_icon_name, value);
  } else if (g_strcmp0(name, "OverlayIconName") == 0) {
    set_string(&item->overlay_icon_name, value);
  } else if (g_strcmp0(name, "IconThemePath") == 0) {
    set_string(&item->icon_theme_path, value);
  } else if (g_strcmp0(name, "Menu") == 0) {
    set_string(&item->menu_path, value);
  } else if (g_strcmp0(name, "IconPixmap") == 0) {
    set_pixmap(&item->icon_pixmap, value, &item->icon);
  } else if (g_strcmp0(name, "AttentionIconPixmap") == 0) {
    set_pixmap(&item->attention_icon_pixmap, value, &item->icon);
  } else if (g_strcmp0(name, "OverlayIconPixmap") == 0) {
    set_pixmap(&item->overlay_icon_pixmap, value, &item->overlay);
  } else if (g_strcmp0(name, "ItemIsMenu") == 0) {
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
      item->item_is_menu = g_variant_get_boolean(value);
    }
  } else if (g_strcmp0(name, "Status") == 0) {
    if (g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
      enum saber_sni_status was = item->status;

      item->status = parse_status(g_variant_get_string(value, NULL));

      /* Status selects WHICH pixmap is effective, so a change to it invalidates
      the decode just as surely as new pixels do. */
      if (was != item->status) {
        item->icon.dirty = true;
      }
    }
  }

  /* Category, WindowId, ToolTip and AttentionMovieName are read and dropped on
  purpose: storing a property nothing draws turns a proxy into a second copy of
  the specification. */
}

/* ---------------------------------------------------------------- */
/* fetching                                                          */
/* ---------------------------------------------------------------- */

struct prop_fetch {
  struct saber_sni_item *item;
  char *name;
};

static void
on_get_property(GObject *source, GAsyncResult *res, gpointer data)
{
  struct prop_fetch *ctx = data;
  GError *error = NULL;
  GVariant *reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

  if (reply == NULL) {
    /* Action purpose: The cancellation check must come first. A cancelled call
    means the item has already been freed, so ctx->item is dead memory and even
    reading its name for a diagnostic is a use-after-free. */
    bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

    if (!cancelled) {
      /* Ordinary: the specification marks most of these optional. */
      g_debug("saber: %s unavailable on a tray item: %s",
          ctx->name,
          error != NULL ? error->message : "unknown error");
    }

    g_clear_error(&error);
    g_free(ctx->name);
    g_free(ctx);
    return;
  }

  GVariant *value = NULL;

  g_variant_get(reply, "(v)", &value);
  apply_property(ctx->item, ctx->name, value);
  g_variant_unref(value);
  g_variant_unref(reply);

  notify_changed(ctx->item->sni);

  g_free(ctx->name);
  g_free(ctx);
}

/* Function purpose: The fallback for an item whose GetAll fails. One property
that throws takes the whole batch down with it, so an item can be perfectly
usable and still answer nothing to a bulk request; asking one at a time costs
round trips and returns whatever it can actually supply. */
static void
fetch_individually(struct saber_sni_item *item)
{
  static const char *const wanted[] = {
    "Id",
    "Title",
    "Status",
    "IconName",
    "IconThemePath",
    "IconPixmap",
    "AttentionIconName",
    "AttentionIconPixmap",
    "OverlayIconName",
    "OverlayIconPixmap",
    "ItemIsMenu",
    "Menu",
    NULL,
  };

  for (size_t i = 0; wanted[i] != NULL; i++) {
    struct prop_fetch *ctx = g_new0(struct prop_fetch, 1);

    ctx->item = item;
    ctx->name = g_strdup(wanted[i]);

    g_dbus_connection_call(item->sni->connection,
        item->bus_name,
        item->object_path,
        PROPERTIES_INTERFACE,
        "Get",
        g_variant_new("(ss)", ITEM_INTERFACE, wanted[i]),
        G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START,
        ITEM_CALL_TIMEOUT_MS,
        item->cancellable,
        on_get_property,
        ctx);
  }
}

static void
on_get_all(GObject *source, GAsyncResult *res, gpointer data)
{
  struct saber_sni_item *item = data;
  GError *error = NULL;
  GVariant *reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);

  if (reply == NULL) {
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      g_clear_error(&error);
      return;
    }

    g_debug("saber: GetAll failed for tray item %s (%s); reading properties "
            "one at a time",
        item->service,
        error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    fetch_individually(item);
    return;
  }

  GVariantIter *iter = NULL;
  const char *name = NULL;
  GVariant *value = NULL;

  g_variant_get(reply, "(a{sv})", &iter);

  while (g_variant_iter_next(iter, "{&sv}", &name, &value)) {
    apply_property(item, name, value);
    g_variant_unref(value);
  }

  g_variant_iter_free(iter);
  g_variant_unref(reply);

  notify_changed(item->sni);
}

static void
fetch_all(struct saber_sni_item *item)
{
  g_dbus_connection_call(item->sni->connection,
      item->bus_name,
      item->object_path,
      PROPERTIES_INTERFACE,
      "GetAll",
      g_variant_new("(s)", ITEM_INTERFACE),
      G_VARIANT_TYPE("(a{sv})"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START,
      ITEM_CALL_TIMEOUT_MS,
      item->cancellable,
      on_get_all,
      item);
}

static gboolean
on_refetch_due(gpointer data)
{
  struct saber_sni_item *item = data;

  item->refetch_source = 0;
  fetch_all(item);

  return G_SOURCE_REMOVE;
}

/* Function purpose: React to NewIcon / NewTitle / NewStatus / NewAttentionIcon
/ NewOverlayIcon. Every one is handled the same way -- re-read everything --
because these signals carry no payload, so a fetch is required whatever
arrives, and items disagree in practice about which signal implies which
property. Notably these are not PropertiesChanged: most items never emit that
at all, which is why a GDBusProxy property cache is useless here. */
static void
on_item_signal(GDBusConnection *connection,
    const char *sender,
    const char *object_path,
    const char *interface_name,
    const char *signal_name,
    GVariant *parameters,
    gpointer data)
{
  struct saber_sni_item *item = data;

  (void)connection;
  (void)sender;
  (void)object_path;
  (void)interface_name;
  (void)signal_name;
  (void)parameters;

  /* A fetch already scheduled will read whatever is current when it runs, so a
  second signal arriving first needs no second fetch. */
  if (item->refetch_source != 0) {
    return;
  }

  item->refetch_source =
      g_timeout_add(ITEM_REFETCH_DEBOUNCE_MS, on_refetch_due, item);
}

/* ---------------------------------------------------------------- */
/* item lifetime                                                     */
/* ---------------------------------------------------------------- */

static struct saber_sni_item *
item_create(struct saber_sni *sni,
    const char *service,
    const char *bus_name,
    const char *object_path)
{
  struct saber_sni_item *item = g_new0(struct saber_sni_item, 1);

  item->sni = sni;
  item->service = g_strdup(service);
  item->bus_name = g_strdup(bus_name);
  item->object_path = g_strdup(object_path);
  item->cancellable = g_cancellable_new();
  item->status = SABER_SNI_PASSIVE;

  /* Every string accessor promises non-NULL, so they start empty rather than
  unset and no caller has to test. */
  item->id = g_strdup("");
  item->title = g_strdup("");
  item->icon_name = g_strdup("");
  item->attention_icon_name = g_strdup("");
  item->overlay_icon_name = g_strdup("");
  item->icon_theme_path = g_strdup("");
  item->menu_path = g_strdup("");

  /* Action purpose: Subscribe before the first fetch. An item that changes
  between the two would otherwise be missed entirely -- the fetch carries the
  old value and the signal announcing the new one arrives while nothing is
  listening. */
  item->signal_sub = g_dbus_connection_signal_subscribe(sni->connection,
      item->bus_name,
      ITEM_INTERFACE,
      NULL,
      item->object_path,
      NULL,
      G_DBUS_SIGNAL_FLAGS_NONE,
      on_item_signal,
      item,
      NULL);

  fetch_all(item);

  return item;
}

static void
item_destroy(gpointer data)
{
  struct saber_sni_item *item = data;

  if (item == NULL) {
    return;
  }

  /* Action purpose: Cancel first. A fetch in flight holds this pointer as its
  user data, and completing after the free would be a use-after-free driven by
  whichever application happened to be slow. */
  g_cancellable_cancel(item->cancellable);

  /* A debounced fetch still pending holds the pointer just as a call in flight
  does, and a GSource is not cancelled by the GCancellable. */
  g_clear_handle_id(&item->refetch_source, g_source_remove);

  if (item->watch_id != 0) {
    g_bus_unwatch_name(item->watch_id);
    item->watch_id = 0;
  }

  if (item->signal_sub != 0 && item->sni->connection != NULL) {
    g_dbus_connection_signal_unsubscribe(item->sni->connection,
        item->signal_sub);
    item->signal_sub = 0;
  }

  g_object_unref(item->cancellable);

  g_free(item->service);
  g_free(item->bus_name);
  g_free(item->object_path);
  g_free(item->id);
  g_free(item->title);
  g_free(item->icon_name);
  g_free(item->attention_icon_name);
  g_free(item->overlay_icon_name);
  g_free(item->icon_theme_path);
  g_free(item->menu_path);

  g_clear_pointer(&item->icon_pixmap, g_variant_unref);
  g_clear_pointer(&item->attention_icon_pixmap, g_variant_unref);
  g_clear_pointer(&item->overlay_icon_pixmap, g_variant_unref);

  g_free(item->icon.argb);
  g_free(item->overlay.argb);

  g_free(item);
}

/* ---------------------------------------------------------------- */
/* registry                                                          */
/* ---------------------------------------------------------------- */

static int
find_index(const struct saber_sni *sni, const char *service)
{
  if (sni->items == NULL) {
    return -1;
  }

  for (guint i = 0; i < sni->items->len; i++) {
    const struct saber_sni_item *item = g_ptr_array_index(sni->items, i);

    if (g_strcmp0(item->service, service) == 0) {
      return (int)i;
    }
  }

  return -1;
}

/* Function purpose: Drop an item whose application has gone. Emitting before
removing would hand a listener a service string it could still look up and
find, so the order here means a host reacting synchronously sees the registry
the signal describes. */
static void
unregister_item(struct saber_sni *sni, guint index)
{
  struct saber_sni_item *item = g_ptr_array_index(sni->items, index);
  char *service = g_strdup(item->service);

  g_ptr_array_remove_index(sni->items, index);

  emit_signal(sni,
      "StatusNotifierItemUnregistered",
      g_variant_new("(s)", service));
  g_free(service);

  notify_changed(sni);
}

static void
on_item_vanished(GDBusConnection *connection, const char *name, gpointer data)
{
  struct saber_sni *sni = data;

  (void)connection;

  /* Action purpose: INTEROPERABILITY RULE (b). This is not a fallback for a
  missing Unregister method -- the specification has no such method at all. An
  item exists for exactly as long as its bus name does, so watching
  NameOwnerChanged IS the deregistration mechanism, and without it the tray
  accumulates icons for applications that exited. */
  if (sni->items == NULL) {
    return;
  }

  for (guint i = sni->items->len; i > 0; i--) {
    const struct saber_sni_item *item = g_ptr_array_index(sni->items, i - 1);

    if (g_strcmp0(item->bus_name, name) == 0) {
      unregister_item(sni, i - 1);
    }
  }
}

/* Function purpose: Work out what an application actually meant by its
registration argument.

INTEROPERABILITY RULE (a). The argument is documented as a service name and is
in practice one of two different things: a bus name, or an object path with the
bus name left implicit in the sender. Qt and KDE items send the name; several
GTK and Electron ones send the path. A watcher that handles only one form shows
an empty tray for half the desktop with no diagnostic anywhere. Neither form is
wrong -- the specification simply never pinned it down. */
static void
split_service(const char *service,
    const char *sender,
    char **bus_name,
    char **object_path)
{
  if (service != NULL && service[0] == '/') {
    *bus_name = g_strdup(sender);
    *object_path = g_strdup(service);
    return;
  }

  *bus_name =
      g_strdup(service != NULL && service[0] != '\0' ? service : sender);
  *object_path = g_strdup(ITEM_DEFAULT_OBJECT_PATH);
}

static bool
register_item(struct saber_sni *sni, const char *service, const char *sender)
{
  char *bus_name = NULL;
  char *object_path = NULL;

  split_service(service, sender, &bus_name, &object_path);

  /* Action purpose: Any session peer can call this, and both halves go on to
  g_dbus_connection_call, g_bus_watch_name and g_dbus_connection_signal_subscribe
  -- each of which opens with a g_return_if_fail on the argument. An invalid name
  would therefore leak the floating parameter tuple of every property fetch,
  return a watch id of 0 that on_item_vanished can never fire for, leaving a tray
  entry that can never be removed, and abort the process outright under
  G_DEBUG=fatal-criticals. Checked before first use, not at each of them. */
  if (bus_name == NULL || object_path == NULL || !g_dbus_is_name(bus_name) ||
      !g_variant_is_object_path(object_path)) {
    g_warning("saber: refusing a StatusNotifierItem registration with an "
              "invalid bus name or object path (%s, %s)",
        bus_name != NULL ? bus_name : "(none)",
        object_path != NULL ? object_path : "(none)");
    g_free(bus_name);
    g_free(object_path);
    return false;
  }

  char *canonical = g_strconcat(bus_name, object_path, NULL);

  if (find_index(sni, canonical) >= 0) {
    /* Registering twice is not worth refusing: an application that reconnects
    to the bus re-registers, and has no way of knowing we still hold its
    previous entry. */
    g_free(canonical);
    g_free(bus_name);
    g_free(object_path);
    return true;
  }

  struct saber_sni_item *item =
      item_create(sni, canonical, bus_name, object_path);

  g_free(canonical);
  g_free(bus_name);
  g_free(object_path);

  item->watch_id = g_bus_watch_name(G_BUS_TYPE_SESSION,
      item->bus_name,
      G_BUS_NAME_WATCHER_FLAGS_NONE,
      NULL,
      on_item_vanished,
      sni,
      NULL);

  g_ptr_array_add(sni->items, item);

  emit_signal(sni,
      "StatusNotifierItemRegistered",
      g_variant_new("(s)", item->service));
  notify_changed(sni);

  return true;
}

static void
on_host_vanished(GDBusConnection *connection, const char *name, gpointer data)
{
  struct saber_sni *sni = data;

  (void)connection;

  if (sni->hosts == NULL || !g_hash_table_contains(sni->hosts, name)) {
    return;
  }

  g_hash_table_remove(sni->hosts, name);

  /* Only claim the host is gone when ours is gone too, which it is not while
  this process runs. Saying otherwise tells every item to stop publishing to a
  tray that is still on screen. */
  if (!sni->host_registered && g_hash_table_size(sni->hosts) == 0) {
    emit_signal(sni, "StatusNotifierHostUnregistered", NULL);
  }
}

static void
host_watch_destroy(gpointer data)
{
  guint id = GPOINTER_TO_UINT(data);

  if (id != 0) {
    g_bus_unwatch_name(id);
  }
}

static bool
register_host(struct saber_sni *sni, const char *service, const char *sender)
{
  const char *name =
      service != NULL && service[0] != '\0' ? service : sender;

  /* Same reasoning as register_item: an unvalidated name here would take a
  watch id of 0 and leave an immortal entry in sni->hosts, which is what
  IsStatusNotifierHostRegistered answers from. */
  if (name == NULL || !g_dbus_is_name(name)) {
    g_warning("saber: refusing a StatusNotifierHost registration with an "
              "invalid bus name (%s)",
        name != NULL ? name : "(none)");
    return false;
  }

  if (g_hash_table_contains(sni->hosts, name)) {
    return true;
  }

  guint id = g_bus_watch_name(G_BUS_TYPE_SESSION,
      name,
      G_BUS_NAME_WATCHER_FLAGS_NONE,
      NULL,
      on_host_vanished,
      sni,
      NULL);

  g_hash_table_insert(sni->hosts, g_strdup(name), GUINT_TO_POINTER(id));

  emit_signal(sni, "StatusNotifierHostRegistered", NULL);

  return true;
}

/* ---------------------------------------------------------------- */
/* watcher dispatch                                                  */
/* ---------------------------------------------------------------- */

static void
handle_method(GDBusConnection *connection,
    const char *sender,
    const char *object_path,
    const char *interface_name,
    const char *method_name,
    GVariant *parameters,
    GDBusMethodInvocation *invocation,
    gpointer data)
{
  struct saber_sni *sni = data;

  (void)connection;
  (void)object_path;
  (void)interface_name;

  if (g_strcmp0(method_name, "RegisterStatusNotifierItem") == 0) {
    const char *service = NULL;

    g_variant_get(parameters, "(&s)", &service);

    if (!register_item(sni, service, sender)) {
      g_dbus_method_invocation_return_error_literal(invocation,
          G_DBUS_ERROR,
          G_DBUS_ERROR_INVALID_ARGS,
          "Invalid service name or object path");
      return;
    }

    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }

  if (g_strcmp0(method_name, "RegisterStatusNotifierHost") == 0) {
    const char *service = NULL;

    g_variant_get(parameters, "(&s)", &service);

    if (!register_host(sni, service, sender)) {
      g_dbus_method_invocation_return_error_literal(invocation,
          G_DBUS_ERROR,
          G_DBUS_ERROR_INVALID_ARGS,
          "Invalid service name");
      return;
    }

    g_dbus_method_invocation_return_value(invocation, NULL);
    return;
  }

  g_dbus_method_invocation_return_error(invocation,
      G_DBUS_ERROR,
      G_DBUS_ERROR_UNKNOWN_METHOD,
      "Unknown method %s",
      method_name);
}

static GVariant *
handle_get_property(GDBusConnection *connection,
    const char *sender,
    const char *object_path,
    const char *interface_name,
    const char *property_name,
    GError **error,
    gpointer data)
{
  struct saber_sni *sni = data;

  (void)connection;
  (void)sender;
  (void)object_path;
  (void)interface_name;

  if (g_strcmp0(property_name, "RegisteredStatusNotifierItems") == 0) {
    GVariantBuilder builder;

    g_variant_builder_init(&builder, G_VARIANT_TYPE("as"));

    for (guint i = 0; i < sni->items->len; i++) {
      const struct saber_sni_item *item = g_ptr_array_index(sni->items, i);

      g_variant_builder_add(&builder, "s", item->service);
    }

    return g_variant_builder_end(&builder);
  }

  if (g_strcmp0(property_name, "IsStatusNotifierHostRegistered") == 0) {
    /* Action purpose: The most load-bearing value in this file. An application
    asks once, at startup; a FALSE answer means it shows no tray icon at all and
    never asks again. */
    return g_variant_new_boolean(sni->host_registered ||
                                 g_hash_table_size(sni->hosts) > 0);
  }

  if (g_strcmp0(property_name, "ProtocolVersion") == 0) {
    return g_variant_new_int32(WATCHER_PROTOCOL_VERSION);
  }

  g_set_error(error,
      G_DBUS_ERROR,
      G_DBUS_ERROR_UNKNOWN_PROPERTY,
      "Unknown property %s",
      property_name);

  return NULL;
}

static const GDBusInterfaceVTable interface_vtable = {
  .method_call = handle_method,
  .get_property = handle_get_property,
  .set_property = NULL,
  .padding = { NULL },
};

/* ---------------------------------------------------------------- */
/* name ownership                                                    */
/* ---------------------------------------------------------------- */

static void
on_bus_acquired(GDBusConnection *connection, const char *name, gpointer data)
{
  struct saber_sni *sni = data;
  GError *error = NULL;

  (void)name;

  sni->connection = connection;
  sni->registration_id = g_dbus_connection_register_object(connection,
      WATCHER_OBJECT_PATH,
      sni->introspection->interfaces[0],
      &interface_vtable,
      sni,
      NULL,
      &error);

  if (sni->registration_id == 0) {
    g_warning("saber: could not export %s: %s",
        WATCHER_INTERFACE,
        error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
  }
}

static void
on_name_acquired(GDBusConnection *connection, const char *name, gpointer data)
{
  struct saber_sni *sni = data;

  (void)connection;
  (void)name;

  /* Action purpose: Saber is the watcher AND a host, so mark itself registered
  the moment the name lands rather than round-tripping a call to itself. The
  signal still goes out, because items already running when Saber started are
  listening for exactly it -- that is their one chance to appear without being
  restarted. */
  sni->host_registered = true;
  emit_signal(sni, "StatusNotifierHostRegistered", NULL);
  notify_changed(sni);
}

static void
on_name_lost(GDBusConnection *connection, const char *name, gpointer data)
{
  struct saber_sni *sni = data;

  (void)connection;

  /* Action purpose: Deliberately not a REPLACE -- two watchers fighting over
  this name would flap every item on the desktop between them. Saber keeps
  running because the tray is one zone of a panel rather than the whole
  process, but the tray zone will stay empty and the reason has to be said
  once, since nothing else reports it. */
  sni->host_registered = false;
  g_warning("saber: %s is owned by another tray host; the tray zone will be "
            "empty. Do not run sofi -tray-daemon alongside saber.",
      name);
  notify_changed(sni);
}

/* ---------------------------------------------------------------- */
/* public interface                                                  */
/* ---------------------------------------------------------------- */

struct saber_sni *
saber_sni_create(saber_sni_changed_func cb, void *user)
{
  GError *error = NULL;
  GDBusNodeInfo *introspection =
      g_dbus_node_info_new_for_xml(introspection_xml, &error);

  if (introspection == NULL) {
    g_warning("saber: could not parse the tray watcher interface: %s",
        error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
    return NULL;
  }

  struct saber_sni *sni = g_new0(struct saber_sni, 1);

  sni->introspection = introspection;
  sni->cb = cb;
  sni->user = user;
  sni->items = g_ptr_array_new_with_free_func(item_destroy);
  sni->hosts = g_hash_table_new_full(g_str_hash,
      g_str_equal,
      g_free,
      host_watch_destroy);

  /* Action purpose: The host name is per-process by specification. Owning it is
  what makes Saber visible as a host to any OTHER watcher, which is what would
  let the tray work if Saber loses the race for the watcher name. */
  char *host_name =
      g_strdup_printf("org.kde.StatusNotifierHost-%d", (int)getpid());

  sni->host_owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
      host_name,
      G_BUS_NAME_OWNER_FLAGS_NONE,
      NULL,
      NULL,
      NULL,
      NULL,
      NULL);
  g_free(host_name);

  /* DO_NOT_QUEUE so that losing is reported at once rather than leaving Saber
  silently queued behind a tray that is not going to exit. */
  sni->watcher_owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
      WATCHER_BUS_NAME,
      G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
      on_bus_acquired,
      on_name_acquired,
      on_name_lost,
      sni,
      NULL);

  if (sni->watcher_owner_id == 0) {
    g_warning("saber: could not reach the session bus for the tray.");
    saber_sni_destroy(sni);
    return NULL;
  }

  return sni;
}

void
saber_sni_destroy(struct saber_sni *sni)
{
  if (sni == NULL) {
    return;
  }

  g_clear_handle_id(&sni->changed_source, g_source_remove);

  /* Before the items: each item unsubscribes from the connection as it is
  freed. */
  if (sni->items != NULL) {
    g_ptr_array_free(sni->items, TRUE);
    sni->items = NULL;
  }

  if (sni->registration_id != 0 && sni->connection != NULL) {
    g_dbus_connection_unregister_object(sni->connection, sni->registration_id);
    sni->registration_id = 0;
  }

  if (sni->watcher_owner_id != 0) {
    g_bus_unown_name(sni->watcher_owner_id);
  }

  if (sni->host_owner_id != 0) {
    g_bus_unown_name(sni->host_owner_id);
  }

  if (sni->hosts != NULL) {
    g_hash_table_destroy(sni->hosts);
  }

  if (sni->introspection != NULL) {
    g_dbus_node_info_unref(sni->introspection);
  }

  g_free(sni);
}

bool
saber_sni_is_watcher(const struct saber_sni *sni)
{
  return sni != NULL && sni->host_registered;
}

unsigned int
saber_sni_count(const struct saber_sni *sni)
{
  if (sni == NULL || sni->items == NULL) {
    return 0;
  }

  return sni->items->len;
}

struct saber_sni_item *
saber_sni_nth(const struct saber_sni *sni, unsigned int n)
{
  if (sni == NULL || sni->items == NULL || n >= sni->items->len) {
    return NULL;
  }

  return g_ptr_array_index(sni->items, n);
}

struct saber_sni_item *
saber_sni_find(const struct saber_sni *sni, const char *service)
{
  if (sni == NULL) {
    return NULL;
  }

  int index = find_index(sni, service);

  return index < 0 ? NULL : g_ptr_array_index(sni->items, (guint)index);
}

const char *
saber_sni_item_service(const struct saber_sni_item *item)
{
  return item == NULL ? "" : item->service;
}

const char *
saber_sni_item_bus_name(const struct saber_sni_item *item)
{
  return item == NULL ? "" : item->bus_name;
}

const char *
saber_sni_item_object_path(const struct saber_sni_item *item)
{
  return item == NULL ? "" : item->object_path;
}

const char *
saber_sni_item_id(const struct saber_sni_item *item)
{
  return item == NULL ? "" : item->id;
}

const char *
saber_sni_item_title(const struct saber_sni_item *item)
{
  if (item == NULL) {
    return "";
  }

  /* Title is optional and a great many items never set it, while Id is
  mandatory. Falling back here keeps every drawing site from reimplementing the
  same guess. */
  return item->title[0] != '\0' ? item->title : item->id;
}

enum saber_sni_status
saber_sni_item_status(const struct saber_sni_item *item)
{
  return item == NULL ? SABER_SNI_PASSIVE : item->status;
}

const char *
saber_sni_item_icon_name(const struct saber_sni_item *item)
{
  if (item == NULL) {
    return "";
  }

  if (item->status == SABER_SNI_NEEDS_ATTENTION &&
      item->attention_icon_name[0] != '\0') {
    return item->attention_icon_name;
  }

  return item->icon_name;
}

const char *
saber_sni_item_overlay_icon_name(const struct saber_sni_item *item)
{
  return item == NULL ? "" : item->overlay_icon_name;
}

const char *
saber_sni_item_icon_theme_path(const struct saber_sni_item *item)
{
  return item == NULL ? "" : item->icon_theme_path;
}

const char *
saber_sni_item_menu_path(const struct saber_sni_item *item)
{
  return item == NULL ? "" : item->menu_path;
}

bool
saber_sni_item_has_menu(const struct saber_sni_item *item)
{
  /* Action purpose: INTEROPERABILITY RULE (c). ItemIsMenu is advisory -- it
  asks that a left click show the menu instead of activating -- and is NOT the
  test for whether a menu exists. An item whose entire interface IS its menu
  routinely omits the property. A non-empty Menu object path is the fact. */
  return item != NULL && item->menu_path[0] != '\0';
}

bool
saber_sni_item_is_menu(const struct saber_sni_item *item)
{
  return item != NULL && item->item_is_menu;
}

/* ---------------------------------------------------------------- */
/* icon decode -- the one place here taking hostile input             */
/* ---------------------------------------------------------------- */

/* Function purpose: Choose which of an item's offered sizes to decode. Entries
that fail validation are skipped rather than aborting the search: one malformed
size must not cost the item an icon it also supplied correctly. */
static bool
choose_pixmap(GVariant *pixmap, int *out_w, int *out_h, GVariant **out_bytes)
{
  GVariantIter iter;
  gint32 w = 0;
  gint32 h = 0;
  GVariant *bytes = NULL;
  int best_w = 0;
  int best_h = 0;
  GVariant *best = NULL;

  g_variant_iter_init(&iter, pixmap);

  while (g_variant_iter_next(&iter, "(ii@ay)", &w, &h, &bytes)) {
    gsize length = 0;
    const guchar *data = g_variant_get_fixed_array(bytes, &length, 1);

    /* Action purpose: Every one of these is a value the SENDER chose, and this
    runs before a single pixel is read. The multiplication cannot overflow
    because the dimension cap is applied first. */
    bool sane = w > 0 && h > 0 && w <= ITEM_ICON_MAX_DIM &&
                h <= ITEM_ICON_MAX_DIM && data != NULL &&
                length >= (gsize)w * (gsize)h * 4u;

    if (!sane) {
      g_variant_unref(bytes);
      continue;
    }

    bool better;

    if (best == NULL) {
      better = true;
    } else if (best_w < ITEM_ICON_PREFERRED_DIM) {
      /* Nothing big enough yet: anything larger is an improvement. */
      better = w > best_w;
    } else {
      /* Already big enough: only a smaller one that still is improves on it. */
      better = w >= ITEM_ICON_PREFERRED_DIM && w < best_w;
    }

    if (!better) {
      g_variant_unref(bytes);
      continue;
    }

    if (best != NULL) {
      g_variant_unref(best);
    }

    best = bytes;
    best_w = (int)w;
    best_h = (int)h;
  }

  if (best == NULL) {
    return false;
  }

  *out_w = best_w;
  *out_h = best_h;
  *out_bytes = best;

  return true;
}

/* Function purpose: Turn one validated pixmap into a premultiplied
native-endian ARGB32 buffer. Two conversions, both mandatory and neither
implied by the D-Bus type. */
static void
decode_icon(struct icon_cache *cache, GVariant *pixmap)
{
  cache->dirty = false;

  g_clear_pointer(&cache->argb, g_free);
  cache->width = 0;
  cache->height = 0;

  if (pixmap == NULL) {
    return;
  }

  int w = 0;
  int h = 0;
  GVariant *bytes = NULL;

  if (!choose_pixmap(pixmap, &w, &h, &bytes)) {
    return;
  }

  gsize length = 0;
  const guchar *src = g_variant_get_fixed_array(bytes, &length, 1);
  uint32_t *dst = g_malloc((gsize)w * (gsize)h * 4u);

  for (gsize i = 0, n = (gsize)w * (gsize)h; i < n; i++) {
    /* Action purpose: The wire format is ARGB32 in NETWORK BYTE ORDER -- the
    bytes arrive A, R, G, B in that order whatever the machine is -- while a
    cairo ARGB32 pixel is a native-endian 32-bit word. GUINT32_FROM_BE is the
    byteswap that reconciles them: a real swap on little-endian, a no-op on
    big-endian. Skipping it yields an icon whose channels are reversed, which
    reads as visibly wrong colours rather than as a crash. */
    uint32_t raw;

    memcpy(&raw, src + i * 4u, sizeof(raw));

    uint32_t px = GUINT32_FROM_BE(raw);
    uint32_t a = (px >> 24) & 0xFFu;
    uint32_t r = (px >> 16) & 0xFFu;
    uint32_t g = (px >> 8) & 0xFFu;
    uint32_t b = px & 0xFFu;

    /* Action purpose: And the wire format carries STRAIGHT alpha where cairo
    wants it PREMULTIPLIED. Copying without multiplying puts a bright halo
    around everything translucent, which is most anti-aliased icon edges. */
    if (a == 0) {
      dst[i] = 0;
      continue;
    }

    if (a != 0xFFu) {
      r = (r * a) / 0xFFu;
      g = (g * a) / 0xFFu;
      b = (b * a) / 0xFFu;
    }

    dst[i] = (a << 24) | (r << 16) | (g << 8) | b;
  }

  g_variant_unref(bytes);

  cache->argb = (uint8_t *)dst;
  cache->width = w;
  cache->height = h;
}

static bool
cache_result(const struct icon_cache *cache,
    const uint8_t **data,
    size_t *length,
    int *width,
    int *height)
{
  if (cache->argb == NULL) {
    return false;
  }

  if (data != NULL) {
    *data = cache->argb;
  }

  if (length != NULL) {
    *length = (size_t)cache->width * (size_t)cache->height * 4u;
  }

  if (width != NULL) {
    *width = cache->width;
  }

  if (height != NULL) {
    *height = cache->height;
  }

  return true;
}

bool
saber_sni_item_icon_argb32(struct saber_sni_item *item,
    const uint8_t **data,
    size_t *length,
    int *width,
    int *height)
{
  if (item == NULL) {
    return false;
  }

  if (item->icon.dirty) {
    GVariant *pixmap = item->status == SABER_SNI_NEEDS_ATTENTION &&
                               item->attention_icon_pixmap != NULL
                           ? item->attention_icon_pixmap
                           : item->icon_pixmap;

    decode_icon(&item->icon, pixmap);
  }

  return cache_result(&item->icon, data, length, width, height);
}

bool
saber_sni_item_overlay_argb32(struct saber_sni_item *item,
    const uint8_t **data,
    size_t *length,
    int *width,
    int *height)
{
  if (item == NULL) {
    return false;
  }

  if (item->overlay.dirty) {
    decode_icon(&item->overlay, item->overlay_icon_pixmap);
  }

  return cache_result(&item->overlay, data, length, width, height);
}

/* ---------------------------------------------------------------- */
/* actions                                                           */
/* ---------------------------------------------------------------- */

/* Fire and forget: no reply type and no callback. An application that declines
to be activated is not something the user can act on, and raising a dialog for
somebody else's bug is worse than the click doing nothing. */
static void
call_item(struct saber_sni_item *item, const char *method, GVariant *args)
{
  if (item == NULL || item->sni->connection == NULL) {
    if (args != NULL) {
      g_variant_unref(g_variant_ref_sink(args));
    }
    return;
  }

  g_dbus_connection_call(item->sni->connection,
      item->bus_name,
      item->object_path,
      ITEM_INTERFACE,
      method,
      args,
      NULL,
      G_DBUS_CALL_FLAGS_NO_AUTO_START,
      ITEM_CALL_TIMEOUT_MS,
      item->cancellable,
      NULL,
      NULL);
}

void
saber_sni_item_activate(struct saber_sni_item *item, int x, int y)
{
  call_item(item, "Activate", g_variant_new("(ii)", x, y));
}

void
saber_sni_item_secondary_activate(struct saber_sni_item *item, int x, int y)
{
  call_item(item, "SecondaryActivate", g_variant_new("(ii)", x, y));
}

void
saber_sni_item_context_menu(struct saber_sni_item *item, int x, int y)
{
  call_item(item, "ContextMenu", g_variant_new("(ii)", x, y));
}

void
saber_sni_item_scroll(struct saber_sni_item *item,
    int delta,
    enum saber_sni_orientation orientation)
{
  const char *axis =
      orientation == SABER_SNI_HORIZONTAL ? "horizontal" : "vertical";

  call_item(item, "Scroll", g_variant_new("(is)", delta, axis));
}
