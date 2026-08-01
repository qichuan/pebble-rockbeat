#include "render.h"

#include "chart.h"
#include "feedback.h"
#include "game.h"
#include "rb_config.h"
#include "save.h"


// ---------------------------------------------------------------------------
// Palette
//
// Colours are built from their 8-bit ARGB value rather than looked up by name.
// The design specifies hex, and every colour in it is already on the 64-colour
// grid -- two bits per channel, each pair meaning 0/85/170/255 -- so the byte is
// an exact, mechanical transcription of the hex: 0b11 then RR GG BB.
//
// Doing it by name invites silent mistakes. Grepping gcolor_definitions.h for
// these values returned confident, WRONG answers (it named #555500 "Indigo"),
// and a wrong colour constant is not something a build catches.
// ---------------------------------------------------------------------------

#define RB_ARGB(byte) ((GColor8){ .argb = (uint8_t)(byte) })

#define RB_C_LANE_TOP_BED   RB_ARGB(0b11010100)  // #555500 olive
#define RB_C_LANE_TOP_RAIL  RB_ARGB(0b11100100)  // #AA5500
#define RB_C_LANE_TOP_ACC   RB_ARGB(0b11111000)  // #FFAA00 orange
#define RB_C_LANE_BOT_BED   RB_ARGB(0b11000101)  // #005555 teal
#define RB_C_LANE_BOT_RAIL  RB_ARGB(0b11000110)  // #0055AA
#define RB_C_LANE_BOT_ACC   RB_ARGB(0b11001011)  // #00AAFF blue

// The bottom lane's badge: a bar for SELECT above a down arrow for DOWN. Kept
// local to this file rather than added to rb_config.h because they are not
// tunables -- they are the geometry of one mark, sized once to fit the ring, and
// nothing outside render.c can use them.
//
// The bar is deliberately WIDER than the arrow (16 against 13) and the arrow is
// below the design size, so the two marks are not competing for the same weight.
// The top lane's arrow is untouched at RB_ARROW_LEN/RB_ARROW_HALF.
//
// Stack height is BAR_H + BAR_GAP + BADGE_ARROW_LEN = 19, which is ODD so it
// centres exactly on the ring; keep it odd if you retune these. Worst pixel is a
// bar corner at (8, 9) -- r=12.0 against the ring's 15px clear radius.
#define RB_BADGE_BAR_W 16
#define RB_BADGE_BAR_H 6
#define RB_BADGE_BAR_GAP 4
#define RB_BADGE_ARROW_LEN 9
#define RB_BADGE_ARROW_HALF 6

// One record colour per song, so the art tile is not the same plate every time.
static GColor prv_song_art(uint8_t song) {
  switch (song) {
    case 1:  return RB_ARGB(0b11000110);  // #0055AA
    case 2:  return RB_ARGB(0b11100100);  // #AA5500
    default: return RB_ARGB(0b11100001);  // #AA0055
  }
}

static GColor prv_lane_accent(uint8_t lane) {
  return (lane == RB_LANE_TOP) ? RB_C_LANE_TOP_ACC : RB_C_LANE_BOT_ACC;
}

static GColor prv_lane_bed(uint8_t lane) {
  return (lane == RB_LANE_TOP) ? RB_C_LANE_TOP_BED : RB_C_LANE_BOT_BED;
}

static GColor prv_lane_rail(uint8_t lane) {
  return (lane == RB_LANE_TOP) ? RB_C_LANE_TOP_RAIL : RB_C_LANE_BOT_RAIL;
}

static const char *prv_judgment_text(RbJudgment judgment) {
  switch (judgment) {
    case RB_JUDGE_PERFECT: return "PERFECT!";
    case RB_JUDGE_GOOD:    return "GOOD";
    case RB_JUDGE_MISS:    return "MISS";
    default:               return "";
  }
}

// ---------------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------------

