#include "render.h"

#include "chart.h"
#include "clock.h"
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
//
// There are TWO palettes because there are two screens, and the second one is
// not a dimmed version of the first -- see the flint block below for what was
// measured and why the firmware's own reduction is not usable.
// ---------------------------------------------------------------------------

#define RB_ARGB(byte) ((GColor8){ .argb = (uint8_t)(byte) })

#if defined(PBL_COLOR)

#define RB_C_LANE_TOP_BED   RB_ARGB(0b11010100)  // #555500 olive
#define RB_C_LANE_TOP_RAIL  RB_ARGB(0b11100100)  // #AA5500
#define RB_C_LANE_TOP_ACC   RB_ARGB(0b11111000)  // #FFAA00 orange
#define RB_C_LANE_BOT_BED   RB_ARGB(0b11000101)  // #005555 teal
#define RB_C_LANE_BOT_RAIL  RB_ARGB(0b11000110)  // #0055AA
#define RB_C_LANE_BOT_ACC   RB_ARGB(0b11001011)  // #00AAFF blue

// A lane's edge is its own bed colour, which makes it invisible -- and that is
// correct here: the FILL is what makes the band read as a lane, so outlining it
// would only add a line the design does not have.
#define RB_C_LANE_TOP_EDGE  RB_C_LANE_TOP_BED
#define RB_C_LANE_BOT_EDGE  RB_C_LANE_BOT_BED

#define RB_C_HILITE   GColorYellow     // combo numeral, rules, selected plates
#define RB_C_DIM      GColorLightGray  // secondary type
#define RB_C_FAINT    GColorDarkGray   // tertiary type
#define RB_C_TRACK    GColorDarkGray   // the unfilled part of the progress bar
#define RB_C_TRACK_EDGE GColorDarkGray // invisible: the track's own colour

// One record colour per song, so the art tile is not the same plate every time.
#define RB_C_ART_0    RB_ARGB(0b11100001)  // #AA0055
#define RB_C_ART_1    RB_ARGB(0b11000110)  // #0055AA
#define RB_C_ART_2    RB_ARGB(0b11100100)  // #AA5500

// The body of a note, inside its white rim. On colour that is the lane's own
// accent, so a note is a filled disc.
#define RB_C_LANE_TOP_NOTE  RB_C_LANE_TOP_ACC
#define RB_C_LANE_BOT_NOTE  RB_C_LANE_BOT_ACC

// A struck ring fills with the lane colour behind a white rim, and a PERFECT
// adds a white core.
#define RB_C_STRUCK_CORE GColorWhite

#else

// --- flint, and any other 2-colour screen ---------------------------------
//
// This palette exists because the firmware's automatic reduction was MEASURED
// and found to destroy the design, not because a B/W screen was assumed to need
// help. Installing the emery palette on flint and decoding the screenshot gave
// exactly two distinct pixel values, and both lane beds -- #555500 olive and
// #005555 teal -- had landed on pure BLACK, the same value as the background.
// The lanes had vanished, and with them every cue distinguishing them. That is
// not a bug in the reduction: it is a luminance threshold, and it cannot know
// which of two mid-tone colours was carrying meaning.
//
// The rule this palette follows is that on a black screen INK IS SCARCE, so
// anything hue used to carry is carried by shape or position instead:
//   - beds go black and a lane is drawn as an OUTLINE, because a white bed
//     would hide the white notes travelling along it;
//   - both accents go white, because the two lanes are already told apart by
//     position and by the badge inside each target ring, which names the
//     buttons that play them;
//   - the greys go white, because a mid grey IS black here, and a line of type
//     that reduces to the background is a line the player never sees. The
//     hierarchy that grey used to carry is carried by type size instead.
#define RB_C_LANE_TOP_BED   GColorBlack
#define RB_C_LANE_TOP_RAIL  GColorWhite
#define RB_C_LANE_TOP_ACC   GColorWhite
#define RB_C_LANE_BOT_BED   GColorBlack
#define RB_C_LANE_BOT_RAIL  GColorWhite
#define RB_C_LANE_BOT_ACC   GColorWhite

#define RB_C_LANE_TOP_EDGE  GColorWhite
#define RB_C_LANE_BOT_EDGE  GColorWhite

