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

// Bests are per song -- `song` indexes the same table as chart_get(). A shared
// best would be meaningless across songs of different length and density.
uint32_t save_high_score(uint8_t song);
uint16_t save_best_combo(uint8_t song);

// Stores score/combo if either beats that song's stored best. Returns true if
// the score itself was a new record, so the results screen can say so.
bool save_record(uint8_t song, uint32_t score, uint16_t max_combo);