// A triangle, apex up or down, drawn as stacked 1px fill_rect strips rather than
// text: the Gothic system fonts carry no arrow glyphs, so a literal triangle
// character renders as tofu. No GPath either -- that would mean a heap
// allocation. The caller sets the fill colour.
//
// The two orientations are one taper on one axis, so pointing_up only chooses
// which end the apex sits at.
//
// len/half are arguments rather than RB_ARROW_LEN/RB_ARROW_HALF directly because
// the two badges want different sizes: the top lane's arrow is the whole mark
// and stays at the design size, while the bottom lane's shares its ring with the
// SELECT bar and is drawn smaller so the bar can carry the weight.
static void prv_draw_arrow(GContext *ctx, int16_t cx, int16_t cy, int16_t len,
                           int16_t half_w, bool pointing_up) {
  for (int16_t i = 0; i < len; i++) {
    const int16_t step = pointing_up ? (int16_t)(i + 1) : (int16_t)(len - i);
    const int16_t half = (int16_t)((half_w * step) / len);
    graphics_fill_rect(ctx,
                       GRect((int16_t)(cx - half), (int16_t)(cy - len / 2 + i),
                             (int16_t)(half * 2 + 1), 1),
                       0, GCornerNone);
  }
}

// The badge inside a resting target ring: the buttons that play this lane.
//
// One mark per button, stacked in the buttons' own physical order. The top lane
// has a single button and gets a single UP arrow; the bottom lane has two and
// gets two marks, a bar for SELECT above a down arrow for DOWN.
//
// That count is the whole point. The badge used to be one arrow per lane, which
// forced the lower one to point RIGHT -- the direction the notes travel -- since
// an arrow names a direction and the lane has two buttons in different
// directions. It therefore named no button at all, and players were not finding
// DOWN. Two marks name both, and a plain DOWN arrow was rejected for the
// opposite failure: it reads as "an UP/DOWN game" and steers players off
// UP+SELECT, the adjacent pair that makes sixteenth-note alternation possible.
//
// The bar is the larger of the two marks and the arrow is drawn below the design
// size. Two marks in one ring compete, and the bar is the one that has to win a
// squint: an arrow is already a familiar shape that survives being small, while
// a short bar reads as a stray tick.
//
// The stack is derived from the constants rather than hardcoded, so retuning any
// of them keeps it centred. The caller sets the fill colour.
static void prv_draw_lane_badge(GContext *ctx, int16_t cx, int16_t cy, uint8_t lane) {
  if (lane == RB_LANE_TOP) {
    prv_draw_arrow(ctx, cx, cy, RB_ARROW_LEN, RB_ARROW_HALF, true);
    return;
  }

  const int16_t span =
      (int16_t)(RB_BADGE_BAR_H + RB_BADGE_BAR_GAP + RB_BADGE_ARROW_LEN);
  const int16_t top = (int16_t)(cy - span / 2);
  graphics_fill_rect(ctx,
                     GRect((int16_t)(cx - RB_BADGE_BAR_W / 2), top, RB_BADGE_BAR_W,
                           RB_BADGE_BAR_H),
                     0, GCornerNone);
  prv_draw_arrow(
      ctx, cx,
      (int16_t)(top + RB_BADGE_BAR_H + RB_BADGE_BAR_GAP + RB_BADGE_ARROW_LEN / 2),
      RB_BADGE_ARROW_LEN, RB_BADGE_ARROW_HALF, false);
}

static void prv_draw_text(GContext *ctx, const char *text, const char *font_key, GRect box,
                          GTextAlignment align, GColor color) {
  graphics_context_set_text_color(ctx, color);
  graphics_draw_text(ctx, text, fonts_get_system_font(font_key), box,
                     GTextOverflowModeTrailingEllipsis, align, NULL);
}

static void prv_fill(GContext *ctx, GRect rect, GColor color) {
  graphics_context_set_fill_color(ctx, color);
  graphics_fill_rect(ctx, rect, 0, GCornerNone);
}

