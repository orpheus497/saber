PORTNAME=	saber
DISTVERSIONPREFIX=	v
DISTVERSION=	1.0.0
CATEGORIES=	x11

MAINTAINER=	orpheus497@gmail.com
COMMENT=	Vertical desktop panel for the hikari-sakura Wayland compositor
WWW=		https://github.com/orpheus497/saber

LICENSE=	MIT
LICENSE_FILE=	${WRKSRC}/LICENSE

# Saber is FreeBSD-only by construction, not by omission: its privilege model is
# the base system's operator group, and its mount and file monitoring go through
# getfsstat(2) and kqueue. The tree's own Makefile refuses any other uname. That
# needs no expression here -- a port only ever builds on FreeBSD -- and is noted
# so nobody adds a portability shim to make it build elsewhere.

USES=		gnome pkgconfig
USE_GNOME=	glib20 gdkpixbuf librsvg2
USE_GITHUB=	yes
GH_ACCOUNT=	orpheus497

BUILD_DEPENDS=	wayland-protocols>=1.41:graphics/wayland-protocols
LIB_DEPENDS=	libwayland-client.so:graphics/wayland \
		libcairo.so:graphics/cairo \
		libpango-1.0.so:x11-toolkits/pango \
		libucl.so:textproc/libucl \
		libxkbcommon.so:x11/libxkbcommon

# The compositor Saber is built for. It is a RUN dependency and not a build one:
# Saber links no wlroots and includes no compositor header -- it speaks
# published Wayland protocols and hikari's control socket, and nothing else
# (.devdocs/DECISIONS_LOG.md D-003). A user running Saber on another wlroots
# compositor loses the sheet indicator and keeps everything else, which is why
# this is not a hard requirement either.
#RUN_DEPENDS=	hikari:x11-wm/hikari-sakura

# wayland-scanner ships with graphics/wayland, already pulled in by LIB_DEPENDS.
# The tree's own Makefile is BSD make, so the default do-build and do-install
# need no override -- unlike sakura, whose Zig installer does.

OPTIONS_DEFINE=		TRAY DASH SPREAD
OPTIONS_DEFAULT=	TRAY DASH SPREAD

TRAY_DESC=	StatusNotifierItem system tray host
DASH_DESC=	Full-screen application grid
SPREAD_DESC=	Full-screen window grid

# Turning the tray off is the supported way to keep using sofi's own tray
# daemon: exactly one process on a session bus can own
# org.kde.StatusNotifierWatcher, so the two cannot coexist (D-002).
TRAY_MAKE_ARGS=		WITH_TRAY=YES
TRAY_MAKE_ARGS_OFF=	WITH_TRAY=NO
DASH_MAKE_ARGS=		WITH_DASH=YES
DASH_MAKE_ARGS_OFF=	WITH_DASH=NO
SPREAD_MAKE_ARGS=	WITH_SPREAD=YES
SPREAD_MAKE_ARGS_OFF=	WITH_SPREAD=NO

MAKE_ARGS+=	PREFIX=${PREFIX} ETC_PREFIX=${PREFIX} VERSION=${DISTVERSION}

# Saber installs nothing setuid or setgid and writes no sudoers fragment: the
# session actions rely on membership of the base system's operator group, which
# already owns /sbin/shutdown and /dev/acpi. pkg-message tells the administrator
# how to grant it; nothing here grants it automatically, because a package that
# silently adds users to privileged groups is a package nobody should trust.
# The tree's install writes a live saber.conf. A package must not overwrite a
# file the administrator has edited, so it ships as .sample and pkg materialises
# it on first install only -- the same treatment sakura gives its config.ini.
post-install:
	@${STRIP_CMD} ${STAGEDIR}${PREFIX}/bin/saber
	@${STRIP_CMD} ${STAGEDIR}${PREFIX}/bin/saberctl
	@${MV} ${STAGEDIR}${PREFIX}/etc/saber/saber.conf \
		${STAGEDIR}${PREFIX}/etc/saber/saber.conf.sample

.include <bsd.port.mk>
