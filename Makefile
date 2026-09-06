# [COMMENT] Script function and purpose: Build Saber, the vertical panel of the
# Hikari Sakura desktop. FreeBSD only, BSD make only (DECISIONS_LOG D-007 --
# this deliberately mirrors hikari-sakura's Makefile idiom rather than sofi's
# inherited meson, so one contributor moves between the compositor and the
# panel without changing build tooling).

# [COMMENT] Action purpose: Every WITH_* switch below is tested with
# `defined(X) && ${X:tu} != "NO"` rather than the shorter `.ifdef X`, because
# `.ifdef` tests whether a variable is DEFINED, not what it is set to -- so
# `make WITH_TRAY=NO` would still compile the tray in, and there would be no
# way to turn any feature off from the command line at all. `:tu` upper-cases
# the value first so `no`, `No` and `NO` all work. Command-line variables
# cannot be removed with `.undef` (bmake keeps them in the cmdline scope and a
# subsequent `.ifdef` still matches), which is why the check is made at each
# site rather than normalised once here. Inherited verbatim from
# hikari-sakura's Makefile, where the shorter form was a real defect.
WITH_ALL = YES

.if defined(WITH_ALL) && ${WITH_ALL:tu} != "NO"
WITH_TRAY = YES
WITH_LAUNCHER_ENTRY = YES
WITH_SHEETS = YES
WITH_DEVICES = YES
WITH_DASH = YES
WITH_SPREAD = YES
.endif

OS != uname
VERSION ?= "CURRENT"
PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
ETC_PREFIX ?= ${PREFIX}

# [COMMENT] Action purpose: Saber is FreeBSD-only and says so at the top of the
# build rather than failing later with a confusing link error. The platform
# interfaces it relies on -- getfsstat(2) behind GUnixMountMonitor, kqueue
# behind GFileMonitor, the operator-group privilege model, sysctl -- have no
# portable equivalent, and pretending otherwise would only produce a binary
# that misbehaves. See BLUEPRINT.md section 9.
.if ${OS} != "FreeBSD"
.error Saber is FreeBSD-only. See BLUEPRINT.md section 0 and DECISIONS_LOG D-012.
.endif

# [COMMENT] Action purpose: OBJS holds only what exists. It grows one phase at a
# time as PLANS.md schedules each module -- Phase 3 adds config/theme/display/
# surface/render/anim, Phase 4 the window model, and so on. Listing modules
# before they are written would break the build for anyone who checks out an
# intermediate commit, which is the whole reason the phases are separate.
OBJS = \
	anim.o \
	appinfo.o \
	buffer.o \
	config.o \
	dash.o \
	dbusmenu.o \
	devices.o \
	display.o \
	dnd.o \
	ipc.o \
	main.o \
	match.o \
	model.o \
	panel.o \
	quicklist.o \
	render.o \
	session.o \
	sheets.o \
	sni.o \
	spread.o \
	surface.o \
	theme.o \
	toplevel.o \
	trash.o \
	unity.o

# [COMMENT] Action purpose: saberctl(1) is a second binary, not a mode of the
# first, and links NOTHING but libc. It runs on every bound keypress (D-008),
# so linking wayland, cairo, pango, gdk-pixbuf and librsvg to print one line
# would pay for the whole desktop's shared libraries per keystroke. It needs
# the glib INCLUDE paths, because ipc.h declares the server API in terms of
# GError -- but not one glib library, which is why it gets its own CFLAGS and
# no LIBS at all. Verified: `saber` needs 17 shared libraries, `saberctl` needs
# exactly libc.so.7.
CTL_OBJS = saberctl.o

WAYLAND_PROTOCOLS != ${PKG_CONFIG} --variable pkgdatadir wayland-protocols

.PHONY: all clean distclean dist install uninstall groupcheck features FORCE
.PATH: src

# Allow specification of /extra/ CFLAGS and LDFLAGS
CFLAGS += ${CFLAGS_EXTRA}
LDFLAGS += ${LDFLAGS_EXTRA}

