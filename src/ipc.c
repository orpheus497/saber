/* Script function and purpose: Server side of the panel's control socket. Line
based, one request per connection, response then close -- hikari's grammar,
reproduced verb for verb where the verbs coincide.

Nothing here knows what a dash or a sheet is. Every verb resolves to a member of
the caller's saber_ipc_handlers table, which is what lets the whole module be
driven from a test harness with no panel behind it. */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>

#include <gio/gio.h>
#include <glib.h>

#include <saber/ipc.h>

/* Room held back so a report that fills the buffer can still be closed off
with "END\n" and its terminator. */
#define REPORT_RESERVE 5

struct saber_ipc_report {
  char buf[SABER_IPC_MAX_RESPONSE];
  size_t len;
  bool overflow;
};

struct ipc_client {
  struct saber_ipc *ipc;
  GSocket *socket;
  GSource *source;
  size_t len;
  char buf[SABER_IPC_MAX_REQUEST];
};

struct saber_ipc {
  char *path;
  GSocket *listener;
  GSource *source;
  GList *clients;
  unsigned int nr_clients;
  struct saber_ipc_handlers handlers;
};

GQuark
saber_ipc_error_quark(void)
{
  return g_quark_from_static_string("saber-ipc-error-quark");
}

void
saber_ipc_report_line(struct saber_ipc_report *report, const char *fmt, ...)
{
  if (report == NULL || report->overflow) {
    return;
  }

  size_t room = sizeof(report->buf) - report->len - REPORT_RESERVE;
  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(report->buf + report->len, room, fmt, ap);
  va_end(ap);

  if (n < 0 || (size_t)n >= room) {
    report->overflow = true;
    return;
  }

  report->len += (size_t)n;
  report->buf[report->len++] = '\n';
  report->buf[report->len] = '\0';
}

static void
client_destroy(struct ipc_client *client)
{
  struct saber_ipc *ipc = client->ipc;

  if (client->source != NULL) {
    g_source_destroy(client->source);
    g_source_unref(client->source);
  }

  g_socket_close(client->socket, NULL);
  g_object_unref(client->socket);

  ipc->clients = g_list_remove(ipc->clients, client);
  ipc->nr_clients--;

  g_free(client);
}

/* Function purpose: Write a whole response to a non-blocking socket.

Responses are a few dozen bytes and the kernel buffer is orders of magnitude
larger, so a partial write here means the peer is pathological rather than
merely slow. Retrying on EINTR is worth it; spinning on EAGAIN is not, since
that would block the panel's main loop on a client. */
static void
write_all(int fd, const char *buf, size_t len)
{
  size_t off = 0;

  while (off < len) {
    ssize_t n = write(fd, buf + off, len - off);

    if (n > 0) {
      off += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) {
      continue;
    }
    return;
  }
}

static void
respond(struct ipc_client *client, const char *text)
{
  write_all(g_socket_get_fd(client->socket), text, strlen(text));
}

static const char *
result_message(enum saber_ipc_result result)
{
  switch (result) {
  case SABER_IPC_RESULT_OK:
    return "ok\n";
  case SABER_IPC_RESULT_NOT_BUILT:
    return "error feature not built\n";
  case SABER_IPC_RESULT_NOTHING_FOCUSED:
    return "error nothing focused\n";
  case SABER_IPC_RESULT_NO_FAVOURITE:
    return "error no such favourite\n";
  case SABER_IPC_RESULT_BUSY:
    return "error panel busy\n";
  case SABER_IPC_RESULT_FAILED:
    return "error command failed\n";
  }

  return "error command failed\n";
}

/* Returns false on anything that is not exactly one in-range decimal number,
so a garbled request becomes an error rather than sheet 0. */
static bool
parse_number(const char *arg, int min, int max, int *out)
{
  if (arg == NULL || arg[0] == '\0') {
    return false;
  }

  char *end = NULL;
  long value = strtol(arg, &end, 10);

  if (end == arg || *end != '\0') {
    return false;
  }
  if (value < min || value > max) {
    return false;
  }

  *out = (int)value;

  return true;
}

