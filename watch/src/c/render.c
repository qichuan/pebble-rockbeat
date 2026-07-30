#include "render.h"

#include "audio.h"
#include "chart.h"
#include "feedback.h"
#include "game.h"
#include "rb_config.h"
#include "save.h"

// ---------------------------------------------------------------------------
// Layout
//
// Recomputed from layer_get_bounds() every frame rather than hardcoded, so the
// 200x228 emery geometry is a consequence of the constants rather than baked
// into them.
// ---------------------------------------------------------------------------

typedef struct {
  GRect bounds;
  int16_t lane_h;
  int16_t lane_top[RB_LANE_COUNT];
  int16_t lane_cy[RB_LANE_COUNT];
  int16_t target_cx;
} RbLayout;

// Lane bands are sized against a THREE-lane split even though only two lanes
// are played, and this is deliberate.
//
// The whole ergonomic premise is that a lane sits at the vertical position of
// the button that plays it. The buttons do not move when the game drops a lane:
// UP, SELECT and DOWN stay where they are on the case, with SELECT on the
// screen's exact vertical centre. Dividing the playfield by RB_LANE_COUNT
// instead -- the obvious change -- gives two 90px bands centred at y=69 and
// y=159, so the SELECT lane would sit 45px BELOW the SELECT button and the game
// would be pointing at the wrong hardware.
//
// So the geometry stays keyed to the button positions, and the space the third
// lane used to occupy is simply left out of the playfield.
#define RB_LANE_SLOTS 3

static RbLayout prv_layout(Layer *layer) {
  RbLayout lay;
  lay.bounds = layer_get_bounds(layer);
  lay.lane_h = (int16_t)((lay.bounds.size.h - RB_HUD_TOP_H - RB_HUD_BOT_H) / RB_LANE_SLOTS);
  for (uint8_t i = 0; i < RB_LANE_COUNT; i++) {
    lay.lane_top[i] = (int16_t)(RB_HUD_TOP_H + i * lay.lane_h);
    lay.lane_cy[i] = (int16_t)(lay.lane_top[i] + lay.lane_h / 2);
  }
  lay.target_cx = (int16_t)(lay.bounds.size.w - RB_TARGET_INSET_R);
  return lay;
}

// ---------------------------------------------------------------------------
// Palette
//
// Compound-literal GColor macros are not constant expressions, so these are
// switch functions rather than static const arrays.
// ---------------------------------------------------------------------------

static GColor prv_lane_accent(uint8_t lane) {
  switch (lane) {
    case RB_LANE_TOP: return GColorPictonBlue;
    case RB_LANE_MID: return GColorRed;
    default:          return GColorYellow;
  }
}

// Dark beds that tint toward each lane's accent, so a glance at any row tells
// you which button it belongs to even with no note on screen.
static GColor prv_lane_bed(uint8_t lane) {
  switch (lane) {
    case RB_LANE_TOP: return GColorOxfordBlue;
    case RB_LANE_MID: return GColorBulgarianRose;
    default:          return GColorArmyGreen;
  }
}

static GColor prv_judgment_color(RbJudgment judgment) {
  switch (judgment) {
    case RB_JUDGE_PERFECT: return GColorYellow;
    case RB_JUDGE_GOOD:    return GColorBrightGreen;
    case RB_JUDGE_MISS:    return GColorLightGray;
    default:               return GColorWhite;
  }
}

