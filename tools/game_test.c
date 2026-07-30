// Host-side tests for the judgment windows, scoring and combo.
//
// Runs off-watch via tools/run_tests.sh. Everything here is possible because
// game.c takes `now_ms` as a parameter instead of reading a clock -- the tests
// simply supply the timeline. game_start(chart, 0) anchors the song origin at
// zero, so throughout this file a "now" value IS the elapsed song time.

#include <stdio.h>
#include <string.h>

#include "chart.h"
#include "game.h"
#include "rb_config.h"

static int s_failures;
static int s_checks;

#define CHECK(cond, ...)                                     \
  do {                                                       \
    s_checks++;                                              \
    if (!(cond)) {                                           \
      s_failures++;                                          \
      printf("  FAIL %s:%d: ", __func__, __LINE__);          \
      printf(__VA_ARGS__);                                   \
      printf("\n");                                          \
    }                                                        \
  } while (0)

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

static Chart prv_make_chart(const ChartNote *notes, uint16_t count) {
  Chart c;
  memset(&c, 0, sizeof(c));
  c.title = "test";
  c.notes = notes;
  c.note_count = count;
  c.bpm = 120;
  c.lead_in_ms = 0;
  c.end_ms = 100000;
  return c;
}

static RbJudgment prv_hit(uint8_t lane, uint32_t at_ms) {
  return game_judge_hit(lane, at_ms, NULL);
}

// ---------------------------------------------------------------------------
// Judgment windows
// ---------------------------------------------------------------------------

static void test_windows(void) {
  static const ChartNote notes[] = { { 10000, RB_LANE_MID, RB_NOTE_NORMAL } };
  const Chart chart = prv_make_chart(notes, 1);

  const struct {
    int32_t offset;
    RbJudgment expect;
  } cases[] = {
    {                    0, RB_JUDGE_PERFECT },
    {      RB_PERFECT_MS,   RB_JUDGE_PERFECT },
    {     -RB_PERFECT_MS,   RB_JUDGE_PERFECT },
    {    RB_PERFECT_MS + 1, RB_JUDGE_GOOD    },
    {  -(RB_PERFECT_MS + 1), RB_JUDGE_GOOD   },
    {         RB_GOOD_MS,   RB_JUDGE_GOOD    },
    {        -RB_GOOD_MS,   RB_JUDGE_GOOD    },
    {       RB_GOOD_MS + 1, RB_JUDGE_MISS    },
    {         RB_MISS_MS,   RB_JUDGE_MISS    },
    {        -RB_MISS_MS,   RB_JUDGE_MISS    },
    {       RB_MISS_MS + 1, RB_JUDGE_NONE    },
    {     -(RB_MISS_MS + 1), RB_JUDGE_NONE   },
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    game_start(&chart, 0);
    const RbJudgment got = prv_hit(RB_LANE_MID, (uint32_t)(10000 + cases[i].offset));
    CHECK(got == cases[i].expect, "offset %+d: expected %d, got %d", (int)cases[i].offset,
          (int)cases[i].expect, (int)got);

    // A press outside the miss window must leave the note alive for its own
    // auto-miss later, rather than silently consuming it.
    if (cases[i].expect == RB_JUDGE_NONE) {
      CHECK(game_note_judgment(0) == RB_JUDGE_NONE, "offset %+d consumed the note",
            (int)cases[i].offset);
    }
  }
}

// ---------------------------------------------------------------------------
// Candidate selection
// ---------------------------------------------------------------------------

