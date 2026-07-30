#pragma once

// ---------------------------------------------------------------------------
// Buttons -> timestamped hit events.
// ---------------------------------------------------------------------------

#include <pebble.h>

typedef struct {
  // press_now_ms is a clock_now_ms() reading taken the instant the button went
  // down, before any other work in the handler.
  void (*on_lane_hit)(uint8_t lane, uint32_t press_now_ms);
  void (*on_back)(void);
} RbInputHandlers;

void input_init(const RbInputHandlers *handlers);

// Pass to window_set_click_config_provider().
void input_click_config_provider(void *context);
