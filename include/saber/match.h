/* Script function and purpose: Resolve a Wayland app_id to a desktop entry --
Saber's replacement for Unity's BAMF (BLUEPRINT.md 5.6).

Neither wlr-foreign-toplevel-management nor ext-foreign-toplevel-list carries a
pid, so no Wayland client has an authoritative process-to-window link. The
ladder below plus the launch window is the ceiling available; there is no
pid-based path to add later. */

#if !defined(SABER_MATCH_H)
#define SABER_MATCH_H

#include <stdbool.h>

#include <saber/appinfo.h>

struct saber_match;

struct saber_match *
saber_match_create(struct saber_appinfo_index *index);

void
saber_match_destroy(struct saber_match *match);

/* Function purpose: Resolve an app_id, first hit wins: exact desktop-file ID,
case-insensitive ID, StartupWMClass, Exec basename, reverse-DNS tail of the ID,
then a launch this process recorded within the launch window. Returns a
borrowed entry, or NULL when nothing matches. A launch-window hit is consumed:
resolving twice will not bind the same launch to two windows. */
struct saber_appinfo *
saber_match_resolve(struct saber_match *match, const char *app_id);

/* Function purpose: Record that Saber has just started this entry, so the next
otherwise-unmatched toplevel binds to it. Called by whoever launches, because
only the launcher knows which entry was asked for. */
void
saber_match_note_launch(struct saber_match *match, const char *desktop_id);

bool
saber_match_is_launching(const struct saber_match *match,
    const char *desktop_id);

#endif
