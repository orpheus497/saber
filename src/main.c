/* Script function and purpose: Entry point for saber(1), the vertical panel of
the Hikari Sakura desktop. Loads the configuration, builds the theme, connects
to the compositor, wires the seventeen modules to one another and runs the GLib
main loop until a signal or a lost connection ends it.

The library version calls in report_build() are not decoration. They reference
runtime symbols rather than compile-time macros on purpose, so that a
successful `make` is evidence the shared libraries were genuinely found and
linked and not merely that their headers were on the include path. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <grp.h>
#include <limits.h>
#include <sys/types.h>

#include <cairo.h>
#include <glib-unix.h>
#include <glib.h>
#include <wayland-client.h>

#include "wlr-foreign-toplevel-management-unstable-v1-protocol.h"

#include <saber/anim.h>
#include <saber/appinfo.h>
#include <saber/config.h>
#include <saber/devices.h>
#include <saber/display.h>
#include <saber/match.h>
#include <saber/model.h>
#include <saber/panel.h>
#include <saber/render.h>
#include <saber/saber.h>
#include <saber/sheets.h>
#include <saber/sni.h>
#include <saber/theme.h>
#include <saber/toplevel.h>
#include <saber/trash.h>
#include <saber/unity.h>

/* Function purpose: Print the feature set this binary was actually built with.
Exists so `make WITH_ALL=NO` is verifiable against the binary rather than only
against the Makefile, which is the difference between believing a switch works
and knowing it does. */
static void
report_build(void)
{
  printf("saber %s\n", SABER_VERSION);
  printf("prefix        %s\n", SABER_PREFIX);
  printf("configuration %s\n", SABER_SYSTEM_CONFIG);
  printf("\nfeatures:\n");

#ifdef HAVE_TRAY
  printf("  tray             yes\n");
#else
  printf("  tray             no\n");
#endif
#ifdef HAVE_LAUNCHER_ENTRY
  printf("  launcher-entry   yes\n");
#else
  printf("  launcher-entry   no\n");
#endif
#ifdef HAVE_SHEETS
  printf("  sheets           yes\n");
#else
  printf("  sheets           no\n");
#endif
#ifdef HAVE_DEVICES
  printf("  devices          yes\n");
#else
  printf("  devices          no\n");
#endif
#ifdef HAVE_DASH
  printf("  dash             yes\n");
#else
  printf("  dash             no\n");
#endif
#ifdef HAVE_SPREAD
  printf("  spread           yes\n");
#else
  printf("  spread           no\n");
#endif
#ifdef HAVE_VIRTUAL_INPUT
  printf("  virtual-input    yes\n");
#else
  printf("  virtual-input    no\n");
#endif

  printf("\nlinked against:\n");
  printf("  glib             %u.%u.%u\n",
      glib_major_version,
      glib_minor_version,
      glib_micro_version);
  printf("  cairo            %s\n", cairo_version_string());
}

/* Function purpose: Report whether this user can carry out the session actions,
using the same test the panel itself will use to decide whether to draw them.

The result is informational here and load-bearing later: Saber HIDES suspend,
reboot and shut down when the answer is no, rather than offering buttons that
fail. See DECISIONS_LOG D-013 for why membership of the base system's existing
`operator` group is the whole privilege model, and why Saber ships nothing
setuid, setgid or sudoers-shaped. */
static bool
in_operator_group(void)
{
  struct group *operator_group = getgrnam("operator");

  if (operator_group == NULL) {
    return false;
  }

  gid_t groups[NGROUPS_MAX];
  int count = getgroups(NGROUPS_MAX, groups);

  if (count < 0) {
    return false;
  }

  for (int i = 0; i < count; i++) {
    if (groups[i] == operator_group->gr_gid) {
      return true;
    }
  }

  return false;
}

static void
usage(FILE *stream, const char *argv0)
{
  fprintf(stream,
      "usage: %s [-h] [-v] [-b]\n"
      "\n"
      "  -h, --help      this message\n"
      "  -v, --version   version only\n"
      "  -b, --build     build configuration, features and linked libraries\n",
      argv0);
}

struct saber_app {
  struct saber_config config;
  struct saber_theme theme;

  struct saber_display *display;
  struct saber_appinfo_index *index;
  struct saber_match *match;
  struct saber_model *model;
  struct saber_toplevels *toplevels;
  struct saber_sheets *sheets;
  struct saber_trash *trash;
  struct saber_devices *devices;
  struct saber_sni *sni;
  struct saber_unity *unity;
  struct saber_icons *icons;
  struct saber_panels *panels;