/* Function purpose: Answer a verb that takes no argument. Folds the three
checks every such verb needs -- stray argument, absent handler, handler result
-- into one place, so a verb added later cannot quietly skip one. */
static void
respond_nullary(struct ipc_client *client,
    const char *arg,
    enum saber_ipc_result (*handler)(void *),
    void *user)
{
  if (arg != NULL) {
    respond(client, "error command takes no arguments\n");
  } else if (handler == NULL) {
    respond(client, "error feature not built\n");
  } else {
    respond(client, result_message(handler(user)));
  }
}

static void
respond_sheet(struct ipc_client *client,
    const char *arg,
    enum saber_ipc_result (*handler)(int, void *),
    void *user)
{
  int sheet = 0;

  if (!parse_number(arg, 0, 9, &sheet)) {
    respond(client, "error sheet number must be 0-9\n");
  } else if (handler == NULL) {
    respond(client, "error feature not built\n");
  } else {
    respond(client, result_message(handler(sheet, user)));
  }
}

static void
respond_visibility(struct ipc_client *client,
    const char *arg,
    enum saber_ipc_visibility action)
{
  const struct saber_ipc_handlers *handlers = &client->ipc->handlers;

  if (arg != NULL) {
    respond(client, "error command takes no arguments\n");
  } else if (handlers->visibility == NULL) {
    respond(client, "error feature not built\n");
  } else {
    respond(client,
        result_message(handlers->visibility(action, handlers->user)));
  }
}

/* Function purpose: Frame a multi-line status reply. The handler supplies the
lines; the END sentinel and the length check are this module's, so a panel
cannot forget either. */
static void
respond_status(struct ipc_client *client, const char *arg)
{
  const struct saber_ipc_handlers *handlers = &client->ipc->handlers;

  if (arg != NULL) {
    respond(client, "error command takes no arguments\n");
    return;
  }
  if (handlers->status == NULL) {
    respond(client, "error feature not built\n");
    return;
  }

  struct saber_ipc_report report = { .len = 0, .overflow = false };

  report.buf[0] = '\0';

  enum saber_ipc_result result = handlers->status(&report, handlers->user);

  if (result != SABER_IPC_RESULT_OK) {
    respond(client, result_message(result));
    return;
  }
  if (report.overflow) {
    respond(client, "error response too long\n");
    return;
  }

  memcpy(report.buf + report.len, "END\n", 5);

  respond(client, report.buf);
}

