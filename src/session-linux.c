/* Script function and purpose: Suspend, reboot, shut down, lock and log out on
Linux, asked of logind over the system bus. Saber installs nothing setuid or
setgid and writes no polkit rule: the privilege decision belongs to logind and
polkit, and their own Can* answers are what the panel hides an unavailable
action on -- the same discipline operator-group membership provides on FreeBSD
(D-013, D-A02). The interface is spoken, not linked, so systemd-logind and
elogind are both answered and neither one is a dependency. */

/* Action purpose: The build driver compiles exactly one session backend per
platform, so this guard is not what selects it. It is here so a FreeBSD build
reads this file as an empty translation unit rather than a screenful of errors,
which is what keeps a whole-tree syntax-only sweep over every source worth
running -- the same reason src/appinfo.c guards closefrom(2). */
#if defined(__linux__)

#include <stdbool.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h> /* FD_CLOEXEC, for the exec-failure pipe in spawn() */
#include <sys/types.h>
#include <sys/wait.h> /* WIFEXITED and friends, for the child-exit report */
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib.h>

#include <saber/session.h>

#define LOGIND_NAME "org.freedesktop.login1"
#define LOGIND_MANAGER_PATH "/org/freedesktop/login1"
#define LOGIND_MANAGER_INTERFACE "org.freedesktop.login1.Manager"
#define LOGIND_SESSION_INTERFACE "org.freedesktop.login1.Session"
#define DBUS_PROPERTIES_INTERFACE "org.freedesktop.DBus.Properties"

/* Action purpose: The capability probes are synchronous and run on the main
loop with a menu half-open in front of the user, so this timeout is how long
the WHOLE panel -- not just the menu -- stops responding for in the worst case.
logind answers Can* out of state it already holds, so 300ms is a vast margin
over any real reply and short enough that a wedged bus is a stutter rather than
a freeze. */
#define LOGIND_PROBE_TIMEOUT_MS 300

/* Action purpose: The action calls are asynchronous and may sit behind a polkit
prompt, which is paced by the user and not by the bus. G_MAXINT is GDBus's "do
not time out"; the 25-second default would cancel a prompt somebody is still
typing into, and a shut down cancelled out from under the user is exactly the
silent nothing this backend is written to avoid. */
#define LOGIND_ACTION_TIMEOUT_MS G_MAXINT

/* Action purpose: The one session in the process's life, held both ways because
logind addresses it both ways: Session.Lock is a method on the object and
Manager.TerminateSession takes the id. */
struct session_identity {
  char *id;
  char *path;
};

static const char *
action_override(const struct saber_config *config,
    enum saber_session_action action)
{
  if (config == NULL) {
    return NULL;
  }

  switch (action) {
    case SABER_SESSION_LOCK:
      return config->session.lock;
    case SABER_SESSION_LOGOUT:
      return config->session.logout;
    case SABER_SESSION_SUSPEND:
      return config->session.suspend;
    case SABER_SESSION_REBOOT:
      return config->session.reboot;
    case SABER_SESSION_POWEROFF:
      return config->session.poweroff;
    default:
      return NULL;
  }
}

/* Function purpose: The system bus, which is where logind is. Saber already
holds a session-bus connection for StatusNotifierItem; GDBus hands out one
shared connection per bus type, so this is the same singleton the rest of GIO
uses rather than a second connection of saber's own.

The failure is deliberately not cached: it costs one connect to a socket that
is not there, and a machine whose bus starts after the panel then works without
a restart. */
static GDBusConnection *
system_bus(void)
{
  static GDBusConnection *bus;

  if (bus != NULL) {
    return bus;
  }

  GError *error = NULL;

  bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);

  if (bus == NULL) {
    /* Not a warning. A machine with no system bus is a supported
    configuration -- session { } names the commands instead -- and the panel
    hides what it cannot do rather than complaining on every menu open. */
    g_debug("saber: no system bus for logind: %s",
        error != NULL ? error->message : "unknown error");
    g_clear_error(&error);
  }

  return bus;
}