static void test_nearest_note_wins(void) {
  static const ChartNote notes[] = {
    { 10000, RB_LANE_MID, RB_NOTE_NORMAL },
    { 10200, RB_LANE_MID, RB_NOTE_NORMAL },
  };
  const Chart chart = prv_make_chart(notes, 2);

  game_start(&chart, 0);
  // Just outside Perfect, so the judgment proves which note was chosen. Derived
  // from the config rather than hardcoded -- a literal here silently changes
  // meaning the moment the windows are retuned, which is exactly what happened.
  const uint32_t offset = RB_PERFECT_MS + 10;
  CHECK(prv_hit(RB_LANE_MID, 10000 + offset) == RB_JUDGE_GOOD,
        "expected GOOD on the nearer note");
  CHECK(game_note_judgment(0) != RB_JUDGE_NONE, "first note should have been consumed");
  CHECK(game_note_judgment(1) == RB_JUDGE_NONE, "second note should be untouched");
}

static void test_tie_breaks_to_earlier(void) {
  static const ChartNote notes[] = {
    { 10000, RB_LANE_MID, RB_NOTE_NORMAL },
    { 10200, RB_LANE_MID, RB_NOTE_NORMAL },
  };
  const Chart chart = prv_make_chart(notes, 2);

  game_start(&chart, 0);
  prv_hit(RB_LANE_MID, 10100);  // exactly equidistant
  CHECK(game_note_judgment(0) != RB_JUDGE_NONE, "tie should resolve to the EARLIER note");
  CHECK(game_note_judgment(1) == RB_JUDGE_NONE, "later note should be untouched on a tie");
}

static void test_wrong_lane_is_a_stray(void) {
  static const ChartNote notes[] = { { 10000, RB_LANE_MID, RB_NOTE_NORMAL } };
  const Chart chart = prv_make_chart(notes, 1);

  game_start(&chart, 0);
  // Build a combo first, so we can prove a stray does not break it.
  CHECK(prv_hit(RB_LANE_TOP, 10000) == RB_JUDGE_NONE, "TOP press on a MID note must be a stray");
  CHECK(game_note_judgment(0) == RB_JUDGE_NONE, "stray must not consume the MID note");
}

static void test_stray_preserves_combo(void) {
  static const ChartNote notes[] = {
    {  1000, RB_LANE_MID, RB_NOTE_NORMAL },
    { 20000, RB_LANE_MID, RB_NOTE_NORMAL },
  };
  const Chart chart = prv_make_chart(notes, 2);

  game_start(&chart, 0);
  prv_hit(RB_LANE_MID, 1000);
  CHECK(game_combo() == 1, "combo should be 1 after one hit");

  // Nowhere near any note.
  CHECK(prv_hit(RB_LANE_TOP, 10000) == RB_JUDGE_NONE, "expected a stray");
  CHECK(game_combo() == 1, "a stray must NOT break the combo");
}

// ---------------------------------------------------------------------------
// Auto-miss
// ---------------------------------------------------------------------------

static void test_auto_miss(void) {
  static const ChartNote notes[] = { { 10000, RB_LANE_MID, RB_NOTE_NORMAL } };
  const Chart chart = prv_make_chart(notes, 1);

  game_start(&chart, 0);
  CHECK(game_step(10000 + RB_MISS_MS) == 0, "note must not expire inside its window");
  CHECK(game_count(RB_JUDGE_MISS) == 0, "no miss expected yet");

  CHECK(game_step(10000 + RB_MISS_MS + 1) == 1, "note should auto-miss one ms past the window");
  CHECK(game_count(RB_JUDGE_MISS) == 1, "miss tally should be 1");

  // Must not fire again on subsequent steps.
  CHECK(game_step(30000) == 0, "auto-miss must fire exactly once");
  CHECK(game_count(RB_JUDGE_MISS) == 1, "miss tally must stay at 1");
}

static void test_auto_miss_resets_combo(void) {
  static const ChartNote notes[] = {
    {  1000, RB_LANE_MID, RB_NOTE_NORMAL },
    { 10000, RB_LANE_MID, RB_NOTE_NORMAL },
  };
  const Chart chart = prv_make_chart(notes, 2);

  game_start(&chart, 0);
  prv_hit(RB_LANE_MID, 1000);
  CHECK(game_combo() == 1, "combo should be 1");

  game_step(10000 + RB_MISS_MS + 1);
  CHECK(game_combo() == 0, "auto-miss must reset the combo");
  CHECK(game_max_combo() == 1, "max combo should remember the peak");
}