static void
dispatch(struct ipc_client *client, char *line)
{
  const struct saber_ipc_handlers *handlers = &client->ipc->handlers;

  while (*line == ' ' || *line == '\t') {
    line++;
  }

  size_t len = strlen(line);

  while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
    line[--len] = '\0';
  }

  char *arg = strpbrk(line, " \t");

  if (arg != NULL) {
    *arg++ = '\0';
    while (*arg == ' ' || *arg == '\t') {
      arg++;
    }
    if (*arg == '\0') {
      arg = NULL;
    }
  }

  if (strcmp(line, "dash") == 0) {
    respond_nullary(client, arg, handlers->dash, handlers->user);
  } else if (strcmp(line, "spread") == 0) {
    /* Action purpose: The only optional argument in the grammar. A desktop
    file ID never contains whitespace, so a multi-word argument is a quoting
    mistake at the keybinding rather than an app_id, and is worth saying so
    instead of filtering on a string nothing will ever match. */
    if (arg != NULL && strpbrk(arg, " \t") != NULL) {
      respond(client, "error app id must be a single token\n");
    } else if (arg != NULL && !g_utf8_validate(arg, -1, NULL)) {
      /* Action purpose: Rejected rather than repaired. This reaches
      g_utf8_casefold, which requires valid input, and a desktop file ID that is
      not UTF-8 cannot match anything -- so silently folding it to replacement
      characters would answer "no such application" to what is really a mangled
      keybinding, and say nothing about why. */
      respond(client, "error app id must be valid UTF-8\n");
    } else if (handlers->spread == NULL) {
      respond(client, "error feature not built\n");
    } else {
      respond(client, result_message(handlers->spread(arg, handlers->user)));
    }
  } else if (strcmp(line, "launch") == 0) {
    int favourite = 0;

    if (!parse_number(arg, 1, 10, &favourite)) {
      respond(client, "error favourite number must be 1-10\n");
    } else if (handlers->launch == NULL) {
      respond(client, "error feature not built\n");
    } else {
      respond(client,
          result_message(handlers->launch(favourite, handlers->user)));
    }
  } else if (strcmp(line, "overlay") == 0) {
    bool on = arg != NULL && strcmp(arg, "on") == 0;

    if (arg == NULL || (!on && strcmp(arg, "off") != 0)) {
      respond(client, "error overlay argument must be on or off\n");
    } else if (handlers->overlay == NULL) {
      respond(client, "error feature not built\n");
    } else {
      respond(client, result_message(handlers->overlay(on, handlers->user)));
    }
  } else if (strcmp(line, "show") == 0) {
    respond_visibility(client, arg, SABER_IPC_VISIBILITY_SHOW);
  } else if (strcmp(line, "hide") == 0) {
    respond_visibility(client, arg, SABER_IPC_VISIBILITY_HIDE);
  } else if (strcmp(line, "toggle") == 0) {
    respond_visibility(client, arg, SABER_IPC_VISIBILITY_TOGGLE);
  } else if (strcmp(line, "sheet") == 0) {
    respond_sheet(client, arg, handlers->sheet, handlers->user);
  } else if (strcmp(line, "pin") == 0) {
    respond_sheet(client, arg, handlers->pin, handlers->user);
  } else if (strcmp(line, "reload") == 0) {
    respond_nullary(client, arg, handlers->reload, handlers->user);
  } else if (strcmp(line, "status") == 0) {
    respond_status(client, arg);
  } else if (strcmp(line, "quit") == 0) {
    respond_nullary(client, arg, handlers->quit, handlers->user);
  } else {
    respond(client, "error unknown command\n");
  }
}

static gboolean
client_readable(GSocket *socket, GIOCondition condition, gpointer data)
{
  struct ipc_client *client = data;

  /* Action purpose: Read before honouring a hangup, which is the opposite of
  the usual order. A client that writes its request and closes its write end --
  which is exactly what `printf ... | nc -U` does -- can present G_IO_HUP
  alongside data still queued for us, and taking the hangup first would drop a
  request that arrived intact. A genuine hangup arrives again as a zero-length
  read one line below. */
  if ((condition & (G_IO_IN | G_IO_PRI)) == 0) {
    client_destroy(client);
    return G_SOURCE_REMOVE;
  }

  ssize_t n = read(g_socket_get_fd(socket),
      client->buf + client->len,
      sizeof(client->buf) - client->len);

  if (n < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
      return G_SOURCE_CONTINUE;
    }
    client_destroy(client);
    return G_SOURCE_REMOVE;
  }
  if (n == 0) {
    client_destroy(client);
    return G_SOURCE_REMOVE;
  }

  client->len += (size_t)n;

  char *nl = memchr(client->buf, '\n', client->len);

  if (nl == NULL) {
    /* Action purpose: A full buffer with no newline is a request that will
    never terminate. Refuse it instead of waiting forever or growing. */
    if (client->len == sizeof(client->buf)) {
      respond(client, "error request too long\n");
      client_destroy(client);
      return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
  }

  *nl = '\0';
  /* Tolerate CRLF from a hand-typed session. */
  if (nl > client->buf && nl[-1] == '\r') {
    nl[-1] = '\0';
  }

  dispatch(client, client->buf);
  client_destroy(client);

  return G_SOURCE_REMOVE;
}

