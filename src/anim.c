/* Script function and purpose: Easing curves and the frame-driven tween clock
described in include/saber/anim.h. Nothing here reads a clock of its own: every
entry point takes the timestamp, which is what lets the panel drive motion from
wl_callback.done and keeps the whole module testable without a compositor. */

#include <math.h>

#include <glib.h>

#include <saber/anim.h>

struct saber_clock {
  GPtrArray *tweens; /* borrowed struct saber_tween * */
  int64_t now_ms;
  int64_t base_ms; /* accumulated wrap offset for saber_clock_stamp */
  uint32_t last_raw;
  bool stamped;
  bool busy;
  bool dormant; /* nothing was running at the last advance */
};

double
saber_ease(enum saber_easing easing, double t)
{
  if (t <= 0.0) {
    return 0.0;
  }

  if (t >= 1.0) {
    return 1.0;
  }

  switch (easing) {
  case SABER_EASE_OUT_CUBIC: {
    double inverse = 1.0 - t;

    return 1.0 - inverse * inverse * inverse;
  }

  case SABER_EASE_IN_OUT_CUBIC:
    if (t < 0.5) {
      return 4.0 * t * t * t;
    } else {
      double inverse = -2.0 * t + 2.0;

      return 1.0 - (inverse * inverse * inverse) / 2.0;
    }

  /* Action purpose: A decaying cosine rather than a true spring solve. It is
  zero at t=0, overshoots once around t=0.3 and has decayed to within a quarter
  percent of 1.0 by t=1, where the clamp above lands it exactly -- which is all
  a 150ms launch throb needs and costs one exp and one cos. */
  case SABER_EASE_SPRING:
    return 1.0 - exp(-6.0 * t) * cos(6.0 * t);

  case SABER_EASE_LINEAR:
  default:
    return t;
  }
}

double
saber_anim_wiggle(double phase)
{
  return sin(phase * 2.0 * G_PI);
}

double
saber_anim_throb(double phase, double depth)
{
  return 1.0 + depth * sin(phase * G_PI);
}

void
saber_tween_init(struct saber_tween *tween, double value)
{
  tween->from = value;
  tween->to = value;
  tween->value = value;
  tween->start_ms = 0;
  tween->duration_ms = 0;
  tween->easing = SABER_EASE_LINEAR;
  tween->running = false;
  tween->repeat = false;
}

static void
tween_begin(struct saber_tween *tween,
    double from,
    double to,
    int duration_ms,
    enum saber_easing easing,
    int64_t now_ms,
    bool repeat)
{
  tween->from = from;
  tween->to = to;
  tween->value = from;
  tween->start_ms = now_ms;
  tween->duration_ms = duration_ms > 0 ? duration_ms : 0;
  tween->easing = easing;
  tween->repeat = repeat;
  tween->running = true;

  /* Action purpose: A zero-length cycle cannot repeat. Without this a repeating
  tween started with duration 0 -- which is what `animation-ms = 0` produces --
  stays running for ever, because saber_tween_advance answers `running = repeat`
  on the zero-duration path. The clock is then permanently busy and its owner
  repaints for ever: exactly the failure that once held a core for an hour
  (D-036). A repeat of nothing lands on its target and stops. */
  if (tween->duration_ms == 0) {
    tween->value = to;
    tween->running = false;
  }
}

void
saber_tween_start(struct saber_tween *tween,
    double from,
    double to,
    int duration_ms,
    enum saber_easing easing,
    int64_t now_ms)
{
  tween_begin(tween, from, to, duration_ms, easing, now_ms, false);
}

void
saber_tween_start_repeating(struct saber_tween *tween,
    double from,
    double to,
    int duration_ms,
    enum saber_easing easing,
    int64_t now_ms)
{
  tween_begin(tween, from, to, duration_ms, easing, now_ms, true);
}

void
saber_tween_stop(struct saber_tween *tween)
{
  if (!tween->running) {
    return;
  }

  tween->running = false;
  tween->repeat = false;
  tween->value = tween->to;
}

