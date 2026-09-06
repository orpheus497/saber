/* Script function and purpose: hikari control-socket client. One request per
connection -- connect, send a line, read to EOF, the compositor closes -- driven
entirely through GIO's async calls so nothing here ever blocks the GLib main
loop. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib.h>

#include <saber/sheets.h>

/* Action purpose: Invalidations arrive in bursts -- one sheet switch fires a
minimized change on every view it hides -- so they are gathered for this long
and answered with a single `state` request. The floor exists only for the case
where a sheet switch changes nothing visible and therefore triggers nothing. */
#define SHEETS_COALESCE_MS 40
#define SHEETS_FLOOR_MS 500

#define SHEETS_READ_CHUNK 512
#define SHEETS_RESPONSE_MAX 4096
#define SHEETS_QUEUE_MAX 32

struct sheets_request {
  struct saber_sheets *sheets;
  char *line;
  bool is_state;
  saber_sheets_reply_cb cb;
  void *user;

  GSocketConnection *conn;
  GString *response;
};

struct saber_sheets {
  int refcount;
  bool disposed;

  char *socket_path;
  GSocketClient *client;
  GCancellable *cancellable;

  GQueue *queue;
  struct sheets_request *active;

  bool dirty;
  bool state_pending;
  guint coalesce_source;
  guint floor_source;
  gint64 last_state_us;

  struct saber_sheets_state state;
  bool state_valid;
  enum saber_sheets_status status;

  saber_sheets_state_cb cb;
  void *user;
};

static void start_next(struct saber_sheets *sheets);
static void queue_state(struct saber_sheets *sheets);

static struct saber_sheets *
sheets_ref(struct saber_sheets *sheets)
{
  sheets->refcount++;
  return sheets;
}

static void
sheets_unref(struct saber_sheets *sheets)
{
  if (--sheets->refcount > 0) {
    return;
  }

  g_clear_object(&sheets->client);
  g_clear_object(&sheets->cancellable);
  g_queue_free(sheets->queue);
  g_free(sheets->socket_path);
  g_free(sheets->state.output);
  g_free(sheets);
}

static void
request_free(struct sheets_request *request)
{
  g_clear_object(&request->conn);

  if (request->response != NULL) {
    g_string_free(request->response, TRUE);
  }

  g_free(request->line);
  g_free(request);
}

const char *
saber_sheets_status_string(enum saber_sheets_status status)
{
  switch (status) {
    case SABER_SHEETS_OK:
      return "ok";
    case SABER_SHEETS_UNAVAILABLE:
      return "no control socket";
    case SABER_SHEETS_STALE_SOCKET:
      return "stale socket file, left by an unclean compositor exit";
    case SABER_SHEETS_BUSY:
      return "compositor busy";
    case SABER_SHEETS_UNKNOWN_COMMAND:
      return "unknown command";
    case SABER_SHEETS_BAD_SHEET_NUMBER:
      return "sheet number must be 0-9";
    case SABER_SHEETS_NO_ACTIVE_WORKSPACE:
      return "no active workspace";
    case SABER_SHEETS_NO_FOCUSED_VIEW:
      return "no focused view";
    case SABER_SHEETS_VIEW_BUSY:
      return "view busy";
    case SABER_SHEETS_RESPONSE_TOO_LONG:
      return "response too long";
    case SABER_SHEETS_PROTOCOL_ERROR:
      return "unrecognised response";
    case SABER_SHEETS_IO_ERROR:
      return "socket i/o failed";
  }

  return "unknown status";
}

/* Function purpose: Map one documented `error ...` line onto a status. Anything
else beginning with "error" is a compositor newer than this client, which is a
protocol mismatch rather than a transport failure. */
static enum saber_sheets_status
status_from_error_line(const char *line)
{
  if (strcmp(line, "error compositor busy") == 0) {
    return SABER_SHEETS_BUSY;
  }
  if (strcmp(line, "error unknown command") == 0) {
    return SABER_SHEETS_UNKNOWN_COMMAND;
  }
  if (strcmp(line, "error sheet number must be 0-9") == 0) {
    return SABER_SHEETS_BAD_SHEET_NUMBER;
  }
  if (strcmp(line, "error no active workspace") == 0) {
    return SABER_SHEETS_NO_ACTIVE_WORKSPACE;
  }
  if (strcmp(line, "error no focused view") == 0) {
    return SABER_SHEETS_NO_FOCUSED_VIEW;
  }
  if (strcmp(line, "error view busy") == 0) {
    return SABER_SHEETS_VIEW_BUSY;
  }
  if (strcmp(line, "error response too long") == 0) {
    return SABER_SHEETS_RESPONSE_TOO_LONG;
  }

  return SABER_SHEETS_PROTOCOL_ERROR;
}

