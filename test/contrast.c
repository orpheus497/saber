/* Script function and purpose: Hold the theme's colour invariants and the
easing curves to measured values. The role map is derived from contrast rather
than from fixed palette indices, and every defect that drove that change was
found by measurement, so nothing but a measurement keeps it fixed -- the next
palette edit otherwise reintroduces the whole class silently.

This drives saber_theme_init directly rather than going through saber.lua, so
it links theme.c and anim.c and needs no configuration parser and no
compositor. The WCAG arithmetic below is written out here on purpose: a test
that reused the implementation's own helper would agree with it while both were
wrong. */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <saber/anim.h>
#include <saber/theme.h>

/* hikari-sakura's three shipped palettes, verbatim from the `ui { palette }`
blocks of its hikari.conf. They are positionally identical and semantically
divergent -- slot 1 is red on Sakura, muted dark red on Sakura Dark and muted
emerald GREEN on Sakura Light -- which is the whole reason a role may not be a
fixed slot index. */
static const char *const palettes[][SABER_PALETTE_SLOTS] = {
  { "#2b1e3a", "#c96464", "#df9f87", "#e4b382", "#8e7cc3", "#b18fc7",
    "#9fa0a6", "#d4d4d9", "#5e5966", "#df8787", "#f2bda8", "#f5cf9e",
    "#aba0d9", "#cfaedc", "#b8b9be", "#f0edf2" },
  { "#161618", "#802b2b", "#8c5338", "#782639", "#63255a", "#70264b",
    "#525257", "#9e9ea3", "#2e2e32", "#a34242", "#ab6c4e", "#9e3b52",
    "#873b7d", "#963d6b", "#6f6f75", "#cacace" },
  { "#d9d9d9", "#437a54", "#357a80", "#2a6c85", "#3d598a", "#4a789c",
    "#a1a5b0", "#4b515d", "#e8e9ec", "#529968", "#459ea6", "#3a8ba8",
    "#5274b0", "#5f9bc9", "#757a85", "#1e2126" },
};

static const char *const palette_names[] = {
  "Sakura", "Sakura Dark", "Sakura Light",
};

#define PALETTE_COUNT ((int)(sizeof(palettes) / sizeof(palettes[0])))

/* The compiled-in fallback is checked as a fourth palette, because a panel
started with no saber.lua and no hikari.lua renders from it and it is the one
palette no configuration can correct. */
#define FALLBACK_PALETTE (-1)

/* WCAG 2.1 AA: 4.5:1 for normal text, 3:1 for a graphical object. */
#define AA_TEXT 4.5
#define AA_MARK 3.0

/* A fill, not text, so AA does not apply -- but the backlight is the only
signifier that an application is running, and the slot it used to share with
`dim` measured 1.16:1 against the ground on Sakura Light, which is invisible.
This is the floor that catches that returning. */
#define BACKLIGHT_FLOOR 1.35

static int failures;

static void
fail(const char *palette, const char *what, const char *detail)
{
  printf("  FAIL  %-12s %-22s %s\n", palette, what, detail);
  failures++;
}

/* Function purpose: sRGB channel to linear light. Written from the WCAG 2.1
definition rather than shared with theme.c so the two can disagree. */
static double
channel(double c)
{
  return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static double
luminance(struct saber_color c)
{
  return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b);
}

static double
contrast(struct saber_color a, struct saber_color b)
{
  double la = luminance(a);
  double lb = luminance(b);
  double hi = la > lb ? la : lb;
  double lo = la > lb ? lb : la;

  return (hi + 0.05) / (lo + 0.05);
}

static bool
same_colour(struct saber_color a, struct saber_color b)
{
  return fabs(a.r - b.r) < 1e-9 && fabs(a.g - b.g) < 1e-9 &&
      fabs(a.b - b.b) < 1e-9;
}

static void
format_hex(struct saber_color c, char out[8])
{
  snprintf(out, 8, "#%02x%02x%02x", (int)(c.r * 255.0 + 0.5),
      (int)(c.g * 255.0 + 0.5), (int)(c.b * 255.0 + 0.5));
}

/* Action purpose: The ruled exemptions, and the only ones. Sakura Dark carries
no slot bright enough to serve as `accent`: color12 measures 2.54:1 against its
own ground and color4 1.67:1. color13 reaches 2.73:1 but is not a candidate --
`accent` is chosen between color12 and color4 because color12 is the slot
hikari paints a focused window's marker with, so a highlighted tile and a
focused window agree about what "chosen" looks like, and that correspondence is
worth more than two tenths of a ratio nobody can read either way. The shortfall
was therefore accepted rather than rescued with a colour from outside the
palette, and `progress` follows `accent` so it inherits the same shortfall by
construction.

Each is recorded as one named palette-and-role pair carrying the value the role
actually resolves to, NOT as a lowered threshold: the same shortfall on any
other palette, or on any other role of this one, still fails -- and so does a
regression that makes even an exempted pair worse than it is here. */
static const struct {
  const char *palette;
  const char *role;
  double granted;
} exemptions[] = {
  { "Sakura Dark", "accent", 2.54 },
  { "Sakura Dark", "progress", 2.54 },
};