.if defined(DEBUG) && ${DEBUG:tu} != "NO"
# [COMMENT] Action purpose: Debug build -- full symbols, no optimisation,
# strict warnings, assertions live. Unlike the compositor, Saber is an ordinary
# Wayland client with no DRM backend and no direct dmabuf mapping, so ASan has
# nothing here to false-positive on and is safe to enable freely.
CFLAGS += -g -Werror -Wno-unused-function -Wno-unused-variable -O0
.if defined(ASAN) && ${ASAN:tu} != "NO"
CFLAGS += -fsanitize=address
LDFLAGS += -fsanitize=address
.endif
.else
CFLAGS += -DNDEBUG -O2
.endif

.if defined(WITH_TRAY) && ${WITH_TRAY:tu} != "NO"
CFLAGS += -DHAVE_TRAY=1
.endif

.if defined(WITH_LAUNCHER_ENTRY) && ${WITH_LAUNCHER_ENTRY:tu} != "NO"
CFLAGS += -DHAVE_LAUNCHER_ENTRY=1
.endif

.if defined(WITH_SHEETS) && ${WITH_SHEETS:tu} != "NO"
CFLAGS += -DHAVE_SHEETS=1
.endif

.if defined(WITH_DEVICES) && ${WITH_DEVICES:tu} != "NO"
CFLAGS += -DHAVE_DEVICES=1
.endif

.if defined(WITH_DASH) && ${WITH_DASH:tu} != "NO"
CFLAGS += -DHAVE_DASH=1
.endif

.if defined(WITH_SPREAD) && ${WITH_SPREAD:tu} != "NO"
CFLAGS += -DHAVE_SPREAD=1
.endif

# [COMMENT] Action purpose: WITH_VIRTUAL_INPUT is OPT-IN and deliberately
# excluded from WITH_ALL, which is why it is tested here rather than being set
# with the others above.
#
# It is the only route to a working Lock entry under D-003: the compositor's
# `lock` is a keybinding-only action with no CLI and no control-socket verb, so
# the sole pure-client path is to read whichever binding maps to `lock` out of
# hikari.conf and synthesise that keysym through
# zwp_virtual_keyboard_manager_v1, which the compositor does advertise. It is
# off by default because it breaks SILENTLY when the user rebinds the key, and
# a lock button that does nothing is worse than no lock button at all.
#
# NOTE for whoever implements this: virtual-keyboard-unstable-v1.xml is a
# wlr-protocols file and is NOT shipped by wayland-protocols -- verified on
# this host. It must be vendored into protocol/ alongside the two wlr XMLs
# already there before this switch can build anything.
.if defined(WITH_VIRTUAL_INPUT) && ${WITH_VIRTUAL_INPUT:tu} != "NO"
CFLAGS += -DHAVE_VIRTUAL_INPUT=1
.endif

# [COMMENT] Action purpose: SABER_PREFIX and SABER_ETC_PREFIX resolve the
# shipped default configuration through compile-time absolute paths, so a
# modified PATH or a relocated tree cannot substitute a different file. `-I.`
# is load-bearing: wayland-scanner writes the generated *-protocol.h into the
# repository root, not into include/.
CFLAGS += -Wall -I. -Iinclude
CFLAGS += -DSABER_PREFIX='"${PREFIX}"' -DSABER_ETC_PREFIX='"${ETC_PREFIX}"'

WAYLAND_CFLAGS != ${PKG_CONFIG} --cflags wayland-client wayland-cursor
WAYLAND_LIBS != ${PKG_CONFIG} --libs wayland-client wayland-cursor

CAIRO_CFLAGS != ${PKG_CONFIG} --cflags cairo
CAIRO_LIBS != ${PKG_CONFIG} --libs cairo

PANGO_CFLAGS != ${PKG_CONFIG} --cflags pangocairo
PANGO_LIBS != ${PKG_CONFIG} --libs pangocairo

