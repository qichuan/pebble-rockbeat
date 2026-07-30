#include "input.h"

#include "chart.h"
#include "clock.h"

static RbInputHandlers s_handlers;

// One handler serves both lane buttons; the recognizer tells us which one
// fired. That keeps the timestamp on the very first line of exactly one
// function rather than duplicated across two.
static void prv_lane_down(ClickRecognizerRef recognizer, void *context) {
  // FIRST statement, unconditionally. Everything after it -- the button lookup,
  // the judging, the drawing -- is latency that would otherwise be charged to
  // the player's timing.
  const uint32_t press_now_ms = clock_now_ms();
  (void)context;

  const ButtonId button = click_recognizer_get_button_id(recognizer);
  uint8_t lane = RB_LANE_NONE;
  if (button == BUTTON_ID_UP) {
    lane = RB_LANE_TOP;
  } else if (button == BUTTON_ID_SELECT) {
    lane = RB_LANE_MID;
  }

  if (s_handlers.on_lane_hit != NULL) {
    s_handlers.on_lane_hit(lane, press_now_ms);
  }
}

static void prv_back_click(ClickRecognizerRef recognizer, void *context) {
  (void)recognizer;
  (void)context;
  if (s_handlers.on_back != NULL) {
    s_handlers.on_back();
  }
}

void input_init(const RbInputHandlers *handlers) {
  s_handlers = *handlers;
}

void input_click_config_provider(void *context) {
  (void)context;

  // Raw DOWN handlers on the two lane buttons. A rhythm game has to time the
  // press, not the release: window_single_click_subscribe fires on release, so
  // however long the player held the button would be added to their timing.
  // The up_handler is NULL -- hold length carries no meaning here.
  window_raw_click_subscribe(BUTTON_ID_UP, prv_lane_down, NULL, NULL);
  window_raw_click_subscribe(BUTTON_ID_SELECT, prv_lane_down, NULL, NULL);

  // DOWN is subscribed, but it is NOT a gameplay lane -- two lanes means there
  // is no third band for it to point at, and aliasing it onto one would break
  // the "lane position matches button position" rule the whole layout rests on.
  // It reports RB_LANE_NONE, which the menu screens use to move the song
  // selection and which the playing screen ignores outright.
  window_raw_click_subscribe(BUTTON_ID_DOWN, prv_lane_down, NULL, NULL);

  // BACK must use single-click: pebble.h states the back button cannot take a
  // repeating, long or raw handler. That suits us -- BACK is never a gameplay
  // button, so release-timing is irrelevant.
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_click);
}