// The song the title screen has selected, derived from the chart that is loaded
// rather than tracked separately. game_chart() is always one of the entries in
// the chart table, so a pointer match IS the index -- and it cannot drift out of
// step with what is being drawn the way a second copy of the index could.
static uint8_t prv_selected_song(void) {
  const Chart *const chart = game_chart();
  for (uint8_t i = 0; i < chart_count(); i++) {
    if (chart_get(i) == chart) {
      return i;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------
// HUD -- score left, combo right
// ---------------------------------------------------------------------------

static void prv_draw_hud(GContext *ctx, GRect bounds) {
  const int16_t w = bounds.size.w;

  graphics_context_set_antialiased(ctx, false);
  prv_fill(ctx, bounds, GColorBlack);

  prv_draw_text(ctx, "SCORE", FONT_KEY_GOTHIC_14, GRect(7, -3, 90, 18),
                GTextAlignmentLeft, GColorLightGray);

  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)game_score());
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_24_BOLD, GRect(7, 13, 110, 32),
                GTextAlignmentLeft, GColorWhite);

  // The combo is the loudest thing on the screen by design -- a big yellow
  // numeral with its label tucked underneath.
  snprintf(buf, sizeof(buf), "%u", (unsigned)game_combo());
  prv_draw_text(ctx, buf, FONT_KEY_LECO_32_BOLD_NUMBERS,
                GRect((int16_t)(w - 98), -6, 90, 42), GTextAlignmentRight, GColorYellow);
  prv_draw_text(ctx, "COMBO", FONT_KEY_GOTHIC_14, GRect((int16_t)(w - 98), 35, 90, 18),
                GTextAlignmentRight, GColorWhite);
}

// ---------------------------------------------------------------------------
// Field -- lanes, notes, targets, popup
//
// Layer-local coordinates: the layer's origin is the top of the upper lane, so
// the lane tops are 0 and RB_LANE_BOT_Y - RB_LANE_TOP_Y.
// ---------------------------------------------------------------------------

static int16_t prv_lane_y(uint8_t lane) {
  return (lane == RB_LANE_TOP) ? RB_LANE_TOP_Y : RB_LANE_BOT_Y;
}

static int16_t prv_lane_cy(uint8_t lane) {
  return (int16_t)(prv_lane_y(lane) + RB_LANE_H / 2);
}

static void prv_draw_lane_beds(GContext *ctx, int16_t w) {
  for (uint8_t lane = 0; lane < RB_LANE_COUNT; lane++) {
    const int16_t y = prv_lane_y(lane);

    prv_fill(ctx, GRect(0, y, w, RB_LANE_H), prv_lane_bed(lane));

    // A rail down the middle of the bed: the line the notes travel along, and
    // what makes an empty lane still read as a lane.
    prv_fill(ctx, GRect(0, (int16_t)(y + RB_LANE_RAIL_DY), RB_TARGET_ZONE_X, RB_LANE_RAIL_H),
             prv_lane_rail(lane));

    // The target zone is plain black so a note crossing the target is never read
    // against a coloured bed, with a bright rule marking the boundary.
    prv_fill(ctx, GRect(RB_TARGET_ZONE_X, y, (int16_t)(w - RB_TARGET_ZONE_X), RB_LANE_H),
             GColorBlack);
    prv_fill(ctx, GRect(RB_TARGET_DIVIDER_X, y, RB_TARGET_DIVIDER_W, RB_LANE_H),
             GColorLightGray);
  }
}

