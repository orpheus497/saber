/* Script function and purpose: Compile-time identity and paths shared by every
translation unit. Kept deliberately small -- module-specific declarations live
in that module's own header, one per source file under src, as
ARCHITECTURE_MAPPING.md lays out. */

#if !defined(SABER_SABER_H)
#define SABER_SABER_H

#include "version.h"

/* Action purpose: SABER_PREFIX and SABER_ETC_PREFIX arrive from the Makefile as
compile-time absolute paths rather than being resolved from the environment at
run time, so a modified PATH or a relocated tree cannot substitute a different
configuration file. The fallbacks exist only so an editor or a language server
that has not been handed the Makefile's flags still parses this header. */
#if !defined(SABER_PREFIX)
#define SABER_PREFIX "/usr/local"
#endif

#if !defined(SABER_ETC_PREFIX)
#define SABER_ETC_PREFIX "/usr/local"
#endif

#define SABER_SYSTEM_CONFIG SABER_ETC_PREFIX "/etc/saber/saber.lua"

/* Action purpose: The compositor's configuration is read for two blocks, so
that retheming hikari retints the panel with it and the desktop moves at one
speed rather than at two unrelated ones (BLUEPRINT.md 6.1): `ui.palette` for the
sixteen colours and `ui.animation` for the duration. Nothing else in that file
is Saber's business, and neither block is ever written back. */
#define SABER_HIKARI_SYSTEM_CONFIG SABER_ETC_PREFIX "/etc/hikari/hikari.lua"

#endif