static gboolean
listener_readable(GSocket *socket, GIOCondition condition, gpointer data)
{
  struct saber_ipc *ipc = data;

  if ((condition & G_IO_IN) == 0) {
    return G_SOURCE_CONTINUE;
  }

  GSocket *peer = g_socket_accept(socket, NULL, NULL);

  if (peer == NULL) {
    return G_SOURCE_CONTINUE;
  }

  /* Action purpose: Refuse rather than queue once the budget is spent. A
  keybinding opens one connection and closes it; anything holding eight open at
  once is not a keybinding. */
  if (ipc->nr_clients >= SABER_IPC_MAX_CLIENTS) {
    g_socket_close(peer, NULL);
    g_object_unref(peer);
    return G_SOURCE_CONTINUE;
  }

  /* Action purpose: Close-on-exec, set explicitly because accept(2) does not
  inherit it from the listening socket. The panel forks to launch applications,
  and every one of them would otherwise inherit whatever control-socket
  connections happened to be open. */
  if (fcntl(g_socket_get_fd(peer), F_SETFD, FD_CLOEXEC) < 0) {
    g_socket_close(peer, NULL);
    g_object_unref(peer);
    return G_SOURCE_CONTINUE;
  }

  g_socket_set_blocking(peer, FALSE);

  struct ipc_client *client = g_new0(struct ipc_client, 1);

  client->ipc = ipc;
  client->socket = peer;

  ipc->clients = g_list_prepend(ipc->clients, client);
  ipc->nr_clients++;

  client->source =
      g_socket_create_source(peer, G_IO_IN | G_IO_HUP | G_IO_ERR, NULL);
  g_source_set_callback(
      client->source, G_SOURCE_FUNC(client_readable), client, NULL);
  g_source_attach(client->source, g_main_context_get_thread_default());

  return G_SOURCE_CONTINUE;
}

/* Function purpose: Decide whether the socket file at `path` may be replaced,
and remove it when it may.

Connecting is the only honest test. Unlinking unconditionally -- which is what
hikari does -- would let a second panel steal the socket from the first and
leave both answering half the keybindings; refusing on the mere presence of the
file would make an unclean exit permanently unrecoverable. So a successful
connect means a panel is already running and Saber must not start, ECONNREFUSED
means the file is a corpse and may be removed, and anything else is left
strictly alone. */
static bool
claim_path(const char *path, const struct sockaddr_un *addr, GError **error)
{
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

  if (fd < 0) {
    g_set_error(error,
        SABER_IPC_ERROR,
        SABER_IPC_ERROR_SOCKET,
        "could not create a probe socket: %s",
        g_strerror(errno));
    return false;
  }

  int rc = connect(fd, (const struct sockaddr *)addr, sizeof(*addr));
  int connect_errno = errno;

  close(fd);

  if (rc == 0) {
    g_set_error(error,
        SABER_IPC_ERROR,
        SABER_IPC_ERROR_ALREADY_RUNNING,
        "another panel is already listening on %s",
        path);
    return false;
  }

  if (connect_errno == ENOENT) {
    return true;
  }

  if (connect_errno == ECONNREFUSED) {
    if (unlink(path) < 0 && errno != ENOENT) {
      g_set_error(error,
          SABER_IPC_ERROR,
          SABER_IPC_ERROR_SOCKET,
          "could not remove the stale socket at %s: %s",
          path,
          g_strerror(errno));
      return false;
    }
    return true;
  }

  g_set_error(error,
      SABER_IPC_ERROR,
      SABER_IPC_ERROR_SOCKET,
      "%s exists and is not a socket Saber may replace: %s",
      path,
      g_strerror(connect_errno));

  return false;
}