static bool
exempt(const char *palette, const char *role, double measured)
{
  for (size_t i = 0; i < sizeof(exemptions) / sizeof(exemptions[0]); i++) {
    if (strcmp(exemptions[i].palette, palette) != 0 ||
        strcmp(exemptions[i].role, role) != 0) {
      continue;
    }

    /* Worse than when the exemption was granted is a regression, not a
    continuation of the exemption. A little slack absorbs rounding only. */
    if (measured + 0.01 < exemptions[i].granted) {
      return false;
    }

    return true;
  }

  return false;
}

static void
check_text(const char *palette,
    const char *role,
    struct saber_color fg,
    struct saber_color ground)
{
  double ratio = contrast(fg, ground);

  if (ratio >= AA_TEXT || exempt(palette, role, ratio)) {
    return;
  }

  char hex[8];
  char detail[96];

  format_hex(fg, hex);
  snprintf(detail, sizeof(detail), "%s on ground is %.2f:1, below AA %.1f:1",
      hex, ratio, AA_TEXT);
  fail(palette, role, detail);
}

static void
check_theme(const char *name, const struct saber_color *palette)
{
  struct saber_theme theme;

  saber_theme_init(&theme, palette, 0.92, 1.0);

  char hex[8];

  char hint_hex[8];

  format_hex(theme.hint, hint_hex);
  printf("  %-12s light_ground=%d  hint=%s at %.2f:1\n", name,
      saber_theme_is_light(&theme) ? 1 : 0, hint_hex,
      contrast(theme.hint, theme.background));

  /* Text-bearing roles. */
  check_text(name, "foreground", theme.foreground, theme.background);
  check_text(name, "accent", theme.accent, theme.background);
  check_text(name, "urgent", theme.urgent, theme.background);

  /* `hint` carries recessed content that is still read: the dash placeholder,
  its empty-state line, an unselected filter chip, the number of an empty
  sheet. It is held to the graphical-object floor and no higher, because
  holding it to body AA would stop it reading as recessed at all -- the whole
  point of the role.

  This is the floor the comment here used to claim while the code checked only
  that the colour existed. It did not catch what it was written to catch:
  `dim` was carrying this content on palette[8], at 1.34:1 and 1.16:1 against
  two of the three shipped grounds. */
  double hint = contrast(theme.hint, theme.background);

  if (hint < AA_MARK && !exempt(name, "hint", hint)) {
    char detail[96];

    format_hex(theme.hint, hex);
    snprintf(detail, sizeof(detail),
        "%s on ground is %.2f:1, below the 3:1 graphical-object floor", hex,
        hint);
    fail(name, "hint", detail);
  }

  /* The same guard the backlight split needed, for the same reason: the two
  roles are only separate while they resolve to different colours, and a map
  that quietly recollided them would put the hairlines and the placeholder back
  on one value without failing anything. */
  if (same_colour(theme.hint, theme.dim)) {
    fail(name, "hint", "identical to dim -- the roles have recollided");
  }

  /* `dim` is the structural role -- hairlines, frame outlines, tracks, and a
  menu item that is genuinely unavailable, which WCAG 2.1 exempts from any
  contrast requirement. It is held to being a colour and nothing more, and now
  that is what the check says as well as what it does. */
  if (contrast(theme.dim, theme.background) < 1.0) {
    fail(name, "dim", "not a colour");
  }

  /* The count disc carries a numeral, so the pair is text. */
  double badge = contrast(theme.badge_fg, theme.badge_bg);

  if (badge < AA_TEXT) {
    char detail[96];

    format_hex(theme.badge_fg, hex);
    snprintf(detail, sizeof(detail), "%s on badge_bg is %.2f:1, below AA",
        hex, badge);
    fail(name, "badge_fg/badge_bg", detail);
  }

  /* The focus arrow and the progress overlay are marks, not text. */
  if (contrast(theme.progress, theme.background) < AA_MARK &&
      !exempt(name, "progress", contrast(theme.progress, theme.background))) {
    fail(name, "progress", "below the 3:1 graphical-object floor");
  }

  /* The defect that drove the split: `backlight` and `dim` were both
  palette[8], so a running application and an unavailable action were painted
  identically on every palette. */
  if (same_colour(theme.backlight, theme.dim)) {
    fail(name, "backlight", "identical to dim -- the roles have recollided");
  }

  double lit = contrast(theme.backlight, theme.background);

  if (lit < BACKLIGHT_FLOOR) {
    char detail[96];

    format_hex(theme.backlight, hex);
    snprintf(detail, sizeof(detail), "%s is %.2f:1 against the column",
        hex, lit);
    fail(name, "backlight", detail);
  }

  /* `badge_bg` follows `urgent` and `progress` follows `accent`, so a palette
  that breaks one would otherwise break the other silently. */
  if (!same_colour(theme.badge_bg, theme.urgent)) {
    fail(name, "badge_bg", "no longer follows urgent");
  }

  if (!same_colour(theme.progress, theme.accent)) {
    fail(name, "progress", "no longer follows accent");
  }

  /* The overlay role carries colour only; its alpha is overlay_opacity, which
  is what the dash strip and the spread backdrop already read. A value below
  1.0 here would silently weaken the documented guarantee that
  overlay-opacity = 1.0 lets nothing through. */
  if (fabs(theme.overlay.a - 1.0) > 1e-9) {
    fail(name, "overlay", "carries an alpha of its own");
  }

  /* Urgency is a literal pair chosen by ground, because no single red clears
  AA against both the Sakura Dark and the Sakura Light grounds: the required
  luminance windows do not intersect. */
  const char *want = saber_theme_is_light(&theme) ? "#9b2226" : "#df8787";
  struct saber_color expected;

  if (!saber_color_parse(want, &expected)) {
    fail(name, "urgent", "test fixture will not parse");
  } else if (!same_colour(theme.urgent, expected)) {
    char detail[96];

    format_hex(theme.urgent, hex);
    snprintf(detail, sizeof(detail), "is %s, expected %s for this ground",
        hex, want);
    fail(name, "urgent", detail);
  }
}