static void prv_draw_targets(GContext *ctx, uint32_t elapsed_ms) {
  for (uint8_t lane = 0; lane < RB_LANE_COUNT; lane++) {
    const GPoint centre = GPoint(RB_TARGET_CX, prv_lane_cy(lane));
    const RbJudgment flash = feedback_lane_flash(lane, elapsed_ms);
    const GColor accent = prv_lane_accent(lane);

    if (flash == RB_JUDGE_NONE) {
      // Resting: a hollow ring in the lane's accent with one mark inside it per
      // button that plays the lane, so the mapping -- including the second
      // button on the lower lane -- is legible without a legend.
      graphics_context_set_stroke_color(ctx, accent);
      graphics_context_set_stroke_width(ctx, RB_TARGET_RING_W);
      graphics_draw_circle(ctx, centre, RB_TARGET_R - RB_TARGET_RING_W / 2);
      graphics_context_set_stroke_width(ctx, 1);

      graphics_context_set_fill_color(ctx, accent);
      prv_draw_lane_badge(ctx, centre.x, centre.y, lane);
      continue;
    }

    // Struck: the ring fills with the lane colour behind a white rim, and a
    // perfect adds a white core -- the design's "flat burst", with no gradients
    // and nothing that has to be animated frame by frame.
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, centre, RB_TARGET_R);
    graphics_context_set_fill_color(ctx, accent);
    graphics_fill_circle(ctx, centre, (int16_t)(RB_TARGET_R - RB_TARGET_RING_W));
    if (flash == RB_JUDGE_PERFECT) {
      graphics_context_set_fill_color(ctx, GColorWhite);
      graphics_fill_circle(ctx, centre, (int16_t)(RB_TARGET_R - RB_TARGET_RING_W - 6));
    }
  }
}

// x = target - (time until the note is due) * speed. A note still in the future
// has a positive delta and therefore sits left of the target; the note reaches
// the target exactly when delta hits zero. This single line is the whole
// relationship between the song clock and the picture.
static int16_t prv_note_x(int32_t delta_ms) {
  return (int16_t)(RB_TARGET_CX - (delta_ms * RB_SCROLL_PX_PER_SEC) / 1000);
}

static void prv_draw_notes(GContext *ctx, int16_t w, uint32_t elapsed_ms) {
  const Chart *const chart = game_chart();
  if (chart == NULL) {
    return;
  }

  const int16_t cull_left = -RB_CULL_MARGIN;
  const int16_t cull_right = (int16_t)(w + RB_CULL_MARGIN);

  // Scanning forward from the live cursor, hit times ascend, so x descends
  // monotonically -- the first note off the left edge ends the loop and nothing
  // ever walks the whole chart.
  for (uint16_t i = game_first_live(); i < chart->note_count; i++) {
    const ChartNote *const note = &chart->notes[i];
    const int32_t delta_ms = (int32_t)note->hit_time_ms - (int32_t)elapsed_ms;
    const int16_t x = prv_note_x(delta_ms);

    if (x < cull_left) {
      break;
    }
    if (x > cull_right || game_note_judgment(i) != RB_JUDGE_NONE) {
      continue;  // already resolved: hit notes vanish, missed notes are dropped
    }

    const bool big = (note->type == RB_NOTE_BIG);
    const int16_t r = big ? RB_NOTE_R_BIG : RB_NOTE_R_NORMAL;
    const GPoint centre = GPoint(x, prv_lane_cy(note->lane));

    // White disc first, lane colour inside it: a filled ring drawn as two fills
    // rather than a stroked circle, so the rim stays exactly RB_NOTE_RING_W at
    // every radius and big notes do not thin out.
    graphics_context_set_fill_color(ctx, GColorWhite);
    graphics_fill_circle(ctx, centre, r);
    graphics_context_set_fill_color(ctx, prv_lane_accent(note->lane));
    graphics_fill_circle(ctx, centre, (int16_t)(r - RB_NOTE_RING_W));

    // A dot of the lane's own bed colour in the middle. It reads as a hole in
    // the note, and it is what keeps the two lanes distinguishable at a glance
    // where the accents sit close together on a squashed palette.
    graphics_context_set_fill_color(ctx, prv_lane_bed(note->lane));
    graphics_fill_circle(ctx, centre, RB_NOTE_DOT_R);
  }
}

