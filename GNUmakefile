# Script function and purpose: Build Saber with GNU make, which is what `make`
# is on Linux. The bmake driver in Makefile builds the same tree on FreeBSD.
#
# Action purpose: Two drivers rather than one portable one, because there is no
# portable one to write. The conditionals, shell assignment and variable
# modifiers a build of this shape needs are not in POSIX make, and the two
# dialects spell every one of them differently. What makes two drivers safe
# instead of merely duplicated is that GNU make looks for GNUmakefile before
# Makefile while bmake does not look for GNUmakefile at all -- so each `make`
# finds its own dialect with no detection logic, no configure step and nothing
# generated -- and that every object, protocol and XML path lives once, in
# build/sources.mk, which both dialects parse identically. Only logic is
# written twice.
#
# The feature switches, targets and variables here are the same ones the bmake
# driver accepts, and `make features` answers identically under both.

include build/sources.mk

VERSION ?= CURRENT
PKG_CONFIG ?= pkg-config

# Action purpose: /usr, not the /usr/local the bmake driver defaults to. On
# FreeBSD /usr/local is where ports install and /usr is the base system; on
# Linux it is the other way round, and a panel installed outside the package
# manager's prefix is a panel the desktop's own .desktop scanning may not find.
# ETC_PREFIX stays independent of it so configuration lands in /etc rather than
# /usr/etc.
#
# Action purpose: ETC_PREFIX is EMPTY rather than "/", because every use of it
# already supplies the separator -- `${ETC_PREFIX}/etc/saber`. Setting it to a
# slash makes each of those a doubled one, which the filesystem accepts and
# `saber --build` then prints verbatim as `//etc/saber/settings.lua`, reading like
# a bug in the path handling rather than the cosmetic artefact it is.
PREFIX ?= /usr
ETC_PREFIX ?=

# Function purpose: Non-empty when a WITH_* switch is set to anything but "no"
# in any case. Both halves are load-bearing: an unset switch must read as off,
# and `WITH_TRAY=no` must turn the tray off as surely as `WITH_TRAY=NO` does.
# The bmake driver spells this `defined(X) && ${X:tu} != "NO"` and its comment
# records that the shorter `.ifdef` form was a real defect in hikari-sakura's
# Makefile, because .ifdef tests whether a variable is DEFINED and not what it
# is set to -- so `make WITH_TRAY=NO` compiled the tray in anyway. GNU make has
# the same trap: `ifdef` here is also a definedness test, and `$(call upper,)`
# of an unset switch is the empty string, which is not "NO".
upper = $(shell printf '%s' '$(1)' | tr '[:lower:]' '[:upper:]')
enabled = $(if $(strip $(1)),$(if $(filter NO,$(call upper,$(1))),,yes),)

WITH_ALL ?= YES

ifeq ($(call enabled,$(WITH_ALL)),yes)
WITH_TRAY ?= YES
WITH_LAUNCHER_ENTRY ?= YES
WITH_SHEETS ?= YES
WITH_DEVICES ?= YES
WITH_DASH ?= YES
WITH_SPREAD ?= YES
endif

WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable pkgdatadir wayland-protocols)

# Action purpose: Lua is spelled differently by every packager -- FreeBSD's
# lang/lua54 port installs lua-5.4.pc, Debian and Ubuntu install lua5.4.pc,
# and several distributions provide an unversioned lua.pc alias. Probing a list
# is the only thing that works everywhere; hardcoding any single name breaks on
# some host that has Lua installed and working.
LUA_CANDIDATES = lua5.4 lua-5.4 lua54 lua5.3 lua-5.3 lua53 lua
LUA_PKG := $(firstword $(foreach c,$(LUA_CANDIDATES),\
	$(shell $(PKG_CONFIG) --exists $(c) 2>/dev/null && echo $(c))))

ifeq ($(LUA_PKG),)
$(error no Lua pkg-config module found; tried: $(LUA_CANDIDATES))
endif

.PHONY: all clean distclean dist install uninstall groupcheck features test FORCE

vpath %.c src

CFLAGS += $(CFLAGS_EXTRA)
LDFLAGS += $(LDFLAGS_EXTRA)

ifeq ($(call enabled,$(DEBUG)),yes)
CFLAGS += -g -Werror -Wno-unused-function -Wno-unused-variable -O0
ifeq ($(call enabled,$(ASAN)),yes)
CFLAGS += -fsanitize=address
LDFLAGS += -fsanitize=address
endif
else
CFLAGS += -DNDEBUG -O2
endif

