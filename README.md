<h1 align="center">Saber</h1>
<p align="center"><b>S</b>akura <b>A</b>pplication <b>B</b>ar and <b>E</b>ntry <b>R</b>ail</p>
<p align="center"><i>The persistent vertical panel of the Hikari Sakura desktop — launcher, running indicators, quicklists, system tray and session controls, in one always-present column.</i></p>

---

> **Status: design phase.** Nothing is built yet. This repository currently contains the
> architecture, the decisions behind it and the implementation roadmap, under
> [`.devdocs/`](.devdocs/). Start with [`.devdocs/BLUEPRINT.md`](.devdocs/BLUEPRINT.md).

## What Saber is

Saber is a vertical panel for Wayland in the tradition of the Unity 7 launcher, built for and
only for the [`hikari-sakura`](https://github.com/orpheus497/hikari-sakura) compositor on
FreeBSD.

Its reason to exist is that nothing else in the desktop is **persistent**. Every `sofi`
surface appears on a keypress and dismisses on selection. A launcher in the Unity sense is a
different kind of object: always on screen, aimed at without looking, showing at a glance what
is running — and it is where the system tray lives.

Down the column, top to bottom: an application menu button, your pinned and running
applications with their window-count pips and focus arrow, the sheet indicator, mounted
devices, the trash, the system tray, and the session controls.

## The Sakura set

Four programs, built to be used together, each usable on its own:

| | Program | Written in | Role |
|---|---|---|---|
| 1 | [**sakura**](https://github.com/orpheus497/sakura) | Zig | **Display manager.** A TUI login manager on a FreeBSD virtual terminal. Talks to OpenPAM directly; no toolkit, no session bus |
| 2 | [**hikari-sakura**](https://github.com/orpheus497/hikari-sakura) | C | **Compositor.** Stacking, with tiling; built on views, groups and *sheets*. Draws its own top bar and lock screen |
| 3 | [**sofi**](https://github.com/orpheus497/sofi) | C | **Launcher.** Application menu, notification daemon, and a general-purpose indexer |
| 4 | **saber** — this repository | C | **Panel.** The desktop's persistent surface |

### How Saber fits

Saber takes the panel surfaces and leaves the rest alone:

* **The compositor keeps all telemetry.** CPU, RAM, temperature, network, battery, volume,
  media and the clock live in `hikari-sakura`'s own top bar, fed by `hikari-topbar`. Saber
  does not duplicate any of it — a panel anchored to the left edge sits *underneath* that bar
  automatically, with nothing to configure.
* **`sofi` keeps notifications.** `sofi -notification-daemon` and
  `sofi -show notification-history` are unaffected.
* **Saber takes the system tray.** This one is an either/or, not a preference: exactly one
  process on a session bus can own `org.kde.StatusNotifierWatcher`.
  **Do not run `sofi -tray-daemon` alongside `saber`.** Build with `WITH_TRAY=NO` if you would
  rather keep the existing arrangement.

## Theming

Saber has no theme of its own. It reads `ui { palette }` straight out of your `hikari.conf`,
so the panel is painted from the same sixteen colours as the compositor, the top bar and the
lock screen. Drop a terminal colour scheme into `hikari.conf` and the whole desktop follows,
panel included.

Unity's mechanics are reproduced; Unity's Ambiance colours are not.

## Configuration

`~/.config/saber/saber.conf`, in UCL — the same syntax and the same parser as `hikari.conf`.
Every key has a default and the file is optional.

```ucl
panel {
  output    = "all"      # all | primary | <output name>
  edge      = left       # left | right
  icon-size = 48         # 24-64
  autohide  = never      # never | auto
}

theme {
  inherit-hikari = true  # import ui { palette } from hikari.conf
  backlight      = palette
}

launcher {
  favourites = [ "firefox.desktop", "sofi.desktop" ]
}
```

Favourites in the configuration **seed** the launcher on first run; the live order — the one
you get by dragging tiles — is kept in `$XDG_DATA_HOME/saber/favourites` and wins thereafter.
Saber never rewrites your configuration file. Delete the state file to re-seed from it.

## Session controls, and why there is no `sudo` here

Suspend, reboot and shut down run as you, through FreeBSD's existing `operator` group. Nothing
in Saber is setuid or setgid, there is no `sudoers` fragment, and no group is created:

```
-r-sr-xr--  root operator   /sbin/shutdown      setuid root, group-executable by operator
crw-rw-r--  root operator   /dev/acpi           group-writable by operator
```

A member of `operator` runs `shutdown -p now`, `shutdown -r now` and `acpiconf -s3` with no
escalation at all. If you are not in that group:

```sh
pw groupmod operator -m <user>
```

`make install` prints this line when it applies; it never runs it, because adding a user to a
privileged group is an administrator's decision. Saber checks its own membership at startup
and **hides** the actions it cannot perform, so you get a panel with no dead buttons rather
than one with failing ones. `session { … }` exists to override the commands for a host that
deliberately keeps its users out of `operator`.

## Building

FreeBSD only. This is unlikely to change.

**Dependencies:** wayland, wayland-protocols, cairo, pango, glib/gio, gdk-pixbuf2, librsvg2,
libucl, xkbcommon.

```sh
make
make install
```

`WITH_ALL` defaults to `YES`. Individual features build out:

| Flag | Default | What it does |
| --- | --- | --- |
| `WITH_TRAY` | on | StatusNotifierItem host. Turn off to keep `sofi -tray-daemon` |
| `WITH_LAUNCHER_ENTRY` | on | Count and progress badges from `com.canonical.Unity.LauncherEntry` |
| `WITH_SHEETS` | on | Sheet indicator, via the compositor's control socket |
| `WITH_DEVICES` | on | Mounted-volume tiles |
| `WITH_DASH` | on | Application grid |
| `WITH_SPREAD` | on | Window grid |
| `WITH_VIRTUAL_INPUT` | **off** | Synthesised lock keysym — see *Known limits* |

## Known limits

Stated rather than left to be discovered. Each follows from Saber being a **pure Wayland
client**: it asks `hikari-sakura` for nothing that is not already published.

* **The window spread shows no thumbnails.** The compositor advertises an *output* image-capture
  source only, and only behind a build flag that is off by default; `wlr-screencopy` has no
  per-window request. The spread is an icon-and-title grid. (`sofi`'s `-window-thumbnail` is
  X11-only for the same reason.)
* **There is no Lock and no Logout.** `lock` is a keybinding-only compositor action with no
  CLI and no socket verb, and FreeBSD has no `logind` to ask for a session end. Both entries
  are hidden unless you configure `session { lock, logout }` with something to run. A
  `WITH_VIRTUAL_INPUT` build can synthesise your configured lock keysym instead, but it breaks
  silently if you rebind the key, which is why it is off by default.
* **No hold-Super number overlay.** A panel cannot observe modifier state without holding
  keyboard focus. Bind `saberctl launch <N>` and `saberctl overlay` in `hikari.conf` instead.
* **Autohide has `never` and `auto`, not `dodge`.** "Hide when a window would overlap" needs
  window geometry, and no foreign-toplevel protocol publishes any.
* **Window matching is by `app_id`.** Neither foreign-toplevel protocol carries a pid, so an
  application whose `app_id` matches no desktop entry gets a generic tile. Saber tries five
  resolution rules and a launch-time window before giving up.
* **Do not run two tray hosts.** See *How Saber fits*.

## Documentation

* [`.devdocs/BLUEPRINT.md`](.devdocs/BLUEPRINT.md) — the authoritative architecture
* [`.devdocs/DECISIONS_LOG.md`](.devdocs/DECISIONS_LOG.md) — every decision, with its reasoning
* [`.devdocs/PLANS.md`](.devdocs/PLANS.md) — the implementation roadmap

## License

MIT. Portions ported from [`sofi`](https://github.com/orpheus497/sofi), also MIT — see
`.devdocs/ARCHITECTURE_MAPPING.md` for what came from where.