static bool
parse_state(char **lines, struct saber_sheets_state *out)
{
  if (g_strv_length(lines) < 4) {
    return false;
  }

  int current = 0;
  if (sscanf(lines[0], "sheet %d", &current) != 1 || current < 0 ||
      current >= SABER_SHEET_COUNT) {
    return false;
  }

  if (!g_str_has_prefix(lines[1], "output ")) {
    return false;
  }

  int counts[SABER_SHEET_COUNT];
  if (!g_str_has_prefix(lines[2], "counts ")) {
    return false;
  }

  const char *cursor = lines[2] + strlen("counts ");
  for (int i = 0; i < SABER_SHEET_COUNT; i++) {
    char *end = NULL;
    long value = strtol(cursor, &end, 10);

    if (end == cursor || value < 0 || value > G_MAXINT) {
      return false;
    }

    counts[i] = (int)value;
    cursor = end;
  }

  if (strcmp(lines[3], "END") != 0) {
    return false;
  }

  g_free(out->output);
  out->output = g_strdup(lines[1] + strlen("output "));
  out->current = current;
  memcpy(out->counts, counts, sizeof(counts));

  return true;
}

static enum saber_sheets_status
parse_response(struct sheets_request *request)
{
  char **lines = g_strsplit(request->response->str, "\n", -1);
  enum saber_sheets_status status = SABER_SHEETS_PROTOCOL_ERROR;

  if (lines[0] == NULL) {
    status = SABER_SHEETS_PROTOCOL_ERROR;
  } else if (g_str_has_prefix(lines[0], "error")) {
    status = status_from_error_line(lines[0]);
  } else if (request->is_state) {
    if (parse_state(lines, &request->sheets->state)) {
      request->sheets->state_valid = true;
      status = SABER_SHEETS_OK;
    }
  } else if (strcmp(lines[0], "ok") == 0) {
    status = SABER_SHEETS_OK;
  }

  g_strfreev(lines);
  return status;
}

/* Function purpose: Single exit path for every request, successful or not. It
publishes the status, hands the reply to the caller, releases the connection and
starts whatever is behind it in the queue. */
static void
finish_request(struct sheets_request *request,
    enum saber_sheets_status status)
{
  struct saber_sheets *sheets = request->sheets;
  saber_sheets_reply_cb reply = request->cb;
  void *reply_user = request->user;
  bool was_state = request->is_state;

  if (request->conn != NULL) {
    g_io_stream_close(G_IO_STREAM(request->conn), NULL, NULL);
  }

  if (!sheets->disposed) {
    sheets->status = status;
    sheets->active = NULL;

    if (was_state) {
      sheets->state_pending = false;
      sheets->last_state_us = g_get_monotonic_time();
    }
  }

  request_free(request);

  if (!sheets->disposed) {
    if (reply != NULL) {
      reply(status, reply_user);
    }

    if (was_state && status == SABER_SHEETS_OK && sheets->cb != NULL) {
      sheets->cb(&sheets->state, sheets->user);
    }

    if (was_state && sheets->dirty) {
      sheets->dirty = false;
      queue_state(sheets);
    }

    /* Action purpose: A switch or a pin changes what the next `state` would
    report, and the toplevel churn it causes may be nothing at all when the
    sheets involved are empty. Refresh off our own command rather than wait. */
    if (!was_state && status == SABER_SHEETS_OK) {
      saber_sheets_invalidate(sheets);
    }

    start_next(sheets);
  }

  sheets_unref(sheets);
}

static void
on_read(GObject *source, GAsyncResult *result, gpointer data)
{
  struct sheets_request *request = data;
  GError *error = NULL;
  GBytes *bytes =
      g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);

  if (bytes == NULL) {
    bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    finish_request(request,
        cancelled ? SABER_SHEETS_UNAVAILABLE : SABER_SHEETS_IO_ERROR);
    return;
  }

  gsize size = g_bytes_get_size(bytes);

  /* Action purpose: The compositor closes the connection once it has answered,
  so a zero-length read is the end of the response and not an error. */
  if (size == 0) {
    g_bytes_unref(bytes);
    finish_request(request, parse_response(request));
    return;
  }

  gsize len = 0;
  const char *chunk = g_bytes_get_data(bytes, &len);
  g_string_append_len(request->response, chunk, (gssize)len);
  g_bytes_unref(bytes);

  if (request->response->len > SHEETS_RESPONSE_MAX) {
    finish_request(request, SABER_SHEETS_RESPONSE_TOO_LONG);
    return;
  }

  g_input_stream_read_bytes_async(G_INPUT_STREAM(source),
      SHEETS_READ_CHUNK,
      G_PRIORITY_DEFAULT,
      request->sheets->cancellable,
      on_read,
      request);
}

