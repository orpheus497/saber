/* Script function and purpose: The assembly. One panel per output, each owning
a layer surface, its buffer pool, its own tween clock and the layout that maps
a pointer position back to a model item and a sub-region.

The panel set is a single object rather than a bare list because the display
layer takes exactly one pointer listener and one output listener: routing an
event to the right column is this module's job, not the caller's. */

#if !defined(SABER_PANEL_H)
#define SABER_PANEL_H

#include <stdbool.h>

#include <saber/appinfo.h>
#include <saber/config.h>
#include <saber/devices.h>
#include <saber/display.h>
#include <saber/model.h>
#include <saber/render.h>
#include <saber/sheets.h>
#include <saber/sni.h>
#include <saber/theme.h>
#include <saber/toplevel.h>
#include <saber/trash.h>

/* Everything a column draws from. All borrowed and all must outlive the panel
set; any of the optional modules may be NULL, in which case its tile simply is
not drawn rather than being drawn dead. */
struct saber_panel_deps {
  const struct saber_config *config;
  const struct saber_theme *theme;
  struct saber_display *display;
  struct saber_icons *icons;
  struct saber_model *model;
  struct saber_toplevels *toplevels;
  struct saber_sheets *sheets;
  struct saber_trash *trash;
  struct saber_devices *devices;
  struct saber_sni *sni;
  /* Also how a folder-opening tile finds a real file manager: the entry
  carrying the FileManager category, else the one registered against
  inode/directory, both filtered so a terminal can never answer. NULL leaves
  only $FILEMANAGER and xdg-open, and xdg-open applies no such filter. */
  struct saber_appinfo_index *index;
};

struct saber_panels;

/* Function purpose: The spread's toggle, injected rather than linked, because
the spread is behind WITH_SPREAD and a column must still work without it. The
signature is saber_spread_toggle's, so the application can pass one straight
through. `output` is the one whose tile was clicked; `app_id` NULL would mean
an unfiltered spread, which the panel never asks for. While this is unset a
tile with several windows cycles through them instead. */
typedef void (*saber_panel_spread_func)(const char *app_id,
    struct saber_output *output,
    void *user);

void
saber_panels_set_spread(struct saber_panels *panels,
    saber_panel_spread_func func,
    void *user);

/* Function purpose: What the BFB opens. Same injection shape as the spread and
for the same reason -- the Dash is a separate surface with its own module, and
routing through a hook keeps the column working under WITH_DASH=NO. The BFB is
inert while this is unset. */
typedef void (*saber_panel_dash_func)(struct saber_output *output, void *user);

void
saber_panels_set_dash(struct saber_panels *panels,
    saber_panel_dash_func func,
    void *user);

/* Function purpose: Create a column on every output the configuration selects,
and keep doing so as outputs come and go. Registers the display's output and
pointer listeners, so the caller must not also claim them. */
struct saber_panels *
saber_panels_create(const struct saber_panel_deps *deps);

void
saber_panels_destroy(struct saber_panels *panels);

/* Function purpose: The single repaint entry point. Everything that can change
what a column shows -- the model, a badge, a toplevel, the trash count, the
mount list, the tray, the sheet state -- funnels through here. */
void
saber_panels_refresh(struct saber_panels *panels);

unsigned int
saber_panels_count(const struct saber_panels *panels);

/* Function purpose: Dismiss any open quicklist or sheet grid. Exists so the
Dash and the spread can clear the column's popups before they raise their own
modal surface -- without it a quicklist stays mapped and keyboard-holding
underneath a full-output overlay, with no way to reach it. Safe to call when
nothing is open. */
void
saber_panels_close_menu(struct saber_panels *panels);

#endif
