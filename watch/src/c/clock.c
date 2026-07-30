#include "clock.h"

#include "rb_config.h"

// ---------------------------------------------------------------------------
// The game clock is real seconds, interpolated by a tick.
//
// Two things about this platform force the shape of this file.
//
// FIRST: time_ms()'s two halves disagree. The seconds field tracks real time
// exactly; the millisecond field, documented as "milliseconds since the last
// second", actually advances only ~150-190 per real second while still wrapping
// at 1000. A `seconds*1000 + ms` clock therefore crawls and then lurches 1010ms
// in a single frame, which teleports the whole playfield forward by a second's
// worth of scrolling and makes the game unhittable. So the millisecond field is
// never read. Only the seconds field is trusted.
//
// SECOND: whole seconds are far too coarse for a rhythm game, so the gap between
// them has to be filled. A fixed-interval AppTimer supplies the subdivision --
// but its period is NOT RB_CLOCK_TICK_MS. A timer asked for 10ms fires nearer
// 11ms at rest, and during app startup it is slower still and changes fast: the
// frame interval alone was measured falling 69ms -> 37ms over the first six
// seconds as the app settled. So the tick period has to be measured, and the
// measurement is always slightly stale.
//
// ---------------------------------------------------------------------------
// Why the tick INTERPOLATES rather than accumulates
//
// The previous design accumulated a measured step: each tick added its estimated
// period to a running total. That makes the tick period a prediction of the next
// second, measured over the last one -- and a prediction is exactly the wrong
// instrument when the thing being predicted is changing by 2x between
// consecutive seconds, which is what startup does.
//
// It failed in both directions, and both were measured here:
//   * Seeded at 10ms against a true ~11ms tick, and smoothed at 1/8, the clock
//     needed ~25 seconds to converge and ran ~14% slow throughout. Time lost
//     that way was never recovered, because only the rate was ever corrected,
//     never the accumulated error.
//   * Correcting the accumulated error instead, but still accumulating, moved
//     the failure rather than removing it: a period measured during a slow
//     startup second and applied to a fast one drove the clock to 1.8x for
//     several seconds, and the correction loop then had to unwind it.
//
// The music makes any of this immediately audible. The speaker plays in real
// time and cannot be steered, so every millisecond the clock is wrong is a
// millisecond the music sits away from the notes: the runaway above put it
// 1346ms ahead by the first chunk boundary and ~2100ms ahead by the fourth.
//
// So the clock does not predict. Real seconds are authoritative:
//
//     now = s_base_ms + min(ticks_since_boundary * period, 1000)
//
// s_base_ms advances by exactly 1000 on each increment of time(). The tick only
// fills in the current second, and its contribution is clamped so it can never
// reach past the boundary it is filling towards.
//
// The consequences are worth stating, because they are the whole reason for the
// shape:
//   * Error CANNOT accumulate. The clock is exact at every second boundary
//     whatever the tick has been doing, so there is no transient to converge out
//     of and no drift over a song of any length.
//   * A bad period estimate is bounded to the second it occurs in. Too high and
//     the interpolation saturates and waits for the boundary; too low and it
//     falls short and the boundary takes up the slack. Neither leaks into the
//     next second.
//   * Monotonicity is structural, not a rule to be observed. Within a second the
//     tick count only rises; across one, the new base is the old base plus 1000
//     and the interpolation it replaces was at most 1000. So the clock can never
//     step backwards -- which matters because a clock that did would drag
//     pending notes back through their hit windows.
//
// The period is still measured, and still by counting ticks between successive
// increments of time() rather than against elapsed time. time() has one-second
// resolution, so anything built on elapsed time is chasing up to a second of
// quantisation noise -- an earlier cumulative average could not follow a rate
// change and a feedback controller oscillated. Each increment of time() is an
// exact one-second boundary, so a tick count between two of them is an exact
// ticks-per-second with no quantisation error at all. It is now used only to
// shape the interpolation, never to carry time.
// ---------------------------------------------------------------------------

// The period is held in Q8 fixed point (1/256 ms). An integer cannot express the
// truth: the tick lands near 10.8ms here, and rounding to 10 or 11 is a 2-7%
// error across the second it is filling. No floats, per house rule.
#define Q8 8

static AppTimer *s_tick;
static uint32_t s_base_ms;           // clock value at the last second boundary
static uint32_t s_period_q8;         // smoothed estimate of the true tick period
static uint32_t s_ticks;
static uint32_t s_ticks_at_boundary;
static time_t s_last_s;
static bool s_measured;
static bool s_running;

static void prv_tick(void *data);

static void prv_schedule(void) {
  if (s_running && s_tick == NULL) {
    s_tick = app_timer_register(RB_CLOCK_TICK_MS, prv_tick, NULL);
  }
}

static void prv_tick(void *data) {
  (void)data;
  s_tick = NULL;

  s_ticks++;

  const time_t now_s = time(NULL);
  if (now_s != s_last_s) {
    if (s_last_s != 0) {
      const time_t gap_s = now_s - s_last_s;
      if (gap_s == 1) {
        const uint32_t ticks = s_ticks - s_ticks_at_boundary;
        if (ticks >= RB_CLOCK_MIN_TICKS_PER_SEC && ticks <= RB_CLOCK_MAX_TICKS_PER_SEC) {
          const uint32_t measured_q8 = (1000u << Q8) / ticks;
          if (!s_measured) {
            // Take the first measurement whole. Smoothing towards it from the
            // RB_CLOCK_TICK_MS seed only prolongs the one second in which the
            // interpolation is knowingly wrong.
            s_period_q8 = measured_q8;
            s_measured = true;
          } else {
            // Smoothed so one jittery second cannot visibly swing the tempo.
            s_period_q8 =
                ((s_period_q8 * (RB_CLOCK_SMOOTH - 1)) + measured_q8) / RB_CLOCK_SMOOTH;
          }
        }
        s_base_ms += 1000u;
      } else if (gap_s > 1 && gap_s <= RB_CLOCK_MAX_GAP_S) {
        // The tick did not run for a few seconds -- the app was descheduled.
        // Real time passed and the song really is that much further on, so the
        // clock follows it rather than pretending otherwise.
        s_base_ms += 1000u * (uint32_t)gap_s;
      } else {
        // A backwards or implausibly large jump is a wall-clock STEP (an NTP
        // correction, a timezone change), not elapsed time. Credit one second
        // and carry on; teleporting the playfield on a settings change would be
        // a far worse failure than losing a moment of song.
        s_base_ms += 1000u;
      }
    }
    s_last_s = now_s;
    s_ticks_at_boundary = s_ticks;
  }

  prv_schedule();
}

void clock_init(void) {
  s_base_ms = 0;
  s_period_q8 = (uint32_t)RB_CLOCK_TICK_MS << Q8;
  s_ticks = 0;
  s_ticks_at_boundary = 0;
  s_last_s = 0;
  s_measured = false;
  s_running = true;
  prv_schedule();
}

void clock_stop(void) {
  s_running = false;
  if (s_tick != NULL) {
    app_timer_cancel(s_tick);
    s_tick = NULL;
  }
}

uint32_t clock_now_ms(void) {
  uint32_t within_ms = ((s_ticks - s_ticks_at_boundary) * s_period_q8) >> Q8;
  // Never reach past the boundary being filled towards. This clamp is what makes
  // monotonicity structural -- see the header comment.
  if (within_ms > RB_CLOCK_MAX_INTERP_MS) {
    within_ms = RB_CLOCK_MAX_INTERP_MS;
  }
  return s_base_ms + within_ms;
}
