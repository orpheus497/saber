-- saber.lua -- configuration for saber, the Hikari Sakura panel.
--
-- Lua. This file is a chunk that RETURNS a table, and that table is the
-- configuration -- the same language hikari's own configuration is moving to,
-- so one syntax covers the desktop.
--
-- EVERY KEY TAKES EITHER SPELLING. The manual writes them hyphenated
-- (icon-size, animation-ms, overlay-opacity) and Lua can only write a
-- hyphenated key as ["icon-size"], so the underscored spelling answers to the
-- same key and is what this file uses: `icon_size = 32` and
-- `["icon-size"] = 32` are one setting, not two.
--
-- IT IS CODE, AND IT IS SANDBOXED. The chunk is run with math, string, table
-- and a handful of basic functions, and with no io, no os, no package, no
-- require, no load and no dofile -- a panel's settings have no reason to open
-- a file or start a process. Note also that every value is a Lua value: the
-- words are quoted strings, so `edge = "left"` and not `edge = left`. The
-- unquoted form reads a name nothing defines, so the file is refused and the
-- message names the word and says to quote it -- which matters most if you are
-- carrying a saber.conf over by hand, because every value in that format was
-- an unquoted word.
--
-- A FILE THAT WILL NOT PARSE IS REFUSED, NOT PARTLY APPLIED. Saber reports it
-- with the line number Lua gives and keeps the settings it is already running
-- with, so a typo here cannot cost you a configured panel.
--
-- A VALUE THE PARSER DOES NOT RECOGNISE IS THE GENTLER CASE. `edge = "rigth"`
-- is valid Lua, so the file loads: the word is named on stderr and that one
-- key keeps its default, while every other key in the file still applies. The
-- same holds for an unknown name in items.order and for an unusable colour in
-- theme.roles -- the cost is one key, never the file. Values are matched
-- without regard to case, so "Left" and "left" are the same word.
--
-- EVERY KEY IS OPTIONAL. This whole file may be deleted and Saber still
-- starts. Each key below is written with the value it already has, so changing
-- nothing changes nothing -- with TWO exceptions, both called out where they
-- appear: launcher.favourites has NO built-in default, so deleting it leaves
-- the launcher with running windows only, and panel.animation_ms is left
-- commented out because writing it at all refuses the compositor's own
-- duration.
--
-- Copy it to ~/.config/saber/saber.lua to make it yours. Saber never rewrites
-- this file, or yours.