static void prv_draw_popup(GContext *ctx, uint32_t elapsed_ms) {
  // The popup belongs to the lane that was struck, not to the screen: it sits
  // inside that lane's band so the eye is told WHERE as well as WHAT. Driven off
  // that lane's own flash, so the plate and the lit target appear and vanish
  // together instead of on two separate timers.
  const uint8_t lane = feedback_last_lane();
  if (lane >= RB_LANE_COUNT) {
    return;
  }
  const RbJudgment judgment = feedback_lane_flash(lane, elapsed_ms);
  if (judgment == RB_JUDGE_NONE) {
    return;
  }

  const int16_t y = (int16_t)(prv_lane_y(lane) + (RB_LANE_H - RB_POPUP_H) / 2);
  const GRect box = GRect(RB_POPUP_INSET_X, y, RB_POPUP_W, RB_POPUP_H);

  prv_fill(ctx, box, (judgment == RB_JUDGE_MISS) ? GColorLightGray : GColorYellow);
  prv_draw_text(ctx, prv_judgment_text(judgment), FONT_KEY_GOTHIC_14_BOLD,
                GRect(box.origin.x, (int16_t)(box.origin.y + 5), box.size.w, 18),
                GTextAlignmentCenter, GColorBlack);
}

static void prv_draw_field(GContext *ctx, GRect bounds) {
  const uint32_t elapsed_ms = game_elapsed_ms();

  // Antialiasing is paid for per drawn pixel and is by far the most expensive
  // thing here: this layer draws a dozen or more circles, several of them
  // stroked, 25 times a second. The static screens keep it; this one cannot
  // afford it, and at this size the difference is not visible in motion.
  prv_draw_lane_beds(ctx, bounds.size.w);
  prv_draw_targets(ctx, elapsed_ms);
  prv_draw_notes(ctx, bounds.size.w, elapsed_ms);
  prv_draw_popup(ctx, elapsed_ms);
}

// ---------------------------------------------------------------------------
// Song band -- art tile, title/artist, progress
// ---------------------------------------------------------------------------

static void prv_draw_band(GContext *ctx, GRect bounds) {
  const int16_t w = bounds.size.w;
  const Chart *const chart = game_chart();
  const GColor art = prv_song_art(prv_selected_song());

  // Art tile: a record, drawn rather than shipped. Real per-song artwork would
  // be a bitmap resource each, and this app's entire resource budget is 4KB.
  prv_fill(ctx, GRect(0, RB_BAND_Y, RB_BAND_ART_W, RB_BAND_H), art);
  const GPoint disc = GPoint(RB_BAND_ART_W / 2, RB_BAND_Y + RB_BAND_H / 2);
  graphics_context_set_fill_color(ctx, GColorWhite);
  graphics_fill_circle(ctx, disc, RB_BAND_DISC_R);
  graphics_context_set_fill_color(ctx, art);
  graphics_fill_circle(ctx, disc, RB_BAND_HOLE_R);

  // Text sits on a black plate behind a bright rule, never on the artwork.
  prv_fill(ctx, GRect(RB_BAND_ART_W, RB_BAND_Y, RB_BAND_RULE_W, RB_BAND_H), GColorYellow);

  if (chart != NULL) {
    const int16_t text_x = (int16_t)(RB_BAND_ART_W + RB_BAND_RULE_W + 5);
    const int16_t text_w = (int16_t)(w - text_x - 3);

    // Word-wrapped rather than pre-split into two lines: the design breaks
    // two-word titles across lines, and letting the layout engine do it means a
    // new song needs no per-song line breaking in the generator.
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, chart->title, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       GRect(text_x, RB_BAND_Y - 3, text_w, 34), GTextOverflowModeWordWrap,
                       GTextAlignmentLeft, NULL);
    prv_draw_text(ctx, chart->artist, FONT_KEY_GOTHIC_14,
                  GRect(text_x, RB_BAND_Y + 29, text_w, 18),
                  GTextAlignmentLeft, RB_C_LANE_TOP_ACC);
  }

  // Progress, full width along the very bottom.
  const int16_t bar_y = RB_PROGRESS_Y;
  prv_fill(ctx, GRect(0, bar_y, w, RB_PROGRESS_H), GColorDarkGray);
  if (chart != NULL && chart->end_ms > 0) {
    uint32_t done = (game_elapsed_ms() * (uint32_t)w) / chart->end_ms;
    if (done > (uint32_t)w) {
      done = (uint32_t)w;
    }
    prv_fill(ctx, GRect(0, bar_y, (int16_t)done, RB_PROGRESS_H), GColorWhite);
  }
}