static const char *prv_judgment_text(RbJudgment judgment) {
  switch (judgment) {
    case RB_JUDGE_PERFECT: return "PERFECT";
    case RB_JUDGE_GOOD:    return "GOOD";
    case RB_JUDGE_MISS:    return "MISS";
    default:               return "";
  }
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

// Drawn as stacked fill_rect rows rather than text: the Gothic system fonts
// carry no arrow glyphs, so a literal triangle character renders as tofu. No
// GPath either -- that would mean a heap allocation.
static void prv_draw_triangle(GContext *ctx, int16_t cx, int16_t cy, int16_t half_w,
                              int16_t h, bool pointing_up) {
  for (int16_t i = 0; i < h; i++) {
    const int16_t w = pointing_up ? (int16_t)((half_w * (i + 1)) / h)
                                  : (int16_t)((half_w * (h - i)) / h);
    graphics_fill_rect(ctx, GRect(cx - w, (int16_t)(cy - h / 2 + i), (int16_t)(w * 2 + 1), 1),
                       0, GCornerNone);
  }
}

// The button this lane belongs to, drawn in the right margin beside the real
// buttons: a caret up for UP, a dot for SELECT, a caret down for DOWN.
static void prv_draw_lane_badge(GContext *ctx, const RbLayout *lay, uint8_t lane) {
  const int16_t cx = (int16_t)(lay->bounds.size.w - 7);
  const int16_t cy = lay->lane_cy[lane];

  graphics_context_set_fill_color(ctx, prv_lane_accent(lane));
  switch (lane) {
    case RB_LANE_TOP: prv_draw_triangle(ctx, cx, cy, 5, 9, true); break;
    case RB_LANE_MID: graphics_fill_circle(ctx, GPoint(cx, cy), 4); break;
    default:          prv_draw_triangle(ctx, cx, cy, 5, 9, false); break;
  }
}

static void prv_draw_text(GContext *ctx, const char *text, const char *font_key, GRect box,
                          GTextAlignment align, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, fonts_get_system_font(font_key), box,
                     GTextOverflowModeTrailingEllipsis, align, NULL);
}

// ---------------------------------------------------------------------------
// Playfield
// ---------------------------------------------------------------------------

static void prv_draw_lanes(GContext *ctx, const RbLayout *lay) {
  for (uint8_t lane = 0; lane < RB_LANE_COUNT; lane++) {
    graphics_context_set_fill_color(ctx, prv_lane_bed(lane));
    graphics_fill_rect(ctx, GRect(0, lay->lane_top[lane], lay->bounds.size.w, lay->lane_h),
                       0, GCornerNone);

    // A one-pixel black rule between lanes keeps the three beds from reading as
    // a single gradient.
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_rect(ctx, GRect(0, lay->lane_top[lane], lay->bounds.size.w, 1), 0, GCornerNone);
  }
}

static void prv_draw_targets(GContext *ctx, const RbLayout *lay, uint32_t elapsed_ms) {
  for (uint8_t lane = 0; lane < RB_LANE_COUNT; lane++) {
    const GPoint centre = GPoint(lay->target_cx, lay->lane_cy[lane]);
    const RbJudgment flash = feedback_lane_flash(lane, elapsed_ms);

    // A dark disc under the ring so a note crossing the target stays legible
    // against the lane bed.
    graphics_context_set_fill_color(ctx, GColorBlack);
    graphics_fill_circle(ctx, centre, RB_TARGET_R);

    if (flash != RB_JUDGE_NONE) {
      graphics_context_set_fill_color(ctx, prv_judgment_color(flash));
      graphics_fill_circle(ctx, centre, RB_TARGET_R - RB_TARGET_RING_W);
    }

    graphics_context_set_stroke_color(ctx, prv_lane_accent(lane));
    graphics_context_set_stroke_width(ctx, RB_TARGET_RING_W);
    graphics_draw_circle(ctx, centre, RB_TARGET_R);
    graphics_context_set_stroke_width(ctx, 1);
  }
}

// x = target - (time until the note is due) * speed. A note still in the future
// has a positive delta and therefore sits left of the target; the note reaches
// the target exactly when delta hits zero. This single line is the whole
// relationship between the song clock and the picture.
static int16_t prv_note_x(int16_t target_cx, int32_t delta_ms) {
  return (int16_t)(target_cx - (delta_ms * RB_SCROLL_PX_PER_SEC) / 1000);
}