/* Function purpose: Read one of logind's Can* answers.

The reply is a STRING, and its four values do not split two and two. "yes" and
"challenge" both mean the action is available: "challenge" is polkit saying it
will authenticate the user first, which is a working action and must be
offered, not hidden. "no" is a refusal this user cannot talk its way out of,
and "na" is the verb not existing on this hardware or configuration at all --
the one answer that is not about permission. Anything unrecognised is treated
as unavailable, because hiding an entry is the safe direction to be wrong in. */
static bool
capability_allows(const char *answer)
{
  if (answer == NULL) {
    return false;
  }

  return strcmp(answer, "yes") == 0 || strcmp(answer, "challenge") == 0;
}

/* Function purpose: One Can* probe, turned into the yes/no the panel asks for.
Not cached, unlike the FreeBSD group check it stands in for: an inhibitor or a
polkit rule can change the answer under a running panel, and a menu built from
a stale yes is a dead button. */
static bool
manager_capability(const char *method)
{
  GDBusConnection *bus = system_bus();

  if (bus == NULL) {
    return false;
  }

  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(bus,
      LOGIND_NAME,
      LOGIND_MANAGER_PATH,
      LOGIND_MANAGER_INTERFACE,
      method,
      NULL,
      G_VARIANT_TYPE("(s)"),
      G_DBUS_CALL_FLAGS_NONE,
      LOGIND_PROBE_TIMEOUT_MS,
      NULL,
      &error);

  if (reply == NULL) {
    /* No logind on the bus, or it would not answer: the action is unavailable
    and the panel hides it, exactly as a missing operator group does. */
    g_debug("saber: logind %s: %s", method,
        error != NULL ? error->message : "unknown error");
    g_clear_error(&error);

    return false;
  }

  const char *answer = NULL;

  g_variant_get(reply, "(&s)", &answer);

  bool allowed = capability_allows(answer);

  g_variant_unref(reply);

  return allowed;
}

/* Function purpose: Ask the manager for a session object path. GetSession and
GetSessionByPID differ only in the method and its argument and both answer a
single path, so they share this. `args` is consumed by the call. */
static char *
session_path(GDBusConnection *bus, const char *method, GVariant *args)
{
  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(bus,
      LOGIND_NAME,
      LOGIND_MANAGER_PATH,
      LOGIND_MANAGER_INTERFACE,
      method,
      args,
      G_VARIANT_TYPE("(o)"),
      G_DBUS_CALL_FLAGS_NONE,
      LOGIND_PROBE_TIMEOUT_MS,
      NULL,
      &error);

  if (reply == NULL) {
    g_debug("saber: logind %s: %s", method,
        error != NULL ? error->message : "unknown error");
    g_clear_error(&error);

    return NULL;
  }

  char *path = NULL;

  g_variant_get(reply, "(o)", &path);
  g_variant_unref(reply);

  return path;
}

/* Function purpose: Read the session's own id off the session object. Needed
only where XDG_SESSION_ID was not usable, because the pid lookup answers a path
and the one method addressed by id would otherwise have nothing to send. */
static char *
session_id(GDBusConnection *bus, const char *path)
{
  GError *error = NULL;
  GVariant *reply = g_dbus_connection_call_sync(bus,
      LOGIND_NAME,
      path,
      DBUS_PROPERTIES_INTERFACE,
      "Get",
      g_variant_new("(ss)", LOGIND_SESSION_INTERFACE, "Id"),
      G_VARIANT_TYPE("(v)"),
      G_DBUS_CALL_FLAGS_NONE,
      LOGIND_PROBE_TIMEOUT_MS,
      NULL,
      &error);

  if (reply == NULL) {
    g_debug("saber: logind session id: %s",
        error != NULL ? error->message : "unknown error");
    g_clear_error(&error);

    return NULL;
  }

  GVariant *value = NULL;

  g_variant_get(reply, "(v)", &value);

  char *id = NULL;

  if (value != NULL && g_variant_is_of_type(value, G_VARIANT_TYPE_STRING)) {
    id = g_variant_dup_string(value, NULL);
  }

  g_clear_pointer(&value, g_variant_unref);
  g_variant_unref(reply);

  return id;
}