// ---------------------------------------------------------------------------
// Menus -- title, pause, results. Full screen, above everything else.
// ---------------------------------------------------------------------------

static void prv_draw_panel(GContext *ctx, GRect box) {
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, box, 6, GCornersAll);
  graphics_context_set_stroke_color(ctx, GColorWhite);
  graphics_context_set_stroke_width(ctx, 2);
  graphics_draw_round_rect(ctx, box, 6);
  graphics_context_set_stroke_width(ctx, 1);
}

static void prv_draw_lane_hint(GContext *ctx, int16_t w) {
  // The two lane colours behind the panel, so the button mapping is being
  // taught before the first note ever arrives.
  prv_fill(ctx, GRect(0, RB_LANE_TOP_Y, w, RB_LANE_H), RB_C_LANE_TOP_BED);
  prv_fill(ctx, GRect(0, RB_LANE_BOT_Y, w, RB_LANE_H), RB_C_LANE_BOT_BED);
}

static void prv_draw_title(GContext *ctx, GRect bounds) {
  const int16_t w = bounds.size.w;

  prv_fill(ctx, bounds, GColorBlack);
  prv_draw_lane_hint(ctx, w);
  prv_draw_panel(ctx, GRect(8, RB_TITLE_PANEL_Y, w - 16, RB_TITLE_PANEL_H));

  prv_draw_text(ctx, "ROCKBEAT", FONT_KEY_GOTHIC_24_BOLD,
                GRect(10, RB_TITLE_PANEL_Y + 4, w - 20, 30),
                GTextAlignmentCenter, RB_C_LANE_TOP_ACC);

  const uint8_t count = chart_count();
  const uint8_t selected = prv_selected_song();
  const uint8_t visible = (count < RB_TITLE_ROWS) ? count : RB_TITLE_ROWS;
  uint8_t first = 0;
  if (count > visible) {
    first = (selected >= visible) ? (uint8_t)(selected - visible + 1) : 0;
  }

  for (uint8_t row = 0; row < visible; row++) {
    const uint8_t index = (uint8_t)(first + row);
    const int16_t y = (int16_t)(RB_TITLE_LIST_Y + row * RB_TITLE_ROW_H);
    const bool is_selected = (index == selected);

    if (is_selected) {
      graphics_context_set_fill_color(ctx, GColorYellow);
      graphics_fill_rect(ctx, GRect(12, y, w - 24, RB_TITLE_ROW_H - 2), 4, GCornersAll);
    }
    prv_draw_text(ctx, chart_get(index)->title, FONT_KEY_GOTHIC_14_BOLD,
                  GRect(16, (int16_t)(y - 2), w - 32, RB_TITLE_ROW_H),
                  GTextAlignmentCenter, is_selected ? GColorBlack : GColorLightGray);
  }

  const int16_t after_list = (int16_t)(RB_TITLE_LIST_Y + visible * RB_TITLE_ROW_H);

  char best[32];
  snprintf(best, sizeof(best), "BEST %lu", (unsigned long)save_high_score(selected));
  prv_draw_text(ctx, best, FONT_KEY_GOTHIC_18_BOLD,
                GRect(10, (int16_t)(after_list + 2), w - 20, 22),
                GTextAlignmentCenter, GColorLightGray);
  prv_draw_text(ctx, "SELECT to play", FONT_KEY_GOTHIC_14_BOLD,
                GRect(10, (int16_t)(after_list + 24), w - 20, 18),
                GTextAlignmentCenter, GColorWhite);
  prv_draw_text(ctx, "UP / DOWN choose", FONT_KEY_GOTHIC_14,
                GRect(10, (int16_t)(after_list + 40), w - 20, 18),
                GTextAlignmentCenter, GColorDarkGray);
}