static void prv_draw_notes(GContext *ctx, const RbLayout *lay, uint32_t elapsed_ms) {
  const Chart *const chart = game_chart();
  if (chart == NULL) {
    return;
  }

  const int16_t cull_left = -RB_CULL_MARGIN;
  const int16_t cull_right = (int16_t)(lay->bounds.size.w + RB_CULL_MARGIN);

  // Scanning forward from the live cursor, hit times ascend, so x descends
  // monotonically -- the first note off the left edge ends the loop and nothing
  // ever walks the whole chart.
  for (uint16_t i = game_first_live(); i < chart->note_count; i++) {
    const ChartNote *const note = &chart->notes[i];
    const int32_t delta_ms = (int32_t)note->hit_time_ms - (int32_t)elapsed_ms;
    const int16_t x = prv_note_x(lay->target_cx, delta_ms);

    if (x < cull_left) {
      break;
    }
    if (x > cull_right || game_note_judgment(i) != RB_JUDGE_NONE) {
      continue;  // already resolved: hit notes vanish, missed notes are dropped
    }

    const bool big = (note->type == RB_NOTE_BIG);
    const int16_t r = big ? RB_NOTE_R_BIG : RB_NOTE_R_NORMAL;
    const GPoint centre = GPoint(x, lay->lane_cy[note->lane]);

    graphics_context_set_fill_color(ctx, prv_lane_accent(note->lane));
    graphics_fill_circle(ctx, centre, r);

    // Big notes get a white outline as well as extra radius, so they are
    // distinguishable by shape and not by colour alone.
    graphics_context_set_stroke_color(ctx, GColorWhite);
    graphics_context_set_stroke_width(ctx, big ? 3 : 1);
    graphics_draw_circle(ctx, centre, r);
    graphics_context_set_stroke_width(ctx, 1);
  }
}

// ---------------------------------------------------------------------------
// HUD
// ---------------------------------------------------------------------------

static void prv_draw_hud(GContext *ctx, const RbLayout *lay, uint32_t elapsed_ms) {
  const Chart *const chart = game_chart();
  const int16_t w = lay->bounds.size.w;

  // Progress bar across the very top.
  graphics_context_set_fill_color(ctx, GColorDarkGray);
  graphics_fill_rect(ctx, GRect(0, 0, w, 3), 0, GCornerNone);
  if (chart != NULL && chart->end_ms > 0) {
    uint32_t done = ((uint32_t)w * elapsed_ms) / chart->end_ms;
    if (done > (uint32_t)w) {
      done = (uint32_t)w;
    }
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_rect(ctx, GRect(0, 0, (int16_t)done, 3), 0, GCornerNone);
  }

  char score_buf[12];
  snprintf(score_buf, sizeof(score_buf), "%lu", (unsigned long)game_score());
  prv_draw_text(ctx, score_buf, FONT_KEY_GOTHIC_18_BOLD, GRect(4, 2, w - 8, 20),
                GTextAlignmentLeft, GColorWhite);

  const RbJudgment last = feedback_last_judgment(elapsed_ms);
  if (last != RB_JUDGE_NONE) {
    prv_draw_text(ctx, prv_judgment_text(last), FONT_KEY_GOTHIC_14_BOLD,
                  GRect(4, 6, w - 8, 18), GTextAlignmentRight, prv_judgment_color(last));
  }

  // Combo, bottom band. Only shown from 2 upward -- "x1" on every single note
  // is noise.
  const int16_t foot_y = (int16_t)(lay->bounds.size.h - RB_HUD_BOT_H);
  const uint16_t combo = game_combo();
  if (combo >= 2) {
    char combo_buf[12];
    snprintf(combo_buf, sizeof(combo_buf), "x%u", (unsigned)combo);
    prv_draw_text(ctx, combo_buf, FONT_KEY_GOTHIC_18_BOLD, GRect(4, foot_y + 1, w - 8, 22),
                  GTextAlignmentLeft, combo >= 10 ? GColorYellow : GColorWhite);
  }

  char tally_buf[24];
  snprintf(tally_buf, sizeof(tally_buf), "%u/%u/%u", (unsigned)game_count(RB_JUDGE_PERFECT),
           (unsigned)game_count(RB_JUDGE_GOOD), (unsigned)game_count(RB_JUDGE_MISS));
  prv_draw_text(ctx, tally_buf, FONT_KEY_GOTHIC_14, GRect(4, foot_y + 4, w - 8, 20),
                GTextAlignmentRight, GColorLightGray);
}

// ---------------------------------------------------------------------------
// Title / pause / results
// ---------------------------------------------------------------------------

