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

#define SABER_SYSTEM_CONFIG SABER_ETC_PREFIX "/etc/saber/saber.conf"

/* Action purpose: The compositor's configuration is read for its `ui { palette }`
block, so that retheming hikari retints the panel with it (BLUEPRINT.md 6.1).
Only the palette is read; nothing else in that file is Saber's business. */
#define SABER_HIKARI_SYSTEM_CONFIG SABER_ETC_PREFIX "/etc/hikari/hikari.conf"

#endif