static void test_hit_note_is_not_auto_missed(void) {
  static const ChartNote notes[] = { { 10000, RB_LANE_MID, RB_NOTE_NORMAL } };
  const Chart chart = prv_make_chart(notes, 1);

  game_start(&chart, 0);
  CHECK(prv_hit(RB_LANE_MID, 10000) == RB_JUDGE_PERFECT, "expected PERFECT");
  CHECK(game_step(30000) == 0, "a note already judged must never auto-miss");
  CHECK(game_count(RB_JUDGE_MISS) == 0, "miss tally should stay 0");
}

// ---------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------

static void test_score_values(void) {
  static const ChartNote normal[] = { { 10000, RB_LANE_MID, RB_NOTE_NORMAL } };
  static const ChartNote big[] = { { 10000, RB_LANE_MID, RB_NOTE_BIG } };

  Chart chart = prv_make_chart(normal, 1);
  game_start(&chart, 0);
  prv_hit(RB_LANE_MID, 10000);
  CHECK(game_score() == RB_SCORE_PERFECT, "perfect normal should score %d, got %lu",
        RB_SCORE_PERFECT, (unsigned long)game_score());

  game_start(&chart, 0);
  prv_hit(RB_LANE_MID, 10000 + RB_GOOD_MS);
  CHECK(game_score() == RB_SCORE_GOOD, "good normal should score %d, got %lu", RB_SCORE_GOOD,
        (unsigned long)game_score());

  chart = prv_make_chart(big, 1);
  game_start(&chart, 0);
  prv_hit(RB_LANE_MID, 10000);
  CHECK(game_score() == RB_SCORE_PERFECT * RB_BIG_MULTIPLIER, "perfect big should score %d, got %lu",
        RB_SCORE_PERFECT * RB_BIG_MULTIPLIER, (unsigned long)game_score());
}

static void test_combo_bonus_and_cap(void) {
  // Enough notes to push the combo past the bonus cap.
  static ChartNote notes[RB_MAX_NOTES];
  const uint16_t count = RB_COMBO_BONUS_CAP + 10;
  for (uint16_t i = 0; i < count; i++) {
    notes[i].hit_time_ms = 1000 + (uint32_t)i * 1000;
    notes[i].lane = RB_LANE_MID;
    notes[i].type = RB_NOTE_NORMAL;
  }
  const Chart chart = prv_make_chart(notes, count);

  game_start(&chart, 0);
  uint32_t expected = 0;
  for (uint16_t i = 0; i < count; i++) {
    // Bonus uses the combo as it stood BEFORE this note.
    const uint16_t prior = i;
    const uint16_t capped = (prior < RB_COMBO_BONUS_CAP) ? prior : RB_COMBO_BONUS_CAP;
    expected += RB_SCORE_PERFECT + (uint32_t)capped * RB_COMBO_BONUS_PER;

    prv_hit(RB_LANE_MID, notes[i].hit_time_ms);
    CHECK(game_score() == expected, "note %u: expected %lu, got %lu", (unsigned)i,
          (unsigned long)expected, (unsigned long)game_score());
  }
  CHECK(game_combo() == count, "combo should equal the note count");
}

// ---------------------------------------------------------------------------
// Pause / resume
// ---------------------------------------------------------------------------

static void test_pause_resume_preserves_song_time(void) {
  static const ChartNote notes[] = { { 10000, RB_LANE_MID, RB_NOTE_NORMAL } };
  const Chart chart = prv_make_chart(notes, 1);

  game_start(&chart, 0);
  game_step(5000);
  CHECK(game_elapsed_ms() == 5000, "elapsed should track the clock");

  // Pause for 3000ms of wall time, then resume.
  game_pause(5000);
  game_resume(8000);

  game_step(13000);  // 8000 clock + 5000 more = song time 10000
  CHECK(game_elapsed_ms() == 10000, "elapsed should be continuous across a pause, got %lu",
        (unsigned long)game_elapsed_ms());

  // The note is due at song time 10000, which is now clock time 13000.
  CHECK(prv_hit(RB_LANE_MID, 13000) == RB_JUDGE_PERFECT,
        "a press at the same song offset must still be PERFECT after a pause");
}

