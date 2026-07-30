#include "clock.h"

#include "rb_config.h"

// ---------------------------------------------------------------------------
// The game clock is driven by a fixed-rate tick, NOT by time_ms().
//
// The obvious implementation -- accumulate deltas of `seconds*1000 + ms` from
// time_ms() -- is what this file used to do, and it produced a clock that
// crawled and then lurched. Measured on the emery emulator: most frames
// advanced 33-38ms, but roughly once a second the clock jumped 1010ms in a
// single frame. The cause is that time_ms()'s two halves disagree there. The
// seconds field tracks real time exactly; the millisecond field, documented as
// "milliseconds since the last second", actually advances only ~150-190 per
// real second while still wrapping at 1000. So when the seconds field ticked,
// the delta became 1000 + (a few ms) and the whole playfield teleported forward
// by a second's worth of scrolling -- which is why notes appeared to arrive at
// random and could not be hit.
//
// A rate correction alone does not fix that: it repairs the AVERAGE rate while
// leaving the per-frame jitter untouched, and jitter is what ruins a rhythm
// game.
//
// So the time base is a fixed-interval AppTimer, which the firmware schedules
// against a real timer, and the only thing taken from the wall clock is the
// SECONDS field -- the half that is trustworthy -- used to discipline the tick
// rate so the song cannot drift against real time. The result is smooth by
// construction: the clock advances in equal steps and never jumps.
//
// The cost is that timestamps quantise to RB_CLOCK_TICK_MS. At 10ms that is
// comfortably inside the 45ms Perfect window, and unlike the old clock it is a
// bounded, predictable error rather than an occasional one-second lurch.
// ---------------------------------------------------------------------------

// The accumulator and the step are held in Q8 fixed point (1/256 ms). An integer
// step cannot express the truth: the emulator's tick lands near 13.5ms, and
// forcing that to 13 or 14 is a 4-7% rate error -- which is a whole beat of drift
// every twenty seconds. No floats, per house rule.
#define Q8 8

static AppTimer *s_tick;
static uint32_t s_accum_q8;
static uint32_t s_step_q8;
static uint32_t s_ticks;
static time_t s_epoch_s;
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

  s_accum_q8 += s_step_q8;
  s_ticks++;

  // Self-calibration. The step is NOT assumed to equal RB_CLOCK_TICK_MS: a timer
  // asked for 10ms does not necessarily fire every 10ms, and on the emery
  // emulator it actually fires at about 14, which left the clock running 15%
  // slow when the step was hardcoded.
  //
  // So measure it. `coarse` is the elapsed real time from the trustworthy
  // seconds field, and s_ticks is how many ticks produced it, giving the true
  // average ms per tick. Averaging since init rather than over a short window
  // matters because `coarse` is quantised to whole seconds -- that ±1s of
  // quantisation error is 33% over 3s but under 2% by a minute in, so the
  // estimate tightens as the song goes on.
  //
  // The step is only ever adjusted; the counter is never assigned. Monotonicity
  // is required (a clock that steps backwards drags pending notes back through
  // their hit windows), and a one-millisecond change of step is imperceptible
  // where a correction applied to the counter would be exactly the lurch this
  // design exists to remove.
  const uint32_t coarse = (uint32_t)(time(NULL) - s_epoch_s) * 1000u;
  if (coarse >= RB_CLOCK_CAL_MIN_MS && s_ticks > 0) {
    // +500 because `coarse` floors to whole seconds, so the true elapsed time
    // sits on average half a second above it. Without that the estimate is
    // biased low and the clock runs permanently slow.
    uint32_t measured = ((coarse + 500u) << Q8) / s_ticks;

    const uint32_t lo = (uint32_t)RB_CLOCK_TICK_MIN_MS << Q8;
    const uint32_t hi = (uint32_t)RB_CLOCK_TICK_MAX_MS << Q8;
    if (measured < lo) {
      measured = lo;
    } else if (measured > hi) {
      measured = hi;
    }

    // Residual offset trim: nudge the rate ~1.5% to pull the clock back toward
    // real time if it has already drifted, rather than assigning the counter.
    const uint32_t now_ms = s_accum_q8 >> Q8;
    if (now_ms > coarse + RB_CLOCK_SYNC_SLACK_MS) {
      measured -= measured / 64u;
    } else if (now_ms + RB_CLOCK_SYNC_SLACK_MS < coarse) {
      measured += measured / 64u;
    }
    s_step_q8 = measured;
  }

  prv_schedule();
}

void clock_init(void) {
  s_accum_q8 = 0;
  s_step_q8 = (uint32_t)RB_CLOCK_TICK_MS << Q8;
  s_ticks = 0;
  s_epoch_s = time(NULL);
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
  return s_accum_q8 >> Q8;
}