static void
check_palettes(void)
{
  printf("palette roles\n");

  for (int p = 0; p < PALETTE_COUNT; p++) {
    struct saber_color palette[SABER_PALETTE_SLOTS];
    bool parsed = true;

    for (int slot = 0; slot < SABER_PALETTE_SLOTS; slot++) {
      if (!saber_color_parse(palettes[p][slot], &palette[slot])) {
        fail(palette_names[p], "fixture", palettes[p][slot]);
        parsed = false;
      }
    }

    if (parsed) {
      check_theme(palette_names[p], palette);
    }
  }

  /* NULL selects the compiled-in fallback. */
  check_theme("fallback", NULL);
}

/* Function purpose: Sakura is the default palette and the one every existing
installation is already looking at, so the role rework is only safe if it
leaves that palette alone. #df8787 is byte-identical to Sakura's own color9,
which is what `urgent` resolved to before the literal pair existed. */
static void
check_sakura_unchanged(void)
{
  struct saber_color palette[SABER_PALETTE_SLOTS];

  for (int slot = 0; slot < SABER_PALETTE_SLOTS; slot++) {
    if (!saber_color_parse(palettes[0][slot], &palette[slot])) {
      fail("Sakura", "fixture", "will not parse");

      return;
    }
  }

  struct saber_theme theme;

  saber_theme_init(&theme, palette, 0.92, 1.0);

  if (!same_colour(theme.urgent, palette[9])) {
    fail("Sakura", "urgent", "is not color9 -- the default theme changed");
  }

  if (!same_colour(theme.background, palette[0])) {
    fail("Sakura", "background", "is not color0");
  }

  if (!same_colour(theme.foreground, palette[15])) {
    fail("Sakura", "foreground", "is not color15");
  }
}

/* Function purpose: Anchor the luminance arithmetic itself. These five ratios
are recorded independently in sofi's doc/palette.sasi for the same palette, so
a drift here means the test's own maths moved, not the theme's. */
static void
check_known_ratios(void)
{
  static const struct {
    const char *fg;
    const char *bg;
    double want;
  } known[] = {
    { "#d4d4d9", "#2b1e3a", 10.54 },
    { "#f0edf2", "#2b1e3a", 13.41 },
    { "#aba0d9", "#2b1e3a", 6.49 },
    { "#8e7cc3", "#2b1e3a", 4.31 },
    { "#9fa0a6", "#2b1e3a", 5.97 },
  };

  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
    struct saber_color fg;
    struct saber_color bg;

    if (!saber_color_parse(known[i].fg, &fg) ||
        !saber_color_parse(known[i].bg, &bg)) {
      fail("anchor", "fixture", known[i].fg);

      continue;
    }

    double got = contrast(fg, bg);

    if (fabs(got - known[i].want) > 0.01) {
      char detail[96];

      snprintf(detail, sizeof(detail), "%s on %s is %.2f:1, expected %.2f:1",
          known[i].fg, known[i].bg, got, known[i].want);
      fail("anchor", "contrast", detail);
    }
  }
}