#define RB_C_HILITE   GColorWhite
#define RB_C_DIM      GColorWhite
#define RB_C_FAINT    GColorWhite
#define RB_C_TRACK    GColorBlack
#define RB_C_TRACK_EDGE GColorWhite  // or the empty track would not be there

// Per-song art colour is one of the things two colours cannot carry. The record
// still reads -- it is a white disc with a black spindle hole on a black tile.
#define RB_C_ART_0    GColorBlack
#define RB_C_ART_1    GColorBlack
#define RB_C_ART_2    GColorBlack

// Notes are HOLLOW here, and that was measured too. With the body white like
// everything else a note became a solid white disc, and a solid white disc is
// the same value as the RAIL it travels along: a screenshot row through the
// rail came back white from x=0 to x=99 with nothing but the 7px centre dots
// breaking it, so a run of notes read as one long white bar rather than as
// notes. Emptying the body puts a black hole in each one, which both separates
// it from the rail and separates it from its neighbour -- consecutive notes in
// a lane are only 2px apart at the closest, on either platform, so the gap was
// never what was doing that work. It also matches the menu icon, whose rings
// are hollow for a related reason.
#define RB_C_LANE_TOP_NOTE  GColorBlack
#define RB_C_LANE_BOT_NOTE  GColorBlack

// With the accent white, a struck ring is a solid white disc -- which would
// make PERFECT and GOOD look identical. The core inverts instead, so PERFECT is
// the one with a black bullseye punched in it.
#define RB_C_STRUCK_CORE GColorBlack

#endif

// The bottom lane's badge: a bar for SELECT above a down arrow for DOWN. Kept
// local to this file rather than added to rb_config.h because they are not
// tunables -- they are the geometry of one mark, sized once to fit the ring, and
// nothing outside render.c can use them.
//
// The bar is deliberately WIDER than the arrow (16 against 13) and the arrow is
// below the design size, so the two marks are not competing for the same weight.
// The top lane's arrow is untouched at RB_ARROW_LEN/RB_ARROW_HALF.
//
// Stack height is BAR_H + BAR_GAP + BADGE_ARROW_LEN, which must be ODD so it
// centres exactly on the ring; keep it odd if you retune these.
#if defined(PBL_PLATFORM_FLINT)
// Stack 15. The ring's clear radius is RB_TARGET_R - RB_TARGET_RING_W = 11, and
// the worst pixel is a bar corner at (6, 7) -- r=9.2.
#define RB_BADGE_BAR_W 13
#define RB_BADGE_BAR_H 5
#define RB_BADGE_BAR_GAP 3
#define RB_BADGE_ARROW_LEN 7
#define RB_BADGE_ARROW_HALF 5
#else
// Stack 19. Worst pixel is a bar corner at (8, 9) -- r=12.0 against the ring's
// 15px clear radius.
#define RB_BADGE_BAR_W 16
#define RB_BADGE_BAR_H 6
#define RB_BADGE_BAR_GAP 4
#define RB_BADGE_ARROW_LEN 9
#define RB_BADGE_ARROW_HALF 6
#endif

// One record colour per song, so the art tile is not the same plate every time.
static GColor prv_song_art(uint8_t song) {
  switch (song) {
    case 1:  return RB_C_ART_1;
    case 2:  return RB_C_ART_2;
    default: return RB_C_ART_0;
  }
}

static GColor prv_lane_accent(uint8_t lane) {
  return (lane == RB_LANE_TOP) ? RB_C_LANE_TOP_ACC : RB_C_LANE_BOT_ACC;
}

static GColor prv_lane_bed(uint8_t lane) {
  return (lane == RB_LANE_TOP) ? RB_C_LANE_TOP_BED : RB_C_LANE_BOT_BED;
}

// The fill inside a note's white rim -- see RB_C_LANE_TOP_NOTE.
static GColor prv_lane_note(uint8_t lane) {
  return (lane == RB_LANE_TOP) ? RB_C_LANE_TOP_NOTE : RB_C_LANE_BOT_NOTE;
}

static GColor prv_lane_rail(uint8_t lane) {
  return (lane == RB_LANE_TOP) ? RB_C_LANE_TOP_RAIL : RB_C_LANE_BOT_RAIL;
}

