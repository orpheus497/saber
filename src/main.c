/* Script function and purpose: Entry point for saber(1), the vertical panel of
the Hikari Sakura desktop.

At Phase 2 this deliberately contains no panel. Its job is to prove the build:
that every dependency in BLUEPRINT.md section 7 resolves and links, that the
WITH_* switches actually reach the compiled binary, and that the generated
protocol headers are present. The panel itself arrives in Phase 3 -- see
.devdocs/PLANS.md.

The library version calls in report_build() are not decoration. They reference
runtime symbols rather than compile-time macros on purpose, so that a
successful `make` is evidence the shared libraries were genuinely found and
linked and not merely that their headers were on the include path. */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <grp.h>
#include <limits.h>
#include <sys/types.h>

#include <cairo.h>
#include <glib.h>

#include <saber/saber.h>

/* Function purpose: Print the feature set this binary was actually built with.
Exists so `make WITH_ALL=NO` is verifiable against the binary rather than only
against the Makefile, which is the difference between believing a switch works
and knowing it does. */
static void
report_build(void)
{
  printf("saber %s\n", SABER_VERSION);
  printf("prefix        %s\n", SABER_PREFIX);
  printf("configuration %s\n", SABER_SYSTEM_CONFIG);
  printf("\nfeatures:\n");

#ifdef HAVE_TRAY
  printf("  tray             yes\n");
#else
  printf("  tray             no\n");
#endif
#ifdef HAVE_LAUNCHER_ENTRY
  printf("  launcher-entry   yes\n");
#else
  printf("  launcher-entry   no\n");
#endif
#ifdef HAVE_SHEETS
  printf("  sheets           yes\n");
#else
  printf("  sheets           no\n");
#endif
#ifdef HAVE_DEVICES
  printf("  devices          yes\n");
#else
  printf("  devices          no\n");
#endif
#ifdef HAVE_DASH
  printf("  dash             yes\n");
#else
  printf("  dash             no\n");
#endif
#ifdef HAVE_SPREAD
  printf("  spread           yes\n");
#else
  printf("  spread           no\n");
#endif
#ifdef HAVE_VIRTUAL_INPUT
  printf("  virtual-input    yes\n");
#else
  printf("  virtual-input    no\n");
#endif

  printf("\nlinked against:\n");
  printf("  glib             %u.%u.%u\n",
      glib_major_version,
      glib_minor_version,
      glib_micro_version);
  printf("  cairo            %s\n", cairo_version_string());
}

/* Function purpose: Report whether this user can carry out the session actions,
using the same test the panel itself will use to decide whether to draw them.

The result is informational here and load-bearing later: Saber HIDES suspend,
reboot and shut down when the answer is no, rather than offering buttons that
fail. See DECISIONS_LOG D-013 for why membership of the base system's existing
`operator` group is the whole privilege model, and why Saber ships nothing
setuid, setgid or sudoers-shaped. */
static bool
in_operator_group(void)
{
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
      return true;
    }
  }

  return false;
}

static void
usage(FILE *stream, const char *argv0)
{
  fprintf(stream,
      "usage: %s [-h] [-v] [-b]\n"
      "\n"
      "  -h, --help      this message\n"
      "  -v, --version   version only\n"
      "  -b, --build     build configuration, features and linked libraries\n",
      argv0);
}

int
main(int argc, char **argv)
{
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      usage(stdout, argv[0]);
      return EXIT_SUCCESS;
    }

    if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
      printf("saber %s\n", SABER_VERSION);
      return EXIT_SUCCESS;
    }

    if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--build") == 0) {
      report_build();
      printf("  operator group   %s\n", in_operator_group() ? "yes" : "no");
      return EXIT_SUCCESS;
    }

    fprintf(stderr, "%s: unknown option: %s\n", argv[0], argv[i]);
    usage(stderr, argv[0]);
    return EXIT_FAILURE;
  }

  /* Action purpose: Fail loudly rather than exiting zero. A scaffolding binary
  that returns success would be indistinguishable from a working panel to an
  autostart file or a service supervisor, and the first symptom would be a
  desktop with no panel and nothing in the log. */
  fprintf(stderr,
      "saber: the panel is not implemented yet -- this is the Phase 2 build "
      "scaffold.\n"
      "saber: run `saber --build` to inspect the build, or see "
      ".devdocs/PLANS.md.\n");

  return EXIT_FAILURE;
}
