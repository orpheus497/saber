# Script function and purpose: The build's dialect-neutral data -- object
# lists, protocol lists and the XML paths behind them. Nothing here is a rule
# and nothing here is conditional.
#
# Saber ships two build drivers: Makefile for bmake, which is what FreeBSD's
# base `make` is, and GNUmakefile for GNU make, which is what `make` is on
# Linux. GNU make looks for GNUmakefile before Makefile and bmake does not look
# for GNUmakefile at all, so each `make` finds its own dialect with no
# detection logic, no configure step and nothing generated. The cost of that is
# two drivers that could drift, and this file is the answer to it: the lists
# live once, and only the dialect-specific logic -- conditionals, shell
# assignment, pkg-config, rules, install -- is written twice.
#
# So the constraint on this file is absolute: every line must parse identically
# under both. That means plain `VAR = a b c` assignments and `#` comments, and
# none of `.if`, `!=`, `:tu`, `${.CURDIR}`, `ifeq`, `$(shell ...)` or a rule.
# Adding any of those here moves a decision into a file that cannot express it.

# Action purpose: Both session backends are listed, and that is deliberate
# rather than an oversight. Each guards its own body on the platform it
# implements -- the same `#if defined(__FreeBSD__)` pattern src/appinfo.c
# already uses for closefrom(2) -- so the one that does not apply compiles to
# an empty translation unit and contributes an empty object to the link. The
# alternative, selecting one in each driver, would put a platform test in both
# drivers and would also make a whole-tree `cc -fsyntax-only src/*.c` sweep
# stop covering the backend that is not selected.
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
	luaguard.o \
	main.o \
	match.o \
	model.o \
	panel.o \
	quicklist.o \
	render.o \
	session-freebsd.o \
	session-linux.o \
	sheets.o \
	sni.o \
	spread.o \
	surface.o \
	theme.o \
	toplevel.o \
	trash.o \
	unity.o

# saberctl(1) is a second binary, not a mode of the first, and links nothing
# but libc. See the note in either driver.
CTL_OBJS = saberctl.o

# Action purpose: Saber is a CLIENT, so every protocol is generated with
# `client-header` and `private-code`. private-code emits the marshalling glue,
# so each protocol contributes an object and not just a header.
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

# Action purpose: Where each protocol's XML comes from, so the two drivers do
# not each carry their own copy of these paths. The four PROTO_DIR_* values are
# relative to wayland-protocols' pkgdatadir, which only a driver can ask
# pkg-config for; the two wlr files are vendored under protocol/ because
# wlr-protocols is packaged nowhere, and hikari-sakura and sofi vendor the same
# two for the same reason.
PROTO_XML_xdg-shell = stable/xdg-shell/xdg-shell.xml
PROTO_XML_xdg-activation-v1 = staging/xdg-activation/xdg-activation-v1.xml
PROTO_XML_viewporter = stable/viewporter/viewporter.xml
PROTO_XML_fractional-scale-v1 = staging/fractional-scale/fractional-scale-v1.xml

PROTO_VENDORED = \
	wlr-layer-shell-unstable-v1 \
	wlr-foreign-toplevel-management-unstable-v1 \
	virtual-keyboard-unstable-v1 \
	wlr-virtual-pointer-unstable-v1

# vinput(1) is a test harness, not part of the product, so it is absent from
# `all` and gets its own protocol pair: `saber` links neither, and adding them
# to PROTOCOL_OBJS would put two protocols into the panel binary that the
# panel never speaks.
VINPUT_PROTOCOL_HEADERS = \
	virtual-keyboard-unstable-v1-protocol.h \
	wlr-virtual-pointer-unstable-v1-protocol.h

VINPUT_PROTOCOL_OBJS = \
	virtual-keyboard-unstable-v1-protocol.o \
	wlr-virtual-pointer-unstable-v1-protocol.o

# Action purpose: The contrast and easing regression test. It drives
# saber_theme_init directly rather than going through saber.lua, so it needs
# neither a configuration parser nor a compositor -- which is what lets it run
# on any host, including one that cannot link the panel itself.
#
# luaguard.o is here because theme.o reads hikari.lua and so builds its
# interpreter through it. It links nothing the test did not already link: the
# test has always needed LUA_LIBS for theme.o's own Lua calls, and luaguard.c
# uses Lua and libc and no glib at all.
TEST_OBJS = \
	theme.o \
	anim.o \
	luaguard.o

# Action purpose: The configuration-budget regression test is a second binary
# rather than more cases in the contrast one, because it shares nothing with
# it: it drives the luaguard interface directly and links neither cairo nor
# gio, so it needs only this one object and Lua. Keeping them separate also
# keeps a slow test slow on its own -- every case in it deliberately runs a
# chunk until a ceiling stops it.
BUDGET_OBJS = \
	luaguard.o