ifeq ($(call enabled,$(WITH_TRAY)),yes)
CFLAGS += -DHAVE_TRAY=1
endif
ifeq ($(call enabled,$(WITH_LAUNCHER_ENTRY)),yes)
CFLAGS += -DHAVE_LAUNCHER_ENTRY=1
endif
ifeq ($(call enabled,$(WITH_SHEETS)),yes)
CFLAGS += -DHAVE_SHEETS=1
endif
ifeq ($(call enabled,$(WITH_DEVICES)),yes)
CFLAGS += -DHAVE_DEVICES=1
endif
ifeq ($(call enabled,$(WITH_DASH)),yes)
CFLAGS += -DHAVE_DASH=1
endif
ifeq ($(call enabled,$(WITH_SPREAD)),yes)
CFLAGS += -DHAVE_SPREAD=1
endif
ifeq ($(call enabled,$(WITH_VIRTUAL_INPUT)),yes)
CFLAGS += -DHAVE_VIRTUAL_INPUT=1
endif

# `-I.` is load-bearing: wayland-scanner writes the generated *-protocol.h into
# the repository root, not into include/.
CFLAGS += -Wall -I. -Iinclude
CFLAGS += -DSABER_PREFIX='"$(PREFIX)"' -DSABER_ETC_PREFIX='"$(ETC_PREFIX)"'

WAYLAND_CFLAGS := $(shell $(PKG_CONFIG) --cflags wayland-client wayland-cursor)
WAYLAND_LIBS := $(shell $(PKG_CONFIG) --libs wayland-client wayland-cursor)
CAIRO_CFLAGS := $(shell $(PKG_CONFIG) --cflags cairo)
CAIRO_LIBS := $(shell $(PKG_CONFIG) --libs cairo)
PANGO_CFLAGS := $(shell $(PKG_CONFIG) --cflags pangocairo)
PANGO_LIBS := $(shell $(PKG_CONFIG) --libs pangocairo)

# gio-2.0 carries glib and gobject transitively and supplies GDBus, the main
# loop, GFileMonitor and GUnixMountMonitor -- the four things that make a
# toolkit-free panel viable without a widget toolkit. gio-unix-2.0 is a
# separate package: gio/gunixmounts.h, which devices.c is built on, lives only
# under it. Both are backed by kqueue and getfsstat on FreeBSD and by inotify
# and /proc/self/mountinfo on Linux, which is why neither platform needs a
# mount or file-monitoring backend of saber's own.
GIO_CFLAGS := $(shell $(PKG_CONFIG) --cflags gio-2.0 gio-unix-2.0)
GIO_LIBS := $(shell $(PKG_CONFIG) --libs gio-2.0 gio-unix-2.0)
PIXBUF_CFLAGS := $(shell $(PKG_CONFIG) --cflags gdk-pixbuf-2.0)
PIXBUF_LIBS := $(shell $(PKG_CONFIG) --libs gdk-pixbuf-2.0)
RSVG_CFLAGS := $(shell $(PKG_CONFIG) --cflags librsvg-2.0)
RSVG_LIBS := $(shell $(PKG_CONFIG) --libs librsvg-2.0)
LUA_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(LUA_PKG))
LUA_LIBS := $(shell $(PKG_CONFIG) --libs $(LUA_PKG))
XKBCOMMON_CFLAGS := $(shell $(PKG_CONFIG) --cflags xkbcommon)
XKBCOMMON_LIBS := $(shell $(PKG_CONFIG) --libs xkbcommon)

CFLAGS += \
	$(WAYLAND_CFLAGS) \
	$(CAIRO_CFLAGS) \
	$(PANGO_CFLAGS) \
	$(GIO_CFLAGS) \
	$(PIXBUF_CFLAGS) \
	$(RSVG_CFLAGS) \
	$(LUA_CFLAGS) \
	$(XKBCOMMON_CFLAGS)

LIBS = \
	$(WAYLAND_LIBS) \
	$(CAIRO_LIBS) \
	$(PANGO_LIBS) \
	$(GIO_LIBS) \
	$(PIXBUF_LIBS) \
	$(RSVG_LIBS) \
	$(LUA_LIBS) \
	$(XKBCOMMON_LIBS) \
	-lm

# Must sit after every `CFLAGS +=` above. Dropping -pthread is what keeps the
# one binary that needs no threads from linking a threading library.
CTL_CFLAGS = $(filter-out -pthread,$(CFLAGS))

all: saber saberctl

# Action purpose: Regenerate version.h on every build. The phony FORCE
# prerequisite keeps the target permanently out of date; the header is written
# to a temporary file and atomically renamed, so an interrupted build can never
# leave a partial version.h behind.
version.h: FORCE
	@echo "#define SABER_VERSION \"$(VERSION)\"" > version.h.tmp && mv version.h.tmp version.h

FORCE:

