/* Script function and purpose: The session actions, and the test for whether
this user can actually perform them. Saber ships nothing setuid, setgid or
sudoers-shaped: the whole privilege model is membership of FreeBSD's existing
`operator` group (D-013). */

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

Lock and logout have no built-in: the compositor exposes no lock verb and
FreeBSD has no logind, so they exist only when session { lock, logout } names a
command. The three power actions fall back to the operator-group commands and
are available exactly when this process is in that group. */
bool
saber_session_available(const struct saber_config *config,
    enum saber_session_action action);

/* Function purpose: Run the action, via fork/exec and never system(). Returns
whether the child was started; the machine going down is its own confirmation.
Refuses anything saber_session_available() rejects. */
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
process. */
bool
saber_session_in_operator_group(void);

#endif
