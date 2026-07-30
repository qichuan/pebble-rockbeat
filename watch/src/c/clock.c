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
static uint32_t s_ticks_at_boundary;
static time_t s_last_s;
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

  // Measure the tick rate against second BOUNDARIES, not against elapsed time.
  //
  // The tick period is not RB_CLOCK_TICK_MS: a timer asked for 10ms fires at
  // ~14ms here, and the true rate changes with load (the title screen runs one
  // timer, gameplay runs three). So it has to be measured continuously.
  //
  // The subtlety is that time() has one-second resolution. Comparing elapsed
  // time against the accumulator means comparing against a value quantised to
  // whole seconds, and every scheme built on that hunted: a cumulative average
  // could not follow a rate change, and a feedback controller oscillated because
  // it was chasing up to a second of quantisation noise.
  //
  // Counting ticks BETWEEN successive increments of time() sidesteps it
  // completely. Each increment is an exact one-second boundary, so the count is
  // an exact ticks-per-second with no quantisation error at all, and it is
  // re-measured every second so it tracks load changes immediately.
  //
  // The step is derived, never the counter assigned, so the clock stays
  // monotonic -- one that steps backwards would drag pending notes back through
  // their hit windows.
  const time_t now_s = time(NULL);
  if (now_s != s_last_s) {
    if (s_last_s != 0 && now_s == s_last_s + 1) {
      const uint32_t ticks = s_ticks - s_ticks_at_boundary;
      if (ticks >= RB_CLOCK_MIN_TICKS_PER_SEC && ticks <= RB_CLOCK_MAX_TICKS_PER_SEC) {
        // Smoothed so a single jittery second cannot swing the tempo.
        const uint32_t measured = (1000u << Q8) / ticks;
        s_step_q8 = ((s_step_q8 * (RB_CLOCK_SMOOTH - 1)) + measured) / RB_CLOCK_SMOOTH;
      }
    }
    s_last_s = now_s;
    s_ticks_at_boundary = s_ticks;
  }

  prv_schedule();
}

void clock_init(void) {
  s_accum_q8 = 0;
  s_step_q8 = (uint32_t)RB_CLOCK_TICK_MS << Q8;
  s_ticks = 0;
  s_ticks_at_boundary = 0;
  s_last_s = 0;
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