/* Function purpose: Which logind session this process belongs to, worked out
once and kept -- a process does not change session, so the answer cannot go
stale the way a capability can. Where there is none, lock and logout have
nothing to act on and the panel hides them.

XDG_SESSION_ID is the cheap answer and is tested rather than trusted: it is
inherited by everything the session starts, including a process that outlives
the session it names, so a lookup that fails falls through to the pid instead of
handing a stale id to TerminateSession. */
static const struct session_identity *
current_session(void)
{
  static struct session_identity identity;
  static bool resolved;

  if (resolved) {
    return &identity;
  }

  GDBusConnection *bus = system_bus();

  if (bus == NULL) {
    /* Nothing was asked, so nothing is settled; a bus that appears later still
    gets its chance. */
    return &identity;
  }

  resolved = true;

  const char *env = g_getenv("XDG_SESSION_ID");

  if (env != NULL && env[0] != '\0') {
    char *path = session_path(bus, "GetSession", g_variant_new("(s)", env));

    if (path != NULL) {
      identity.id = g_strdup(env);
      identity.path = path;

      return &identity;
    }
  }

  identity.path = session_path(bus,
      "GetSessionByPID",
      g_variant_new("(u)", (guint32)getpid()));

  if (identity.path != NULL) {
    identity.id = session_id(bus, identity.path);
  }

  return &identity;
}

const char *
saber_session_action_id(enum saber_session_action action)
{
  switch (action) {
    case SABER_SESSION_LOCK:
      return "lock";
    case SABER_SESSION_LOGOUT:
      return "logout";
    case SABER_SESSION_SUSPEND:
      return "suspend";
    case SABER_SESSION_REBOOT:
      return "reboot";
    case SABER_SESSION_POWEROFF:
      return "poweroff";
    default:
      return "";
  }
}

const char *
saber_session_action_label(enum saber_session_action action)
{
  switch (action) {
    case SABER_SESSION_LOCK:
      return "Lock";
    case SABER_SESSION_LOGOUT:
      return "Log Out";
    case SABER_SESSION_SUSPEND:
      return "Suspend";
    case SABER_SESSION_REBOOT:
      return "Restart";
    case SABER_SESSION_POWEROFF:
      return "Shut Down";
    default:
      return "";
  }
}

/* Function purpose: Linux has no counterpart to answer about. The power verbs
go through logind and polkit decides, so there is no group whose membership
grants anything; an `operator` group does exist on some distributions and
grants none of this, which is why a real getgroups(2) answer would be a
misleading yes rather than a useful one. The symbol stays so the interface is
the same on both platforms, and saber_session_available() is what actually
gates the menu here. */
bool
saber_session_in_operator_group(void)
{
  return false;
}

bool
saber_session_available(const struct saber_config *config,
    enum saber_session_action action)
{
  const char *override = action_override(config, action);

  /* A configured override is site policy -- a locker of your choosing, a sudo
  wrapper, a different suspend state -- and is trusted without asking logind
  anything. It is also the whole answer on a Linux box with no logind at all. */
  if (override != NULL && override[0] != '\0') {
    return true;
  }

  switch (action) {
    /* Action purpose: An empty session { lock } or { logout } means "use the
    built-in" here, as it does for the three power verbs. It means "HIDE the
    entry" in the FreeBSD backend, and that difference is deliberate rather
    than an oversight: FreeBSD has no logind and no compositor lock verb, so
    there is genuinely nothing to run, whereas logind supplies both. What is
    left to ask is whether logind gave us a session to act on.

    Lock is as strong a test as this interface allows: logind's Lock is a signal
    to whichever locker holds the session, and if nothing is listening the
    screen does not lock and no error comes back. That is not discoverable from
    here, so session { lock } remains the way to name a locker outright. */
    case SABER_SESSION_LOCK:
      return current_session()->path != NULL;
    case SABER_SESSION_LOGOUT:
      return current_session()->id != NULL;

    case SABER_SESSION_SUSPEND:
      return manager_capability("CanSuspend");
    case SABER_SESSION_REBOOT:
      return manager_capability("CanReboot");
    case SABER_SESSION_POWEROFF:
      return manager_capability("CanPowerOff");

    default:
      return false;
  }
}

