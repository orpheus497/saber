/* Script function and purpose: The panel's control socket --
$XDG_RUNTIME_DIR/saber.sock, one request line per connection, response then
close. Deliberately modelled on hikari's own control socket so that a user who
has learned one has learned both.

It exists because hikari.conf binds keys to `exec` strings, so a keybinding has
no way to reach an already-running panel except by launching something that
talks to it; saberctl(1) is that something (D-008). Every verb resolves to a
member of a callback table the panel fills in, so this module holds no panel
state and can be driven standalone. */

#if !defined(SABER_IPC_H)
#define SABER_IPC_H

#include <stdbool.h>

#include <gio/gio.h>

/* The wire contract, shared with saberctl(1). Both caps are hikari's numbers:
a longer request is refused rather than buffered, and a ninth client is closed
rather than queued. */
#define SABER_IPC_SOCKET_NAME "saber.sock"
#define SABER_IPC_MAX_REQUEST 512
#define SABER_IPC_MAX_RESPONSE 4096
#define SABER_IPC_MAX_CLIENTS 8

/* Action purpose: Panel-side outcomes only. Everything a request can get wrong
on its own -- an unknown verb, a missing or malformed argument, an over-long
line -- is diagnosed before a handler is reached, so a handler never has to
report it and a NULL handler can mean "not built" rather than "broken". */
enum saber_ipc_result {
  SABER_IPC_RESULT_OK,
  SABER_IPC_RESULT_NOT_BUILT,
  SABER_IPC_RESULT_NOTHING_FOCUSED,
  SABER_IPC_RESULT_NO_FAVOURITE,
  SABER_IPC_RESULT_BUSY,
  SABER_IPC_RESULT_FAILED,
};

enum saber_ipc_visibility {
  SABER_IPC_VISIBILITY_SHOW,
  SABER_IPC_VISIBILITY_HIDE,
  SABER_IPC_VISIBILITY_TOGGLE,
};

struct saber_ipc_report;

/* Function purpose: Append one line of `status` output. The trailing newline
and the terminating END belong to ipc.c; a handler writes only the substance.
Overflow is remembered and answered with `error response too long` rather than
truncating a reply the caller has no way to know was short. */
void
saber_ipc_report_line(struct saber_ipc_report *report, const char *fmt, ...)
    G_GNUC_PRINTF(2, 3);

/* Action purpose: A NULL member is answered with `error feature not built`,
which is how a WITH_DASH=NO build reports the dash verb without ipc.c ever
knowing what a dash is. */
struct saber_ipc_handlers {
  enum saber_ipc_result (*dash)(void *user);
  /* `app_id` is NULL when the spread is unfiltered. */
  enum saber_ipc_result (*spread)(const char *app_id, void *user);
  enum saber_ipc_result (*launch)(int favourite, void *user);
  enum saber_ipc_result (*overlay)(bool on, void *user);
  enum saber_ipc_result (*visibility)(enum saber_ipc_visibility action,
      void *user);
  enum saber_ipc_result (*sheet)(int sheet, void *user);
  enum saber_ipc_result (*pin)(int sheet, void *user);
  enum saber_ipc_result (*reload)(void *user);
  enum saber_ipc_result (*status)(struct saber_ipc_report *report, void *user);
  enum saber_ipc_result (*quit)(void *user);

  void *user;
};

#define SABER_IPC_ERROR (saber_ipc_error_quark())

enum saber_ipc_error {
  SABER_IPC_ERROR_NO_RUNTIME_DIR,
  SABER_IPC_ERROR_ALREADY_RUNNING,
  SABER_IPC_ERROR_SOCKET,
};

GQuark
saber_ipc_error_quark(void);

struct saber_ipc;

/* Function purpose: Bind $XDG_RUNTIME_DIR/saber.sock and serve it from the
GLib main loop. Fails with SABER_IPC_ERROR_ALREADY_RUNNING when another panel
answers on that path, which the caller must treat as fatal: two panels sharing
one socket would answer each other's keybindings. */
struct saber_ipc *
saber_ipc_create(const struct saber_ipc_handlers *handlers, GError **error);

/* An explicit path, for tests and for a second panel on a second seat. */
struct saber_ipc *
saber_ipc_create_at(const char *path,
    const struct saber_ipc_handlers *handlers,
    GError **error);

void
saber_ipc_destroy(struct saber_ipc *ipc);

const char *
saber_ipc_path(const struct saber_ipc *ipc);

#endif
