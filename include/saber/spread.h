/* Script function and purpose: The window spread -- the full-screen window
switcher `saberctl spread [app_id]` toggles and a tile click routes to. Same
surface model as the Dash: OVERLAY layer, EXCLUSIVE keyboard, translucent
theme.overlay backdrop.

Icon and title, never a thumbnail. hikari advertises an OUTPUT image-capture
source and no foreign-toplevel one, and wlr-screencopy has no per-window
request, so no Wayland client can obtain a window's contents here. The grid is
an icon-and-title grid by design (BLUEPRINT.md 4.1). */

#if !defined(SABER_SPREAD_H)
#define SABER_SPREAD_H

#include <stdbool.h>

#include <saber/appinfo.h>
#include <saber/config.h>
#include <saber/display.h>
#include <saber/match.h>
#include <saber/model.h>
#include <saber/render.h>
#include <saber/theme.h>
#include <saber/toplevel.h>

/* All borrowed and all must outlive the spread. `toplevels` NULL leaves the
spread with nothing to show, which is inert rather than fatal. */
struct saber_spread_deps {
  struct saber_display *display;
  const struct saber_config *config;
  const struct saber_theme *theme;
  struct saber_icons *icons;
  struct saber_appinfo_index *index;
  struct saber_match *match;
  struct saber_model *model;
  struct saber_toplevels *toplevels;
};

struct saber_spread;

struct saber_spread *
saber_spread_create(const struct saber_spread_deps *deps);

void
saber_spread_destroy(struct saber_spread *spread);

/* Function purpose: Map the spread on `output`, NULL letting the compositor
choose. `app_id` NULL shows every window; otherwise only the windows whose
app_id matches it, case-insensitively. */
bool
saber_spread_show(struct saber_spread *spread,
    const char *app_id,
    struct saber_output *output);

/* Unmaps and DESTROYS the surface -- see saber_dash_hide for why. */
void
saber_spread_hide(struct saber_spread *spread);

/* Function purpose: Toggle. A spread already up under a DIFFERENT filter is
re-filtered rather than dismissed, so a click on a second tile moves the spread
to that application instead of closing it. Returns whether it is visible
afterwards. */
bool
saber_spread_toggle(struct saber_spread *spread,
    const char *app_id,
    struct saber_output *output);

bool
saber_spread_is_visible(const struct saber_spread *spread);

/* Function purpose: Rebuild the window list under an open spread. The window
set changes while the spread is up -- a window closes, or one appears on another
sheet -- and a grid that lied about it would activate a dead handle. */
void
saber_spread_refresh(struct saber_spread *spread);

#endif