# Action purpose: Every object depends on every generated protocol header, and
# on every project header. The first makes the build correct under `-j`:
# prerequisites are unordered, so without it an object could compile before
# wayland-scanner has written a header it includes. The second is not a nicety
# -- its absence produced a SIGSEGV. Editing a struct in include/saber/*.h
# otherwise recompiles nothing, leaving objects built before the edit holding
# the old layout while newer ones hold the new one, so one binary reads the
# same struct at two different sets of offsets. It is silent, it survives a
# successful build, and it bites only on an incremental one.
SABER_HEADERS := $(wildcard include/saber/*.h)

$(OBJS) $(PROTOCOL_OBJS): $(PROTOCOL_HEADERS)
$(OBJS) $(CTL_OBJS): $(SABER_HEADERS)
config.o main.o: version.h

saber: version.h $(PROTOCOL_HEADERS) $(PROTOCOL_OBJS) $(OBJS)
	$(CC) $(LDFLAGS) $(CFLAGS) -o $@ $(OBJS) $(PROTOCOL_OBJS) $(LIBS)

# Action purpose: saberctl(1) is the only object in the tree not compiled with
# $(CFLAGS), and it is deliberately absent from the $(PROTOCOL_HEADERS)
# dependency above: it includes no generated protocol header, and making it
# wait on wayland-scanner would tie the one binary that needs no Wayland to the
# one tool that provides it. It runs on every bound keypress, so linking the
# desktop's shared libraries to print one line would be paid per keystroke. It
# needs the glib INCLUDE paths, because ipc.h declares the server API in terms
# of GError, but not one glib library.
saberctl.o: src/saberctl.c version.h
	$(CC) $(CTL_CFLAGS) -c $< -o $@

saberctl: version.h $(CTL_OBJS)
	$(CC) $(LDFLAGS) $(CTL_CFLAGS) -o $@ $(CTL_OBJS)

# Action purpose: One rule template over the lists in build/sources.mk rather
# than sixteen hand-written rules, so adding a protocol means adding it there
# and nowhere else. The four from wayland-protocols resolve against its
# pkgdatadir; the vendored ones resolve against protocol/.
define proto_rules
$(1)-protocol.h:
	wayland-scanner client-header $(2) $$@
$(1)-protocol.c:
	wayland-scanner private-code $(2) $$@
endef

$(foreach p,xdg-shell xdg-activation-v1 viewporter fractional-scale-v1,\
	$(eval $(call proto_rules,$(p),$(WAYLAND_PROTOCOLS)/$(PROTO_XML_$(p)))))
$(foreach p,$(PROTO_VENDORED),\
	$(eval $(call proto_rules,$(p),protocol/$(p).xml)))

VINPUT_CFLAGS = -Wall -I. $(WAYLAND_CFLAGS) $(XKBCOMMON_CFLAGS)
VINPUT_LIBS = $(WAYLAND_LIBS) $(XKBCOMMON_LIBS)

$(VINPUT_PROTOCOL_OBJS) vinput.o: $(VINPUT_PROTOCOL_HEADERS)

vinput.o: tools/vinput.c
	$(CC) $(VINPUT_CFLAGS) -c $< -o $@

vinput: vinput.o $(VINPUT_PROTOCOL_OBJS)
	$(CC) $(LDFLAGS) -o $@ vinput.o $(VINPUT_PROTOCOL_OBJS) $(VINPUT_LIBS)

# Action purpose: The contrast and easing regression test links theme.o and
# anim.o only. Every colour defect this panel has had was found by measurement
# and nothing but a measurement keeps it fixed, so the test has to be runnable
# anywhere -- including on a host that cannot link the panel itself, which is
# why it drives saber_theme_init directly instead of reading saber.lua.
test: $(PROTOCOL_HEADERS) $(TEST_OBJS)
	$(CC) $(LDFLAGS) $(CFLAGS) -o test/contrast test/contrast.c $(TEST_OBJS) \
		$(CAIRO_LIBS) $(GIO_LIBS) $(LUA_LIBS) -lm
	@./test/contrast

features:
	@echo "WITH_TRAY            = $(if $(WITH_TRAY),$(WITH_TRAY),(off))"
	@echo "WITH_LAUNCHER_ENTRY  = $(if $(WITH_LAUNCHER_ENTRY),$(WITH_LAUNCHER_ENTRY),(off))"
	@echo "WITH_SHEETS          = $(if $(WITH_SHEETS),$(WITH_SHEETS),(off))"
	@echo "WITH_DEVICES         = $(if $(WITH_DEVICES),$(WITH_DEVICES),(off))"
	@echo "WITH_DASH            = $(if $(WITH_DASH),$(WITH_DASH),(off))"
	@echo "WITH_SPREAD          = $(if $(WITH_SPREAD),$(WITH_SPREAD),(off))"
	@echo "WITH_VIRTUAL_INPUT   = $(if $(WITH_VIRTUAL_INPUT),$(WITH_VIRTUAL_INPUT),(off))"
	@echo "LUA                  = $(LUA_PKG)"

clean:
	@echo "cleaning generated protocol sources"
	@rm -f $(PROTOCOL_HEADERS) $(PROTOCOL_HEADERS:.h=.c)
	@rm -f $(VINPUT_PROTOCOL_HEADERS) $(VINPUT_PROTOCOL_HEADERS:.h=.c)
	@echo "cleaning headers"
	@rm -f version.h
	@echo "cleaning object files"
	@rm -f $(OBJS) $(PROTOCOL_OBJS) $(CTL_OBJS)
	@rm -f vinput.o $(VINPUT_PROTOCOL_OBJS)
	@echo "cleaning executables"
	@rm -f saber saberctl vinput test/contrast

distclean: clean
	@rm -f saber-$(VERSION).tar.gz

# Action purpose: Two sub-makes in one recipe rather than two prerequisites,
# which is how this driver gets what the bmake one spells `.ORDER`. GNU make
# has no .ORDER, and prerequisites are unordered, so distclean could otherwise
# run concurrently with the archive under `-j` and remove version.h between its
# regeneration and the tar. Recipe lines run in sequence whatever -j says.
dist:
	$(MAKE) distclean
	$(MAKE) saber-$(VERSION).tar.gz

# GNU tar spells the archive prefix --transform; FreeBSD tar spells it -s.
saber-$(VERSION).tar.gz: version.h
	@tar --transform 's#^#saber-$(VERSION)/#' -czf saber-$(VERSION).tar.gz \
		version.h \
		src/*.c \
		include/saber/*.h \
		protocol/*.xml \
		test/*.c \
		etc/saber/settings.lua \
		build/sources.mk \
		Makefile \
		GNUmakefile \
		compile_flags.txt \
		.clang-format \
		LICENSE \
		README.md

# Action purpose: Saber installs nothing setuid or setgid and writes no
# privilege rule of any kind. On Linux the session actions go through logind
# over D-Bus, which authorises them per-session through polkit, so there is no
# group to join and nothing for an administrator to do -- which is why
# groupcheck says something different here than it does under bmake.
#
# Action purpose: The shipped configuration is installed ONLY where there is
# none. `install` replaces its destination, so a rebuild from a newer checkout
# would silently discard whatever the administrator had edited into
# /etc/saber/settings.lua -- and that file promises in its own header that saber
# never rewrites it. Skipping is said out loud rather than done quietly, because
# an install that declines to apply the new shipped defaults has to say so.
install: saber saberctl
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	mkdir -p $(DESTDIR)$(ETC_PREFIX)/etc/saber
	mkdir -p $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
	install -m 644 share/icons/saber.svg \
		$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps
	install -m 555 saber $(DESTDIR)$(PREFIX)/bin
	install -m 555 saberctl $(DESTDIR)$(PREFIX)/bin
	@if [ -f $(DESTDIR)$(ETC_PREFIX)/etc/saber/settings.lua ]; then \
		echo "saber: $(DESTDIR)$(ETC_PREFIX)/etc/saber/settings.lua exists; left exactly as it is."; \
		echo "       The shipped copy is etc/saber/settings.lua in this tree; merge by hand if you want it."; \
	else \
		install -m 644 etc/saber/settings.lua $(DESTDIR)$(ETC_PREFIX)/etc/saber; \
	fi
	@$(MAKE) groupcheck

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/saber
	rm -f $(DESTDIR)$(PREFIX)/bin/saberctl
	rm -f $(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/saber.svg
	@echo "saber: $(DESTDIR)$(ETC_PREFIX)/etc/saber left in place; remove it by hand if you mean to."

# Action purpose: Report whether the session actions will work; never change
# anything. The panel runs the same check at startup and HIDES the actions it
# cannot perform, so a session manager that cannot be reached leaves a menu
# with no dead buttons rather than one with failing ones.
groupcheck:
	@if [ -S /run/systemd/private ] || [ -d /run/systemd/system ] || \
	    [ -S /run/dbus/system_bus_socket ]; then \
		echo "saber: a session manager is reachable; suspend, reboot, shut down, lock and logout will be offered."; \
		echo "       Each is authorised per-session by polkit, so there is no group to join."; \
	else \
		echo "saber: no logind or system bus found on this host."; \
		echo "       The session menu will hide the actions it cannot perform."; \
		echo "       Point session { suspend, reboot, poweroff, lock, logout } in"; \
		echo "       saber.lua at commands of your own to override that."; \
	fi