return {
  panel = {
    -- Which outputs carry a panel: "all", "primary", or an output name as
    -- reported by the compositor (e.g. "eDP-1").
    output = "all",

    -- Which screen edge, "left" or "right". The panel anchors to the full
    -- height of the usable area, so it sits underneath the compositor's top
    -- bar automatically -- there is no offset to configure and none to get
    -- wrong.
    edge = "left",

    -- Icon edge length in logical pixels, 24-64. The tile is this plus the
    -- padding below, so the defaults give a 44px column. 32 and 48 are sizes
    -- every icon theme ships natively; values between them make raster icons
    -- resample.
    icon_size = 32,

    -- The gutter around the icon, 0-64: half of it either side. It sets the
    -- column's width AND the vertical pitch of the tiles, so raising it widens
    -- the strip and fits fewer tiles into the same height.
    padding = 12,

    -- "never" reserves the strip and windows tile around it.
    --
    -- "auto" reserves nothing: windows use the whole screen and the column
    -- hides itself. Hidden, it draws nothing and accepts the pointer only in a
    -- two-pixel strip at the screen edge, so whatever is behind it stays
    -- clickable through the space it would occupy. Push into that strip and it
    -- comes back; move off it and it goes away again.
    --
    -- There is no "dodge". Hiding only when a window would overlap needs
    -- window geometry, and no foreign-toplevel protocol publishes any. See
    -- the README, under "Known limits".
    autohide = "never",

    -- How far the pointer must travel INSIDE the reveal strip before an
    -- autohidden panel comes back, 0-100000. Distance, not time: a pointer
    -- that merely stops at the edge never opens the column, and one
    -- deliberately pushed along it does. Higher is harder. Ignored when
    -- autohide = "never".
    --
    -- `saberctl show`, `hide` and `toggle` override this at any time, and work
    -- under autohide = "never" too.
    reveal_pressure = 240,

    -- Animation duration in milliseconds, 0-10000. It governs EVERY animation,
    -- not merely some of them: the hover fade uses it exactly as written, and
    -- the dash's slide-in, the spread's fade-and-rise, menus fading up, the
    -- launch throb and the urgent wiggle are all scaled from it against a
    -- 180ms baseline. So the value here is a speed for the whole panel.
    --
    -- 0 disables animation entirely -- every tile and surface changes state
    -- instantly, and nothing is left quietly running at its own duration.
    --
    -- WRITING THIS KEY AT ALL TAKES THE DECISION OFF THE COMPOSITOR, which is
    -- why it is commented out below and the other keys in this file are not.
    -- Left out, the panel inherits hikari's own ui.animation while
    -- theme.inherit_hikari is true, so the desktop moves as one thing instead
    -- of at two unrelated speeds:
    --
    --   panel.animation_ms      written here -- always wins
    --   ui.animation.duration   in hikari.lua -- next, 0-1000
    --   180                     the built-in default, if neither exists
    --
    -- hikari's `enabled = false` arrives as 0, which is the same statement
    -- made the other way round: the compositor animates nothing, so neither
    -- does the panel. NOTE THAT hikari-sakura SHIPS IT OFF -- on a stock
    -- desktop the inherited value is 0 and the panel settles instantly. Write
    -- the key here to keep the panel moving while the compositor does not.
    --
    -- `easing` is deliberately NOT imported. The panel picks a curve per
    -- animation and a window-motion curve is the wrong one for a hover fade.
    --
    -- animation_ms = 180,
  },

  theme = {
    -- Read ui.palette out of your hikari.lua, so retheming the compositor
    -- retints the panel with it -- the same contract the compositor already
    -- has with its top bar and its lock screen. Drop a terminal colour scheme
    -- into hikari.lua and the whole desktop follows.
    --
    -- Set false, or supply a palette block below, to break that link.
    inherit_hikari = true,

    -- An explicit palette overrides the import. All sixteen slots are
    -- required: a partial palette is reported and ignored rather than tinting
    -- half the panel. Conventional terminal layout, 0-7 normal and 8-15
    -- bright.
    --
    -- It is written out in full here rather than elided, because all sixteen
    -- have to be there for the block to count and a half-written example is
    -- no use to anybody. These ARE the built-in values -- hikari-sakura's own
    -- palette -- so uncommenting the block changes nothing until you start
    -- editing it. The trailing names are what each slot is for; they are what
    -- the role derivations below talk about when they name a slot.
    --
    -- palette = {
    --   color0  = "#2b1e3a", -- base
    --   color1  = "#c96464", -- red
    --   color2  = "#df9f87", -- orange
    --   color3  = "#e4b382", -- yellow
    --   color4  = "#8e7cc3", -- violet
    --   color5  = "#b18fc7", -- mauve
    --   color6  = "#9fa0a6", -- grey
    --   color7  = "#d4d4d9", -- text
    --   color8  = "#5e5966", -- bright base
    --   color9  = "#df8787", -- bright red
    --   color10 = "#f2bda8", -- bright orange
    --   color11 = "#f5cf9e", -- bright yellow
    --   color12 = "#aba0d9", -- bright violet
    --   color13 = "#cfaedc", -- bright mauve
    --   color14 = "#b8b9be", -- bright grey
    --   color15 = "#f0edf2", -- bright text
    -- },

    -- WHICH COLOUR CARRIES WHICH MEANING. The palette says what colours exist;
    -- these eleven roles say what each one is FOR. The defaults are derived from
    -- the palette by measurement rather than read off fixed slot numbers,
    -- because the same slot does not mean the same thing in two palettes --
    -- slot 1 is a red in one of hikari-sakura's three themes and a muted green
    -- in another, so a role map pinned to slots paints urgency green there.
    --
    -- This block exists to disagree with the derivation. Each key takes either
    -- a palette reference ("colorN") or a literal ("#rgb", "#rrggbb",
    -- "#rrggbbaa") -- both written as strings -- and what you write WINS over
    -- the computed value.
    --
    --   background  the column itself              color0
    --   foreground  pips, separators, text         color15
    --   accent      focus arrow, current sheet     the better of color12 and
    --                                              color4 against the
    --                                              background
    --   backlight   the running-tile fill          the background, moved 45%
    --                                              of the way towards the
    --                                              accent
    --   urgent      wiggle tint, urgent badge      a literal red; see below
    --   badge_bg    the count disc                 follows urgent
    --   badge_fg    the ink on the count disc      the better of color0 and
    --                                              color15 against badge_bg
    --   progress    the progress overlay           follows accent
    --   hint        recessed content that is        the background, walked out
    --               still read: the dash            towards color6 until it
    --               placeholder, its empty-state    reads 3.6:1 against the
    --               line, an unselected filter      column
    --               chip, the number of an
    --               empty sheet
    --   dim         hairlines, frame outlines,     color8
    --               tracks, a genuinely
    --               unavailable menu item
    --   overlay     dash and spread backdrop       follows background --
    --                                              colour only; its alpha is
    --                                              overlay_opacity, below
    --
    -- A derived role follows your override of the role it derives from:
    -- re-point accent and the backlight and the progress bar move with it,
    -- re-point urgent and the count disc goes too.
    --
    -- The backlight is derived rather than taken from a slot because it used
    -- to BE slot 8 -- the same slot as dim -- so "running" and "unavailable"
    -- were the same colour on every theme, and on two of the three shipped
    -- palettes that colour was invisible against the column.
    --
    -- `hint` is derived for the second half of that same defect, which the
    -- backlight split left behind: dim was also carrying the recessed content
    -- a user has to READ, and slot 8 measures 2.29:1, 1.34:1 and 1.16:1
    -- against the three shipped grounds -- so "Search applications", "No
    -- matching applications" and the numbers in the sheet grid were invisible
    -- on two of the three themes. It is settled AT 3.6:1 rather than lifted
    -- above it, because a placeholder that outreads the labels around it has
    -- stopped being a placeholder; taking color6 outright would do that on the
    -- default palette, where it measures 5.97:1.
    --
    -- 3.6:1 is also what sofi settles its own `hint` at, by the same walk, so
    -- a placeholder in the dash and one in a sofi panel recede together.
    --
    -- URGENCY IS THE ONE ROLE THE PALETTE DOES NOT CONTROL. It is a literal
    -- held outside the palette and picked by the ground it is painted on:
    --
    --   dark background   #df8787          light background   #9b2226
    --
    -- No single red stays legible on both the darkest and the lightest ground
    -- the desktop ships -- the two requirements do not overlap at all -- so
    -- one literal would be a choice of which theme to fail on. #df8787 is
    -- byte-identical to the default palette's color9, which is what urgency
    -- already resolved to there, so the default theme is unchanged by this. To
    -- put urgency back under your palette's control, that is what this block
    -- is for: `urgent = "color9"`.
    --
    -- Light or dark is MEASURED from the background's own luminance rather
    -- than configured, so a terminal colour scheme dropped into hikari.lua
    -- gets the right answer without having to say which kind it is.
    --
    -- A KNOWN LIMIT, ON ONE PALETTE. Under hikari-sakura's SAKURA DARK the
    -- accent cannot reach the 4.5:1 that text wants against the background.
    -- Its two candidates measure 2.54:1 (color12) and 1.67:1 (color4), and
    -- color13 -- which is not a candidate, because color12 is the slot the
    -- compositor marks a focused window with and that correspondence is worth
    -- more than a ratio nobody can read either way -- reaches only 2.73:1.
    -- The better of the two is chosen and the shortfall stands rather than
    -- being papered over with a colour the theme does not contain. The accent
    -- is a mark rather than body text, so it reads -- but it is under the bar,
    -- and overriding `accent` here is the way out.
    --
    -- roles = {
    --   background = "color0",
    --   foreground = "color15",
    --   accent     = "color12",
    --   backlight  = "#655982",
    --   urgent     = "#df8787",
    --   badge_bg   = "color9",
    --   badge_fg   = "color0",
    --   progress   = "color12",
    --   hint       = "#7c7986",
    --   dim        = "color8",
    --   overlay    = "color0",
    -- },

    -- The tile fill behind a running application's icon.
    --   "palette"   a palette colour, consistent with the rest of the desktop
    --   "dominant"  tinted from the icon itself, as Unity did. Sampled with a
    --               bias towards the saturated pixels, so a mostly-grey icon
    --               with one coloured mark takes that mark's colour rather
    --               than washing out to grey. An icon carrying no colour at
    --               all falls back to "palette", so the column never shows a
    --               muddy fill.
    --   "off"       no fill
    backlight = "palette",

    -- Opacity of the column itself, applied to the panel background. Not to
    -- icons or text, and -- since the key below exists -- no longer to the
    -- dash or the spread.
    opacity = 0.92,

    -- Opacity of the dash strip and the window spread's backdrop: the two
    -- surfaces that sit OVER the desktop rather than beside it.
    --
    -- This is the substitute for a blur, because a blur is not available.
    -- hikari advertises no blur protocol of any kind, and a Wayland client
    -- cannot read the screen behind itself, so there is nothing to bind and
    -- nothing to sample. What a client CAN do exactly is choose how much of
    -- the desktop it lets through, which is this key.
    --
    -- It is written to the surface as given, not arrived at by stacking
    -- translucent passes: the `overlay` role above carries the colour and this
    -- key carries its alpha, so the number here is the number the compositor
    -- blends with:
    --
    --   1.00  opaque. Nothing behind the dash or the spread shows through.
    --         This is the default, and it is the only value that guarantees
    --         it.
    --   0.90  a tenth of the desktop reads through -- enough to place the
    --         surface over the wallpaper without competing with its own text.
    --   0.75  clearly see-through. Legible over a plain wallpaper, hard work
    --         over a busy photograph.
    --   0.00  invisible backdrop; the grid appears to float on the desktop.
    --
    -- Below roughly 0.85 a detailed wallpaper starts to compete with the
    -- labels, which is the reason the default is not lower.
    overlay_opacity = 1.0,
  },

  launcher = {
    -- Desktop-file IDs, in order, pinned to the launcher.
    --
    -- THIS KEY HAS NO BUILT-IN DEFAULT. It is the one exception to the note at
    -- the top of the file: delete it and the launcher starts empty, showing
    -- only windows that are already open. The two entries below are a
    -- suggestion, not a fallback.
    --
    -- This SEEDS the list on first run only. The live order lives in
    -- $XDG_DATA_HOME/saber/favourites and wins from then on. Delete that file
    -- to seed from here again.
    --
    -- A single entry may drop the braces: `favourites = "sofi.desktop"` is
    -- read as the one-element list it plainly is, because a one-element list
    -- is the common case and a silent no-op is the worst available answer to
    -- it. items.order below takes the same shorthand.
    favourites = {
      "sofi.desktop",
      "firefox.desktop",
    },
  },

  -- Which portions the column carries, and in what order.
  --
  -- A feature compiled out with a WITH_* switch draws nothing here -- with one
  -- exception worth knowing: under WITH_DASH=NO the bfb tile is still drawn
  -- and still takes clicks, and those clicks do nothing, because there is no
  -- Dash for it to open. Leave "bfb" out of the order on such a build.
  items = {
    -- The column, first to last. The portion names are:
    --
    --   bfb       application menu button; opens the Dash
    --   apps      the band of pinned favourites and running applications
    --   sheets    current sheet, with a ten-cell switcher on click
    --   devices   mounted removable volumes, one tile each
    --   trash
    --   tray      StatusNotifierItem host, one tile per item
    --   session   suspend, reboot, shut down
    --
    -- "apps" is where the column SPLITS, and that is the whole reason the
    -- order matters: everything listed before it is pinned to the top and
    -- everything listed after it is anchored to the bottom, so the power
    -- button stays reachable however many windows are open. The band itself
    -- takes the space left between the two and scrolls when it runs out.
    --
    -- A name that is not on the list above is reported on stderr and skipped.
    -- A name written twice counts once. "apps" cannot be turned off: leave it
    -- out and it is put back directly after the bfb, where it has always been.
    order = {
      "bfb",
      "apps",
      "sheets",
      "devices",
      "trash",
      "tray",
      "session",
    },

    -- The older per-portion form, still honoured. A false here removes that
    -- portion from the order above wherever it sits, so a file written before
    -- `order` existed behaves exactly as it did. Setting one to true does not
    -- ADD a portion the order leaves out -- the order is what says what
    -- appears. There is no key for "apps".
    --
    -- bfb     = true,
    -- sheets  = true,
    -- devices = true,
    -- trash   = true,
    -- tray    = true,
    -- session = true,
  },

  session = {
    -- An EMPTY string means "use the built-in command", and what the built-in
    -- is depends on the platform. Saber installs nothing setuid or setgid and
    -- writes no sudoers rule and no polkit rule on either.
    --
    -- ON FREEBSD the three power actions go through membership of the base
    -- system's operator group, which already owns /sbin/shutdown (setuid root,
    -- group-executable) and /dev/acpi (group-writable):
    --
    --   suspend    acpiconf -s3
    --   reboot     shutdown -r now    (never /sbin/reboot, which is not
    --                                 setuid)
    --   poweroff   shutdown -p now
    --
    -- If you are not in the group, an administrator runs:
    --
    --   pw groupmod operator -m <user>
    --
    -- Without it Saber HIDES these entries rather than offering buttons that
    -- fail. There is no built-in lock or logout: the compositor exposes no
    -- lock verb and FreeBSD has no logind, so for those two an empty string
    -- means HIDE THE ENTRY. Point them at whatever you use and they appear.
    --
    -- ON LINUX all five go to logind, so an empty string is a working built-in
    -- for every one of them. An action is offered when logind says it can be
    -- done -- "challenge" counts, because that means it will ask you rather
    -- than refuse -- and hidden otherwise. One caveat on lock: logind signals
    -- whichever screen locker holds the session, and with no locker running
    -- the screen does not lock and nothing reports a failure, because that is
    -- not something the interface can be asked. Naming your locker here is the
    -- way to be certain.
    --
    -- Set a string on either platform to override the built-in -- e.g.
    -- "sudo shutdown -p now" on a host that deliberately keeps its users out
    -- of operator, or a locker of your own choosing.
    suspend  = "",
    reboot   = "",
    poweroff = "",
    lock     = "",
    logout   = "",
  },
}

