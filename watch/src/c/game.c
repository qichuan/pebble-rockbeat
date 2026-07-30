#include "game.h"

#include "rb_config.h"

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

static const Chart *s_chart;
static uint8_t s_judged[RB_MAX_NOTES];  // RbJudgment per note
static uint16_t s_first_live;
static uint32_t s_score;
static uint16_t s_combo;
static uint16_t s_max_combo;
static uint16_t s_counts[RB_JUDGE_COUNT];

// Song timeline. s_origin_ms is the clock reading at which elapsed == 0; every
// elapsed value in the game is (clock - origin). Keeping the origin rather than
// an accumulated elapsed is what makes pause/resume a one-line adjustment.
static uint32_t s_origin_ms;
static uint32_t s_paused_at_ms;
static uint32_t s_elapsed_ms;

static RbScreen s_screen = RB_SCREEN_TITLE;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static int32_t prv_abs32(int32_t v) {
  return v < 0 ? -v : v;
}

// The cursor only steps over notes that are fully resolved. A note judged out
// of order (the player hit a later lane early) leaves a hole behind the
// cursor, which is fine -- both scans skip resolved notes explicitly. What the
// cursor buys us is that neither scan ever restarts from index 0.
static void prv_advance_cursor(void) {
  while (s_first_live < s_chart->note_count && s_judged[s_first_live] != RB_JUDGE_NONE) {
    s_first_live++;
  }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void game_start(const Chart *chart, uint32_t now_ms) {
  s_chart = chart;
  s_first_live = 0;
  s_score = 0;
  s_combo = 0;
  s_max_combo = 0;
  s_origin_ms = now_ms;
  s_paused_at_ms = 0;
  s_elapsed_ms = 0;

  for (uint16_t i = 0; i < RB_JUDGE_COUNT; i++) {
    s_counts[i] = 0;
  }
  for (uint16_t i = 0; i < RB_MAX_NOTES; i++) {
    s_judged[i] = RB_JUDGE_NONE;
  }
}

// Pause records where the song got to; resume RE-ANCHORS the origin to that
// point. Deliberately not "origin += pause duration": clock_now_ms() clamps any
// single delta to RB_CLOCK_MAX_STEP_MS, and the frame timer is stopped while
// paused, so the measured duration of a pause longer than a second is simply
// wrong. Measuring it would shift the origin too little and teleport the song
// forward, auto-missing every note on screen. Re-anchoring never consults the
// pause duration at all, so it is correct for a pause of any length.
void game_pause(uint32_t now_ms) {
  (void)now_ms;
  s_paused_at_ms = s_elapsed_ms;
}

void game_resume(uint32_t now_ms) {
  s_origin_ms = now_ms - s_paused_at_ms;
}

uint32_t game_elapsed_ms(void) {
  return s_elapsed_ms;
}

uint32_t game_origin_ms(void) {
  return s_origin_ms;
}

// ---------------------------------------------------------------------------
// Judgment
// ---------------------------------------------------------------------------

static void prv_apply(uint16_t index, RbJudgment judgment) {
  s_judged[index] = (uint8_t)judgment;
  s_counts[judgment]++;

  if (judgment == RB_JUDGE_MISS) {
    s_combo = 0;
  } else {
    uint32_t base = (judgment == RB_JUDGE_PERFECT) ? RB_SCORE_PERFECT : RB_SCORE_GOOD;
    if (s_chart->notes[index].type == RB_NOTE_BIG) {
      base *= RB_BIG_MULTIPLIER;
    }
    // Bonus uses the combo as it stood BEFORE this note, so the first hit of a
    // run scores base and the reward ramps from there.
    const uint16_t bonus_combo =
        (s_combo < RB_COMBO_BONUS_CAP) ? s_combo : RB_COMBO_BONUS_CAP;
    s_score += base + (uint32_t)bonus_combo * RB_COMBO_BONUS_PER;

    s_combo++;
    if (s_combo > s_max_combo) {
      s_max_combo = s_combo;
    }
  }

  prv_advance_cursor();
}

RbJudgment game_judge_hit(uint8_t lane, uint32_t press_now_ms, const ChartNote **out_note) {
  if (out_note != NULL) {
    *out_note = NULL;
  }
  if (s_chart == NULL) {
    return RB_JUDGE_NONE;
  }

  const uint32_t elapsed_ms = press_now_ms - s_origin_ms;
  uint16_t best = UINT16_MAX;
  int32_t best_abs = 0;

  for (uint16_t i = s_first_live; i < s_chart->note_count; i++) {
    const ChartNote *const note = &s_chart->notes[i];
    const int32_t delta = (int32_t)elapsed_ms - (int32_t)note->hit_time_ms;

    if (delta > RB_MISS_MS) {
      continue;  // window already closed; game_advance() will retire it
    }
    if (delta < -RB_MISS_MS) {
      break;  // chart is time-sorted, so every later note is further away still
    }
    if (s_judged[i] != RB_JUDGE_NONE || note->lane != lane) {
      continue;
    }

    const int32_t abs_delta = prv_abs32(delta);
    // Strict < keeps the earlier note on a tie, which is the intuitive
    // resolution when two notes are equidistant from the press.
    if (best == UINT16_MAX || abs_delta < best_abs) {
      best = i;
      best_abs = abs_delta;
    }
  }

  if (best == UINT16_MAX) {
    return RB_JUDGE_NONE;  // stray press -- ignored, combo survives
  }

  RbJudgment judgment;
  if (best_abs <= RB_PERFECT_MS) {
    judgment = RB_JUDGE_PERFECT;
  } else if (best_abs <= RB_GOOD_MS) {
    judgment = RB_JUDGE_GOOD;
  } else {
    // Inside the miss window but outside Good: the player did commit to this
    // note, so it is consumed and the combo breaks rather than the note being
    // left to auto-miss later.
    judgment = RB_JUDGE_MISS;
  }

  if (out_note != NULL) {
    *out_note = &s_chart->notes[best];
  }
  prv_apply(best, judgment);
  return judgment;
}

uint16_t game_step(uint32_t now_ms) {
  if (s_chart == NULL) {
    return 0;
  }

  s_elapsed_ms = now_ms - s_origin_ms;

  uint16_t misses = 0;
  for (uint16_t i = s_first_live; i < s_chart->note_count; i++) {
    const int32_t delta = (int32_t)s_elapsed_ms - (int32_t)s_chart->notes[i].hit_time_ms;
    if (delta <= RB_MISS_MS) {
      break;  // time-sorted: nothing later can have expired either
    }
    if (s_judged[i] == RB_JUDGE_NONE) {
      s_judged[i] = RB_JUDGE_MISS;
      s_counts[RB_JUDGE_MISS]++;
      s_combo = 0;
      misses++;
    }
  }

  prv_advance_cursor();
  return misses;
}

bool game_is_finished(void) {
  return s_chart != NULL && s_elapsed_ms >= s_chart->end_ms;
}

// ---------------------------------------------------------------------------
// Accessors
// ---------------------------------------------------------------------------

uint32_t game_score(void) {
  return s_score;
}

uint16_t game_combo(void) {
  return s_combo;
}

uint16_t game_max_combo(void) {
  return s_max_combo;
}

uint16_t game_count(RbJudgment judgment) {
  return (judgment < RB_JUDGE_COUNT) ? s_counts[judgment] : 0;
}

uint16_t game_accuracy_pct(void) {
  if (s_chart == NULL || s_chart->note_count == 0) {
    return 0;
  }
  // A Good is worth half a Perfect, so a clean run of Goods reads as 50% rather
  // than as full marks.
  const uint32_t earned =
      (uint32_t)s_counts[RB_JUDGE_PERFECT] * 100u + (uint32_t)s_counts[RB_JUDGE_GOOD] * 50u;
  return (uint16_t)(earned / s_chart->note_count);
}

RbScreen game_screen(void) {
  return s_screen;
}

void game_set_screen(RbScreen screen) {
  s_screen = screen;
}

uint16_t game_first_live(void) {
  return s_first_live;
}

RbJudgment game_note_judgment(uint16_t index) {
  return (index < RB_MAX_NOTES) ? (RbJudgment)s_judged[index] : RB_JUDGE_NONE;
}

const Chart *game_chart(void) {
  return s_chart;
}
