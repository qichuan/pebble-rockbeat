#pragma once

// ---------------------------------------------------------------------------
// All drawing. This is the only file in the project containing graphics_*
// calls -- the logic modules stay pixel-free, and the frame timer only mutates
// state and marks the layer dirty.
// ---------------------------------------------------------------------------

#include <pebble.h>

void render_update_proc(Layer *layer, GContext *ctx);