  GMainLoop *loop;
};

static void
app_repaint(struct saber_app *app)
{
  if (app->panels != NULL) {
    saber_panels_refresh(app->panels);
  }
}

static void
on_model_changed(void *user)
{
  app_repaint(user);
}

/* An entry installed, edited or removed while the panel runs must reach the
tiles without a restart; the index has already re-scanned by this point. */
static void
on_appinfo_changed(void *user)
{
  struct saber_app *app = user;

  saber_model_refresh(app->model);
  app_repaint(app);
}

/* Action purpose: The sheet edge trigger (BLUEPRINT.md, sheets.h). The control
socket has no change event, and a sheet switch reaches this client only as
foreign-toplevel minimized bits moving, so every disturbance of the window set
invalidates the cached sheet state. Nothing else calls this, and when it is
missed the sheet tile silently freezes on its first reading. */
static void
app_invalidate_sheets(struct saber_app *app)
{
  if (app->sheets != NULL) {
    saber_sheets_invalidate(app->sheets);
  }
}

static void
app_sync_focus(struct saber_app *app)
{
  saber_model_set_focus(app->model, saber_toplevels_activated(app->toplevels));
}

static void
on_toplevel_added(void *user, struct saber_toplevel *toplevel)
{
  struct saber_app *app = user;

  saber_model_window_added(app->model, toplevel, toplevel->app_id);
  app_sync_focus(app);
  app_invalidate_sheets(app);
  app_repaint(app);
}

static void
on_toplevel_changed(void *user,
    struct saber_toplevel *toplevel,
    uint32_t changes)
{
  struct saber_app *app = user;

  /* A late or corrected app_id rebinds the window to whatever tile it now
  resolves to, rather than leaving it on the one it first landed on. */
  if ((changes & SABER_TOPLEVEL_CHANGE_APP_ID) != 0) {
    saber_model_window_removed(app->model, toplevel);
    saber_model_window_added(app->model, toplevel, toplevel->app_id);
  }

  if ((changes & SABER_TOPLEVEL_CHANGE_STATE) != 0) {
    app_sync_focus(app);
    app_invalidate_sheets(app);
  }

  app_repaint(app);
}

static void
on_toplevel_closed(void *user, struct saber_toplevel *toplevel)
{
  struct saber_app *app = user;

  saber_model_window_removed(app->model, toplevel);
  app_sync_focus(app);
  app_invalidate_sheets(app);
  app_repaint(app);
}

static const struct saber_toplevel_listener app_toplevel_listener = {
  .added = on_toplevel_added,
  .changed = on_toplevel_changed,
  .closed = on_toplevel_closed,
};

/* Action purpose: The manager display.c binds during startup is bound inside
the FIRST of saber_display_create's two round trips, and the compositor answers
a bind by sending one `toplevel` event per window that already exists. Those
arrive in the SECOND round trip -- before anything has attached a listener --
so every window open when Saber starts is dispatched into nothing and the
column comes up empty until the user opens something new. It fails silently and
looks exactly like a compositor that does not support the protocol.

Binding a second manager here, and only then attaching the listener, makes the
compositor resend the whole list to an object that is listening for it. The
first manager is stopped and released below.

The proper fix belongs in display.c -- bind this global lazily, or let the
caller listen before the second round trip -- at which point this whole
function should go. */
struct toplevel_rebind {
  struct zwlr_foreign_toplevel_manager_v1 *manager;
};

static void
rebind_global(void *data,
    struct wl_registry *registry,
    uint32_t name,
    const char *interface,
    uint32_t version)
{
  struct toplevel_rebind *rebind = data;

  if (rebind->manager != NULL ||
      strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) != 0) {
    return;
  }

  rebind->manager = wl_registry_bind(registry, name,
      &zwlr_foreign_toplevel_manager_v1_interface, version < 3 ? version : 3);
}

static void
rebind_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
  (void)data;
  (void)registry;
  (void)name;
}

static const struct wl_registry_listener rebind_registry_listener = {
  .global = rebind_global,
  .global_remove = rebind_global_remove,
};

/* The stopped manager may still deliver a handle or two before `finished`;
both are released here so the startup path leaves nothing dangling. */
static void
stale_toplevel(void *data,
    struct zwlr_foreign_toplevel_manager_v1 *manager,
    struct zwlr_foreign_toplevel_handle_v1 *handle)
{
  (void)data;
  (void)manager;

