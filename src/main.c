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
#include <saber/dash.h>
#include <saber/devices.h>
#include <saber/display.h>
#include <saber/dnd.h>
#include <saber/ipc.h>
#include <saber/match.h>
#include <saber/model.h>
#include <saber/panel.h>
#include <saber/render.h>
#include <saber/saber.h>
#include <saber/sheets.h>
#include <saber/sni.h>
#include <saber/spread.h>
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
  struct saber_dnd *dnd;
  struct saber_dash *dash;
  struct saber_spread *spread;
  struct saber_ipc *ipc;

  /* Set while the click that dismissed the dash is being handed on. The dash
  has already hidden itself by then, so a BFB tile reached through that click
  would find it closed and toggle it straight back open -- and the button that
  opens the dash could never be the one that closes it. */
  bool dash_dismissing;

  GMainLoop *loop;
};

/* Which modal surface is being raised, so app_clear_modals leaves that one
alone and dismisses the rest. */
enum saber_modal {
  MODAL_DASH,
  MODAL_SPREAD,
};

/* Function purpose: Leave exactly one modal surface up. The Dash and the
spread are both full-output OVERLAY surfaces asking for EXCLUSIVE keyboard, and
before this each could be opened over the other -- two surfaces holding the
same keyboard, `saberctl status` reporting both open, and a quicklist left
mapped underneath with no way to reach it. Every entry point that raises one of
them calls this first. Lives in main.c because D-015 keeps the Dash and spread
reachable only through hooks, so this is the one place that holds both. */
static void
app_clear_modals(struct saber_app *app, enum saber_modal raising)
{
  saber_panels_close_menu(app->panels);

#ifdef HAVE_SPREAD
  if (raising != MODAL_SPREAD && app->spread != NULL &&
      saber_spread_is_visible(app->spread)) {
    saber_spread_hide(app->spread);
  }
#endif

#ifdef HAVE_DASH
  if (raising != MODAL_DASH && app->dash != NULL &&
      saber_dash_is_visible(app->dash)) {
    saber_dash_hide(app->dash);
  }
#endif

  (void)raising;
}

/* Function purpose: The saberctl verb table. ipc.c holds no panel logic, so
every verb resolves to one of these; a NULL member is answered "error feature
not built", which is how a WITH_DASH=NO build needs no conditional there. */
static enum saber_ipc_result
ipc_dash(void *user)
{
#ifdef HAVE_DASH
  struct saber_app *app = user;

  if (app->dash != NULL) {
    if (!saber_dash_is_visible(app->dash)) {
      app_clear_modals(app, MODAL_DASH);
    }

    saber_dash_toggle(app->dash, NULL);

    return SABER_IPC_RESULT_OK;
  }
#else
  (void)user;
#endif

  return SABER_IPC_RESULT_NOT_BUILT;
}

static enum saber_ipc_result
ipc_spread(const char *app_id, void *user)
{
#ifdef HAVE_SPREAD
  struct saber_app *app = user;

  if (app->spread != NULL) {
    if (!saber_spread_is_visible(app->spread)) {
      app_clear_modals(app, MODAL_SPREAD);
    }

    saber_spread_toggle(app->spread, app_id, NULL);

    return SABER_IPC_RESULT_OK;
  }
#else
  (void)app_id;
  (void)user;
#endif

  return SABER_IPC_RESULT_NOT_BUILT;
}

#ifdef HAVE_SHEETS
/* Function purpose: Report a sheet request the compositor refused.

Every caller used to pass no reply callback, so `saberctl sheet 3` answered `ok`
the moment the request was queued -- before the socket had been written to, let
alone answered -- and a refusal produced no message, no revert and no log line.
The request is asynchronous and the control socket has already been answered by
the time the reply lands, so the exit status cannot carry it; a warning naming
the reason is what is left, and it is a great deal better than silence. */
static void
on_sheet_reply(enum saber_sheets_status status, void *user)
{
  (void)user;

  if (status != SABER_SHEETS_OK) {
    g_warning("saber: the compositor refused the sheet request: %s",
        saber_sheets_status_string(status));
  }
}
#endif

