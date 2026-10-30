/* Script function and purpose: The session actions, and the test for whether
this user can actually perform them. Saber ships nothing setuid, setgid or
sudoers-shaped on either platform: on FreeBSD the whole privilege model is
membership of the base system's existing `operator` group (D-013), and on Linux
it is logind's own answer with polkit deciding (D-A02). One interface, one
backend compiled per platform -- src/session-freebsd.c or src/session-linux.c,
chosen by the build driver and not by a conditional in here. */

#if !defined(SABER_SESSION_H)
#define SABER_SESSION_H

#include <stdbool.h>

#include <glib.h>

#include <saber/config.h>

enum saber_session_action {
  SABER_SESSION_LOCK,
  SABER_SESSION_LOGOUT,
  SABER_SESSION_SUSPEND,
  SABER_SESSION_REBOOT,
  SABER_SESSION_POWEROFF,
  SABER_SESSION_ACTION_COUNT,
};

/* Function purpose: Whether the action can actually be carried out, so the
panel can HIDE what it cannot do rather than offer a button that fails.

The three power actions fall back to a built-in: on FreeBSD the operator-group
commands, available exactly when this process is in that group; on Linux
logind's Suspend, Reboot and PowerOff, available when CanSuspend, CanReboot or
CanPowerOff answers "yes" or "challenge".

Lock and logout differ by platform, and so does what an empty session { lock,
logout } means for them. FreeBSD has no logind and the compositor exposes no
lock verb, so there they exist only when a command is named. Linux has
Session.Lock and Manager.TerminateSession, so there an empty string means the
built-in, as it does for the three power actions. */
bool
saber_session_available(const struct saber_config *config,
    enum saber_session_action action);

/* Function purpose: Run the action. A session { } override is always a command,
run via fork/exec and never system(); a built-in is whatever the platform
itself offers -- fork/exec of /sbin/shutdown on FreeBSD, a logind method call on
Linux. Returns whether the command was started, not whether it finished; the
machine going down is its own confirmation. Refuses anything
saber_session_available() rejects. */
bool
saber_session_run(const struct saber_config *config,
    enum saber_session_action action,
    GError **error);

/* Stable identifier ("poweroff") and display label ("Shut Down"). */
const char *
saber_session_action_id(enum saber_session_action action);

const char *
saber_session_action_label(enum saber_session_action action);

/* Function purpose: The privilege probe itself -- getgroups(2) against
getgrnam("operator"). Exposed so `saber --build` and the panel report the same
answer. The result is cached: group membership cannot change under a running
process.

Linux has no group in its privilege path -- polkit decides there -- so the Linux
backend answers false and saber_session_available() is the probe that matters on
that platform. */
bool
saber_session_in_operator_group(void);

#endif