// The clock clamps any single delta to RB_CLOCK_MAX_STEP_MS and the frame timer
// is stopped while paused, so the *measured* length of a long pause is garbage.
// Resume must not depend on it.
static void test_long_pause_does_not_skip_the_song(void) {
  static const ChartNote notes[] = { { 10000, RB_LANE_MID, RB_NOTE_NORMAL } };
  const Chart chart = prv_make_chart(notes, 1);

  game_start(&chart, 0);
  game_step(5000);
  game_pause(5000);

  // An hour of wall time later, however much of it the clock actually saw.
  game_resume(3600000);

  game_step(3600000);
  CHECK(game_elapsed_ms() == 5000, "a long pause must not advance the song, got %lu",
        (unsigned long)game_elapsed_ms());
  CHECK(game_count(RB_JUDGE_MISS) == 0, "a long pause must not auto-miss anything");

  game_step(3605000);
  CHECK(prv_hit(RB_LANE_MID, 3605000) == RB_JUDGE_PERFECT,
        "the note must still be hittable after a long pause");
}

// ---------------------------------------------------------------------------
// The built-in chart
// ---------------------------------------------------------------------------

static void test_builtin_chart_is_valid(void) {
  const Chart *const chart = chart_get_builtin();
  CHECK(chart != NULL, "builtin chart must exist");
  CHECK(chart->note_count > 0, "builtin chart must have notes");
  CHECK(chart->note_count <= RB_MAX_NOTES, "builtin chart has %u notes, pool is %d",
        (unsigned)chart->note_count, RB_MAX_NOTES);

  uint32_t last_in_lane[RB_LANE_COUNT] = { 0 };
  bool seen_lane[RB_LANE_COUNT] = { false };

  for (uint16_t i = 0; i < chart->note_count; i++) {
    const ChartNote *const n = &chart->notes[i];
    CHECK(n->lane < RB_LANE_COUNT, "note %u has lane %u", (unsigned)i, (unsigned)n->lane);
    CHECK(n->type < RB_NOTE_TYPE_COUNT, "note %u has type %u", (unsigned)i, (unsigned)n->type);

    if (i > 0) {
      CHECK(n->hit_time_ms >= chart->notes[i - 1].hit_time_ms,
            "chart must be sorted ascending; note %u breaks it", (unsigned)i);
    }

    // THE invariant the generated chart has to respect. Two notes in one lane
    // closer than 2*RB_MISS_MS have overlapping judgment windows, which means a
    // single press sits inside both and they stop being individually hittable.
    // The chart generator enforces SAME_LANE_MIN_MS (250ms) precisely because it
    // clears this bound: the chart is every melody note, so the tightest
    // same-lane interval is an eighth at 118 BPM (254ms).
    if (seen_lane[n->lane]) {
      const uint32_t gap = n->hit_time_ms - last_in_lane[n->lane];
      CHECK(gap > 2 * RB_MISS_MS, "lane %u notes only %lums apart at %lums -- windows overlap",
            (unsigned)n->lane, (unsigned long)gap, (unsigned long)n->hit_time_ms);
    }
    seen_lane[n->lane] = true;
    last_in_lane[n->lane] = n->hit_time_ms;
  }

  CHECK(chart->end_ms > chart->notes[chart->note_count - 1].hit_time_ms,
        "end_ms must trail the last note");
}