static enum saber_ipc_result
ipc_sheet(int sheet, void *user)
{
#ifdef HAVE_SHEETS
  struct saber_app *app = user;

  if (app->sheets != NULL) {
    saber_sheets_switch(app->sheets, sheet, on_sheet_reply, app);

    return SABER_IPC_RESULT_OK;
  }
#else
  (void)sheet;
  (void)user;
#endif

  return SABER_IPC_RESULT_NOT_BUILT;
}

static enum saber_ipc_result
ipc_pin(int sheet, void *user)
{
#ifdef HAVE_SHEETS
  struct saber_app *app = user;

  if (app->sheets != NULL) {
    saber_sheets_pin(app->sheets, sheet, on_sheet_reply, app);

    return SABER_IPC_RESULT_OK;
  }
#else
  (void)sheet;
  (void)user;
#endif

  return SABER_IPC_RESULT_NOT_BUILT;
}

/* A keybinding launch waiting on its activation token. Holds the model rather
than the application struct, because the reply is asynchronous and only these
two are needed once it lands. */
struct ipc_launch {
  struct saber_model *model;
  struct saber_appinfo *app; /* owned: one reference, released here */
};

static void
ipc_launch_with_token(const char *token, void *user)
{
  struct ipc_launch *pending = user;
  struct saber_model *model = pending->model;
  struct saber_appinfo *app = pending->app;

  g_free(pending);

  if (saber_appinfo_launch(app, NULL, NULL, token)) {
    saber_model_note_launch(model, app->id);
  } else {
    g_warning("saber: failed to launch '%s'", app->id);
  }

  saber_appinfo_unref(app);
}

static enum saber_ipc_result
ipc_launch(int favourite, void *user)
{
  struct saber_app *app = user;
  const struct saber_item *item = saber_model_nth(app->model, favourite - 1);

  if (item == NULL || item->type != SABER_ITEM_APP) {
    return SABER_IPC_RESULT_NO_FAVOURITE;
  }

  /* Action purpose: Unity's Super+N focuses a running application rather than
  starting a second copy. Unminimise first -- on hikari the minimised bit means
  "on a sheet you are not looking at", so activating without clearing it raises
  a window that stays invisible. */
  if (item->windows != NULL && item->windows->len > 0) {
    struct saber_toplevel *window = g_ptr_array_index(item->windows, 0);

    saber_toplevel_unset_minimized(window);
    saber_toplevel_activate(window);

    return SABER_IPC_RESULT_OK;
  }

  if (item->app == NULL) {
    return SABER_IPC_RESULT_FAILED;
  }

  /* Action purpose: Ask for an activation token, exactly as a click on the tile
  does. This path is what `Logo+1..4` runs, and without a token the window it
  starts arrives unfocused -- so the keybinding started the application and then
  left the user to click it, which is not what the binding is for.

  The reply is asynchronous and the control connection has already been answered
  by the time it lands, so the verb reports that the launch was accepted rather
  than that it succeeded; a failure after this point is warned about by
  ipc_launch_with_token. */
  struct ipc_launch *pending = g_new0(struct ipc_launch, 1);

  pending->model = app->model;
  pending->app = saber_appinfo_ref(item->app);

  saber_display_request_activation(app->display, NULL, item->id,
      ipc_launch_with_token, pending);

  return SABER_IPC_RESULT_OK;
}

static void
app_repaint(struct saber_app *app);