static void prv_draw_pause(GContext *ctx, GRect bounds) {
  const int16_t w = bounds.size.w;

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

static void prv_draw_results(GContext *ctx, GRect bounds) {
  const int16_t w = bounds.size.w;
  const uint16_t misses = game_count(RB_JUDGE_MISS);
  const uint16_t accuracy = game_accuracy_pct();

  prv_fill(ctx, bounds, GColorBlack);
  prv_draw_lane_hint(ctx, w);
  prv_draw_panel(ctx, GRect(6, 14, w - 12, 200));

  prv_draw_text(ctx, prv_rank(accuracy, misses), FONT_KEY_BITHAM_30_BLACK,
                GRect(8, 18, w - 16, 36), GTextAlignmentCenter, GColorYellow);

  char buf[32];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)game_score());
  prv_draw_text(ctx, buf, FONT_KEY_LECO_32_BOLD_NUMBERS, GRect(8, 54, w - 16, 38),
                GTextAlignmentCenter, GColorWhite);

  if (game_score() >= save_high_score(prv_selected_song()) && game_score() > 0) {
    prv_draw_text(ctx, "NEW BEST", FONT_KEY_GOTHIC_14_BOLD, GRect(8, 92, w - 16, 20),
                  GTextAlignmentCenter, GColorYellow);
  }

  snprintf(buf, sizeof(buf), "%u%% accuracy", (unsigned)accuracy);
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_18, GRect(8, 112, w - 16, 24),
                GTextAlignmentCenter, GColorLightGray);

  snprintf(buf, sizeof(buf), "PERFECT   %u", (unsigned)game_count(RB_JUDGE_PERFECT));
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_14, GRect(20, 138, w - 40, 18),
                GTextAlignmentLeft, GColorWhite);
  snprintf(buf, sizeof(buf), "GOOD      %u", (unsigned)game_count(RB_JUDGE_GOOD));
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_14, GRect(20, 155, w - 40, 18),
                GTextAlignmentLeft, GColorWhite);
  snprintf(buf, sizeof(buf), "MISS      %u", (unsigned)misses);
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_14, GRect(20, 172, w - 40, 18),
                GTextAlignmentLeft, GColorWhite);

  snprintf(buf, sizeof(buf), "max combo x%u", (unsigned)game_max_combo());
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_14, GRect(8, 192, w - 16, 18),
                GTextAlignmentCenter, GColorLightGray);
}

static void prv_draw_menu(GContext *ctx, GRect bounds) {

  switch (game_screen()) {
    case RB_SCREEN_TITLE:
      prv_draw_title(ctx, bounds);
      break;
    case RB_SCREEN_PAUSED:
      // Drawn straight over the live playfield underneath -- nothing clears the
      // background, so the player sees exactly what they are coming back to.
      prv_draw_pause(ctx, bounds);
      break;
    case RB_SCREEN_RESULTS:
      prv_draw_results(ctx, bounds);
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------

void render_update_proc(Layer *layer, GContext *ctx) {
  const GRect bounds = layer_get_bounds(layer);
  const RbScreen screen = game_screen();
  const bool playing = (screen == RB_SCREEN_PLAYING || screen == RB_SCREEN_PAUSED);

  // Antialiasing is paid for per drawn pixel and is by far the most expensive
  // thing on this screen: a gameplay frame draws a dozen or more circles, some
  // of them stroked, 25 times a second. The static screens keep it; the
  // playfield cannot afford it, and at this size the difference is not visible
  // in motion.
  graphics_context_set_antialiased(ctx, !playing);

  prv_fill(ctx, bounds, GColorBlack);

  if (playing) {
    prv_draw_hud(ctx, bounds);
    prv_draw_field(ctx, bounds);
    prv_draw_band(ctx, bounds);
  }
  // The pause panel is drawn over the live playfield, so the player sees
  // exactly what they are coming back to.
  prv_draw_menu(ctx, bounds);
}
