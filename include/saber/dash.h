/* Script function and purpose: The Dash -- the application grid the BFB opens
and `saberctl dash` toggles. A zwlr_layer_shell_v1 surface on the OVERLAY layer
with EXCLUSIVE keyboard interactivity, carrying a search field at its top left,
a grid of desktop entries, and a strip of XDG category filters along its bottom
edge. The two filters intersect: a category narrows what is on screen and the
query narrows it further.

The surface covers the whole output -- that is what holds the seat's keyboard
and catches the click that dismisses -- but only a third of it is painted: the
dash is a panel docked against config->panel.edge, and the desktop beside it is
left untouched rather than dimmed.

That painted rectangle is a translucent theme.overlay fill, never a blur:
hikari does not advertise ext-background-effect and a Wayland client cannot read
the screen behind itself (BLUEPRINT.md 5.7). */

#if !defined(SABER_DASH_H)
#define SABER_DASH_H

#include <stdbool.h>

#include <saber/appinfo.h>
#include <saber/config.h>
#include <saber/display.h>
#include <saber/match.h>
#include <saber/model.h>
#include <saber/render.h>
#include <saber/theme.h>

/* All borrowed and all must outlive the dash. `model` may be NULL, in which
case a launch is recorded with match.c only and no tile throbs for it. */
struct saber_dash_deps {
  struct saber_display *display;
  const struct saber_config *config;
  const struct saber_theme *theme;
  struct saber_icons *icons;
  struct saber_appinfo_index *index;
  struct saber_match *match;
  struct saber_model *model;
};

struct saber_dash;

/* Function purpose: Build the dash. No surface is made here: the dash exists
from startup so a keybinding can reach it, but occupies no output and holds no
input until it is shown. */
struct saber_dash *
saber_dash_create(const struct saber_dash_deps *deps);

void
saber_dash_destroy(struct saber_dash *dash);

/* Function purpose: Map the dash on `output`, NULL letting the compositor
choose. Takes the display's pointer and keyboard listeners for as long as it is
up, and restores the previous pair on hide. */
bool
saber_dash_show(struct saber_dash *dash, struct saber_output *output);

/* Function purpose: Unmap and DESTROY the surface -- not hide it. A layer
surface with EXCLUSIVE interactivity holds the seat's keyboard for as long as it
exists, so anything short of destroying it leaves the session deaf. */
void
saber_dash_hide(struct saber_dash *dash);

/* Returns whether the dash is visible afterwards. */
bool
saber_dash_toggle(struct saber_dash *dash, struct saber_output *output);

bool
saber_dash_is_visible(const struct saber_dash *dash);

#endif