/* Function purpose: Re-read the configuration and apply what can be applied
without restarting. Shared by `saberctl reload` and SIGHUP.

The new file is parsed into a scratch structure and only the fields that are
safe to change under a running panel are copied across, because `app.config` is
handed to every subsystem by pointer and its strings are read live -- freeing
and reallocating them underneath a running model would be a use-after-free
looking for somewhere to happen. What is copied is scalar or a fixed array:
the panel's geometry and behaviour, and the whole theme block, which contains
no pointers.

What deliberately does not reload: `panel { output }`, which decides how many
columns exist, and the `items { }` booleans, which decide whether the tray,
sheets, devices and trash subsystems are constructed at all. Both are settled at
startup and changing them means recreating surfaces and subsystems, which is a
restart in everything but name. */
static bool
app_reload_config(struct saber_app *app)
{
  struct saber_config next;
  enum saber_config_status status = SABER_CONFIG_ABSENT;

  memset(&next, 0, sizeof(next));
  saber_config_load_status(&next, NULL, &status);

  /* Action purpose: A file that would not parse must not reach the running
  panel. saber_config_load fills in defaults for everything it could not read,
  which is right at startup -- a broken file costs the user their settings,
  never their panel -- but here those defaults would land on top of the
  settings already in force, so one typo would throw away the theme the panel
  is running and `reload` would answer `ok`. Refused instead; the parse error is
  already on stderr. An absent file is not refused: the defaults are then the
  configuration, exactly as they are at startup. */
  if (status == SABER_CONFIG_UNREADABLE) {
    saber_config_fini(&next);

    return false;
  }

  app->config.panel.icon_size = next.panel.icon_size;
  app->config.panel.padding = next.panel.padding;
  app->config.panel.autohide = next.panel.autohide;
  app->config.panel.reveal_pressure = next.panel.reveal_pressure;
  app->config.panel.animation_ms = next.panel.animation_ms;
  app->config.theme = next.theme;

  saber_config_fini(&next);

  saber_theme_init(&app->theme,
      app->config.theme.palette_valid ? app->config.theme.palette : NULL,
      app->config.theme.opacity, app->config.theme.overlay_opacity);

  saber_panels_reload(app->panels);
  app_repaint(app);

  return true;
}

static enum saber_ipc_result
ipc_reload(void *user)
{
  return app_reload_config(user) ? SABER_IPC_RESULT_OK
                                 : SABER_IPC_RESULT_FAILED;
}

/* Action purpose: SIGHUP is the conventional "re-read your configuration" for a
resident process, and a panel started from a compositor autostart has no other
channel a shell script can reach it on. Runs on the main loop rather than in the
handler, so it may allocate and touch the panel like any other event. */
static gboolean
on_sighup(gpointer user)
{
  app_reload_config(user);

  return G_SOURCE_CONTINUE;
}

/* Function purpose: `saberctl overlay on|off` -- the hold-Super launch-number
legend, driven from the compositor because a panel cannot observe a modifier it
has no keyboard focus for. Bind the key's press and release to the two forms. */
static enum saber_ipc_result
ipc_overlay(bool on, void *user)
{
  struct saber_app *app = user;

  if (app->panels == NULL) {
    return SABER_IPC_RESULT_FAILED;
  }

  saber_panels_set_overlay(app->panels, on);

  return SABER_IPC_RESULT_OK;
}

/* Function purpose: `saberctl show`, `hide` and `toggle`.

These were advertised in `--help` from the beginning and answered `error feature
not built` in every build, because the handler was never installed and there was
no visibility control behind it to install one for. Both halves exist now. */
static enum saber_ipc_result
ipc_visibility(enum saber_ipc_visibility action, void *user)
{
  struct saber_app *app = user;

  if (app->panels == NULL) {
    return SABER_IPC_RESULT_FAILED;
  }

  switch (action) {
  case SABER_IPC_VISIBILITY_SHOW:
    saber_panels_set_visible(app->panels, true);
    break;

  case SABER_IPC_VISIBILITY_HIDE:
    saber_panels_set_visible(app->panels, false);
    break;

  case SABER_IPC_VISIBILITY_TOGGLE:
    saber_panels_toggle_visible(app->panels);
    break;
  }

  return SABER_IPC_RESULT_OK;
}

static enum saber_ipc_result
ipc_status(struct saber_ipc_report *report, void *user)
{
  struct saber_app *app = user;

  saber_ipc_report_line(report, "version %s", SABER_VERSION);
  saber_ipc_report_line(report, "panels %d",
      (int)saber_panels_count(app->panels));
  saber_ipc_report_line(report, "items %d", (int)saber_model_size(app->model));
  saber_ipc_report_line(report, "dash %s",
      app->dash != NULL && saber_dash_is_visible(app->dash) ? "open" : "closed");
  saber_ipc_report_line(report, "spread %s",
      app->spread != NULL && saber_spread_is_visible(app->spread) ? "open"
                                                                 : "closed");

  return SABER_IPC_RESULT_OK;
}

