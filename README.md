<h1 align="center">Saber</h1>
<p align="center"><i>The persistent vertical panel of the Hikari Sakura desktop — launcher, running indicators, quicklists, application menu, window spread, system tray and session controls, in one always-present column.</i></p>

<p align="center">
  <b>FreeBSD only.</b> ·
  <a href="#building">Building</a> ·
  <a href="#configuration">Configuration</a> ·
  <a href="#making-the-keys-work">Keybindings</a> ·
  <a href="#troubleshooting">Troubleshooting</a> ·
  <a href="#known-limits">Known limits</a>
</p>

---

> **Status: working, not yet released.** The panel runs, draws, and every planned surface
> exists. What is missing is listed under [Known limits](#known-limits) and is stated rather
> than left to be discovered.

## What Saber is

Saber is a vertical panel for Wayland in the tradition of the Unity 7 launcher, built for and
only for the [`hikari-sakura`](https://github.com/orpheus497/hikari-sakura) compositor on
FreeBSD.

Its reason to exist is that nothing else in the desktop is **persistent**. Every `sofi` surface
appears on a keypress and dismisses on selection. A launcher in the Unity sense is a different
kind of object: always on screen, aimed at without looking, showing at a glance what is
running — and it is where the system tray lives.

It is a **pure Wayland client**. Windows are listed and acted on through
`wlr-foreign-toplevel-management`, surfaces are placed with `wlr-layer-shell`, and the
compositor's sheets are read over its own control socket. Nothing about the compositor is
patched, and Saber asks it for nothing it does not already publish. It draws itself with cairo
and pango rather than linking a widget toolkit.

## The Sakura set

Four programs, built to be used together, each usable on its own:

| | Program | Written in | Role |
|---|---|---|---|
| 1 | [**sakura**](https://github.com/orpheus497/sakura) | Zig | **Display manager.** A TUI login manager on a FreeBSD virtual terminal. Talks to OpenPAM directly; no toolkit, no session bus, no login-manager framework |
| 2 | [**hikari-sakura**](https://github.com/orpheus497/hikari-sakura) | C | **Compositor.** Stacking, with tiling; built on views, groups and *sheets*. Draws its own top bar and lock screen |
| 3 | [**sofi**](https://github.com/orpheus497/sofi) | C | **Launcher.** A general-purpose indexer: application menu, notification daemon, notification history |
| 4 | **saber** — this repository | C | **Panel.** The desktop's persistent surface |

### How Saber fits

Saber takes the panel surfaces and leaves the rest alone.

* **The compositor keeps all telemetry.** CPU, RAM, temperature, network, battery, volume,
  media and the clock live in `hikari-sakura`'s own top bar, fed by `hikari-topbar`. Saber
  duplicates none of it — a panel anchored to the left edge sits *underneath* that bar
  automatically, with nothing to configure and no height to keep in sync.
* **`sofi` keeps notifications.** `sofi -notification-daemon` and
  `sofi -show notification-history` are unaffected and should keep running.
* **Saber takes the system tray, and this one is an either/or.** Exactly one process on a
  session bus can own `org.kde.StatusNotifierWatcher`. See
  [Troubleshooting](#the-system-tray-is-empty).

## The column

Top to bottom, each item switchable in `items { }`:

| Item | Left click | Middle click | Right click | Scroll |
|---|---|---|---|---|
| **BFB** (the winged orb) | Open the Dash | — | — | — |
| **Application tile** | Launch · focus · minimise if already focused · spread when 2+ windows | New instance | Quicklist | Cycle that app's windows |
| **Sheet** | Sheet grid | — | — | Step sheet |
| **Device** | Open the mount point | Unmount | — | — |
| **Trash** | Open the trash | — | Open / Empty Trash | — |
| **Tray item** | Activate, or its menu | Secondary activate | Context menu | Forwarded |
| **Session** | Session menu | — | — | — |

### Tile decoration

Each is a Unity signifier drawn from a palette role, never a fixed colour:

| Signifier | Meaning |
|---|---|
| Pips, inner edge | 1 / 2 / 3+ open windows |
| Arrow, outer edge | This application holds focus |
| Backlight | The tile fill behind a running application |
| Count badge | `count` from `com.canonical.Unity.LauncherEntry` |
| Progress bar | `progress`, 0.0–1.0, from the same |
| Wiggle and red tint | `urgent` |
| Throb | Launching, until the window appears |

### Quicklists

Right-clicking an application composes its menu in this order: the application's **dynamic**
menu published over `com.canonical.dbusmenu` → its desktop entry's **static** `Actions=` → its
window list when there is more than one → Keep in launcher / Unpin → Quit.

Saber renders the menu itself. Under StatusNotifierItem and DBusMenu an application publishes a
*description* of a menu, and there is no method in either protocol that asks it to display
anything — rendering is the host's job, and that is exactly what lets the panel theme it.

### The Dash

`saberctl dash`, or click the BFB. A panel docked to the same edge as the column, one third of
the output wide and full height, with the desktop beside it left undimmed. The search field is
top-left; the category strip runs along the bottom.

Typing filters; categories filter; the two compose. Ranking puts prefix matches above interior
ones, across name, generic name and `Exec` basename. `Tab` cycles categories, arrows move and
scroll the grid, `Enter` launches, `Escape` dismisses — as does a click outside the panel.

### The window spread

`saberctl spread [app_id]`, or click a tile with two or more windows. A grid of every open
window — including windows living on sheets you are not currently looking at, which is the
whole point of it. `Enter` un-minimises and activates; `Delete` closes.

There are **no thumbnails**, and that is a design decision forced by the compositor. See
[Known limits](#known-limits).

## Theming

Saber has no theme file. It reads `ui { palette }` straight out of your `hikari.conf`, so the
panel is painted from the same sixteen colours as the compositor, its top bar and its lock
screen. Drop a terminal colour scheme into `hikari.conf` and the whole desktop follows, panel
included.

Unity's mechanics are reproduced; Unity's Ambiance colours are not.

## Configuration

`~/.config/saber/saber.conf`, in UCL — the same syntax and the same parser as `hikari.conf`.
**Every key is optional and the whole file may be deleted**; the values below are the defaults.
A copy with all of this documented in place is installed at
`${PREFIX}/etc/saber/saber.conf`.

```ucl
panel {
  output          = "all"    # all | primary | <output name, e.g. eDP-1>
  edge            = left     # left | right
  icon-size       = 32       # 24-64; tile width is this + 12, so 32 gives a 44px column
  autohide        = never    # never | auto
  reveal-pressure = 240
  animation-ms    = 180      # 0 disables animation entirely
}

theme {
  inherit-hikari = true      # import ui { palette } from hikari.conf
  backlight      = palette   # palette | dominant | off
  opacity        = 0.92
}

launcher {
  favourites = [ "firefox.desktop", "thunar.desktop" ]
}

items {
  bfb = true; sheets = true; devices = true
  trash = true; tray = true; session = true
}

session {
  # Empty = use the built-in operator-group command. See Session controls.
  suspend = ""; reboot = ""; poweroff = ""
  # Empty = HIDE the entry. There is nothing to run by default; see Known limits.
  lock = ""; logout = ""
}
```

`icon-size` 32 and 48 are sizes every icon theme ships natively; values between them make
raster icons resample, which is visible on a surface that is on screen permanently.

### Favourites are stored twice, on purpose

`launcher { favourites }` **seeds** the launcher on first run. The live order — the one you get
by dragging tiles — is kept in `$XDG_DATA_HOME/saber/favourites` and wins from then on. Saber
never rewrites your configuration file. Delete the state file to re-seed from it.

## Running it

Saber is resident, so it belongs in the compositor's autostart rather than on a key:

```sh
echo 'saber &' >> ~/.config/hikari/autostart
chmod +x ~/.config/hikari/autostart
```

Starting it there also removes a caveat the tray would otherwise have. A tray application asks
exactly once, at its own startup, whether a host exists; one that finds none shows no icon and
never asks again. A panel already running when your session starts is never the process that
arrives too late.

## Making the keys work

**This is the step most likely to be missed.** Saber is a layer-shell client, and a layer-shell
client **cannot grab global keys** — there is no protocol for it. Every keyboard shortcut
therefore has to be a `hikari.conf` binding that runs `saberctl`.

Add to `hikari.conf`:

```ucl
actions {
  saber-dash   = "saberctl dash"
  saber-spread = "saberctl spread"
  saber-1      = "saberctl launch 1"
  saber-2      = "saberctl launch 2"
  saber-3      = "saberctl launch 3"
}

bindings {
  keyboard {
    "LA+a" = action-saber-dash
    "LA+s" = action-saber-spread
    "LA+1" = action-saber-1
    "LA+2" = action-saber-2
    "LA+3" = action-saber-3
  }
}
```

> **Why `Logo+Alt` and not `Super+N`.** The shipped `hikari.conf` is densely bound: `L+1`…`L+9`
> are `workspace-switch-to-sheet-N` and `LS+1`…`LS+9` are `view-pin-to-sheet-N`, so both of the
> obvious choices are taken — as are almost all of `L+`, `LS+` and `LC+`. `LA+` has very few
> bindings and is the free space. Hikari keeps only the **first** binding it reads for a key
> and reports the one it dropped at startup, so a collision fails quietly at the keyboard but
> loudly in the log.
>
> Unity's literal Super+N is therefore only available by giving up sheet switching on those
> keys. That is a real trade and yours to make; nothing here makes it for you.

### `saberctl`

```
saberctl dash                  toggle the application grid
saberctl spread [app_id]       toggle the window spread, optionally filtered
saberctl launch <1-9>          launch or focus favourite N
saberctl overlay <on|off>      the hold-Super number overlay
saberctl show | hide | toggle  panel visibility
saberctl sheet <0-9>           switch to sheet N
saberctl pin <0-9>             send the focused window to sheet N
saberctl reload                re-read the configuration
saberctl status                report panel state, terminated by END
saberctl quit                  exit the panel
```

It exits 0 when the panel answers `ok` and 1 otherwise, so it composes in scripts. It speaks to
`$XDG_RUNTIME_DIR/saber.sock`, which is also the single-instance lock: a second `saber` refuses
to start rather than stealing the socket, and a stale socket left by an unclean exit is removed
rather than being mistaken for a live panel.

`saberctl` links **only libc** and is 20 KB. That is deliberate: it runs on every bound
keypress, and linking the desktop's shared libraries to print one line would be paid per
keystroke.

## Session controls

Suspend, reboot and shut down run as you, through FreeBSD's existing `operator` group. **Saber
installs nothing setuid or setgid, writes no `sudoers` rule, and creates no group of its own:**

```
-r-sr-xr--  root operator   /sbin/shutdown      setuid root, group-executable by operator
crw-rw-r--  root operator   /dev/acpi           group-writable by operator
```

A member of `operator` runs `shutdown -p now`, `shutdown -r now` and `acpiconf -s3` with no
escalation at all. If you are not in that group, an administrator runs:

```sh
pw groupmod operator -m <user>
```

`make install` prints this line when it applies; it never runs it, because adding a user to a
privileged group is an administrator's decision. Saber checks its own membership at startup and
**hides** the actions it cannot perform, so declining leaves a menu with no dead buttons rather
than one with failing ones. `session { }` overrides the commands for a host that deliberately
keeps its users out of `operator`.

`/sbin/reboot` is never used — it is not setuid, so it would fail. `shutdown -r now` is.

## Building

FreeBSD only. This is unlikely to change: the privilege model is the base system's `operator`
group, mount and file monitoring go through `getfsstat(2)` and kqueue, and the Makefile refuses
any other `uname`.

### Dependencies

| Port | Tested with | Why |
|---|---|---|
| `graphics/wayland` | 1.26.0 | The protocol, plus `wayland-scanner` and `libwayland-cursor` |
| `graphics/wayland-protocols` | 1.49 | Protocol XML (build only) |
| `graphics/cairo` | 1.18.2 | All drawing |
| `x11-toolkits/pango` | 1.57.1 | Text |
| `devel/glib20` | 2.88.3 | Main loop, GDBus, kqueue file monitor, `getfsstat` mount monitor |
| `graphics/gdk-pixbuf2` | 2.44.1 | Raster icons |
| `graphics/librsvg2-rust` | 2.62.3 | SVG icons, rendered at exact device size |
| `textproc/libucl` | 0.9.4 | `saber.conf` and the `hikari.conf` palette import |
| `x11/libxkbcommon` | 1.13.2 | Keyboard handling in the Dash, spread and menus |

```sh
pkg install wayland wayland-protocols cairo pango glib gdk-pixbuf2 \
            librsvg2-rust libucl libxkbcommon
```

Two `wlr-*` protocol XMLs are not packaged anywhere and are vendored under `protocol/`. The
rest are generated from `wayland-protocols` at build time.

### Build and install

```sh
make
make install            # PREFIX defaults to /usr/local
```

Saber is **not in the ports tree**, and no port skeleton ships here yet. Build it with `make`
as above; a `ports/` directory with `TRAY`, `DASH` and `SPREAD` options will land alongside an
actual ports submission.

`make install` places `saber` and `saberctl` in `${PREFIX}/bin` (mode 555, neither setuid), the
default configuration in `${ETC_PREFIX}/etc/saber/saber.conf`, and the BFB emblem in
`${PREFIX}/share/icons/hicolor/scalable/apps/`.

### Build switches

`WITH_ALL` defaults to `YES`; every switch accepts `NO` in any case.

| Flag | Default | Effect |
|---|---|---|
| `WITH_TRAY` | on | StatusNotifierItem host. **Turn off to keep `sofi -tray-daemon`** |
| `WITH_LAUNCHER_ENTRY` | on | Count and progress badges |
| `WITH_SHEETS` | on | Sheet indicator and grid, via the control socket |
| `WITH_DEVICES` | on | Mounted-volume tiles |
| `WITH_DASH` | on | Application grid |
| `WITH_SPREAD` | on | Window grid |
| `WITH_VIRTUAL_INPUT` | **off** | Synthesised lock keysym — see Known limits |

```sh
make WITH_TRAY=NO          # keep sofi's tray host
make WITH_ALL=NO           # panel column only
make DEBUG=YES             # symbols, no optimisation, -Werror
make DEBUG=YES ASAN=YES    # plus AddressSanitizer
make features              # print what a build would enable, without building
```

`saber --build` reports the feature set, the linked library versions and whether you can carry
out the session actions — so a claim about a switch can be checked against the binary rather
than the Makefile.

## Troubleshooting

### The system tray is empty

Exactly one process on a session bus can own `org.kde.StatusNotifierWatcher`. If another host
already holds it, Saber says so at startup and leaves the zone empty rather than fighting for
the name:

```
saber: org.kde.StatusNotifierWatcher is owned by another tray host; the tray zone
       will be empty. Do not run sofi -tray-daemon alongside saber.
```

The usual cause is `sofi -tray-daemon` in `~/.config/hikari/autostart`. Remove that line, or
build with `WITH_TRAY=NO` to keep it and let `sofi` own the tray. Applications must be
restarted afterwards — a tray application asks for a host only once.

The same applies to `com.canonical.Unity`: if another launcher owns it, count badges and
progress bars stay dead, because applications only emit that state when the name is owned.

### Clicking a folder opens a terminal

Saber resolves a file manager as `$FILEMANAGER` → a desktop entry in the `FileManager` category
→ the `inode/directory` default → `xdg-open`, and refuses any candidate that is a terminal
(`Terminal=true`, or `TerminalEmulator` in its categories). If your `inode/directory` default
points at something uninstalled, `xdg-open` can still fall through to a terminal that merely
*declares* the MIME type. Set the variable and it stops guessing:

```sh
FILEMANAGER=thunar saber &
```

### Keyboard shortcuts do nothing

Saber cannot grab global keys. See [Making the keys work](#making-the-keys-work) — the bindings
have to exist in `hikari.conf` and call `saberctl`.

### Typing in the Dash does nothing

If the panel takes clicks but no keys, the compositor's seat has no active keyboard — it then
sends neither a keymap nor a focus event, and no client on that seat receives key events. This
is compositor-side. Restarting the session is the reliable fix.

### Windows already open get no tile

Fixed. If you see this, the build predates the fix — the foreign-toplevel manager used to be
bound before any listener existed, so the compositor's initial burst of one event per existing
window was dispatched into nothing.

## Known limits

Stated rather than left to be discovered. Each follows from Saber being a **pure Wayland
client** that asks `hikari-sakura` for nothing it does not already publish.

* **The window spread shows no thumbnails.** The compositor advertises an *output*
  image-capture source only, and only behind a build flag that is off by default;
  `wlr-screencopy` has no per-window request. The spread is an icon-and-title grid.
* **There is no Lock and no Logout.** `lock` is a keybinding-only compositor action with no CLI
  and no socket verb, and FreeBSD has no `logind` to ask for a session end. Both entries hide
  unless you point `session { lock, logout }` at something. A `WITH_VIRTUAL_INPUT` build can
  synthesise your configured lock keysym instead, but it breaks silently if you rebind the key,
  which is why it is off by default.
* **No hold-Super number overlay.** A panel cannot observe modifier state without holding
  keyboard focus. `saberctl overlay` exists for a held binding instead.
* **Autohide has `never` and `auto`, not `dodge`.** "Hide when a window would overlap" needs
  window geometry, and no foreign-toplevel protocol publishes any. `auto` is not yet
  implemented either.
* **Window matching is by `app_id`.** Neither foreign-toplevel protocol carries a pid, so there
  is no authoritative process-to-window link available to any Wayland client. Saber tries five
  resolution rules and a launch-time window before falling back to a generic tile.
* **Drag-and-drop onto tiles is written but unproven**, and tile reordering by drag is not
  implemented.
* **Sheet semantics leak through, deliberately.** On `hikari-sakura` a window's "minimised" bit
  means *"not on the sheet you are looking at"*, and sheet 0's windows are never hidden. Saber
  renders that asymmetry rather than smoothing it away.

## Documentation

* `man saber` — the complete reference *(not yet written)*
* This file — what Saber is, how it fits, and what it deliberately does not do

## License

MIT. Portions ported from [`sofi`](https://github.com/orpheus497/sofi), also MIT, which is
itself a fork of `rofi` and `simpleswitcher`. All four copyright notices are retained in
[`LICENSE`](LICENSE), and each ported file records its provenance in its own header.
