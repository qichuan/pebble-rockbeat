#pragma once

// ---------------------------------------------------------------------------
// The only caller of persist_* in the app.
//
// Isolating it here keeps game.c free of Pebble APIs (so it stays host-testable)
// and keeps flash writes off the frame loop -- save_record() is called exactly
// once, on the transition into the results screen.
// ---------------------------------------------------------------------------

#include <pebble.h>
#include <stdbool.h>
#include <stdint.h>

void save_load(void);

uint32_t save_high_score(void);
uint16_t save_best_combo(void);

// Stores score/combo if either beats the stored best. Returns true if the score
// itself was a new record, so the results screen can say so.
bool save_record(uint32_t score, uint16_t max_combo);

bool save_sound_enabled(void);
bool save_haptics_enabled(void);
void save_set_sound(bool enabled);
void save_set_haptics(bool enabled);