# [COMMENT] Action purpose: gio-2.0 carries glib and gobject transitively and is
# what supplies GDBus, the main loop, the kqueue-backed GFileMonitor and the
# getfsstat-backed GUnixMountMonitor -- the four things that make a
# toolkit-free panel viable on FreeBSD without udev or /proc. See
# DECISIONS_LOG D-006.
# Action purpose: gio-unix-2.0 is a separate package, not part of gio-2.0's
# include path. gio/gunixmounts.h -- the getfsstat-backed mount monitor
# devices.c is built on -- lives only under gio-unix-2.0.
GIO_CFLAGS != ${PKG_CONFIG} --cflags gio-2.0 gio-unix-2.0
GIO_LIBS != ${PKG_CONFIG} --libs gio-2.0 gio-unix-2.0

PIXBUF_CFLAGS != ${PKG_CONFIG} --cflags gdk-pixbuf-2.0
PIXBUF_LIBS != ${PKG_CONFIG} --libs gdk-pixbuf-2.0

RSVG_CFLAGS != ${PKG_CONFIG} --cflags librsvg-2.0
RSVG_LIBS != ${PKG_CONFIG} --libs librsvg-2.0

UCL_CFLAGS != ${PKG_CONFIG} --cflags libucl
UCL_LIBS != ${PKG_CONFIG} --libs libucl

XKBCOMMON_CFLAGS != ${PKG_CONFIG} --cflags xkbcommon
XKBCOMMON_LIBS != ${PKG_CONFIG} --libs xkbcommon

CFLAGS += \
	${WAYLAND_CFLAGS} \
	${CAIRO_CFLAGS} \
	${PANGO_CFLAGS} \
	${GIO_CFLAGS} \
	${PIXBUF_CFLAGS} \
	${RSVG_CFLAGS} \
	${UCL_CFLAGS} \
	${XKBCOMMON_CFLAGS}

LIBS = \
	${WAYLAND_LIBS} \
	${CAIRO_LIBS} \
	${PANGO_LIBS} \
	${GIO_LIBS} \
	${PIXBUF_LIBS} \
	${RSVG_LIBS} \
	${UCL_LIBS} \
	${XKBCOMMON_LIBS} \
	-lm

# [COMMENT] Action purpose: Saber is a CLIENT, so every protocol is generated
# with `client-header` and `private-code` -- not the `server-header` the
# compositor's Makefile uses. private-code emits the marshalling glue, which
# means each protocol contributes a .o and not just a header.
#
# Two of the six are vendored under protocol/ because wlr-protocols is not
# packaged on FreeBSD; the other four come from wayland-protocols. Both
# hikari-sakura and sofi vendor the same way for the same reason.
PROTOCOL_HEADERS = \
	xdg-shell-protocol.h \
	xdg-activation-v1-protocol.h \
	viewporter-protocol.h \
	fractional-scale-v1-protocol.h \
	wlr-layer-shell-unstable-v1-protocol.h \
	wlr-foreign-toplevel-management-unstable-v1-protocol.h

PROTOCOL_OBJS = \
	xdg-shell-protocol.o \
	xdg-activation-v1-protocol.o \
	viewporter-protocol.o \
	fractional-scale-v1-protocol.o \
	wlr-layer-shell-unstable-v1-protocol.o \
	wlr-foreign-toplevel-management-unstable-v1-protocol.o

# Must sit after every `CFLAGS +=` above. `:N-pthread` drops the single flag in
# the pkg-config set that would otherwise drag libthr into saberctl.
CTL_CFLAGS = ${CFLAGS:N-pthread}

all: saber saberctl

# [COMMENT] Action purpose: Regenerate version.h on every build. The phony
# FORCE prerequisite keeps the target permanently out of date; the header is
# written to a temporary file and atomically renamed, so an interrupted build
# can never leave a partial or empty version.h behind.
version.h: FORCE
	@echo "#define SABER_VERSION \"${VERSION}\"" > version.h.tmp && mv version.h.tmp version.h

FORCE:

# [COMMENT] Action purpose: Every object depends on every generated protocol
# header. Stating it here rather than only on the link target is what makes the
# build correct under `-j`: prerequisites of a target are unordered, so without
# this an object could compile before wayland-scanner has written the header it
# includes.
${OBJS} ${PROTOCOL_OBJS}: ${PROTOCOL_HEADERS}
config.o main.o: version.h

saber: version.h ${PROTOCOL_HEADERS} ${PROTOCOL_OBJS} ${OBJS}
	${CC} ${LDFLAGS} ${CFLAGS} -o ${.TARGET} ${OBJS} ${PROTOCOL_OBJS} ${LIBS}

# [COMMENT] Action purpose: An explicit rule rather than the inferred .c.o one,
# because saberctl.o is the only object in the tree NOT compiled with ${CFLAGS}.
# It is deliberately absent from the ${OBJS} ${PROTOCOL_OBJS} dependency on
# ${PROTOCOL_HEADERS} as well: it includes no generated protocol header, and
# making it wait on wayland-scanner would tie the one binary that needs no
# Wayland to the one tool that provides it.
saberctl.o: src/saberctl.c version.h
	${CC} ${CTL_CFLAGS} -c ${.CURDIR}/src/saberctl.c -o ${.TARGET}

saberctl: version.h ${CTL_OBJS}
	${CC} ${LDFLAGS} ${CTL_CFLAGS} -o ${.TARGET} ${CTL_OBJS}

xdg-shell-protocol.h:
	wayland-scanner client-header ${WAYLAND_PROTOCOLS}/stable/xdg-shell/xdg-shell.xml ${.TARGET}
xdg-shell-protocol.c:
	wayland-scanner private-code ${WAYLAND_PROTOCOLS}/stable/xdg-shell/xdg-shell.xml ${.TARGET}

xdg-activation-v1-protocol.h:
	wayland-scanner client-header ${WAYLAND_PROTOCOLS}/staging/xdg-activation/xdg-activation-v1.xml ${.TARGET}
xdg-activation-v1-protocol.c:
	wayland-scanner private-code ${WAYLAND_PROTOCOLS}/staging/xdg-activation/xdg-activation-v1.xml ${.TARGET}

viewporter-protocol.h:
	wayland-scanner client-header ${WAYLAND_PROTOCOLS}/stable/viewporter/viewporter.xml ${.TARGET}
viewporter-protocol.c:
	wayland-scanner private-code ${WAYLAND_PROTOCOLS}/stable/viewporter/viewporter.xml ${.TARGET}

fractional-scale-v1-protocol.h:
	wayland-scanner client-header ${WAYLAND_PROTOCOLS}/staging/fractional-scale/fractional-scale-v1.xml ${.TARGET}
fractional-scale-v1-protocol.c:
	wayland-scanner private-code ${WAYLAND_PROTOCOLS}/staging/fractional-scale/fractional-scale-v1.xml ${.TARGET}

wlr-layer-shell-unstable-v1-protocol.h:
	wayland-scanner client-header protocol/wlr-layer-shell-unstable-v1.xml ${.TARGET}
wlr-layer-shell-unstable-v1-protocol.c:
	wayland-scanner private-code protocol/wlr-layer-shell-unstable-v1.xml ${.TARGET}

wlr-foreign-toplevel-management-unstable-v1-protocol.h:
	wayland-scanner client-header protocol/wlr-foreign-toplevel-management-unstable-v1.xml ${.TARGET}
wlr-foreign-toplevel-management-unstable-v1-protocol.c:
	wayland-scanner private-code protocol/wlr-foreign-toplevel-management-unstable-v1.xml ${.TARGET}