  zwlr_foreign_toplevel_handle_v1_destroy(handle);
}

static void
stale_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *manager)
{
  (void)data;

  zwlr_foreign_toplevel_manager_v1_destroy(manager);
}

static const struct zwlr_foreign_toplevel_manager_v1_listener stale_listener = {
  .toplevel = stale_toplevel,
  .finished = stale_finished,
};

static struct zwlr_foreign_toplevel_manager_v1 *
rebind_toplevel_manager(struct saber_display *display)
{
  if (display->foreign_toplevel_manager == NULL) {
    return NULL;
  }

  zwlr_foreign_toplevel_manager_v1_add_listener(
      display->foreign_toplevel_manager, &stale_listener, NULL);
  zwlr_foreign_toplevel_manager_v1_stop(display->foreign_toplevel_manager);
  display->foreign_toplevel_manager = NULL;

  struct toplevel_rebind rebind = { NULL };
  struct wl_registry *registry = wl_display_get_registry(display->wl_display);

  wl_registry_add_listener(registry, &rebind_registry_listener, &rebind);

  /* One round trip binds the manager; the `toplevel` burst it provokes cannot
  arrive before the sync that ends this trip, so it is still waiting when the
  caller attaches its listener. */
  wl_display_roundtrip(display->wl_display);
  wl_registry_destroy(registry);

  return rebind.manager;
}

static void
on_sheets_state(const struct saber_sheets_state *state, void *user)
{
  (void)state;

  app_repaint(user);
}

static void
on_trash_changed(unsigned int count, void *user)
{
  (void)count;

  app_repaint(user);
}

static void
on_devices_changed(void *user)
{
  app_repaint(user);
}

static void
on_sni_changed(void *user)
{
  app_repaint(user);
}

/* Function purpose: Carry com.canonical.Unity.LauncherEntry state onto the
tile. unity.c accumulates the properties; the model only stores the result, so
this is the one place the two structures meet. */
static void
on_unity_changed(const char *desktop_id,
    const struct saber_unity_entry *entry,
    void *user)
{
  struct saber_app *app = user;
  struct saber_badge badge = {
    .count = entry->count,
    .count_visible = entry->count_visible,
    .progress = entry->progress,
    .progress_visible = entry->progress_visible,
    .urgent = entry->urgent,
  };

  saber_model_set_badge(app->model, desktop_id, &badge);
  app_repaint(app);
}

static void
on_disconnect(void *user)
{
  struct saber_app *app = user;

  g_warning("saber: the compositor went away");

  if (app->loop != NULL) {
    g_main_loop_quit(app->loop);
  }
}

static gboolean
on_signal(gpointer user)
{
  struct saber_app *app = user;

  if (app->loop != NULL) {
    g_main_loop_quit(app->loop);
  }

  /* Kept alive so the explicit g_source_remove after the loop returns has
  something to remove; destroying it from here makes that call a warning. */
  return G_SOURCE_CONTINUE;
}

/* Action purpose: Torn down in reverse dependency order. toplevels holds the
seat the display owns, and the panels hold surfaces on the display, so both go
before it; the model outlives the panels that read it. */
static void
app_shutdown(struct saber_app *app)
{
  saber_panels_destroy(app->panels);
  saber_toplevels_destroy(app->toplevels);
  saber_unity_destroy(app->unity);
  saber_sni_destroy(app->sni);
  saber_devices_destroy(app->devices);
  saber_trash_destroy(app->trash);
  saber_sheets_destroy(app->sheets);
  saber_model_destroy(app->model);
  saber_match_destroy(app->match);
  saber_appinfo_index_destroy(app->index);
  saber_icons_destroy(app->icons);
  saber_display_destroy(app->display);
  saber_config_fini(&app->config);

  if (app->loop != NULL) {
    g_main_loop_unref(app->loop);
  }
}

