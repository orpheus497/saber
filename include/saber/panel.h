/* Script function and purpose: The assembly. One panel per output, each owning
a layer surface, its buffer pool, its own tween clock and the layout that maps
a pointer position back to a model item and a sub-region.

The panel set is a single object rather than a bare list because the display
layer takes exactly one pointer listener and one output listener: routing an
event to the right column is this module's job, not the caller's. */

#if !defined(SABER_PANEL_H)
#define SABER_PANEL_H

#include <stdbool.h>

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
};

struct saber_panels;

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

#endif
