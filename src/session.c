/* Script function and purpose: Suspend, reboot and shut down, run as the
invoking user through the base system's `operator` group (D-013). Saber installs
nothing setuid or setgid and writes no sudoers fragment; where the group is not
held, the action is reported unavailable and the panel hides it. */

#include <stdbool.h>
#include <string.h>

#include <errno.h>
#include <fcntl.h> /* O_CLOEXEC, for the exec-failure pipe in spawn() */
#include <grp.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/wait.h> /* WIFEXITED and friends, for the child-exit report */
#include <unistd.h>

#include <gio/gio.h>
#include <glib.h>

#include <saber/session.h>

/* Action purpose: Absolute paths, not a PATH lookup. /sbin/shutdown is setuid
root and group-executable by operator; /sbin/reboot is NOT setuid and must never
be substituted for it. acpiconf is unprivileged and gets there by writing
/dev/acpi, which operator may. */
#define SHUTDOWN_PATH "/sbin/shutdown"
#define ACPICONF_PATH "/usr/sbin/acpiconf"

static const char *const poweroff_argv[] = {
  SHUTDOWN_PATH, "-p", "now", NULL,
};

static const char *const reboot_argv[] = {
  SHUTDOWN_PATH, "-r", "now", NULL,
};

static const char *const suspend_argv[] = {
  ACPICONF_PATH, "-s3", NULL,
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

static const char *const *
builtin_argv(enum saber_session_action action)
{
  switch (action) {
    case SABER_SESSION_SUSPEND:
      return suspend_argv;
    case SABER_SESSION_REBOOT:
      return reboot_argv;
    case SABER_SESSION_POWEROFF:
      return poweroff_argv;
    default:
      return NULL;
  }
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

bool
saber_session_in_operator_group(void)
{
  static int cached = -1;

  if (cached >= 0) {
    return cached == 1;
  }

  cached = 0;

  struct group *operator_group = getgrnam("operator");

  if (operator_group == NULL) {
    return false;
  }

  gid_t groups[NGROUPS_MAX];
  int count = getgroups(NGROUPS_MAX, groups);

  if (count < 0) {
    return false;
  }

  for (int i = 0; i < count; i++) {
    if (groups[i] == operator_group->gr_gid) {
      cached = 1;
      return true;
    }
  }

  /* Action purpose: getgroups(2) need not include the effective gid, so it is
  tested separately rather than assumed present in the list. */
  if (getegid() == operator_group->gr_gid) {
    cached = 1;
  }

  return cached == 1;
}

bool
saber_session_available(const struct saber_config *config,
    enum saber_session_action action)
{
  const char *override = action_override(config, action);
  bool has_override = override != NULL && override[0] != '\0';

  switch (action) {
    /* Action purpose: No built-in exists for either -- the compositor has no
    lock verb and FreeBSD has no logind (BLUEPRINT 4.2) -- so an empty or absent
    string means the entry is hidden outright, not defaulted. */
    case SABER_SESSION_LOCK:
    case SABER_SESSION_LOGOUT:
      return has_override;

    case SABER_SESSION_SUSPEND:
    case SABER_SESSION_REBOOT:
    case SABER_SESSION_POWEROFF:
      /* A configured override is site policy -- a sudo wrapper, a different
      suspend state -- and is trusted without the group test. */
      return has_override || saber_session_in_operator_group();

    default:
      return false;
  }
}

/* Function purpose: Report a session command that failed, and free the pid.

The status used to be discarded, which made every failure silent: spawn()
returns as soon as fork() succeeds, so a missing or non-executable binary became
_exit(127) in the child and nothing anywhere. A menu entry that appears to do
nothing is the worst possible outcome for suspend or shut down, because the user
cannot tell it from the command having worked and the machine having declined.

Every status reaching here is the command's own. A failed execv never gets this
far -- spawn() detects it through its error pipe and reports it as an error to
the caller -- so 127 is treated as the ordinary exit status it is. Reading it as
"could not execute" was wrong for any command that genuinely chose it, which for
a configured override is an entirely reasonable thing to do.

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

/* Function purpose: Start a detached child. setsid() puts the command in its own
session so that a shutdown already in flight is not taken down with the panel it
was invoked from.

A close-on-exec pipe carries the one thing the exit status cannot say. The child
writes its errno if execv fails and nothing at all if it succeeds, because a
successful exec closes the descriptor for it -- so a read of zero bytes means
the command is running and a read of an int means it never started. Without it
the child's _exit(127) was the only signal available, and 127 is a status a real
command may choose for itself; a `session { }` override exiting 127 was reported
as an execution failure it had nothing to do with.

The read is bounded whatever happens: the child reaches either execv or the
write within microseconds of the fork, and both ends of the pipe are closed on
every path out of here. */
static bool
spawn(char *const *argv, const char *action, GError **error)
{
  int fds[2];

  if (pipe2(fds, O_CLOEXEC) < 0) {
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
    is the right behaviour here -- unlike the built-ins, whose whole point is
    that they are two specific binaries. */
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

  const char *const *builtin = builtin_argv(action);

  if (builtin == NULL) {
    g_set_error(error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_SUPPORTED,
        "%s has no built-in command",
        saber_session_action_id(action));
    return false;
  }

  /* Action purpose: The override path resolves through PATH and so discovers a
  missing command before forking; the built-ins are absolute paths and had no
  equivalent check, so a base system without them produced a fork, an execv
  failure and nothing else. Checked here so the failure is an error the caller
  can show rather than a warning after the fact. */
  if (!g_file_test(builtin[0], G_FILE_TEST_IS_EXECUTABLE)) {
    g_set_error(error,
        G_IO_ERROR,
        G_IO_ERROR_NOT_FOUND,
        "%s: %s is missing or not executable",
        saber_session_action_id(action),
        builtin[0]);
    return false;
  }

  return spawn((char *const *)builtin, saber_session_action_id(action), error);
}
