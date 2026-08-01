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

// Records a press on a lane. Lights that lane's press blink unconditionally, and
// when the press judged, its hit flash and the matching vibe too.
//
// Called for EVERY press, including strays that consumed no note: the target
// answers the button, so a player can see which lane a button drives without
// having to hit something first.
void feedback_hit(uint8_t lane, RbJudgment judgment, const ChartNote *note,
                  uint32_t elapsed_ms);

// RB_JUDGE_NONE when the lane is not currently flashing.
RbJudgment feedback_lane_flash(uint8_t lane, uint32_t elapsed_ms);

// Whether the lane's button was pressed within RB_PRESS_FLASH_MS. Independent of
// feedback_lane_flash(): a stray press blinks without ever being a judgment, and
// a judged one is both at once -- so render.c checks the flash FIRST and only
// falls back to this, or a hit would be drawn as a mere press.
bool feedback_lane_pressed(uint8_t lane, uint32_t elapsed_ms);

// Most recent judgment, for the HUD readout. RB_JUDGE_NONE once it has aged out.
RbJudgment feedback_last_judgment(uint32_t elapsed_ms);

// Lane of the most recent judged hit, or RB_LANE_NONE before the first one.
// The popup is drawn inside the lane that was struck, and that lane has to be
// reported rather than inferred from which flashes happen to be alight.
uint8_t feedback_last_lane(void);

void feedback_set_haptics(bool enabled);
bool feedback_haptics_enabled(void);

// Stops any in-flight pattern. Call when leaving play, so a pulse never
// outlives the song.
void feedback_silence(void);