static void prv_draw_play(GContext *ctx, const RbLayout *lay) {
  const uint32_t elapsed_ms = game_elapsed_ms();

  prv_draw_lanes(ctx, lay);
  prv_draw_targets(ctx, lay, elapsed_ms);
  prv_draw_notes(ctx, lay, elapsed_ms);
  for (uint8_t lane = 0; lane < RB_LANE_COUNT; lane++) {
    prv_draw_lane_badge(ctx, lay, lane);
  }
  prv_draw_hud(ctx, lay, elapsed_ms);
}

// A translucent-looking scrim: emery has alpha in GColor, but filling a large
// rect with a 33% alpha colour is slower than a solid panel and reads muddier on
// the real display, so the playfield is dimmed with a solid panel instead.
static void prv_draw_panel(GContext *ctx, GRect box) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, box, 6, GCornersAll);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_round_rect(ctx, box, 6);
  graphics_context_set_stroke_width(ctx, 1);
}

static void prv_draw_title(GContext *ctx, const RbLayout *lay) {
  const int16_t w = lay->bounds.size.w;

  // Keep the lane beds behind the title so the colour-to-button mapping is
  // already learnable before the song starts.
  prv_draw_lanes(ctx, lay);
  for (uint8_t lane = 0; lane < RB_LANE_COUNT; lane++) {
    prv_draw_lane_badge(ctx, lay, lane);
  }

  prv_draw_panel(ctx, GRect(8, 30, w - 16, 168));

  prv_draw_text(ctx, "ROCKBEAT", FONT_KEY_GOTHIC_28_BOLD, GRect(10, 38, w - 20, 34),
                GTextAlignmentCenter, GColorYellow);

  const Chart *const chart = game_chart();
  if (chart != NULL) {
    prv_draw_text(ctx, chart->title, FONT_KEY_GOTHIC_18, GRect(10, 74, w - 20, 24),
                  GTextAlignmentCenter, GColorWhite);
  }

  char best[32];
  snprintf(best, sizeof(best), "BEST %lu", (unsigned long)save_high_score());
  prv_draw_text(ctx, best, FONT_KEY_GOTHIC_18_BOLD, GRect(10, 102, w - 20, 24),
                GTextAlignmentCenter, GColorLightGray);

  char opts[40];
  snprintf(opts, sizeof(opts), "SOUND %s   BUZZ %s",
           audio_is_enabled() && audio_is_available() ? "ON" : "OFF",
           feedback_haptics_enabled() ? "ON" : "OFF");
  prv_draw_text(ctx, opts, FONT_KEY_GOTHIC_14, GRect(10, 132, w - 20, 20),
                GTextAlignmentCenter, GColorLightGray);

  prv_draw_text(ctx, "SELECT to play", FONT_KEY_GOTHIC_14_BOLD, GRect(10, 156, w - 20, 20),
                GTextAlignmentCenter, GColorWhite);
  prv_draw_text(ctx, "UP sound  DOWN buzz", FONT_KEY_GOTHIC_14, GRect(10, 174, w - 20, 20),
                GTextAlignmentCenter, GColorDarkGray);
}

static void prv_draw_pause(GContext *ctx, const RbLayout *lay) {
  const int16_t w = lay->bounds.size.w;

  prv_draw_panel(ctx, GRect(12, 58, w - 24, 112));
  prv_draw_text(ctx, "PAUSED", FONT_KEY_GOTHIC_28_BOLD, GRect(14, 66, w - 28, 34),
                GTextAlignmentCenter, GColorYellow);
  prv_draw_text(ctx, "SELECT resume", FONT_KEY_GOTHIC_18, GRect(14, 104, w - 28, 24),
                GTextAlignmentCenter, GColorWhite);
  prv_draw_text(ctx, "UP restart", FONT_KEY_GOTHIC_14, GRect(14, 128, w - 28, 20),
                GTextAlignmentCenter, GColorLightGray);
  prv_draw_text(ctx, "DOWN / BACK quit", FONT_KEY_GOTHIC_14, GRect(14, 146, w - 28, 20),
                GTextAlignmentCenter, GColorLightGray);
}

