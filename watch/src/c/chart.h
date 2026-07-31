#pragma once

// ---------------------------------------------------------------------------
// Note-chart data and format.
//
// This file deliberately does NOT include <pebble.h>. chart.c and game.c are
// the pure-logic half of the game, which is what lets tools/run_tests.sh build
// them with the host compiler and unit-test the judgment maths without an
// emulator. Keep it that way: the moment this needs a Pebble type, the tests
// stop building.
// ---------------------------------------------------------------------------

#include <stdbool.h>
#include <stdint.h>

// Lane order is the load-bearing part of the whole design: it must match the
// physical button order down the right edge of the watch. Index 0 is topmost,
// so the enum order IS the on-screen order -- render.c lays the lanes out by
// walking 0..RB_LANE_COUNT downward, and everything else is sized from
// RB_LANE_COUNT rather than from the number 3.
//
// Two lanes, played with UP and SELECT -- the top two buttons, adjacent. The
// thumb rests across both, so alternating hands at sixteenth-note rates does not
// require travelling the length of the button stack, which UP+DOWN did.
//
// The cost is that lane 1 breaks the "a lane sits at its button's vertical
// position" rule: SELECT is the middle button and the lower lane is the lower
// band. Nothing on screen may therefore claim otherwise -- in particular the
// lower lane's badge is a RIGHT arrow, never a DOWN one, because a down arrow
// would point at the button that is not a lane. Both earlier arrangements are
// recorded in README.md.
//
// Note the knock-on effect documented in tools/make_chart.py: with only two
// lanes the same-lane spacing rule leaves offbeats nowhere legal to go.
typedef enum {
  RB_LANE_TOP = 0,  // UP button
  RB_LANE_BOT,      // SELECT button
  RB_LANE_COUNT,
} RbLane;

// Reported for a button press that is not a gameplay lane -- DOWN, which moves
// the song selection and quits from the pause screen but never hits a note.
// Deliberately outside the enum: it must never index a lane-sized array, and
// every consumer has to decide what to do with it rather than silently treating
// it as lane 0.
#define RB_LANE_NONE 0xFF

typedef enum {
  RB_NOTE_NORMAL = 0,
  RB_NOTE_BIG,
  RB_NOTE_TYPE_COUNT,
} RbNoteType;

// 8 bytes with padding. The array is const and lives in ROM; per-note runtime
// state is a parallel array owned by game.c.
//
// INVARIANT: notes MUST be sorted ascending by hit_time_ms. Both the render
// cursor and the judging scan rely on it to avoid walking the whole chart
// every frame.
typedef struct {
  uint32_t hit_time_ms;
  uint8_t lane;  // RbLane
  uint8_t type;  // RbNoteType
} ChartNote;

typedef struct {
  const char *title;
  const char *artist;   // shown under the title in the song band
  const ChartNote *notes;
  uint16_t note_count;
  uint16_t bpm;
  uint32_t lead_in_ms;  // elapsed time of the first bar's downbeat
  uint32_t end_ms;      // elapsed time at which the results screen appears
} Chart;

// How many songs are compiled in. The title screen's selector is driven by this,
// so adding a song to SONGS in tools/make_chart.py is the whole change -- no C
// edit is needed to make it selectable.
uint8_t chart_count(void);

// Never returns NULL: an out-of-range index clamps to the first song, so a
// stale saved selection can never leave the app without a chart to play.
const Chart *chart_get(uint8_t index);

// Forward-compatible hook for loading a chart from a resource file instead of
// having it compiled in. v1 always returns false and leaves out_chart
// untouched, so callers fall back to chart_get().
//
// The intended on-disk layout, little-endian throughout:
//
//   off  size   field
//   0    4      magic "RBCH"
//   4    2      format version (currently 1)
//   6    2      bpm
//   8    4      lead_in_ms
//   12   4      end_ms
//   16   2      note_count
//   18   2      reserved (0)
//   20   32     title, NUL-padded
//   52   6*n    notes: u32 hit_time_ms, u8 lane, u8 type
//
// Implementing this means adding a resource entry, reading it with
// resource_load_byte_range(), and validating that hit_time_ms is ascending --
// the rest of the game already treats the Chart as opaque data.
bool chart_load_from_resource(uint32_t resource_id, Chart *out_chart);