struct saber_ipc *
saber_ipc_create_at(const char *path,
    const struct saber_ipc_handlers *handlers,
    GError **error)
{
  g_return_val_if_fail(path != NULL, NULL);
  g_return_val_if_fail(handlers != NULL, NULL);

  struct sockaddr_un addr = { 0 };
  size_t len = strlen(path);

  if (len >= sizeof(addr.sun_path)) {
    g_set_error(error,
        SABER_IPC_ERROR,
        SABER_IPC_ERROR_SOCKET,
        "socket path is too long for a unix socket: %s",
        path);
    return NULL;
  }

  addr.sun_family = AF_UNIX;
  memcpy(addr.sun_path, path, len + 1);

  if (!claim_path(path, &addr, error)) {
    return NULL;
  }

  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);

  if (fd < 0) {
    g_set_error(error,
        SABER_IPC_ERROR,
        SABER_IPC_ERROR_SOCKET,
        "could not create the control socket: %s",
        g_strerror(errno));
    return NULL;
  }

  /* Action purpose: Restrict the socket before it is reachable, then state the
  mode outright. XDG_RUNTIME_DIR is already 0700, so this is belt and braces --
  but this socket can quit the panel, and it costs two calls. */
  mode_t old_umask = umask(0077);
  int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));

  umask(old_umask);

  if (rc < 0) {
    g_set_error(error,
        SABER_IPC_ERROR,
        SABER_IPC_ERROR_SOCKET,
        "could not bind the control socket at %s: %s",
        path,
        g_strerror(errno));
    close(fd);
    return NULL;
  }

  if (chmod(path, S_IRUSR | S_IWUSR) < 0 ||
      listen(fd, SABER_IPC_MAX_CLIENTS) < 0) {
    g_set_error(error,
        SABER_IPC_ERROR,
        SABER_IPC_ERROR_SOCKET,
        "could not listen on the control socket at %s: %s",
        path,
        g_strerror(errno));
    close(fd);
    unlink(path);
    return NULL;
  }

  GSocket *listener = g_socket_new_from_fd(fd, error);

  if (listener == NULL) {
    close(fd);
    unlink(path);
    return NULL;
  }

  g_socket_set_blocking(listener, FALSE);

  struct saber_ipc *ipc = g_new0(struct saber_ipc, 1);

  ipc->path = g_strdup(path);
  ipc->listener = listener;
  ipc->handlers = *handlers;

  ipc->source = g_socket_create_source(listener, G_IO_IN, NULL);
  g_source_set_callback(
      ipc->source, G_SOURCE_FUNC(listener_readable), ipc, NULL);
  g_source_attach(ipc->source, g_main_context_get_thread_default());

  return ipc;
}

struct saber_ipc *
saber_ipc_create(const struct saber_ipc_handlers *handlers, GError **error)
{
  const char *runtime_dir = g_getenv("XDG_RUNTIME_DIR");

  if (runtime_dir == NULL || runtime_dir[0] == '\0') {
    g_set_error(error,
        SABER_IPC_ERROR,
        SABER_IPC_ERROR_NO_RUNTIME_DIR,
        "XDG_RUNTIME_DIR is unset; there is nowhere to put the control "
        "socket, so keybindings cannot reach the panel");
    return NULL;
  }

  char *path = g_build_filename(runtime_dir, SABER_IPC_SOCKET_NAME, NULL);
  struct saber_ipc *ipc = saber_ipc_create_at(path, handlers, error);

  g_free(path);

  return ipc;
}

void
saber_ipc_destroy(struct saber_ipc *ipc)
{
  if (ipc == NULL) {
    return;
  }

  if (ipc->source != NULL) {
    g_source_destroy(ipc->source);
    g_source_unref(ipc->source);
  }

  /* Action purpose: Drop clients before the listener, re-reading the head each
  time because client_destroy() unlinks the node it is handed. */
  while (ipc->clients != NULL) {
    client_destroy(ipc->clients->data);
  }

  if (ipc->listener != NULL) {
    g_socket_close(ipc->listener, NULL);
    g_object_unref(ipc->listener);
  }

  if (ipc->path != NULL) {
    unlink(ipc->path);
    g_free(ipc->path);
  }

  g_free(ipc);
}

const char *
saber_ipc_path(const struct saber_ipc *ipc)
{
  return ipc != NULL ? ipc->path : NULL;
}