static const char *prv_rank(uint16_t accuracy_pct, uint16_t misses) {
  if (misses == 0 && accuracy_pct >= RB_RANK_S_PCT) {
    return "S";
  }
  if (accuracy_pct >= RB_RANK_A_PCT) {
    return "A";
  }
  if (accuracy_pct >= RB_RANK_B_PCT) {
    return "B";
  }
  return "C";
}

static void prv_draw_results(GContext *ctx, const RbLayout *lay) {
  const int16_t w = lay->bounds.size.w;
  const uint16_t misses = game_count(RB_JUDGE_MISS);
  const uint16_t accuracy = game_accuracy_pct();

  prv_draw_lanes(ctx, lay);
  prv_draw_panel(ctx, GRect(6, 14, w - 12, 200));

  prv_draw_text(ctx, prv_rank(accuracy, misses), FONT_KEY_BITHAM_30_BLACK,
                GRect(8, 18, w - 16, 36), GTextAlignmentCenter, GColorYellow);

  char buf[32];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)game_score());
  prv_draw_text(ctx, buf, FONT_KEY_LECO_32_BOLD_NUMBERS, GRect(8, 54, w - 16, 38),
                GTextAlignmentCenter, GColorWhite);

  if (game_score() >= save_high_score() && game_score() > 0) {
    prv_draw_text(ctx, "NEW BEST", FONT_KEY_GOTHIC_14_BOLD, GRect(8, 92, w - 16, 20),
                  GTextAlignmentCenter, GColorYellow);
  }

  snprintf(buf, sizeof(buf), "%u%% accuracy", (unsigned)accuracy);
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_18, GRect(8, 112, w - 16, 24), GTextAlignmentCenter,
                GColorLightGray);

  snprintf(buf, sizeof(buf), "PERFECT   %u", (unsigned)game_count(RB_JUDGE_PERFECT));
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_14, GRect(20, 138, w - 40, 18), GTextAlignmentLeft,
                GColorYellow);
  snprintf(buf, sizeof(buf), "GOOD      %u", (unsigned)game_count(RB_JUDGE_GOOD));
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_14, GRect(20, 155, w - 40, 18), GTextAlignmentLeft,
                GColorBrightGreen);
  snprintf(buf, sizeof(buf), "MISS      %u", (unsigned)misses);
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_14, GRect(20, 172, w - 40, 18), GTextAlignmentLeft,
                GColorLightGray);

  snprintf(buf, sizeof(buf), "max combo x%u", (unsigned)game_max_combo());
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_14, GRect(8, 192, w - 16, 18), GTextAlignmentCenter,
                GColorDarkGray);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

void render_update_proc(Layer *layer, GContext *ctx) {
  const RbLayout lay = prv_layout(layer);
  const RbScreen screen = game_screen();

  // Antialiasing is paid for per drawn pixel, and it is by far the most
  // expensive thing on this screen: a gameplay frame draws a dozen or more
  // circles, several of them stroked three pixels wide, and it draws them 25
  // times a second. On the emulator that is free. On the watch it was the lag.
  //
  // So it is spent where it is seen and not where it is felt. The title, pause
  // and results screens are drawn ONCE and then sit still under the player's
  // eye, so they keep it. The playfield is in constant motion, where a smooth
  // frame rate reads as quality far more than a smooth circle edge does.
  const bool playing = (screen == RB_SCREEN_PLAYING || screen == RB_SCREEN_PAUSED);
  graphics_context_set_antialiased(ctx, !playing);

  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, lay.bounds, 0, GCornerNone);

  switch (screen) {
    case RB_SCREEN_TITLE:
      prv_draw_title(ctx, &lay);
      break;
    case RB_SCREEN_PLAYING:
      prv_draw_play(ctx, &lay);
      break;
    case RB_SCREEN_PAUSED:
      // The field stays visible behind the panel so the player can see exactly
      // what they are coming back to. The panel itself is static, so it gets
      // antialiasing back once the playfield underneath it has been drawn.
      prv_draw_play(ctx, &lay);
      graphics_context_set_antialiased(ctx, true);
      prv_draw_pause(ctx, &lay);
      break;
    case RB_SCREEN_RESULTS:
      prv_draw_results(ctx, &lay);
      break;
    default:
      break;
  }
}
