#pragma once

// ---------------------------------------------------------------------------
// All drawing. This is the only file in the project containing graphics_*
// calls -- the logic modules stay pixel-free, and the frame timer only mutates
// state and marks the layer dirty.
//
// ONE layer, deliberately. Splitting the screen into separate HUD / playfield /
// song-band layers and marking only the moving one dirty is the obvious way to
// avoid re-running the static text layouts 25 times a second, and it does not
// work on this platform: layer_mark_dirty() schedules a render of the whole
// WINDOW, not of one layer's rect. Measured with a per-proc counter, the three
// layers repainted 927/927/927 times over the same run -- the band, which
// should have repainted about 130 times, tracked the playfield exactly.
//
// So the split bought nothing and cost three extra full-rect background fills
// per frame. Do not reintroduce it without first proving partial redraw exists.
// ---------------------------------------------------------------------------

#include <pebble.h>

void render_update_proc(Layer *layer, GContext *ctx);
