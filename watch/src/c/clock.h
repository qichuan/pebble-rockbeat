#pragma once

// ---------------------------------------------------------------------------
// The authoritative game clock -- and the only caller of time_ms() in the app.
//
// Isolating it here is what keeps game.c free of every Pebble API, which is
// what lets tools/run_tests.sh compile the judgment logic with the host
// compiler.
// ---------------------------------------------------------------------------

#include <pebble.h>

void clock_init(void);

// Monotonic milliseconds since clock_init(). Never decreases.
uint32_t clock_now_ms(void);