/* Function purpose: Report a logind call that failed, for the same reason the
child-exit watch reports a command that exited non-zero: a menu entry that
appears to do nothing is the worst possible outcome for suspend or shut down,
because the user cannot tell it from the command having worked and the machine
having declined. A polkit prompt the user dismissed arrives here as an error
too, which is the correct thing to say about it.

`data` carries the action's id, a static string from saber_session_action_id, so
there is nothing to free here. */
static void
on_logind_reply(GObject *source, GAsyncResult *result, gpointer data)
{
  const char *action = data != NULL ? data : "session command";
  GError *error = NULL;
  GVariant *reply =
      g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);

  if (reply == NULL) {
    g_warning("saber: %s: %s", action,
        error != NULL ? error->message : "logind declined");
    g_clear_error(&error);

    return;
  }

  g_variant_unref(reply);
}

/* Function purpose: The built-in for all five actions: one logind method call,
dispatched and not waited on. Asynchronous because the three power verbs may
stop behind a polkit password prompt, which is paced by the user; a synchronous
call would hold the main loop -- and so the panel, and so whatever is drawing
that prompt -- for as long as they take to type. */
static bool
logind_run(enum saber_session_action action, GError **error)
{
  GDBusConnection *bus = system_bus();

  if (bus == NULL) {
    g_set_error(error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_SUPPORTED,
        "%s: logind is not reachable on the system bus",
        saber_session_action_id(action));

    return false;
  }

  const char *path = LOGIND_MANAGER_PATH;
  const char *interface = LOGIND_MANAGER_INTERFACE;
  const char *method = NULL;
  GVariant *args = NULL;

  switch (action) {
    case SABER_SESSION_LOCK:
      if (current_session()->path == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
            "lock: logind has no session for this process");

        return false;
      }

      path = current_session()->path;
      interface = LOGIND_SESSION_INTERFACE;
      method = "Lock";
      break;

    case SABER_SESSION_LOGOUT:
      if (current_session()->id == NULL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
            "logout: logind has no session for this process");

        return false;
      }

      method = "TerminateSession";
      args = g_variant_new("(s)", current_session()->id);
      break;

    /* Action purpose: The interactive flag is TRUE because "challenge" is
    reported as available. polkit is thereby allowed to ask for authentication,
    which is the whole meaning of that answer; passing FALSE would have the
    call refused outright on precisely the machines where the probe said the
    action works. */
    case SABER_SESSION_SUSPEND:
      method = "Suspend";
      args = g_variant_new("(b)", TRUE);
      break;

    case SABER_SESSION_REBOOT:
      method = "Reboot";
      args = g_variant_new("(b)", TRUE);
      break;

    case SABER_SESSION_POWEROFF:
      method = "PowerOff";
      args = g_variant_new("(b)", TRUE);
      break;

    default:
      g_set_error(error,
          G_IO_ERROR,
          G_IO_ERROR_NOT_SUPPORTED,
          "%s has no built-in command",
          saber_session_action_id(action));

      return false;
  }

  /* The id is a static string, so it can be handed over without a copy and
  needs no destroy notify. */
  g_dbus_connection_call(bus,
      LOGIND_NAME,
      path,
      interface,
      method,
      args,
      NULL,
      G_DBUS_CALL_FLAGS_NONE,
      LOGIND_ACTION_TIMEOUT_MS,
      NULL,
      on_logind_reply,
      (gpointer)saber_session_action_id(action));

  return true;
}

