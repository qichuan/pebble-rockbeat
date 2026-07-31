#pragma once

// ---------------------------------------------------------------------------
// Buttons -> timestamped hit events.
// ---------------------------------------------------------------------------

#include <pebble.h>

typedef struct {
  // press_now_ms is a clock_now_ms() reading taken the instant the button went
  // down, before any other work in the handler.
  //
  // `lane` is what the playfield wants: the band this press strikes, or
  // RB_LANE_NONE for a button that strikes none. `button` is the physical
  // button, and the menus branch on THAT. Both are passed because the mapping
  // is many-to-one -- SELECT and DOWN both play the bottom lane -- so a lane
  // value can no longer identify the button that produced it.
  void (*on_lane_hit)(uint8_t lane, ButtonId button, uint32_t press_now_ms);
  void (*on_back)(void);
} RbInputHandlers;

void input_init(const RbInputHandlers *handlers);

// Pass to window_set_click_config_provider().
void input_click_config_provider(void *context);