static enum saber_ipc_result
ipc_quit(void *user)
{
  struct saber_app *app = user;

  g_main_loop_quit(app->loop);

  return SABER_IPC_RESULT_OK;
}

/* Function purpose: Adapters that give the panel's hooks the shape they want.
The Dash and the spread are separate surfaces with their own modules, so the
panel reaches them through a function pointer rather than a link dependency --
which is also what keeps the column working under WITH_DASH=NO. */
static void
on_bfb_clicked(struct saber_output *output, void *user)
{
  struct saber_app *app = user;

  /* Action purpose: A click that has just dismissed the dash has already done
  this button's work. Toggling here would find the dash closed and reopen it on
  the very press that shut it, so the BFB could never close the dash at all. */
  if (app->dash_dismissing) {
    return;
  }

  if (app->dash != NULL) {
    if (!saber_dash_is_visible(app->dash)) {
      app_clear_modals(app, MODAL_DASH);
    }

    saber_dash_toggle(app->dash, output);
  }
}

/* Action purpose: The press that dismissed the dash still belongs to whatever
it landed on. The dash covers the output while it is up, so without this a
click on a panel tile would be spent closing the dash and the tile would need a
second one.

The dash has already hidden itself by the time this runs, which is what the
guard is for: the BFB's own handler would otherwise see a closed dash and toggle
it back open on the same press, leaving the button unable to close what it
opened. */
static void
on_dash_dismissed(void *user,
    struct saber_output *output,
    double x,
    double y,
    uint32_t button)
{
  struct saber_app *app = user;

  if (app->panels == NULL) {
    return;
  }

  app->dash_dismissing = true;
  saber_panels_click_at(app->panels, output, x, y, button);
  app->dash_dismissing = false;
}

/* Function purpose: Drag-and-drop onto a tile, which until now was 609 lines
that nothing constructed.

A file dragged from a file manager onto an application's tile opens it with that
application. The three callbacks are deliberately thin: dnd.c owns the protocol
and the accept/reject handshake, panel.c owns the hit test, and this is the
seam. */
static bool
on_dnd_accepts(void *user, struct wl_surface *surface, double x, double y)
{
  struct saber_app *app = user;

  return saber_panels_accepts_drop(app->panels, surface, x, y);
}

static void
on_dnd_drop(void *user,
    struct wl_surface *surface,
    double x,
    double y,
    char **uris)
{
  struct saber_app *app = user;

  saber_panels_drop_at(app->panels, surface, x, y, (const char *const *)uris);
}

static const struct saber_dnd_listener app_dnd_listener = {
  .accepts = on_dnd_accepts,
  .motion = NULL,
  .drop = on_dnd_drop,
};

static void
on_spread_requested(const char *app_id, struct saber_output *output, void *user)
{
  struct saber_app *app = user;

  if (app->spread != NULL) {
    if (!saber_spread_is_visible(app->spread)) {
      app_clear_modals(app, MODAL_SPREAD);
    }

    saber_spread_toggle(app->spread, app_id, output);
  }
}

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

  /* A window opening while the spread is up belongs in it. */
#ifdef HAVE_SPREAD
  if (app->spread != NULL && saber_spread_is_visible(app->spread)) {
    saber_spread_refresh(app->spread);
  }
#endif
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

  /* Action purpose: An open spread holds one cell per window and had no way to
  learn that one had gone -- saber_spread_refresh existed with no external
  caller, so every closed window left a cell behind that still offered to raise
  or close it. */
#ifdef HAVE_SPREAD
  if (app->spread != NULL && saber_spread_is_visible(app->spread)) {
    saber_spread_refresh(app->spread);
  }
#endif
}

