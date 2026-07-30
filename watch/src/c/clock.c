#include "clock.h"

#include "rb_config.h"

// ---------------------------------------------------------------------------
// The game clock reads time_ms() where that works, and reconstructs the
// milliseconds from a tick where it does not. Which one is in use is decided at
// runtime, by measurement -- see "Direct mode" below.
//
// The history matters, because it is why the fallback exists at all. This
// project recorded time_ms()'s two halves disagreeing: the seconds field exact,
// the millisecond field advancing only ~150-190 per real second while still
// wrapping at 1000. A `seconds*1000 + ms` clock built on that crawls and then
// lurches 1010ms in a single frame, which teleports the playfield forward by a
// second of scrolling and makes the game unhittable. That is what the tick was
// built to work around.
//
// It does not reproduce on the current emulator, where the field measures
// 1004ms and 982ms of advance per second with no stalls. So it was either an
// older tool version or a misattribution -- but a clock is not the place to bet
// on which, so the workaround stays as a tested fallback and the choice is made
// from evidence at runtime rather than from this comment.
//
// The fallback's problem: whole seconds are far too coarse for a rhythm game, so
// the gap between them has to be filled. A fixed-interval AppTimer supplies the
// subdivision -- but its period is NOT RB_CLOCK_TICK_MS. A timer asked for 10ms
// fires nearer 11ms at rest, and during app startup it is slower still and
// changes fast: the frame interval alone was measured falling 69ms -> 37ms over
// the first six seconds as the app settled. So the tick period has to be
// measured, and the measurement is always slightly stale.
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

// ---------------------------------------------------------------------------
// Direct mode: use time_ms() and run no timer at all.
//
// Everything above exists to work around a millisecond field that does not
// count milliseconds. Where the field DOES work, reconstructing from a tick what
// the platform already reports is both wasteful and worse:
//
//   * it costs an AppTimer wakeup every RB_CLOCK_TICK_MS, all song long, on the
//     same app task that has to draw the frame;
//   * and the reconstruction is only as good as its period estimate. Estimate
//     high and the interpolation saturates against the clamp and the clock
//     STALLS until the next second; estimate low and it falls short and JUMPS
//     at the boundary. Either way it is a hitch once a second, every second,
//     which is precisely the shape of "laggy" that a rhythm game cannot afford
//     -- it moves the notes relative to their own hit windows.
//
// So the field is TESTED rather than assumed, on two counts: how far it ADVANCES
// across one real second (summing forward deltas, a wrap counting as +1000), and
// whether it ever STALLS across a whole tick. Rate alone is not enough -- a
// field that jumps in coarse steps sums to the right total per second while
// standing still in between, and standing still is the stutter this is meant to
// remove.
//
// The obvious test, "how high does it get within a second", does not work, and
// was tried: a field advancing 190 per second while still wrapping at 1000 spans
// a different 190-wide band each second, so roughly one second in five it peaks
// near 999 and looks perfect. Measuring the advance is immune to that, because
// it does not care where in the range the band sits.
//
// Two consecutive good seconds and the clock switches to reading time_ms()
// directly and cancels the tick for good. The detection is also the safety
// check: direct mode is only ever entered on evidence that the field works.
// ---------------------------------------------------------------------------

static bool s_direct;                // sticky: a property of the platform
static uint32_t s_ms_advance;        // summed forward motion of the ms field
static uint16_t s_prev_sample_ms;
static uint8_t s_stalls_in_second;   // samples where the field did not move at all
static uint8_t s_good_seconds;
static time_t s_direct_anchor_s;
static uint32_t s_direct_base_ms;    // keeps the switch continuous
static uint32_t s_last_reported_ms;  // monotonic guard, direct mode only

static void prv_tick(void *data);

static void prv_schedule(void) {
  if (s_running && s_tick == NULL) {
    s_tick = app_timer_register(RB_CLOCK_TICK_MS, prv_tick, NULL);
  }
}

// Interpolated (tick) reading. Also the value direct mode is anchored to, so the
// switch between them cannot show a step.
static uint32_t prv_interpolated_ms(void) {
  uint32_t within_ms = ((s_ticks - s_ticks_at_boundary) * s_period_q8) >> Q8;
  // Never reach past the boundary being filled towards. This clamp is what makes
  // monotonicity structural -- see the header comment.
  if (within_ms > RB_CLOCK_MAX_INTERP_MS) {
    within_ms = RB_CLOCK_MAX_INTERP_MS;
  }
  return s_base_ms + within_ms;
}

