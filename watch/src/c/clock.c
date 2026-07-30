#include "clock.h"

#include "rb_config.h"

static time_t s_last_s;
static uint16_t s_last_ms;
static uint32_t s_accum_ms;

void clock_init(void) {
  s_accum_ms = 0;
  s_last_s = 0;
  s_last_ms = time_ms(&s_last_s, NULL);
}

// Wall time is only ever consulted as a *delta*, never as an absolute. Two
// separate reasons, both of which would be bugs if ignored:
//
//   - epoch_seconds * 1000 is ~1.78e12 in 2026. That does not fit in any 32-bit
//     type, so an absolute ms timestamp would need 64-bit maths on every read.
//   - the wall clock is not monotonic. A phone sync or an NTP correction can
//     move time() in either direction mid-song. Anchoring the song to an
//     absolute start means a backwards jump rewinds the music (every pending
//     note teleports into the future) and a forwards jump instantly auto-misses
//     everything on screen.
//
// Clamping the delta kills both: a backwards step contributes 0, so the song
// can never rewind, and a forwards step contributes at most one clamped frame,
// so notes are never teleported past their hit windows.
//
// The cost is that a genuine stall longer than RB_CLOCK_MAX_STEP_MS makes the
// song run slow. That is the right trade -- what is drawn and what is scored
// both come from this one counter, so they cannot disagree with each other.
uint32_t clock_now_ms(void) {
  time_t s = 0;
  uint16_t ms = 0;
  time_ms(&s, &ms);

  // Clamp the seconds delta BEFORE multiplying, or a large clock jump overflows
  // the int32 multiply before the millisecond clamp below ever gets to run.
  const time_t delta_s = s - s_last_s;
  int32_t delta;
  if (delta_s > 2 || delta_s < -2) {
    delta = (delta_s > 0) ? RB_CLOCK_MAX_STEP_MS : 0;
  } else {
    delta = (int32_t)delta_s * 1000 + ((int32_t)ms - (int32_t)s_last_ms);
  }

  if (delta < 0) {
    delta = 0;
  } else if (delta > RB_CLOCK_MAX_STEP_MS) {
    delta = RB_CLOCK_MAX_STEP_MS;
  }

  s_last_s = s;
  s_last_ms = ms;
  s_accum_ms += (uint32_t)delta;
  return s_accum_ms;
}
