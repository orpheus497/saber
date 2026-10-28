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

/* Action purpose: the distributor's file is `settings.lua` and the user's is
`saber.lua`, so neither name has to be read twice to know which one you are
looking at. They used to share the name `saber.lua` and differ only by which
root they sat under, which meant a path had to be read to its last component
before it said whose file it was -- and sofi had the same problem spelled the
other way round, where `sofi.lua` was the SYSTEM file and `config.lua` the
user's. One rule now covers all three components of the desktop: the file named
after the program is yours, `settings.lua` is the one that shipped.

SABER_LEGACY_SYSTEM_CONFIG is the old path, still read when the new one is
absent, because an install that predates this rename is a working install. */
#define SABER_SYSTEM_CONFIG SABER_ETC_PREFIX "/etc/saber/settings.lua"
#define SABER_LEGACY_SYSTEM_CONFIG SABER_ETC_PREFIX "/etc/saber/saber.lua"

/* Action purpose: The compositor's configuration is read for two blocks, so
that retheming hikari retints the panel with it and the desktop moves at one
speed rather than at two unrelated ones (BLUEPRINT.md 6.1): `ui.palette` for the
sixteen colours and `ui.animation` for the duration. Nothing else in that file
is Saber's business, and neither block is ever written back. */
#define SABER_HIKARI_SYSTEM_CONFIG SABER_ETC_PREFIX "/etc/hikari/hikari.lua"

#endif