static int
run(void)
{
  struct saber_app app;

  memset(&app, 0, sizeof(app));

  saber_config_load(&app.config, NULL);
  saber_theme_init(&app.theme,
      app.config.theme.palette_valid ? app.config.theme.palette : NULL,
      app.config.theme.opacity);

  app.display = saber_display_create(NULL);

  if (app.display == NULL) {
    const char *socket = g_getenv("WAYLAND_DISPLAY");

    fprintf(stderr,
        "saber: cannot connect to a Wayland compositor%s%s%s.\n"
        "saber: Saber is a wlr-layer-shell client and must be started from "
        "inside a running hikari-sakura session.\n",
        socket != NULL ? " on WAYLAND_DISPLAY=" : "",
        socket != NULL ? socket : "",
        socket != NULL ? "" : " (WAYLAND_DISPLAY is not set)");

    saber_config_fini(&app.config);

    return EXIT_FAILURE;
  }

  app.loop = g_main_loop_new(NULL, FALSE);

  if (!saber_display_attach(app.display, NULL)) {
    fprintf(stderr, "saber: could not drive the Wayland connection from the "
                    "main loop.\n");
    app_shutdown(&app);

    return EXIT_FAILURE;
  }

  saber_display_set_disconnect_handler(app.display, on_disconnect, &app);

  app.icons = saber_icons_create();
  app.index = saber_appinfo_index_create();
  app.match = saber_match_create(app.index);
  app.model = saber_model_create(&app.config, app.index, app.match);

  saber_appinfo_index_set_changed(app.index, on_appinfo_changed, &app);
  saber_model_set_changed(app.model, on_model_changed, &app);

  /* Action purpose: Ownership of the manager transfers here, and the seat has
  to follow it -- zwlr_foreign_toplevel_handle_v1.activate takes a seat and has
  no seatless form, so without this every click that should raise a window does
  nothing at all and reports nothing. */
  app.toplevels = saber_toplevels_create(rebind_toplevel_manager(app.display),
      &app_toplevel_listener, &app);

  if (app.toplevels != NULL) {
    saber_toplevels_set_seat(app.toplevels, app.display->seat);

    /* Now that the listener is attached, collect the windows that were
    already open when Saber started. */
    wl_display_roundtrip(app.display->wl_display);
  } else {
    g_warning("saber: the compositor does not advertise "
              "zwlr_foreign_toplevel_management_v1; the window list is empty");
  }

#ifdef HAVE_SHEETS
  if (app.config.items.sheets) {
    app.sheets = saber_sheets_create(on_sheets_state, &app);
  }
#endif

  if (app.config.items.trash) {
    app.trash = saber_trash_create(on_trash_changed, &app);
  }

#ifdef HAVE_DEVICES
  if (app.config.items.devices) {
    app.devices = saber_devices_create(on_devices_changed, &app);
  }
#endif

#ifdef HAVE_TRAY
  if (app.config.items.tray) {
    app.sni = saber_sni_create(on_sni_changed, &app);

    if (app.sni != NULL && !saber_sni_is_watcher(app.sni)) {
      g_warning("saber: another tray host owns "
                "org.kde.StatusNotifierWatcher; the tray zone will be empty");
    }
  }
#endif

#ifdef HAVE_LAUNCHER_ENTRY
  app.unity = saber_unity_create(on_unity_changed, &app);

  if (app.unity != NULL && !saber_unity_is_owner(app.unity)) {
    g_warning("saber: another launcher owns com.canonical.Unity; count "
              "badges and progress bars will stay dead");
  }
#endif

  struct saber_panel_deps deps = {
    .config = &app.config,
    .theme = &app.theme,
    .display = app.display,
    .icons = app.icons,
    .model = app.model,
    .toplevels = app.toplevels,
    .sheets = app.sheets,
    .trash = app.trash,
    .devices = app.devices,
    .sni = app.sni,
  };

  app.panels = saber_panels_create(&deps);

  if (saber_panels_count(app.panels) == 0) {
    fprintf(stderr,
        "saber: no output matched panel { output = \"%s\" }; nothing to "
        "draw on.\n",
        app.config.panel.output != NULL ? app.config.panel.output : "all");
    app_shutdown(&app);

    return EXIT_FAILURE;
  }

  guint sigint = g_unix_signal_add(SIGINT, on_signal, &app);
  guint sigterm = g_unix_signal_add(SIGTERM, on_signal, &app);

  saber_display_flush(app.display);
  g_main_loop_run(app.loop);

  g_source_remove(sigint);
  g_source_remove(sigterm);
  app_shutdown(&app);

  return EXIT_SUCCESS;
}

int
main(int argc, char **argv)
{
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(stdout, argv[0]);
      return EXIT_SUCCESS;
    }

    if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
      printf("saber %s\n", SABER_VERSION);
      return EXIT_SUCCESS;
    }

    if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--build") == 0) {
      report_build();
      printf("  operator group   %s\n", in_operator_group() ? "yes" : "no");
      return EXIT_SUCCESS;
    }

    fprintf(stderr, "%s: unknown option: %s\n", argv[0], argv[i]);
    usage(stderr, argv[0]);
    return EXIT_FAILURE;
  }

  return run();
}
