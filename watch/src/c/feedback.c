#include "feedback.h"

#include "rb_config.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

typedef struct {
  uint8_t judgment;  // RbJudgment
  uint32_t until_ms;
} LaneFlash;

static LaneFlash s_flash[RB_LANE_COUNT];
static uint8_t s_last_judgment;
static uint32_t s_last_until_ms;

// Which lane the most recent judged hit landed in. The judgment popup belongs
// to a lane, and render.c used to work that out by scanning for a lane whose
// flash matched the judgment -- which silently defaulted to lane 0 whenever the
// flashes had expired, and drew a bottom-lane PERFECT in the top lane. The lane
// is known here; there is no reason for anyone to infer it.
static uint8_t s_last_lane = RB_LANE_NONE;

static bool s_haptics_enabled = true;
static uint32_t s_next_vibe_ms;  // earliest elapsed time a normal pulse may fire
static bool s_vibe_armed;        // false until the first hit, so 0 is not "due"

static const uint32_t k_vibe_normal[] = { RB_VIBE_NORMAL_MS };
static const uint32_t k_vibe_big[] = { RB_VIBE_BIG_MS };

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void feedback_reset(void) {
  for (uint8_t i = 0; i < RB_LANE_COUNT; i++) {
    s_flash[i].judgment = RB_JUDGE_NONE;
    s_flash[i].until_ms = 0;
  }
  s_last_judgment = RB_JUDGE_NONE;
  s_last_until_ms = 0;
  s_last_lane = RB_LANE_NONE;
  s_next_vibe_ms = 0;
  s_vibe_armed = false;
}

void feedback_set_haptics(bool enabled) {
  s_haptics_enabled = enabled;
  if (!enabled) {
    vibes_cancel();
  }
}

bool feedback_haptics_enabled(void) {
  return s_haptics_enabled;
}

void feedback_silence(void) {
  vibes_cancel();
}

// ---------------------------------------------------------------------------
// Haptics
//
// The SDK drops a vibration issued while another is ongoing rather than queueing
// it, so the policy here is about making the drops predictable:
//
//   normal notes -- rate-limit, never cancel. Every pulse the player feels is a
//     clean, full-length tap. The chart's tightest spacing is a sixteenth,
//     127ms, which is below RB_VIBE_MIN_GAP_MS -- so a fast run thins out to a
//     pulse on roughly every other note rather than stuttering into one buzz.
//
//   big notes -- cancel first, then fire. A big note is the accent of the bar
//     and must always be felt, so it is allowed to cut a normal pulse short.
//
//   misses and strays -- nothing. Beyond the oddity of buzzing for something the
//     player did not do, an auto-miss pulse would occupy the motor and, under the
//     drop rule, eat the NEXT correct hit's pulse. Vibrating on miss actively
//     degrades the feedback for correct play.
// ---------------------------------------------------------------------------

static void prv_vibe(const ChartNote *note, uint32_t elapsed_ms) {
  if (!s_haptics_enabled || quiet_time_is_active()) {
    return;
  }

  if (note != NULL && note->type == RB_NOTE_BIG) {
    vibes_cancel();
    vibes_enqueue_custom_pattern((VibePattern){
      .durations = k_vibe_big,
      .num_segments = ARRAY_LENGTH(k_vibe_big),
    });
    // Hold off the next normal pulse until this one has finished, so the accent
    // is not immediately truncated by the note after it.
    s_next_vibe_ms = elapsed_ms + RB_VIBE_BIG_MS + RB_VIBE_MIN_GAP_MS;
    s_vibe_armed = true;
    return;
  }

  if (s_vibe_armed && elapsed_ms < s_next_vibe_ms) {
    return;  // motor still busy -- this pulse would be silently dropped anyway
  }

  vibes_enqueue_custom_pattern((VibePattern){
    .durations = k_vibe_normal,
    .num_segments = ARRAY_LENGTH(k_vibe_normal),
  });
  s_next_vibe_ms = elapsed_ms + RB_VIBE_MIN_GAP_MS;
  s_vibe_armed = true;
}

// ---------------------------------------------------------------------------

void feedback_hit(uint8_t lane, RbJudgment judgment, const ChartNote *note,
                  uint32_t elapsed_ms) {
  if (judgment == RB_JUDGE_NONE) {
    return;  // stray press -- no note consumed, so nothing to report
  }

  if (lane < RB_LANE_COUNT) {
    s_flash[lane].judgment = (uint8_t)judgment;
    s_flash[lane].until_ms = elapsed_ms + RB_FLASH_MS;
  }

  // The banner lingers longer than the lane flash so a judgment is still
  // readable once the note itself has gone.
  s_last_judgment = (uint8_t)judgment;
  s_last_until_ms = elapsed_ms + (RB_FLASH_MS * 2);
  s_last_lane = lane;

  if (judgment != RB_JUDGE_MISS) {
    prv_vibe(note, elapsed_ms);
  }
}

RbJudgment feedback_lane_flash(uint8_t lane, uint32_t elapsed_ms) {
  if (lane >= RB_LANE_COUNT || elapsed_ms >= s_flash[lane].until_ms) {
    return RB_JUDGE_NONE;
  }
  return (RbJudgment)s_flash[lane].judgment;
}

uint8_t feedback_last_lane(void) {
  return s_last_lane;
}

RbJudgment feedback_last_judgment(uint32_t elapsed_ms) {
  return (elapsed_ms < s_last_until_ms) ? (RbJudgment)s_last_judgment : RB_JUDGE_NONE;
}