// A full perfect playthrough of the real chart: every note hit dead on, with
// the frame loop stepping in between exactly as it would on the watch.
static void test_perfect_playthrough(void) {
  const Chart *const chart = chart_get_builtin();
  game_start(chart, 0);

  uint32_t expected = 0;
  for (uint16_t i = 0; i < chart->note_count; i++) {
    const ChartNote *const n = &chart->notes[i];
    game_step(n->hit_time_ms);

    const uint16_t capped = (i < RB_COMBO_BONUS_CAP) ? i : RB_COMBO_BONUS_CAP;
    uint32_t base = RB_SCORE_PERFECT;
    if (n->type == RB_NOTE_BIG) {
      base *= RB_BIG_MULTIPLIER;
    }
    expected += base + (uint32_t)capped * RB_COMBO_BONUS_PER;

    const RbJudgment got = prv_hit(n->lane, n->hit_time_ms);
    CHECK(got == RB_JUDGE_PERFECT, "note %u should be PERFECT, got %d", (unsigned)i, (int)got);
  }
  game_step(chart->end_ms);

  CHECK(game_count(RB_JUDGE_PERFECT) == chart->note_count, "every note should be PERFECT");
  CHECK(game_count(RB_JUDGE_MISS) == 0, "a perfect run should have no misses");
  CHECK(game_max_combo() == chart->note_count, "max combo should equal the note count");
  CHECK(game_score() == expected, "expected score %lu, got %lu", (unsigned long)expected,
        (unsigned long)game_score());
  CHECK(game_is_finished(), "chart should report finished at end_ms");
}

// Nobody plays perfectly. Offset every press into the Good window and confirm
// the combo survives -- a chart that only works at zero offset would be a
// judgment bug hiding behind a lucky test.
static void test_sloppy_playthrough_keeps_combo(void) {
  const Chart *const chart = chart_get_builtin();
  game_start(chart, 0);

  for (uint16_t i = 0; i < chart->note_count; i++) {
    const ChartNote *const n = &chart->notes[i];
    game_step(n->hit_time_ms);
    const RbJudgment got = prv_hit(n->lane, n->hit_time_ms + RB_GOOD_MS);
    CHECK(got == RB_JUDGE_GOOD, "note %u should be GOOD, got %d", (unsigned)i, (int)got);
  }

  CHECK(game_count(RB_JUDGE_GOOD) == chart->note_count, "every note should be GOOD");
  CHECK(game_count(RB_JUDGE_MISS) == 0, "a late-but-inside-window run should have no misses");
  CHECK(game_max_combo() == chart->note_count, "combo should survive a consistently late run");
}

// The render and judging scans both start from this cursor, so it must never
// walk backwards.
static void test_cursor_is_monotonic(void) {
  const Chart *const chart = chart_get_builtin();
  game_start(chart, 0);

  uint16_t last = 0;
  for (uint32_t t = 0; t <= chart->end_ms; t += RB_FRAME_MS) {
    game_step(t);
    const uint16_t cursor = game_first_live();
    CHECK(cursor >= last, "cursor went backwards at t=%lu: %u -> %u", (unsigned long)t,
          (unsigned)last, (unsigned)cursor);
    last = cursor;
  }
  CHECK(last == chart->note_count, "cursor should reach the end of the chart, got %u",
        (unsigned)last);
}

// ---------------------------------------------------------------------------

int main(void) {
  test_windows();
  test_nearest_note_wins();
  test_tie_breaks_to_earlier();
  test_wrong_lane_is_a_stray();
  test_stray_preserves_combo();
  test_auto_miss();
  test_auto_miss_resets_combo();
  test_hit_note_is_not_auto_missed();
  test_score_values();
  test_combo_bonus_and_cap();
  test_pause_resume_preserves_song_time();
  test_long_pause_does_not_skip_the_song();
  test_builtin_chart_is_valid();
  test_perfect_playthrough();
  test_sloppy_playthrough_keeps_combo();
  test_cursor_is_monotonic();

  if (s_failures == 0) {
    printf("OK: %d checks passed\n", s_checks);
    return 0;
  }
  printf("FAILED: %d of %d checks\n", s_failures, s_checks);
  return 1;
}
