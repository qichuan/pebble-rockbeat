#pragma once

// ---------------------------------------------------------------------------
// Song clock consumers, judgment, score and combo.
//
// Like chart.h this does NOT include <pebble.h>, and must not start to. The
// module is deliberately sealed off from the watch so tools/run_tests.sh can
// build it with the host compiler.
//
// The consequence worth understanding: game_judge_hit() *returns* a judgment,
// it never fires sound or haptics itself. The caller in main.c turns the
// return value into outputs. That is what makes "audio is an output, never an
// input to timing" a property of the module graph rather than of discipline --
// game.c literally cannot call the speaker.
//
// Note also that this module never reads a clock. Elapsed time is passed in,
// which is what makes the judgment logic reproducible in a test.
// ---------------------------------------------------------------------------

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chart.h"

typedef enum {
  RB_JUDGE_NONE = 0,  // also means "not yet judged" in the per-note state array
  RB_JUDGE_PERFECT,
  RB_JUDGE_GOOD,
  RB_JUDGE_MISS,
  RB_JUDGE_COUNT,
} RbJudgment;

typedef enum {
  RB_SCREEN_TITLE = 0,
  RB_SCREEN_PLAYING,
  RB_SCREEN_PAUSED,
  RB_SCREEN_RESULTS,
  RB_SCREEN_COUNT,
} RbScreen;

// Rewinds all per-note state and the score, and anchors the song timeline so
// that elapsed == 0 at `now_ms`. `now_ms` comes from clock_now_ms(); this
// module never reads a clock itself, which is exactly what makes it testable.
void game_start(const Chart *chart, uint32_t now_ms);

// Recomputes elapsed and retires any note whose window has closed. Returns how
// many notes newly auto-missed this call.
uint16_t game_step(uint32_t now_ms);

// Judge a button press on `lane` that happened at clock time `press_now_ms`.
//
// Picks the nearest unjudged note in that lane within +-RB_MISS_MS, breaking
// ties toward the earlier note. Returns RB_JUDGE_NONE for a stray press with
// no candidate note -- strays are ignored outright and do NOT break the combo,
// so mashing is never punished harder than standing still.
//
// out_note may be NULL; when non-NULL it receives the note that was consumed
// (or NULL for a stray) so the caller can pick a sound and a vibe strength.
//
// Note this takes the raw clock reading, not an elapsed value: the press is
// timestamped the instant the button goes down, and converting to song time
// here keeps that conversion off the latency-critical path.
RbJudgment game_judge_hit(uint8_t lane, uint32_t press_now_ms, const ChartNote **out_note);

// Pause freezes elapsed where it stands; resume shifts the song ORIGIN forward
// by the paused duration, so elapsed is continuous across the gap and no other
// code needs a special case.
void game_pause(uint32_t now_ms);
void game_resume(uint32_t now_ms);

uint32_t game_elapsed_ms(void);

// Clock reading at which elapsed == 0. Only needed by the debug freeze harness,
// which has to convert a desired elapsed value back into a clock value.
uint32_t game_origin_ms(void);

bool game_is_finished(void);

uint32_t game_score(void);
uint16_t game_combo(void);
uint16_t game_max_combo(void);
uint16_t game_count(RbJudgment judgment);

// Accuracy as a whole percent, weighting a Good at half a Perfect. Used for the
// results rank.
uint16_t game_accuracy_pct(void);

// Which screen the app is showing. Lives here rather than in main.c so render.c
// can dispatch on it without depending on the app shell.
RbScreen game_screen(void);
void game_set_screen(RbScreen screen);

// Index of the earliest note not yet resolved -- the starting point for both
// the render scan and the judging scan, so neither walks the whole chart.
uint16_t game_first_live(void);

// Per-note judgment state, for the renderer to skip notes already resolved.
RbJudgment game_note_judgment(uint16_t index);

const Chart *game_chart(void);