static void
check_easing(void)
{
  static const enum saber_easing curves[] = {
    SABER_EASE_LINEAR,
    SABER_EASE_OUT_CUBIC,
    SABER_EASE_IN_OUT_CUBIC,
    SABER_EASE_SPRING,
  };

  for (size_t i = 0; i < sizeof(curves) / sizeof(curves[0]); i++) {
    if (saber_ease(curves[i], 0.0) != 0.0) {
      fail("easing", "endpoint", "not 0 at t=0");
    }

    if (saber_ease(curves[i], 1.0) != 1.0) {
      fail("easing", "endpoint", "not 1 at t=1");
    }

    /* Clamped outside the unit interval rather than extrapolated. */
    if (saber_ease(curves[i], -1.0) != 0.0 ||
        saber_ease(curves[i], 2.0) != 1.0) {
      fail("easing", "clamp", "extrapolates outside 0..1");
    }
  }

  /* The spring's overshoot, measured. Its comment once claimed a peak "around
  t=0.3", which is where the curve is still only 1.0376. */
  double peak = 0.0;
  double peak_at = 0.0;

  for (double t = 0.0; t <= 1.0; t += 0.0005) {
    double v = saber_ease(SABER_EASE_SPRING, t);

    if (v > peak) {
      peak = v;
      peak_at = t;
    }
  }

  if (fabs(peak - 1.0670) > 0.0005) {
    char detail[96];

    snprintf(detail, sizeof(detail), "peak %.4f, expected 1.0670", peak);
    fail("easing", "spring peak", detail);
  }

  if (fabs(peak_at - 0.3925) > 0.002) {
    char detail[96];

    snprintf(detail, sizeof(detail), "peaks at t=%.4f, expected 0.3925",
        peak_at);
    fail("easing", "spring peak", detail);
  }

  /* An idle tile draws at exactly its nominal size: the throb is a scale
  factor and must be 1.0 at both ends of its phase. */
  if (fabs(saber_anim_throb(0.0, 0.08) - 1.0) > 1e-9 ||
      fabs(saber_anim_throb(1.0, 0.08) - 1.0) > 1e-9) {
    fail("easing", "throb", "not 1.0 at the ends of its phase");
  }

  if (fabs(saber_anim_wiggle(0.0)) > 1e-9 ||
      fabs(saber_anim_wiggle(0.5)) > 1e-9) {
    fail("easing", "wiggle", "not zero-crossing at 0 and 0.5");
  }
}

/* Function purpose: animation-ms = 0 is how the whole of the panel's motion is
turned off, and a repeating tween of no length once kept the clock permanently
busy and its owner repainting for ever. A zero-length cycle must settle on its
target and stop. */
static void
check_zero_duration(void)
{
  struct saber_tween tween;

  saber_tween_init(&tween, 0.0);
  saber_tween_start_repeating(&tween, 0.0, 1.0, 0, SABER_EASE_LINEAR, 0);

  if (saber_tween_running(&tween)) {
    fail("anim", "zero duration", "a repeating tween of no length still runs");
  }

  if (fabs(saber_tween_value(&tween) - 1.0) > 1e-9) {
    fail("anim", "zero duration", "did not settle on its target");
  }

  if (saber_tween_advance(&tween, 1000)) {
    fail("anim", "zero duration", "still advancing after settling");
  }

  /* The same for a one-shot, which is the ordinary animation-ms = 0 case. */
  saber_tween_init(&tween, 0.0);
  saber_tween_start(&tween, 0.0, 1.0, 0, SABER_EASE_OUT_CUBIC, 0);

  if (saber_tween_running(&tween) ||
      fabs(saber_tween_value(&tween) - 1.0) > 1e-9) {
    fail("anim", "zero duration", "one-shot of no length did not settle");
  }
}

/* Function purpose: A cancelled animation settles where it would have
finished. Freezing mid-curve is what leaves a tile permanently half-scaled. */
static void
check_cancel_settles(void)
{
  struct saber_tween tween;

  saber_tween_init(&tween, 0.0);
  saber_tween_start(&tween, 0.0, 1.0, 200, SABER_EASE_OUT_CUBIC, 0);
  saber_tween_advance(&tween, 50);
  saber_tween_stop(&tween);

  if (saber_tween_running(&tween)) {
    fail("anim", "cancel", "still running after stop");
  }

  if (fabs(saber_tween_value(&tween) - 1.0) > 1e-9) {
    fail("anim", "cancel", "froze mid-curve instead of settling");
  }
}

int
main(void)
{
  check_known_ratios();
  check_palettes();
  check_sakura_unchanged();
  check_easing();
  check_zero_duration();
  check_cancel_settles();

  if (failures > 0) {
    printf("\n%d failure%s\n", failures, failures == 1 ? "" : "s");

    return 1;
  }

  printf("\nall checks passed\n");

  return 0;
}