// The lane's top and bottom edges. On a colour screen this is the bed's own
// colour, so the lines are invisible and the band is exactly the design's flat
// fill; on a 2-colour screen the bed is black and these lines ARE the lane.
static GColor prv_lane_edge(uint8_t lane) {
  return (lane == RB_LANE_TOP) ? RB_C_LANE_TOP_EDGE : RB_C_LANE_BOT_EDGE;
}

// What fills the hole of a resting ring while its button is held. On colour
// that is the lane's own bed -- deliberately dim, so a press that hit nothing
// reads as the pad going down rather than as a score. Two colours have no dim,
// so the mark INVERTS instead: the hole goes white and prv_badge() takes the
// badge to black, which is still unmistakably not a hit (a hit is a solid disc
// with no badge in it at all).
static GColor prv_lane_press(uint8_t lane) {
  return PBL_IF_COLOR_ELSE(prv_lane_bed(lane), GColorWhite);
}

static GColor prv_badge(uint8_t lane, bool pressed) {
  return (pressed) ? PBL_IF_COLOR_ELSE(prv_lane_accent(lane), GColorBlack)
                   : prv_lane_accent(lane);
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

// ---------------------------------------------------------------------------
// Title carousel
//
// A song title wider than its row scrolls through it. See the long note in
// rb_config.h for why the step is a character rather than a pixel: without a
// clip-box call there is no way to let text overhang its box on a single shared
// canvas, and stepping the START of the string cannot overhang at all.
// ---------------------------------------------------------------------------

// Natural single-line width of `text`. The box is deliberately far wider than
// any screen so nothing wraps and the answer is the text's own width.
static int16_t prv_text_w(const char *text, const char *font_key) {
  return graphics_text_layout_get_content_size(
             text, fonts_get_system_font(font_key), GRect(0, 0, 1000, 200),
             GTextOverflowModeWordWrap, GTextAlignmentLeft)
      .w;
}

// Byte offset of the k-th CHARACTER of `s`. Stepping by byte would be enough
// for the titles compiled in today, all of which are ASCII, but it would cut a
// multi-byte character in half the first time one appeared and render a
// replacement glyph mid-scroll. Continuation bytes are 10xxxxxx.
static uint16_t prv_utf8_offset(const char *str, uint8_t k) {
  uint16_t i = 0;
  while (k > 0 && str[i] != '\0') {
    i++;
    while ((str[i] & 0xC0) == 0x80) {
      i++;
    }
    k--;
  }
  return i;
}

// How many leading characters have to scroll off before the rest of `text` fits
// in `box_w`. 0 means it already fits and nothing should scroll.
//
// Cached, because the scan costs one text measurement per character and the
// answer only changes when the selection does. Song titles are string literals
// in chart.c, so the POINTER is a stable identity for the string -- comparing
// it is both cheaper and safer than comparing contents.
static uint8_t prv_marquee_kmax(const char *text, const char *font_key, int16_t box_w) {
  static const char *s_text;
  static const char *s_font;
  static int16_t s_box_w;
  static uint8_t s_kmax;

  if (text == s_text && font_key == s_font && box_w == s_box_w) {
    return s_kmax;
  }

  uint8_t k = 0;
  while (text[prv_utf8_offset(text, k)] != '\0' &&
         prv_text_w(text + prv_utf8_offset(text, k), font_key) > box_w) {
    k++;
  }

  s_text = text;
  s_font = font_key;
  s_box_w = box_w;
  s_kmax = k;
  return k;
}

// Where the carousel is in its cycle: hold at the start, step through to the
// end, hold there, then begin again. Driven off a clock rather than off a
// counter, so a dropped or late frame changes nothing about where the text is.
static uint8_t prv_marquee_k(uint8_t kmax, uint32_t now_ms) {
  if (kmax == 0) {
    return 0;
  }
  const uint32_t travel_ms = (uint32_t)kmax * RB_MARQUEE_STEP_MS;
  const uint32_t cycle_ms = travel_ms + 2u * RB_MARQUEE_HOLD_MS;
  uint32_t t = now_ms % cycle_ms;

  if (t < RB_MARQUEE_HOLD_MS) {
    return 0;  // holding on the first character
  }
  t -= RB_MARQUEE_HOLD_MS;
  if (t < travel_ms) {
    return (uint8_t)(t / RB_MARQUEE_STEP_MS);
  }
  return kmax;  // holding on the tail
}

// Draws `text` in `box`, scrolling it if it does not fit. Returns nothing the
// caller has to undo: the text is always drawn INSIDE box, because what moves
// is which character starts it.
//
// A scrolling line is left-aligned even where a static one is centred -- the
// tail shortens as it advances, and re-centring every step would make the line
// twitch sideways as well as scroll.
static void prv_draw_title_text(GContext *ctx, const char *text, const char *font_key,
                                GRect box, GTextAlignment align, GColor color,
                                uint32_t now_ms) {
  const uint8_t kmax = prv_marquee_kmax(text, font_key, box.size.w);
  if (kmax == 0) {
    prv_draw_text(ctx, text, font_key, box, align, color);
    return;
  }
  const uint8_t k = prv_marquee_k(kmax, now_ms);
  prv_draw_text(ctx, text + prv_utf8_offset(text, k), font_key, box,
                GTextAlignmentLeft, color);
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

  // The combo box is anchored to the right edge rather than to a fixed x, so
  // the HUD stays correct on either screen width.
  const int16_t combo_x = (int16_t)(w - RB_HUD_COMBO_W - RB_HUD_PAD_R);

  prv_draw_text(ctx, "SCORE", FONT_KEY_GOTHIC_14,
                GRect(RB_HUD_PAD_X, -3, RB_HUD_SCORE_W, 18),
                GTextAlignmentLeft, RB_C_DIM);

  char buf[16];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)game_score());
  prv_draw_text(ctx, buf, RB_HUD_SCORE_FONT,
                GRect(RB_HUD_PAD_X, RB_HUD_SCORE_DY, RB_HUD_SCORE_W, RB_HUD_SCORE_H),
                GTextAlignmentLeft, GColorWhite);

  // The combo is the loudest thing on the screen by design -- a big yellow
  // numeral with its label tucked underneath.
  snprintf(buf, sizeof(buf), "%u", (unsigned)game_combo());
  prv_draw_text(ctx, buf, RB_HUD_COMBO_FONT,
                GRect(combo_x, RB_HUD_COMBO_DY, RB_HUD_COMBO_W, RB_HUD_COMBO_H),
                GTextAlignmentRight, RB_C_HILITE);
  prv_draw_text(ctx, "COMBO", FONT_KEY_GOTHIC_14,
                GRect(combo_x, RB_HUD_LABEL_DY, RB_HUD_COMBO_W, 18),
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

// One lane's band: the bed and its two edges, and NOT the rail. The rail marks
// where notes travel, so it belongs to the playfield; the title and results
// screens draw these same bands behind their panels only to teach the button
// mapping, and a travel line there would be describing motion that is not
// happening. (It also kept the emery screens byte-identical to the design,
// which is how the split was noticed: folding the rail in here put a stripe
// down the margins either side of the title panel.)
static void prv_draw_lane_band(GContext *ctx, int16_t w, uint8_t lane, int16_t y) {
  prv_fill(ctx, GRect(0, y, w, RB_LANE_H), prv_lane_bed(lane));

  // Edges. On colour these are the bed's own colour and draw nothing visible;
  // on a 2-colour screen the bed is black and these are what makes the band a
  // lane rather than more background. Drawing them unconditionally keeps the
  // difference in the palette, where it can be read, instead of in an #if here.
  const GColor edge = prv_lane_edge(lane);
  prv_fill(ctx, GRect(0, y, w, 1), edge);
  prv_fill(ctx, GRect(0, (int16_t)(y + RB_LANE_H - 1), w, 1), edge);
}

static void prv_draw_lane_beds(GContext *ctx, int16_t w) {
  for (uint8_t lane = 0; lane < RB_LANE_COUNT; lane++) {
    const int16_t y = prv_lane_y(lane);

    prv_draw_lane_band(ctx, w, lane, y);

    // A rail down the middle of the bed: the line the notes travel along, and
    // what makes an empty lane still read as a lane.
    prv_fill(ctx, GRect(0, (int16_t)(y + RB_LANE_RAIL_DY), RB_TARGET_ZONE_X, RB_LANE_RAIL_H),
             prv_lane_rail(lane));

    // The target zone is plain black so a note crossing the target is never read
    // against a coloured bed, with a bright rule marking the boundary.
    prv_fill(ctx, GRect(RB_TARGET_ZONE_X, y, (int16_t)(w - RB_TARGET_ZONE_X), RB_LANE_H),
             GColorBlack);
    prv_fill(ctx, GRect(RB_TARGET_DIVIDER_X, y, RB_TARGET_DIVIDER_W, RB_LANE_H),
             RB_C_DIM);
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
      //
      // A press that hit nothing still fills the ring's hole with the lane's own
      // BED colour. Deliberately dim, and deliberately keeping the ring and the
      // badge on top: a hit is a bright white-and-accent burst that covers the
      // badge entirely, so the two can never be confused. This one reads as the
      // pad going down, not as a score.
      const bool pressed = feedback_lane_pressed(lane, elapsed_ms);
      if (pressed) {
        graphics_context_set_fill_color(ctx, prv_lane_press(lane));
        graphics_fill_circle(ctx, centre, (int16_t)(RB_TARGET_R - RB_TARGET_RING_W));
      }

      graphics_context_set_stroke_color(ctx, accent);
      graphics_context_set_stroke_width(ctx, RB_TARGET_RING_W);
      graphics_draw_circle(ctx, centre, RB_TARGET_R - RB_TARGET_RING_W / 2);
      graphics_context_set_stroke_width(ctx, 1);

      graphics_context_set_fill_color(ctx, prv_badge(lane, pressed));
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
      graphics_context_set_fill_color(ctx, RB_C_STRUCK_CORE);
      graphics_fill_circle(
          ctx, centre,
          (int16_t)(RB_TARGET_R - RB_TARGET_RING_W - RB_TARGET_CORE_INSET));
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
    graphics_context_set_fill_color(ctx, prv_lane_note(note->lane));
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

  prv_fill(ctx, box, (judgment == RB_JUDGE_MISS) ? RB_C_DIM : RB_C_HILITE);
  prv_draw_text(ctx, prv_judgment_text(judgment), FONT_KEY_GOTHIC_14_BOLD,
                GRect(box.origin.x, (int16_t)(box.origin.y + RB_POPUP_TEXT_DY),
                      box.size.w, 18),
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
  prv_fill(ctx, GRect(RB_BAND_ART_W, RB_BAND_Y, RB_BAND_RULE_W, RB_BAND_H), RB_C_HILITE);

  if (chart != NULL) {
    const int16_t text_x = (int16_t)(RB_BAND_ART_W + RB_BAND_RULE_W + 5);
    const int16_t text_w = (int16_t)(w - text_x - 3);

    // Word-wrapped rather than pre-split into two lines: the design breaks
    // two-word titles across lines, and letting the layout engine do it means a
    // new song needs no per-song line breaking in the generator. A band too
    // short for two lines ellipsises instead -- see RB_BAND_TITLE_OVERFLOW.
    const GRect title_box =
        GRect(text_x, (int16_t)(RB_BAND_Y + RB_BAND_TITLE_DY), text_w, RB_BAND_TITLE_H);
#if RB_BAND_TITLE_MARQUEE
    // A one-line band has nowhere to wrap to, so a long title scrolls here too.
    // It rides the song clock rather than the wall clock: the band is only ever
    // drawn while a song is loaded, and using the same time base as everything
    // else on this screen means it stops dead when the game is paused instead
    // of scrolling on behind the pause panel.
    prv_draw_title_text(ctx, chart->title, FONT_KEY_GOTHIC_14_BOLD, title_box,
                        GTextAlignmentLeft, GColorWhite, game_elapsed_ms());
#else
    graphics_context_set_text_color(ctx, GColorWhite);
    graphics_draw_text(ctx, chart->title, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
                       title_box, RB_BAND_TITLE_OVERFLOW, GTextAlignmentLeft, NULL);
#endif
    prv_draw_text(ctx, chart->artist, FONT_KEY_GOTHIC_14,
                  GRect(text_x, (int16_t)(RB_BAND_Y + RB_BAND_ARTIST_DY), text_w, 18),
                  GTextAlignmentLeft, RB_C_LANE_TOP_ACC);
  }

  // Progress, full width along the very bottom. The track carries a top edge so
  // that an empty bar is still visible on a screen where the track colour and
  // the background are the same black.
  const int16_t bar_y = RB_PROGRESS_Y;
  prv_fill(ctx, GRect(0, bar_y, w, RB_PROGRESS_H), RB_C_TRACK);
  prv_fill(ctx, GRect(0, bar_y, w, 1), RB_C_TRACK_EDGE);
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
  // The two lane bands behind the panel, so the button mapping is being taught
  // before the first note ever arrives. Same helper the playfield uses, so a
  // 2-colour screen gets the outlines here too rather than two invisible fills
  // -- but without the rail, which belongs to the playfield.
  prv_draw_lane_band(ctx, w, RB_LANE_TOP, RB_LANE_TOP_Y);
  prv_draw_lane_band(ctx, w, RB_LANE_BOT, RB_LANE_BOT_Y);
}

// Scroll indicator for the song list: a hairline track the height of the list
// with a thicker thumb on it, in the reserved right-hand column.
//
// A thumb rather than a pair of little arrows, for two reasons. It answers
// "is there more above / below" the same way arrows do -- the gap above and
// below the thumb IS the answer -- but it also says how MUCH more, and how far
// down the list you are. And it costs no VERTICAL space, which is what the
// title screen has none of: the gaps above and below the list are 2px on both
// platforms, so arrows would have had to come out of a row or a hint.
//
// Drawn only when the list actually scrolls, so four songs look exactly as they
// did before there were five.
static void prv_draw_list_scroll(GContext *ctx, int16_t w, uint8_t count, uint8_t visible,
                                 uint8_t first) {
  if (count <= visible) {
    return;
  }

  const int16_t list_h = (int16_t)(visible * RB_TITLE_ROW_H);
  const int16_t col_x = (int16_t)(w - 12 - RB_TITLE_SCROLL_W);
  const int16_t bar_x = (int16_t)(col_x + (RB_TITLE_SCROLL_W - RB_TITLE_SCROLL_BAR_W) / 2);

  // The track is a single pixel: enough to show the extent being scrolled
  // through without becoming a second piece of furniture next to the rows.
  prv_fill(ctx, GRect((int16_t)(bar_x + RB_TITLE_SCROLL_BAR_W / 2), RB_TITLE_LIST_Y, 1, list_h),
           RB_C_FAINT);

  int16_t thumb_h = (int16_t)((int32_t)list_h * visible / count);
  if (thumb_h < RB_TITLE_SCROLL_MIN_H) {
    thumb_h = RB_TITLE_SCROLL_MIN_H;
  }
  const int16_t travel = (int16_t)(list_h - thumb_h);
  const int16_t thumb_y =
      (int16_t)(RB_TITLE_LIST_Y + (int16_t)((int32_t)travel * first / (count - visible)));

  prv_fill(ctx, GRect(bar_x, thumb_y, RB_TITLE_SCROLL_BAR_W, thumb_h), RB_C_DIM);
}

static void prv_draw_title(GContext *ctx, GRect bounds) {
  const int16_t w = bounds.size.w;

  prv_fill(ctx, bounds, GColorBlack);
  prv_draw_lane_hint(ctx, w);
  prv_draw_panel(ctx, GRect(8, RB_TITLE_PANEL_Y, w - 16, RB_TITLE_PANEL_H));

  prv_draw_text(ctx, "ROCKBEAT", RB_TITLE_FONT,
                GRect(10, RB_TITLE_PANEL_Y + 4, w - 20, RB_TITLE_HEADER_H),
                GTextAlignmentCenter, RB_C_LANE_TOP_ACC);

  const uint8_t count = chart_count();
  const uint8_t selected = prv_selected_song();
  const uint8_t visible = (count < RB_TITLE_ROWS) ? count : RB_TITLE_ROWS;
  uint8_t first = 0;
  if (count > visible) {
    first = (selected >= visible) ? (uint8_t)(selected - visible + 1) : 0;
  }

  // The list gives up a narrow column on its right to the scroll indicator, so
  // the rows are that much shorter whether or not the indicator is showing.
  // Keeping the width constant matters more than reclaiming it: a row that
  // changed width when the fifth song was added would make the indicator's
  // arrival look like a layout bug.
  const int16_t row_w = (int16_t)(w - 24 - RB_TITLE_SCROLL_W);
  const uint32_t now_ms = clock_now_ms();

  for (uint8_t row = 0; row < visible; row++) {
    const uint8_t index = (uint8_t)(first + row);
    const int16_t y = (int16_t)(RB_TITLE_LIST_Y + row * RB_TITLE_ROW_H);
    const bool is_selected = (index == selected);

    if (is_selected) {
      graphics_context_set_fill_color(ctx, RB_C_HILITE);
      graphics_fill_rect(ctx, GRect(12, y, row_w, RB_TITLE_ROW_H - 2), 4, GCornersAll);
    }

    // Only the SELECTED row scrolls. Every row scrolling at once would be four
    // things moving in a menu that is being read, and the selected one is the
    // only title the player is deciding about.
    const GRect box = GRect(16, (int16_t)(y - 2), (int16_t)(row_w - 8), RB_TITLE_ROW_H);
    const GColor color = is_selected ? GColorBlack : RB_C_DIM;
    if (is_selected) {
      prv_draw_title_text(ctx, chart_get(index)->title, FONT_KEY_GOTHIC_14_BOLD, box,
                          GTextAlignmentCenter, color, now_ms);
    } else {
      prv_draw_text(ctx, chart_get(index)->title, FONT_KEY_GOTHIC_14_BOLD, box,
                    GTextAlignmentCenter, color);
    }
  }

  prv_draw_list_scroll(ctx, w, count, visible, first);

  const int16_t after_list = (int16_t)(RB_TITLE_LIST_Y + visible * RB_TITLE_ROW_H);

  char best[32];
  snprintf(best, sizeof(best), "BEST %lu", (unsigned long)save_high_score(selected));
  prv_draw_text(ctx, best, FONT_KEY_GOTHIC_18_BOLD,
                GRect(10, (int16_t)(after_list + 2), w - 20, 22),
                GTextAlignmentCenter, RB_C_DIM);
  prv_draw_text(ctx, "SELECT to play", FONT_KEY_GOTHIC_14_BOLD,
                GRect(10, (int16_t)(after_list + 24), w - 20, 18),
                GTextAlignmentCenter, GColorWhite);
  prv_draw_text(ctx, "UP / DOWN choose", FONT_KEY_GOTHIC_14,
                GRect(10, (int16_t)(after_list + 40), w - 20, 18),
                GTextAlignmentCenter, RB_C_FAINT);
}

static void prv_draw_pause(GContext *ctx, GRect bounds) {
  const int16_t w = bounds.size.w;

  const int16_t py = RB_PAUSE_PANEL_Y;

  prv_draw_panel(ctx, GRect(12, py, w - 24, RB_PAUSE_PANEL_H));
  prv_draw_text(ctx, "PAUSED", RB_PAUSE_TITLE_FONT,
                GRect(14, (int16_t)(py + RB_PAUSE_TITLE_DY), w - 28, 34),
                GTextAlignmentCenter, RB_C_HILITE);
  prv_draw_text(ctx, "SELECT resume", FONT_KEY_GOTHIC_18,
                GRect(14, (int16_t)(py + RB_PAUSE_ROW1_DY), w - 28, 24),
                GTextAlignmentCenter, GColorWhite);
  prv_draw_text(ctx, "UP restart", FONT_KEY_GOTHIC_14,
                GRect(14, (int16_t)(py + RB_PAUSE_ROW2_DY), w - 28, 20),
                GTextAlignmentCenter, RB_C_DIM);
  prv_draw_text(ctx, "DOWN / BACK quit", FONT_KEY_GOTHIC_14,
                GRect(14, (int16_t)(py + RB_PAUSE_ROW3_DY), w - 28, 20),
                GTextAlignmentCenter, RB_C_DIM);
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

  const int16_t py = RB_RESULT_PANEL_Y;

  prv_fill(ctx, bounds, GColorBlack);
  prv_draw_lane_hint(ctx, w);
  prv_draw_panel(ctx, GRect(6, py, w - 12, RB_RESULT_PANEL_H));

  prv_draw_text(ctx, prv_rank(accuracy, misses), FONT_KEY_BITHAM_30_BLACK,
                GRect(8, (int16_t)(py + RB_RESULT_RANK_DY), w - 16, 36),
                GTextAlignmentCenter, RB_C_HILITE);

  char buf[32];
  snprintf(buf, sizeof(buf), "%lu", (unsigned long)game_score());
  prv_draw_text(ctx, buf, FONT_KEY_LECO_32_BOLD_NUMBERS,
                GRect(8, (int16_t)(py + RB_RESULT_SCORE_DY), w - 16, RB_RESULT_SCORE_H),
                GTextAlignmentCenter, GColorWhite);

  if (game_score() >= save_high_score(prv_selected_song()) && game_score() > 0) {
    prv_draw_text(ctx, "NEW BEST", FONT_KEY_GOTHIC_14_BOLD,
                  GRect(8, (int16_t)(py + RB_RESULT_BEST_DY), w - 16, 20),
                  GTextAlignmentCenter, RB_C_HILITE);
  }

  snprintf(buf, sizeof(buf), "%u%% accuracy", (unsigned)accuracy);
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_18,
                GRect(8, (int16_t)(py + RB_RESULT_ACC_DY), w - 16, 24),
                GTextAlignmentCenter, RB_C_DIM);

  // The judgment breakdown. A screen with 60 fewer vertical pixels cannot give
  // it three rows and still show the max combo underneath, so RB_RESULT_COUNTS_
  // STEP == 0 folds it onto one centred line. This is the ONLY place the two
  // platforms show the same information in a different SHAPE rather than just
  // at a different size, and the numbers themselves are identical either way.
  const int16_t counts_y = (int16_t)(py + RB_RESULT_COUNTS_DY);
#if RB_RESULT_COUNTS_STEP == 0
  snprintf(buf, sizeof(buf), "P %u   G %u   M %u",
           (unsigned)game_count(RB_JUDGE_PERFECT),
           (unsigned)game_count(RB_JUDGE_GOOD), (unsigned)misses);
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_14, GRect(8, counts_y, w - 16, 18),
                GTextAlignmentCenter, GColorWhite);
#else
  snprintf(buf, sizeof(buf), "PERFECT   %u", (unsigned)game_count(RB_JUDGE_PERFECT));
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_14, GRect(20, counts_y, w - 40, 18),
                GTextAlignmentLeft, GColorWhite);
  snprintf(buf, sizeof(buf), "GOOD      %u", (unsigned)game_count(RB_JUDGE_GOOD));
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_14,
                GRect(20, (int16_t)(counts_y + RB_RESULT_COUNTS_STEP), w - 40, 18),
                GTextAlignmentLeft, GColorWhite);
  snprintf(buf, sizeof(buf), "MISS      %u", (unsigned)misses);
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_14,
                GRect(20, (int16_t)(counts_y + 2 * RB_RESULT_COUNTS_STEP), w - 40, 18),
                GTextAlignmentLeft, GColorWhite);
#endif

  snprintf(buf, sizeof(buf), "max combo x%u", (unsigned)game_max_combo());
  prv_draw_text(ctx, buf, FONT_KEY_GOTHIC_14,
                GRect(8, (int16_t)(py + RB_RESULT_COMBO_DY), w - 16, 18),
                GTextAlignmentCenter, RB_C_DIM);
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

bool render_title_animates(void) {
  const Chart *const chart = chart_get(prv_selected_song());
  if (chart == NULL) {
    return false;
  }
  // The row box, computed exactly as prv_draw_title does it. PBL_DISPLAY_WIDTH
  // rather than layer bounds because there is no GContext here -- the two agree,
  // and text measurement is the one graphics call that needs neither.
  const int16_t row_w = (int16_t)(PBL_DISPLAY_WIDTH - 24 - RB_TITLE_SCROLL_W);
  return prv_marquee_kmax(chart->title, FONT_KEY_GOTHIC_14_BOLD,
                          (int16_t)(row_w - 8)) > 0;
}

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
