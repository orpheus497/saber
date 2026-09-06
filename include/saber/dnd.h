/* Script function and purpose: The wl_data_device DESTINATION side -- accepting
a file dragged out of a file manager and onto the panel. This module owns the
protocol half only: the offer lifetime, the text/uri-list negotiation and the
pipe the data comes down. It never launches anything and never decides what a
tile means; it hands the parsed URIs and the drop position to its owner, which
does. */

#if !defined(SABER_DND_H)
#define SABER_DND_H

#include <stdbool.h>
#include <stdint.h>

#include <wayland-client.h>

#include <saber/display.h>

/* The only type Saber accepts. RFC 2483: CRLF-separated, `#` lines are
comments. */
#define SABER_DND_MIME_URI_LIST "text/uri-list"

struct saber_dnd;

/* Coordinates are surface-local logical pixels, the same domain
saber_pointer_listener uses, so a hit test written for the pointer works here
unchanged. */
struct saber_dnd_listener {
  /* Function purpose: Answer whether a drop at this point would land on
  something that can take files. The answer becomes the accept/reject feedback
  the source client shows under the cursor, so it must be cheap: it is asked
  again on every motion event. NULL means the panel takes nothing. */
  bool (*accepts)(void *data,
      struct wl_surface *surface,
      double x,
      double y);

  /* Function purpose: The drag moved, entered, or left. `surface` is NULL when
  the drag left the panel entirely, which is the cue to drop any highlight. */
  void (*motion)(void *data,
      struct wl_surface *surface,
      double x,
      double y);

  /* Function purpose: A drop completed and its data arrived. `uris` is a
  NULL-terminated array of URI strings, owned by this module and valid only for
  the duration of the call -- copy anything that outlives it. Fires only for a
  drop that was accepted and that carried at least one usable URI. */
  void (*drop)(void *data,
      struct wl_surface *surface,
      double x,
      double y,
      char **uris);
};

/* Function purpose: Start listening for drags onto this client's surfaces.
Returns NULL when the compositor advertises no wl_data_device_manager or the
seat has not been bound, which costs drag-and-drop and nothing else. */
struct saber_dnd *
saber_dnd_create(struct saber_display *display,
    const struct saber_dnd_listener *listener,
    void *data);

void
saber_dnd_destroy(struct saber_dnd *dnd);

/* Function purpose: Where the drag currently is, for highlighting the tile
under it. False when no drag is over the panel, in which case nothing is
written through the pointers. Any of them may be NULL. */
bool
saber_dnd_hover(const struct saber_dnd *dnd,
    struct wl_surface **surface,
    double *x,
    double *y);

#endif