static void prv_enter_direct(time_t now_s, uint16_t now_ms) {
#if RB_DEBUG_LOG_AUDIO
  APP_LOG(APP_LOG_LEVEL_DEBUG, "clock: time_ms() verified, dropping the tick timer");
#endif
  s_direct_anchor_s = now_s;
  // Anchor so the first direct reading equals the last interpolated one; the two
  // then advance at the same rate, so the changeover is invisible.
  s_direct_base_ms = prv_interpolated_ms() - now_ms;
  s_last_reported_ms = 0;
  s_direct = true;
  clock_stop();       // the tick has no purpose now -- stop paying for it
  s_running = true;   // ...but the clock itself is still running
}

static void prv_tick(void *data) {
  (void)data;
  s_tick = NULL;

  s_ticks++;

  time_t sampled_s = 0;
  uint16_t sampled_ms = 0;
  time_ms(&sampled_s, &sampled_ms);
  // Forward motion since the last tick, with a wrap counted as a full turn.
  const uint32_t delta = (uint32_t)((sampled_ms + 1000u - s_prev_sample_ms) % 1000u);
  s_ms_advance += delta;
  if (delta == 0u && s_stalls_in_second < 255u) {
    s_stalls_in_second++;   // the field did not move across a whole tick
  }
  s_prev_sample_ms = sampled_ms;

  const time_t now_s = sampled_s;
  if (now_s != s_last_s) {
    if (s_last_s != 0) {
      const time_t gap_s = now_s - s_last_s;
      if (gap_s == 1) {
        // Does the millisecond field actually sweep a whole second? Judged on
        // the second just finished, and only on clean one-second steps.
        // Two conditions, and both matter. The RATE has to be right, or the
        // clock is simply wrong. The GRANULARITY has to be fine, or the clock is
        // right on average while standing still between updates -- which for a
        // scrolling playfield is the very stutter this is meant to remove.
        if (s_ms_advance >= RB_CLOCK_DIRECT_MIN_ADVANCE_MS &&
            s_ms_advance <= RB_CLOCK_DIRECT_MAX_ADVANCE_MS &&
            s_stalls_in_second <= RB_CLOCK_DIRECT_MAX_STALLS) {
          s_good_seconds++;
        } else {
          s_good_seconds = 0;
        }
#if RB_DEBUG_LOG_AUDIO
        // Says out loud which clock it is running, because which one is right
        // depends on the machine. On a watch expect advance~1000 and a switch to
        // direct mode; on the emulator, ~190 and no switch.
        APP_LOG(APP_LOG_LEVEL_DEBUG, "clock: ms_advance=%lu stalls=%u good=%u ticks=%lu",
                (unsigned long)s_ms_advance, (unsigned)s_stalls_in_second,
                (unsigned)s_good_seconds, (unsigned long)(s_ticks - s_ticks_at_boundary));
#endif
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
    s_ms_advance = 0;
    s_stalls_in_second = 0;

    if (s_good_seconds >= RB_CLOCK_DIRECT_GOOD_SECONDS) {
      prv_enter_direct(now_s, sampled_ms);
      return;  // deliberately not rescheduled -- direct mode runs no timer
    }
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
  s_ms_advance = 0;
  s_stalls_in_second = 0;
  s_prev_sample_ms = 0;
  s_good_seconds = 0;
  s_running = true;

  // s_direct is NOT reset: whether time_ms() works is a property of the machine,
  // not of this song, so it is established once and kept.
  if (s_direct) {
    time_t now_s = 0;
    uint16_t now_ms = 0;
    time_ms(&now_s, &now_ms);
    s_direct_anchor_s = now_s;
    s_direct_base_ms = 0u - now_ms;   // so the first reading is 0
    s_last_reported_ms = 0;
    return;                            // no timer in direct mode
  }
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
  if (!s_direct) {
    return prv_interpolated_ms();
  }

  time_t now_s = 0;
  uint16_t now_ms = 0;
  time_ms(&now_s, &now_ms);
  const uint32_t now =
      s_direct_base_ms + (uint32_t)(now_s - s_direct_anchor_s) * 1000u + now_ms;

  // The seconds and milliseconds fields are read as one call but are not one
  // register; a sample taken across the wrap can show the new second with the
  // old millisecond and go momentarily backwards. Cheap to clamp, and a clock
  // that steps back drags pending notes through their hit windows.
  if (now < s_last_reported_ms) {
    return s_last_reported_ms;
  }
  s_last_reported_ms = now;
  return now;
}
