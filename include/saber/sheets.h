/* Script function and purpose: Client of hikari's control socket -- the only
surface through which sheets are readable or settable, since no Wayland
protocol expresses them. */

#if !defined(SABER_SHEETS_H)
#define SABER_SHEETS_H

#include <stdbool.h>

#define SABER_SHEET_COUNT 10

/* Action purpose: One value per documented reply of the socket's grammar, plus
the transport conditions the caller must tell apart. SABER_SHEETS_BUSY is the
compositor refusing while it is not in normal mode -- a lock screen or a modal
counts -- which is ordinary and transient, not a fault to report.
SABER_SHEETS_STALE_SOCKET is ECONNREFUSED: a socket file left behind by an
unclean exit, which no amount of retrying will fix. */
enum saber_sheets_status {
  SABER_SHEETS_OK,
  SABER_SHEETS_UNAVAILABLE,
  SABER_SHEETS_STALE_SOCKET,
  SABER_SHEETS_BUSY,
  SABER_SHEETS_UNKNOWN_COMMAND,
  SABER_SHEETS_BAD_SHEET_NUMBER,
  SABER_SHEETS_NO_ACTIVE_WORKSPACE,
  SABER_SHEETS_NO_FOCUSED_VIEW,
  SABER_SHEETS_VIEW_BUSY,
  SABER_SHEETS_RESPONSE_TOO_LONG,
  SABER_SHEETS_PROTOCOL_ERROR,
  SABER_SHEETS_IO_ERROR,
};

/* Action purpose: counts[0] is reported on the same footing as the rest, but
sheet 0's views are never hidden -- they stay visible beneath whichever sheet is
displayed. The asymmetry is the caller's to present. */
struct saber_sheets_state {
  int current;
  char *output;
  int counts[SABER_SHEET_COUNT];
};

struct saber_sheets;

typedef void (*saber_sheets_state_cb)(const struct saber_sheets_state *state,
    void *user);
typedef void (*saber_sheets_reply_cb)(enum saber_sheets_status status,
    void *user);

/* Function purpose: Bind to $XDG_RUNTIME_DIR/hikari.sock and start the idle
floor poll. Succeeds even where no such socket exists: on any other wlroots
compositor the sheet feature is simply inert, which is why nothing here ever
fails fatally. `cb` fires on every successful state refresh. */
struct saber_sheets *
saber_sheets_create(saber_sheets_state_cb cb, void *user);

void
saber_sheets_destroy(struct saber_sheets *sheets);

/* Function purpose: The edge trigger. The socket has no subscribe verb and no
change event, so the caller invokes this when the toplevel set changes -- a
sheet switch hides views, and hiding a view is published as foreign-toplevel's
minimized bit. Repeated calls coalesce into one `state` request. */
void
saber_sheets_invalidate(struct saber_sheets *sheets);

/* Function purpose: Whether the socket file is present, so the caller can hide
the sheet tile entirely rather than draw one that answers nothing. */
bool
saber_sheets_available(const struct saber_sheets *sheets);

/* Returns NULL until the first `state` reply has been parsed. */
const struct saber_sheets_state *
saber_sheets_get_state(const struct saber_sheets *sheets);

enum saber_sheets_status
saber_sheets_last_status(const struct saber_sheets *sheets);

const char *
saber_sheets_status_string(enum saber_sheets_status status);

/* Function purpose: Switch the current workspace to `sheet`, or move the
focused view to it. Both queue behind whatever is in flight and answer through
`cb`, which may be NULL; neither ever blocks the main loop. */
void
saber_sheets_switch(struct saber_sheets *sheets,
    int sheet,
    saber_sheets_reply_cb cb,
    void *user);

void
saber_sheets_pin(struct saber_sheets *sheets,
    int sheet,
    saber_sheets_reply_cb cb,
    void *user);

#endif
