/* Script function and purpose: The tween clock. Every animated quantity in the
panel -- hover glow, launch throb, urgent wiggle -- is a tween advanced from a
timestamp the compositor supplied, never from a wall clock, so the panel's
motion is paced by presentation rather than by how often the main loop happens
to wake. */

#if !defined(SABER_ANIM_H)
#define SABER_ANIM_H

#include <stdbool.h>
#include <stdint.h>

enum saber_easing {
  SABER_EASE_LINEAR,
  SABER_EASE_OUT_CUBIC,
  SABER_EASE_IN_OUT_CUBIC,
  /* Damped overshoot: crosses 1.0 and settles back. The launch throb. */
  SABER_EASE_SPRING,
};

struct saber_tween {
  double from, to;
  double value;
  int64_t start_ms;
  int duration_ms;
  enum saber_easing easing;
  bool running;
  bool repeat;
};

double
saber_ease(enum saber_easing easing, double t);

/* Function purpose: A repeating 0..1 phase mapped to a symmetric -1..1 swing,
so the caller multiplies by an amplitude rather than reimplementing the curve
at every call site. */
double
saber_anim_wiggle(double phase);

/* Half-cycle swell, 1.0 at both ends of the phase and `depth` above 1.0 at its
middle -- a scale factor, so an idle tile draws at exactly its nominal size. */
double
saber_anim_throb(double phase, double depth);

void
saber_tween_init(struct saber_tween *tween, double value);

void
saber_tween_start(struct saber_tween *tween,
    double from,
    double to,
    int duration_ms,
    enum saber_easing easing,
    int64_t now_ms);

/* Loops from `from` to `to` forever; only saber_tween_stop ends it. */
void
saber_tween_start_repeating(struct saber_tween *tween,
    double from,
    double to,
    int duration_ms,
    enum saber_easing easing,
    int64_t now_ms);

/* Function purpose: End the tween where it would have finished. Settling on
the target rather than freezing mid-curve is what keeps a cancelled animation
from leaving a tile permanently half-scaled. */
void
saber_tween_stop(struct saber_tween *tween);

/* Returns whether the tween is still running after the step. */
bool
saber_tween_advance(struct saber_tween *tween, int64_t now_ms);

static inline double
saber_tween_value(const struct saber_tween *tween)
{
  return tween->value;
}

static inline bool
saber_tween_running(const struct saber_tween *tween)
{
  return tween->running;
}

struct saber_clock;

struct saber_clock *
saber_clock_create(void);

void
saber_clock_destroy(struct saber_clock *clock);

/* Function purpose: Registered tweens are borrowed, never owned -- an owner
embeds its tweens in its own per-item state and takes them out again when that
state dies. Adding the same tween twice is a no-op. */
void
saber_clock_add(struct saber_clock *clock, struct saber_tween *tween);

void
saber_clock_remove(struct saber_clock *clock, struct saber_tween *tween);

/* Function purpose: Turn wl_callback.done's 32-bit millisecond stamp into a
monotonic 64-bit one. The wire value wraps roughly every 49 days, and a wrap
mid-session would otherwise throw every live tween into the far future. */
int64_t
saber_clock_stamp(struct saber_clock *clock, uint32_t frame_ms);

/* Advances every registered tween; returns true while any is still running,
which is the panel's signal to ask for another frame callback. */
bool
saber_clock_advance(struct saber_clock *clock, int64_t now_ms);

/* Function purpose: Re-anchor the clock on its next frame, for an owner that
stops the loop by a route the clock cannot see. saber_clock_advance notices the
loop settling on its own, but a surface torn down mid-animation -- the dash and
the spread both destroy theirs on hide -- takes its frame callbacks with it
while tweens are still marked running, so no advance ever records the stop.
Calling this from the hide path keeps the next open from starting its tween
against a stale timestamp. */
void
saber_clock_reset(struct saber_clock *clock);

int64_t
saber_clock_now(const struct saber_clock *clock);

bool
saber_clock_busy(const struct saber_clock *clock);

#endif
