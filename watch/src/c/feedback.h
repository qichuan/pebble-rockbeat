#pragma once

// ---------------------------------------------------------------------------
// Output-side feedback: haptics and hit flashes.
//
// Flash state is keyed off the *game clock*, not a frame counter, so it decays
// deterministically regardless of how many frames actually got drawn.
//
// Nothing here ever feeds back into timing or scoring -- these are outputs.
// ---------------------------------------------------------------------------

#include <pebble.h>
#include <stdbool.h>
#include <stdint.h>

#include "chart.h"
#include "game.h"

void feedback_reset(void);

// Records a judged hit: lights that lane's flash and fires the matching vibe.
void feedback_hit(uint8_t lane, RbJudgment judgment, const ChartNote *note,
                  uint32_t elapsed_ms);

// RB_JUDGE_NONE when the lane is not currently flashing.
RbJudgment feedback_lane_flash(uint8_t lane, uint32_t elapsed_ms);

// Most recent judgment, for the HUD readout. RB_JUDGE_NONE once it has aged out.
RbJudgment feedback_last_judgment(uint32_t elapsed_ms);

void feedback_set_haptics(bool enabled);
bool feedback_haptics_enabled(void);

// Stops any in-flight pattern. Call when leaving play, so a pulse never
// outlives the song.
void feedback_silence(void);