static const struct saber_toplevel_listener app_toplevel_listener = {
  .added = on_toplevel_added,
  .changed = on_toplevel_changed,
  .closed = on_toplevel_closed,
};

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
  /* Before the panels: both hold a surface that owns the seat's keyboard while
  mapped, and tearing the display down under them leaves the session deaf. */
  saber_ipc_destroy(app->ipc);
  saber_dnd_destroy(app->dnd);
  saber_dash_destroy(app->dash);
  saber_spread_destroy(app->spread);
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
      app.config.theme.opacity, app.config.theme.overlay_opacity);

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
  app.toplevels = saber_toplevels_create(
      saber_display_take_foreign_toplevels(app.display),
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
    /* Action purpose: No conflict check here. Bus name ownership is resolved
    asynchronously, so a test taken between the create call and the main loop
    always reads the pre-acquisition value and always warns -- on a healthy
    session as loudly as on a contested one, which is worse than saying nothing.
    sni.c reports the real answer from its own name-lost callback, at the moment
    it is known and with the offending name in the message. */
    app.sni = saber_sni_create(on_sni_changed, &app);
  }
#endif

#ifdef HAVE_LAUNCHER_ENTRY
  /* Same asynchronous-ownership reasoning as the tray above: unity.c warns from
  its own name-lost callback, once the answer exists. */
  app.unity = saber_unity_create(on_unity_changed, &app);
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
    .unity = app.unity,
    .index = app.index,
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

#ifdef HAVE_DASH
  struct saber_dash_deps dash_deps = {
    .display = app.display,
    .config = &app.config,
    .theme = &app.theme,
    .icons = app.icons,
    .index = app.index,
    .match = app.match,
    .model = app.model,
  };

  app.dash = saber_dash_create(&dash_deps);
  saber_dash_set_dismissed(app.dash, on_dash_dismissed, &app);
  saber_panels_set_dash(app.panels, on_bfb_clicked, &app);
#endif

#ifdef HAVE_SPREAD
  struct saber_spread_deps spread_deps = {
    .display = app.display,
    .config = &app.config,
    .theme = &app.theme,
    .icons = app.icons,
    .index = app.index,
    .match = app.match,
    .model = app.model,
    .toplevels = app.toplevels,
  };

  app.spread = saber_spread_create(&spread_deps);
  saber_panels_set_spread(app.panels, on_spread_requested, &app);
#endif

  /* Action purpose: After the panels, because a drop is resolved against a
  column's layout and the listener is handed the application. Returns NULL when
  the compositor advertises no wl_data_device_manager, which costs
  drag-and-drop and nothing else. */
  app.dnd = saber_dnd_create(app.display, &app_dnd_listener, &app);

  /* Action purpose: The socket is also the single-instance lock. ipc.c connects
  before it unlinks, so a live panel is detected rather than having its socket
  stolen; a stale file from an unclean exit is removed instead. */
  struct saber_ipc_handlers handlers = {
    .dash = ipc_dash,
    .spread = ipc_spread,
    .launch = ipc_launch,
    .overlay = ipc_overlay,
    .visibility = ipc_visibility,
    .sheet = ipc_sheet,
    .pin = ipc_pin,
    .reload = ipc_reload,
    .status = ipc_status,
    .quit = ipc_quit,
    .user = &app,
  };

  GError *ipc_error = NULL;

  app.ipc = saber_ipc_create(&handlers, &ipc_error);

  if (app.ipc == NULL) {
    if (g_error_matches(ipc_error, SABER_IPC_ERROR,
            SABER_IPC_ERROR_ALREADY_RUNNING)) {
      fprintf(stderr, "saber: %s\n", ipc_error->message);
      g_error_free(ipc_error);
      app_shutdown(&app);

      return EXIT_FAILURE;
    }

    /* Anything else costs saberctl and nothing else, so it is a warning. */
    g_warning("saber: %s", ipc_error->message);
    g_error_free(ipc_error);
  }

  guint sigint = g_unix_signal_add(SIGINT, on_signal, &app);
  guint sigterm = g_unix_signal_add(SIGTERM, on_signal, &app);
  guint sighup = g_unix_signal_add(SIGHUP, on_sighup, &app);

  saber_display_flush(app.display);
  g_main_loop_run(app.loop);

  g_source_remove(sigint);
  g_source_remove(sigterm);
  g_source_remove(sighup);
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