static void
on_written(GObject *source, GAsyncResult *result, gpointer data)
{
  struct sheets_request *request = data;
  GError *error = NULL;

  if (!g_output_stream_write_all_finish(
          G_OUTPUT_STREAM(source), result, NULL, &error)) {
    bool cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    finish_request(request,
        cancelled ? SABER_SHEETS_UNAVAILABLE : SABER_SHEETS_IO_ERROR);
    return;
  }

  GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(request->conn));
  g_input_stream_read_bytes_async(in,
      SHEETS_READ_CHUNK,
      G_PRIORITY_DEFAULT,
      request->sheets->cancellable,
      on_read,
      request);
}

static void
on_connect(GObject *source, GAsyncResult *result, gpointer data)
{
  struct sheets_request *request = data;
  GError *error = NULL;

  request->conn = g_socket_client_connect_finish(
      G_SOCKET_CLIENT(source), result, &error);

  if (request->conn == NULL) {
    enum saber_sheets_status status = SABER_SHEETS_IO_ERROR;

    /* Action purpose: ECONNREFUSED on a UNIX socket means the file is there but
    nothing is listening -- a stale node left by an unclean exit -- which is a
    different report from the socket simply not existing. */
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED)) {
      status = SABER_SHEETS_STALE_SOCKET;
    } else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      status = SABER_SHEETS_UNAVAILABLE;
    }

    g_clear_error(&error);
    finish_request(request, status);
    return;
  }

  GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(request->conn));
  g_output_stream_write_all_async(out,
      request->line,
      strlen(request->line),
      G_PRIORITY_DEFAULT,
      request->sheets->cancellable,
      on_written,
      request);
}

static void
start_next(struct saber_sheets *sheets)
{
  if (sheets->disposed || sheets->active != NULL) {
    return;
  }

  struct sheets_request *request = g_queue_pop_head(sheets->queue);
  if (request == NULL) {
    return;
  }

  sheets->active = request;
  request->response = g_string_new(NULL);

  GSocketAddress *address = g_unix_socket_address_new(sheets->socket_path);

  sheets_ref(sheets);
  g_socket_client_connect_async(sheets->client,
      G_SOCKET_CONNECTABLE(address),
      sheets->cancellable,
      on_connect,
      request);

  g_object_unref(address);
}

static bool
enqueue(struct saber_sheets *sheets,
    char *line,
    bool is_state,
    saber_sheets_reply_cb cb,
    void *user)
{
  if (sheets->disposed || sheets->socket_path == NULL) {
    g_free(line);
    if (cb != NULL) {
      cb(SABER_SHEETS_UNAVAILABLE, user);
    }
    return false;
  }

  if (g_queue_get_length(sheets->queue) >= SHEETS_QUEUE_MAX) {
    g_free(line);
    if (cb != NULL) {
      cb(SABER_SHEETS_IO_ERROR, user);
    }
    return false;
  }

  struct sheets_request *request = g_new0(struct sheets_request, 1);
  request->sheets = sheets;
  request->line = line;
  request->is_state = is_state;
  request->cb = cb;
  request->user = user;

  g_queue_push_tail(sheets->queue, request);
  start_next(sheets);

  return true;
}

static void
queue_state(struct saber_sheets *sheets)
{
  if (sheets->disposed || sheets->socket_path == NULL) {
    return;
  }

  if (sheets->state_pending) {
    sheets->dirty = true;
    return;
  }

  sheets->state_pending = true;

  if (!enqueue(sheets, g_strdup("state\n"), true, NULL, NULL)) {
    sheets->state_pending = false;
  }
}

static gboolean
on_coalesce(gpointer data)
{
  struct saber_sheets *sheets = data;

  sheets->coalesce_source = 0;
  sheets->dirty = false;
  queue_state(sheets);

  return G_SOURCE_REMOVE;
}