# [COMMENT] Action purpose: Report which optional features this binary was
# built with, without running the panel. `make features` answers the same
# question about a tree that has not been built yet, which is what makes
# `make WITH_ALL=NO` verifiable.
features:
	@echo "WITH_TRAY            = ${WITH_TRAY:U(off)}"
	@echo "WITH_LAUNCHER_ENTRY  = ${WITH_LAUNCHER_ENTRY:U(off)}"
	@echo "WITH_SHEETS          = ${WITH_SHEETS:U(off)}"
	@echo "WITH_DEVICES         = ${WITH_DEVICES:U(off)}"
	@echo "WITH_DASH            = ${WITH_DASH:U(off)}"
	@echo "WITH_SPREAD          = ${WITH_SPREAD:U(off)}"
	@echo "WITH_VIRTUAL_INPUT   = ${WITH_VIRTUAL_INPUT:U(off)}"

clean:
	@echo "cleaning generated protocol sources"
	@rm -f ${PROTOCOL_HEADERS} ${PROTOCOL_HEADERS:S/.h$/.c/}
	@echo "cleaning headers"
	@rm -f version.h
	@echo "cleaning object files"
	@rm -f ${OBJS} ${PROTOCOL_OBJS} ${CTL_OBJS}
	@echo "cleaning executables"
	@rm -f saber saberctl

distclean: clean
	@rm -f saber-${VERSION}.tar.gz

saber-${VERSION}.tar.gz: version.h
	@tar -s "#^#saber-${VERSION}/#" -czf saber-${VERSION}.tar.gz \
		version.h \
		src/*.c \
		include/saber/*.h \
		protocol/*.xml \
		etc/saber/saber.conf \
		Makefile \
		compile_flags.txt \
		.clang-format \
		LICENSE \
		README.md

# [COMMENT] Action purpose: `.ORDER` is what makes `dist` correct under `-j`.
# Prerequisites are unordered, so distclean may otherwise run concurrently with
# the archive and remove version.h between its regeneration and the tar.
.ORDER: distclean saber-${VERSION}.tar.gz

dist: distclean saber-${VERSION}.tar.gz

# [COMMENT] Action purpose: Saber installs NOTHING setuid or setgid and writes
# no sudoers fragment. Suspend, reboot and shut down work through FreeBSD's
# existing `operator` group, which already owns /sbin/shutdown (setuid root,
# group-executable) and /dev/acpi (group-writable). See DECISIONS_LOG D-013.
install: saber saberctl
	mkdir -p ${DESTDIR}${PREFIX}/bin
	mkdir -p ${DESTDIR}${ETC_PREFIX}/etc/saber
	mkdir -p ${DESTDIR}${PREFIX}/share/icons/hicolor/scalable/apps
	install -m 644 share/icons/saber.svg \
		${DESTDIR}${PREFIX}/share/icons/hicolor/scalable/apps
	install -m 555 saber ${DESTDIR}${PREFIX}/bin
	install -m 555 saberctl ${DESTDIR}${PREFIX}/bin
	install -m 644 etc/saber/saber.conf ${DESTDIR}${ETC_PREFIX}/etc/saber
	@${MAKE} -C${.CURDIR} groupcheck

# [COMMENT] Action purpose: Report operator-group membership; never change it.
# Adding a user to a privileged group is an administrator's decision, and a
# build that silently grants privileges is a build nobody should trust. Saber
# runs the same check at startup and HIDES the session actions it cannot
# perform, so declining this leaves a panel with no dead buttons rather than
# one with failing ones.
groupcheck:
	@if id -Gn 2>/dev/null | tr ' ' '\n' | grep -qx operator; then \
		echo "saber: `id -un` is in the operator group; suspend, reboot and shut down will work."; \
	else \
		echo "saber: `id -un` is NOT in the operator group."; \
		echo "       The session menu will hide suspend, reboot and shut down."; \
		echo "       To enable them, an administrator should run:"; \
		echo ""; \
		echo "           pw groupmod operator -m `id -un`"; \
		echo ""; \
		echo "       Saber deliberately does not run this for you."; \
	fi

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/saber
	rm -f ${DESTDIR}${PREFIX}/bin/saberctl
	rm -f ${DESTDIR}${ETC_PREFIX}/etc/saber/saber.conf
	rm -f ${DESTDIR}${PREFIX}/share/icons/hicolor/scalable/apps/saber.svg