/* Function purpose: Report a session command that failed, and free the pid. A
command that exits non-zero with nothing said about it is indistinguishable
from one that worked, which for a power action is the worst outcome available.

Every status reaching here is the command's own: a failed execv never gets this
far, because spawn() detects it through its error pipe and reports it to the
caller, so 127 is treated as the ordinary exit status it is.

`data` carries the action's id, a static string from saber_session_action_id, so
there is nothing to free here. */
static void
on_child_exit(GPid pid, gint status, gpointer data)
{
  const char *action = data != NULL ? data : "session command";

  if (WIFEXITED(status)) {
    int code = WEXITSTATUS(status);

    if (code != 0) {
      g_warning("saber: %s: exited with status %d", action, code);
    }
  } else if (WIFSIGNALED(status)) {
    g_warning("saber: %s: killed by signal %d", action, WTERMSIG(status));
  }

  g_spawn_close_pid(pid);
}

/* Function purpose: Start a detached child for a session { } override. setsid()
puts the command in its own session so that a shutdown already in flight is not
taken down with the panel it was invoked from.

A close-on-exec pipe carries the one thing the exit status cannot say. The child
writes its errno if execv fails and nothing at all if it succeeds, because a
successful exec closes the descriptor for it -- so a read of zero bytes means
the command is running and a read of an int means it never started. 127 is a
status a real command may choose for itself, so it cannot stand in for this.

The read is bounded whatever happens: the child reaches either execv or the
write within microseconds of the fork, and both ends of the pipe are closed on
every path out of here. */
static bool
spawn(char *const *argv, const char *action, GError **error)
{
  int fds[2];

  /* g_unix_open_pipe rather than pipe2: the flag spellings for the latter are
  not visible in every libc's default namespace. */
  if (!g_unix_open_pipe(fds, FD_CLOEXEC, NULL)) {
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "pipe failed");

    return false;
  }

  pid_t pid = fork();

  if (pid < 0) {
    close(fds[0]);
    close(fds[1]);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fork failed");

    return false;
  }

  if (pid == 0) {
    close(fds[0]);
    setsid();
    execv(argv[0], argv);

    /* The write is the whole point of the child's remaining life; there is
    nobody left to report a short one to, so its result is discarded. */
    int failure = errno;
    ssize_t written = write(fds[1], &failure, sizeof(failure));

    (void)written;
    _exit(127);
  }

  close(fds[1]);

  int failure = 0;
  ssize_t got;

  do {
    got = read(fds[0], &failure, sizeof(failure));
  } while (got < 0 && errno == EINTR);

  close(fds[0]);

  if (got == (ssize_t)sizeof(failure)) {
    /* The command never ran, so the child is reaped here rather than handed to
    a watch that would report an exit status it never chose. */
    int status;

    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
      /* retry */
    }

    g_set_error(error, G_IO_ERROR, g_io_error_from_errno(failure), "%s: %s",
        argv[0], g_strerror(failure));

    return false;
  }

  /* The id is a static string, so it can be handed over without a copy and
  needs no destroy notify. */
  g_child_watch_add((GPid)pid, on_child_exit, (gpointer)action);

  return true;
}

bool
saber_session_run(const struct saber_config *config,
    enum saber_session_action action,
    GError **error)
{
  if (!saber_session_available(config, action)) {
    g_set_error(error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_SUPPORTED,
        "%s is not available to this user",
        saber_session_action_id(action));

    return false;
  }

  const char *override = action_override(config, action);

  if (override != NULL && override[0] != '\0') {
    char **argv = NULL;

    if (!g_shell_parse_argv(override, NULL, &argv, error)) {
      return false;
    }

    /* Action purpose: An override may name a bare command, so PATH resolution
    is the right behaviour here -- unlike the built-ins, which are not commands
    at all on this platform. */
    bool ok = false;
    char *resolved = g_find_program_in_path(argv[0]);

    if (resolved == NULL) {
      g_set_error(error,
          G_IO_ERROR,
          G_IO_ERROR_NOT_FOUND,
          "%s: not found",
          argv[0]);
    } else {
      g_free(argv[0]);
      argv[0] = resolved;
      ok = spawn(argv, saber_session_action_id(action), error);
    }

    g_strfreev(argv);

    return ok;
  }

  return logind_run(action, error);
}

#endif /* __linux__ */