/* Function purpose: The floor. Sheet switches are normally learned from
foreign-toplevel churn; this covers the switch that changes nothing visible, and
does nothing at all while the socket is absent. */
static gboolean
on_floor(gpointer data)
{
  struct saber_sheets *sheets = data;

  if (sheets->state_pending ||
      !g_file_test(sheets->socket_path, G_FILE_TEST_EXISTS)) {
    return G_SOURCE_CONTINUE;
  }

  if (g_get_monotonic_time() - sheets->last_state_us <
      SHEETS_FLOOR_MS * G_TIME_SPAN_MILLISECOND) {
    return G_SOURCE_CONTINUE;
  }

  queue_state(sheets);
  return G_SOURCE_CONTINUE;
}

struct saber_sheets *
saber_sheets_create(saber_sheets_state_cb cb, void *user)
{
  struct saber_sheets *sheets = g_new0(struct saber_sheets, 1);

  sheets->refcount = 1;
  sheets->queue = g_queue_new();
  sheets->cb = cb;
  sheets->user = user;
  sheets->status = SABER_SHEETS_UNAVAILABLE;

  const char *runtime_dir = g_getenv("XDG_RUNTIME_DIR");
  if (runtime_dir == NULL || runtime_dir[0] == '\0') {
    return sheets;
  }

  sheets->socket_path = g_build_filename(runtime_dir, "hikari.sock", NULL);
  sheets->client = g_socket_client_new();
  g_socket_client_set_socket_type(sheets->client, G_SOCKET_TYPE_STREAM);
  g_socket_client_set_timeout(sheets->client, 2);
  sheets->cancellable = g_cancellable_new();

  sheets->floor_source = g_timeout_add(SHEETS_FLOOR_MS, on_floor, sheets);
  queue_state(sheets);

  return sheets;
}

void
saber_sheets_destroy(struct saber_sheets *sheets)
{
  if (sheets == NULL) {
    return;
  }

  sheets->disposed = true;

  if (sheets->cancellable != NULL) {
    g_cancellable_cancel(sheets->cancellable);
  }

  g_clear_handle_id(&sheets->coalesce_source, g_source_remove);
  g_clear_handle_id(&sheets->floor_source, g_source_remove);

  struct sheets_request *request = NULL;
  while ((request = g_queue_pop_head(sheets->queue)) != NULL) {
    request_free(request);
  }

  sheets_unref(sheets);
}

void
saber_sheets_invalidate(struct saber_sheets *sheets)
{
  if (sheets == NULL || sheets->disposed || sheets->socket_path == NULL) {
    return;
  }

  sheets->dirty = true;

  if (sheets->coalesce_source == 0) {
    sheets->coalesce_source =
        g_timeout_add(SHEETS_COALESCE_MS, on_coalesce, sheets);
  }
}

bool
saber_sheets_available(const struct saber_sheets *sheets)
{
  return sheets != NULL && sheets->socket_path != NULL &&
      g_file_test(sheets->socket_path, G_FILE_TEST_EXISTS);
}

const struct saber_sheets_state *
saber_sheets_get_state(const struct saber_sheets *sheets)
{
  if (sheets == NULL || !sheets->state_valid) {
    return NULL;
  }

  return &sheets->state;
}

enum saber_sheets_status
saber_sheets_last_status(const struct saber_sheets *sheets)
{
  return sheets == NULL ? SABER_SHEETS_UNAVAILABLE : sheets->status;
}

static void
send_sheet_command(struct saber_sheets *sheets,
    const char *verb,
    int sheet,
    saber_sheets_reply_cb cb,
    void *user)
{
  if (sheets == NULL) {
    if (cb != NULL) {
      cb(SABER_SHEETS_UNAVAILABLE, user);
    }
    return;
  }

  if (sheet < 0 || sheet >= SABER_SHEET_COUNT) {
    if (cb != NULL) {
      cb(SABER_SHEETS_BAD_SHEET_NUMBER, user);
    }
    return;
  }

  enqueue(sheets, g_strdup_printf("%s %d\n", verb, sheet), false, cb, user);
}

void
saber_sheets_switch(struct saber_sheets *sheets,
    int sheet,
    saber_sheets_reply_cb cb,
    void *user)
{
  send_sheet_command(sheets, "sheet", sheet, cb, user);
}

void
saber_sheets_pin(struct saber_sheets *sheets,
    int sheet,
    saber_sheets_reply_cb cb,
    void *user)
{
  send_sheet_command(sheets, "pin", sheet, cb, user);
}
