#include "chart.h"

// ---------------------------------------------------------------------------
// "Granite Groove" -- the built-in demo song.
//
// 120 BPM, so a beat is 500ms and a 4/4 bar is 2000ms. Bar 1 lands on the
// 2400ms lead-in, which gives the player roughly one bar of empty runway to
// read the lanes before the first note arrives (a note is visible for ~1.53s
// before it reaches the target, so anything shorter and the first note is
// already on screen at song start).
//
// 60 notes over 16 bars, shaped in four sections:
//   bars  1-4   quarter notes, one lane at a time -- teaches lane<->button
//   bars  5-8   eighths appear, lanes start alternating
//   bars  9-12  syncopation, big notes on downbeats
//   bars 13-16  dense finale, closing on two big MID hits
//
// The times were generated and checked by script, not by hand. Two invariants
// were verified at authoring time and are re-checked by tools/game_test.c:
// the array is sorted ascending, and no two notes in the SAME lane are closer
// than 200ms -- closer than that and the +-160ms miss window of one note would
// overlap the next, making them individually unjudgeable.
// ---------------------------------------------------------------------------

static const ChartNote s_demo_notes[] = {
  // -- bars 1-4: quarters, teaching the mapping ------------------------------
  {  2400, RB_LANE_MID, RB_NOTE_NORMAL },
  {  3400, RB_LANE_MID, RB_NOTE_NORMAL },
  {  4400, RB_LANE_MID, RB_NOTE_NORMAL },
  {  5400, RB_LANE_MID, RB_NOTE_NORMAL },
  {  6400, RB_LANE_TOP, RB_NOTE_NORMAL },
  {  7400, RB_LANE_TOP, RB_NOTE_NORMAL },
  {  8400, RB_LANE_BOT, RB_NOTE_NORMAL },
  {  9400, RB_LANE_BOT, RB_NOTE_NORMAL },
  {  9900, RB_LANE_MID, RB_NOTE_BIG    },
  // -- bars 5-8: eighths -----------------------------------------------------
  { 10400, RB_LANE_MID, RB_NOTE_NORMAL },
  { 10900, RB_LANE_MID, RB_NOTE_NORMAL },
  { 11400, RB_LANE_TOP, RB_NOTE_NORMAL },
  { 11900, RB_LANE_MID, RB_NOTE_NORMAL },
  { 12400, RB_LANE_MID, RB_NOTE_NORMAL },
  { 12900, RB_LANE_MID, RB_NOTE_NORMAL },
  { 13400, RB_LANE_BOT, RB_NOTE_NORMAL },
  { 13900, RB_LANE_MID, RB_NOTE_NORMAL },
  { 14400, RB_LANE_TOP, RB_NOTE_NORMAL },
  { 14900, RB_LANE_TOP, RB_NOTE_NORMAL },
  { 15400, RB_LANE_MID, RB_NOTE_NORMAL },
  { 15650, RB_LANE_MID, RB_NOTE_NORMAL },
  { 16400, RB_LANE_BOT, RB_NOTE_BIG    },
  { 17400, RB_LANE_MID, RB_NOTE_NORMAL },
  { 17900, RB_LANE_MID, RB_NOTE_NORMAL },
  // -- bars 9-12: syncopation, bigs on downbeats -----------------------------
  { 18400, RB_LANE_MID, RB_NOTE_BIG    },
  { 19150, RB_LANE_TOP, RB_NOTE_NORMAL },
  { 19400, RB_LANE_MID, RB_NOTE_NORMAL },
  { 19900, RB_LANE_BOT, RB_NOTE_NORMAL },
  { 20400, RB_LANE_MID, RB_NOTE_NORMAL },
  { 20650, RB_LANE_MID, RB_NOTE_NORMAL },
  { 20900, RB_LANE_MID, RB_NOTE_NORMAL },
  { 21650, RB_LANE_TOP, RB_NOTE_NORMAL },
  { 22150, RB_LANE_BOT, RB_NOTE_NORMAL },
  { 22400, RB_LANE_TOP, RB_NOTE_BIG    },
  { 22900, RB_LANE_MID, RB_NOTE_NORMAL },
  { 23150, RB_LANE_MID, RB_NOTE_NORMAL },
  { 23900, RB_LANE_BOT, RB_NOTE_NORMAL },
  { 24400, RB_LANE_BOT, RB_NOTE_NORMAL },
  { 24900, RB_LANE_MID, RB_NOTE_NORMAL },
  { 25400, RB_LANE_TOP, RB_NOTE_NORMAL },
  { 25900, RB_LANE_MID, RB_NOTE_NORMAL },
  { 26150, RB_LANE_MID, RB_NOTE_NORMAL },
  // -- bars 13-16: finale ----------------------------------------------------
  { 26400, RB_LANE_MID, RB_NOTE_NORMAL },
  { 26900, RB_LANE_MID, RB_NOTE_NORMAL },
  { 27150, RB_LANE_TOP, RB_NOTE_NORMAL },
  { 27400, RB_LANE_MID, RB_NOTE_NORMAL },
  { 27900, RB_LANE_BOT, RB_NOTE_NORMAL },
  { 28400, RB_LANE_BOT, RB_NOTE_NORMAL },
  { 28900, RB_LANE_MID, RB_NOTE_NORMAL },
  { 29150, RB_LANE_MID, RB_NOTE_NORMAL },
  { 29400, RB_LANE_TOP, RB_NOTE_NORMAL },
  { 29900, RB_LANE_MID, RB_NOTE_NORMAL },
  { 30400, RB_LANE_TOP, RB_NOTE_NORMAL },
  { 30650, RB_LANE_MID, RB_NOTE_NORMAL },
  { 30900, RB_LANE_MID, RB_NOTE_NORMAL },
  { 31400, RB_LANE_BOT, RB_NOTE_NORMAL },
  { 31650, RB_LANE_MID, RB_NOTE_NORMAL },
  { 31900, RB_LANE_MID, RB_NOTE_NORMAL },
  { 32400, RB_LANE_MID, RB_NOTE_BIG    },
  { 33400, RB_LANE_MID, RB_NOTE_BIG    },
};

// end_ms trails the last note by 2s: long enough for its miss window to close
// and for the hit flash to fade before the results screen takes over.
static const Chart s_demo_chart = {
  .title = "Granite Groove",
  .notes = s_demo_notes,
  .note_count = (uint16_t)(sizeof(s_demo_notes) / sizeof(s_demo_notes[0])),
  .bpm = 120,
  .lead_in_ms = 2400,
  .end_ms = 35400,
};

const Chart *chart_get_builtin(void) {
  return &s_demo_chart;
}

bool chart_load_from_resource(uint32_t resource_id, Chart *out_chart) {
  // Not implemented in v1 -- see the format documented in chart.h. Callers are
  // expected to fall back to chart_get_builtin().
  (void)resource_id;
  (void)out_chart;
  return false;
}