-- ---------------------------------------------------------------------------
-- CONTROLLING A RUNNING PANEL
--
-- Saber is resident, so it belongs in the compositor's autostart rather than on
-- a key. Everything you would bind a key to goes through saberctl, which talks
-- to the running panel over $XDG_RUNTIME_DIR/saber.sock and links against libc
-- alone -- it runs on every keypress, so it loads nothing it does not need.
--
--   saberctl dash             open or close the application menu
--   saberctl spread [app-id]  the window spread, optionally for one application
--   saberctl launch N         start or focus the Nth launcher tile, 1-10
--   saberctl overlay on|off   show or hide the launch numbers on the tiles
--   saberctl show|hide|toggle the panel's visibility
--   saberctl sheet N          switch to sheet N, 0-9
--   saberctl pin N            move the focused window to sheet N
--   saberctl reload           re-read this file
--   saberctl status           what the panel currently thinks
--   saberctl quit
--
-- THE OVERLAY WANTS TWO BINDINGS, not one. Unity drew the launch numbers while
-- Super was held; a Wayland client cannot watch a modifier it has no keyboard
-- focus for, so the compositor has to say when the key goes down and when it
-- comes up. In hikari's own configuration:
--
--   actions {
--     saber-overlay-on  = "saberctl overlay on"
--     saber-overlay-off = "saberctl overlay off"
--   }
--
-- and bind the two to the press and the release of whichever key you hold. The
-- numbers drawn are the ones `saberctl launch N` answers to, so the overlay is
-- a legend for bindings you already have rather than a second scheme.
--
-- WHAT `reload` DOES AND DOES NOT DO. It re-reads this file and applies the
-- whole theme block, the column's width (icon_size and padding), the autohide
-- behaviour, the reveal pressure and the animation duration. SIGHUP does the
-- same thing.
--
-- A file that will not parse is refused rather than applied: `reload` answers
-- with an error and the panel keeps the settings it is already running with, so
-- a typo here cannot cost you a configured panel.
--
-- Everything else is settled when the panel starts and needs a restart:
--
--   panel.output          decides how many columns exist
--   panel.edge            decides which edge each one is anchored to
--   items                 order and booleans decide whether the tray, sheets,
--                         devices and trash subsystems are constructed at all
--   launcher.favourites   seeds the launcher, and only on first run
--   session               the five command overrides
--
-- Changing any of those means building surfaces and subsystems that do not
-- exist yet -- a restart in everything but name. Restart the panel for them.
--
-- A REFUSED SHEET SWITCH IS A WARNING, NOT AN EXIT STATUS. `saberctl sheet N`
-- answers as soon as the request is queued, because the compositor's reply
-- arrives after the control connection has been closed. If the compositor
-- declines -- it is locked, or busy, or has no focused window to pin -- Saber
-- says so on stderr with the reason. The exit status cannot carry it.
