#pragma once

// ---------------------------------------------------------------------------
// The authoritative game clock -- and the only caller of time_ms() in the app.
//
// Isolating it here is what keeps game.c free of every Pebble API, which is
// what lets tools/run_tests.sh compile the judgment logic with the host
// compiler.
// ---------------------------------------------------------------------------

#include <pebble.h>

// Starts the tick that drives the clock.
void clock_init(void);

// Stops the tick. Call when the app leaves the foreground.
void clock_stop(void);

// Monotonic milliseconds since clock_init(). Never decreases, and advances in
// even steps -- see the long comment in clock.c for why that evenness is the
// whole point.
uint32_t clock_now_ms(void);
