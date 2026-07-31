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
  } else if (button == BUTTON_ID_SELECT || button == BUTTON_ID_DOWN) {
    // TWO buttons play the bottom lane. See the config provider for why.
    lane = RB_LANE_BOT;
  }

  if (s_handlers.on_lane_hit != NULL) {
    s_handlers.on_lane_hit(lane, button, press_now_ms);
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

  // Raw press handlers on all three. A rhythm game has to time the press, not
  // the release: window_single_click_subscribe fires on release, so however long
  // the player held the button would be added to their timing. The up_handler is
  // NULL -- hold length carries no meaning here.
  //
  // There are two LANES and three lane buttons. UP plays the top lane; SELECT
  // and DOWN BOTH play the bottom one, so the player can alternate with
  // whichever pair sits best under the thumb -- UP/SELECT are adjacent, which is
  // what makes sixteenth-note alternation possible, while UP/DOWN are the ends
  // of the stack and easier to find without looking.
  //
  // The badge on the lower lane stays a RIGHT arrow. It used to be right because
  // a down arrow would have named the one button that did nothing during play,
  // which is no longer true -- but it is still right, because the lane now has
  // TWO buttons and an arrow can only name one of them. Right names none of
  // them: it is the direction the notes travel. See chart.h for why the
  // geometric "lane sits at its button" rule does not survive here.
  window_raw_click_subscribe(BUTTON_ID_UP, prv_lane_down, NULL, NULL);
  window_raw_click_subscribe(BUTTON_ID_SELECT, prv_lane_down, NULL, NULL);
  window_raw_click_subscribe(BUTTON_ID_DOWN, prv_lane_down, NULL, NULL);

  // BACK must use single-click: pebble.h states the back button cannot take a
  // repeating, long or raw handler. That suits us -- BACK is never a gameplay
  // button, so release-timing is irrelevant.
  window_single_click_subscribe(BUTTON_ID_BACK, prv_back_click);
}