bool
saber_tween_advance(struct saber_tween *tween, int64_t now_ms)
{
  if (!tween->running) {
    return false;
  }

  /* Same reasoning as tween_begin: a cycle of no length is not an animation, so
  it settles rather than repeating for ever. */
  if (tween->duration_ms <= 0) {
    tween->value = tween->to;
    tween->running = false;

    return false;
  }

  double elapsed = (double)(now_ms - tween->start_ms);

  /* A frame stamp behind the start means the clock was restamped under us;
  hold at the first value rather than running the curve backwards. */
  if (elapsed < 0.0) {
    elapsed = 0.0;
  }

  double t = elapsed / (double)tween->duration_ms;

  if (tween->repeat) {
    t = fmod(t, 1.0);
  } else if (t >= 1.0) {
    tween->value = tween->to;
    tween->running = false;

    return false;
  }

  tween->value =
      tween->from + (tween->to - tween->from) * saber_ease(tween->easing, t);

  return true;
}

struct saber_clock *
saber_clock_create(void)
{
  struct saber_clock *clock = g_new0(struct saber_clock, 1);

  clock->tweens = g_ptr_array_new();

  return clock;
}

void
saber_clock_destroy(struct saber_clock *clock)
{
  if (clock == NULL) {
    return;
  }

  g_ptr_array_free(clock->tweens, TRUE);
  g_free(clock);
}

void
saber_clock_add(struct saber_clock *clock, struct saber_tween *tween)
{
  guint at;

  if (g_ptr_array_find(clock->tweens, tween, &at)) {
    return;
  }

  g_ptr_array_add(clock->tweens, tween);
}

void
saber_clock_remove(struct saber_clock *clock, struct saber_tween *tween)
{
  g_ptr_array_remove_fast(clock->tweens, tween);
}

int64_t
saber_clock_stamp(struct saber_clock *clock, uint32_t frame_ms)
{
  /* Action purpose: The first frame stamp is rebased onto wherever the clock
  already stood, so tweens started before any frame arrived -- a hover during
  the very first paint -- do not see the compositor's arbitrary epoch land on
  them as one enormous elapsed time and finish instantly.

  A clock whose loop has stopped is re-anchored the same way, and for the same
  reason. While nothing runs no frames arrive, so now_ms stands still, but
  base_ms goes on mapping the compositor's stamp onto the epoch of the first
  frame ever seen -- the time this returns keeps pace with the wall clock. A
  tween started during that gap is anchored to the frozen now_ms by
  saber_clock_now, so it would see the whole gap as elapsed on its very first
  frame: finished before it drew once, and finished without ever returning
  true, so its owner never damages and the settled value is never painted.
  Discarding the gap is what makes saber_clock_now a valid anchor again, and
  costs nothing: no tween can observe time in which none of them ran. */
  if (!clock->stamped || clock->dormant) {
    clock->stamped = true;
    clock->dormant = false;
    clock->base_ms = clock->now_ms - (int64_t)frame_ms;
  } else if (frame_ms < clock->last_raw) {
    clock->base_ms += (int64_t)UINT32_MAX + 1;
  }

  clock->last_raw = frame_ms;

  return clock->base_ms + (int64_t)frame_ms;
}

bool
saber_clock_advance(struct saber_clock *clock, int64_t now_ms)
{
  clock->now_ms = now_ms;
  clock->busy = false;

  for (guint i = 0; i < clock->tweens->len; i++) {
    if (saber_tween_advance(g_ptr_array_index(clock->tweens, i), now_ms)) {
      clock->busy = true;
    }
  }

  /* Nothing is running, so this is the last frame the owner asks for: the next
  stamp, whenever it comes, re-anchors rather than counting the gap. */
  clock->dormant = !clock->busy;

  return clock->busy;
}

void
saber_clock_reset(struct saber_clock *clock)
{
  clock->dormant = true;
}

int64_t
saber_clock_now(const struct saber_clock *clock)
{
  return clock->now_ms;
}

bool
saber_clock_busy(const struct saber_clock *clock)
{
  for (guint i = 0; i < clock->tweens->len; i++) {
    const struct saber_tween *tween = g_ptr_array_index(clock->tweens, i);

    if (tween->running) {
      return true;
    }
  }

  return false;
}
