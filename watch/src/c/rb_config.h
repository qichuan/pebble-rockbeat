#pragma once

// ---------------------------------------------------------------------------
// Rockbeat -- every tunable lives here. No magic numbers in the logic files.
//
// Target is emery (Pebble Time 2): 200x228, 64 colours, 128KB app RAM.
// Layout constants below are *offsets and thicknesses*; the actual rects are
// derived from layer_get_bounds() at draw time so nothing hardcodes 200x228.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Frame loop
// ---------------------------------------------------------------------------

// 33ms is ~30fps, the same figure the sibling games settled on. Note that this
// does NOT bound timing precision: press timestamps come from time_ms() inside
// the click handler, not from the frame tick, so judgment stays sub-frame
// accurate even though the picture only moves 30 times a second.
#define RB_FRAME_MS 33

// ---------------------------------------------------------------------------
// Vertical layout
//
// 228 = 24 + (60 * 3) + 24. That split is not arbitrary: it puts the middle
// lane's centre on y=114, the exact vertical centre of the screen, which is
// where the SELECT button sits. TOP and BOTTOM then land at 54 and 174 --
// symmetric about centre, lining up with UP and DOWN. Keeping the lane order
// glued to the physical button order is the whole ergonomic premise of the
// game, so these two bands are the first thing to preserve if the layout is
// ever revisited.
// ---------------------------------------------------------------------------

#define RB_HUD_TOP_H 24  // score + progress bar; fits FONT_KEY_GOTHIC_18
#define RB_HUD_BOT_H 24  // combo + last judgment

// ---------------------------------------------------------------------------
// Horizontal layout
// ---------------------------------------------------------------------------

// Hit target sits this far in from the right edge, i.e. x=168 on emery. The
// ring (r=18) then spans x 150..186, leaving 14px for the per-lane button hint
// glyph in the right margin -- "beside the buttons", which is where the eye
// already is.
#define RB_TARGET_INSET_R 32
#define RB_TARGET_R 18
#define RB_TARGET_RING_W 3

#define RB_NOTE_R_NORMAL 11
#define RB_NOTE_R_BIG 16

// Notes are culled outside this margin either side of the screen. Slightly
// larger than RB_NOTE_R_BIG so a big note slides off smoothly rather than
// popping.
#define RB_CULL_MARGIN 24

// Travel from x=-16 to the target at x=168 is 184px; at 120px/s that is a
// ~1.53s read-ahead. Slower felt sluggish and crowded the lane with notes;
// faster left no time to react to the third lane once the eye had committed.
#define RB_SCROLL_PX_PER_SEC 120

// ---------------------------------------------------------------------------
// Judgment windows (milliseconds either side of the note's hit_time_ms)
//
// At 120px/s one pixel is ~8.3ms, so these windows are visually legible too:
// Perfect is +-5px of travel, Good +-12px.
//
// Perfect is deliberately not tighter than 45ms. The press timestamp is exact,
// but app-task dispatch jitter between the physical button and the click
// handler is not measurable from inside the app, so the window has to absorb
// an unknown few milliseconds. If the slice feels late, widen this -- do not
// touch the clock, which is already correct.
// ---------------------------------------------------------------------------

#define RB_PERFECT_MS 45
#define RB_GOOD_MS 100
#define RB_MISS_MS 160  // beyond this a note auto-misses; presses become strays

// ---------------------------------------------------------------------------
// Scoring
// ---------------------------------------------------------------------------

#define RB_SCORE_PERFECT 300
#define RB_SCORE_GOOD 100
#define RB_BIG_MULTIPLIER 2

// Combo pays 2 points a step but stops compounding at 50, so a long run is
// worth chasing without letting the back half of a chart dwarf the front.
#define RB_COMBO_BONUS_PER 2
#define RB_COMBO_BONUS_CAP 50

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------

// Largest delta clock_now_ms() will accept from one call to the next. The frame
// timer polls every 33ms, so any observed gap much above that means either the
// app was suspended or the wall clock moved. Absorbing such a gap would jump
// the song; 1000 leaves generous room for a genuinely slow frame while still
// catching a real discontinuity. See the long comment in clock.c.
#define RB_CLOCK_MAX_STEP_MS 1000

// ---------------------------------------------------------------------------
// Feedback
// ---------------------------------------------------------------------------

// How long a lane's hit flash stays lit. Keyed off the game clock, not the
// frame counter, so it is deterministic.
#define RB_FLASH_MS 150

#define RB_MAX_NOTES 96

// ---------------------------------------------------------------------------
// Haptics
//
// The SDK's Vibes doc block states plainly: "if there is an ongoing vibration,
// calling any of the functions to emit (another) vibration will have no
// effect." Patterns are DROPPED, not queued, despite the name "enqueue". Every
// number below exists to make the drops predictable instead of arbitrary.
// ---------------------------------------------------------------------------

// The motor takes ~20-25ms to spin up, so a pulse much under 35ms is never felt
// at the wrist at all. 45 is the shortest that reads as a distinct tap.
#define RB_VIBE_NORMAL_MS 45

// Clearly heavier than a normal tap, still well under the 250ms gap between
// consecutive eighth notes at 120 BPM. Pebble vibration is coarse on/off with no
// amplitude control, so pulse LENGTH is the only lever for "harder".
#define RB_VIBE_BIG_MS 120

// Minimum spacing between normal pulses: pattern length plus spin-down plus
// margin. Below this the motor never fully stops and separate hits smear into
// one continuous buzz, losing the per-note feel. The demo chart's tightest
// spacing is 250ms, so every note still gets its own clean pulse.
#define RB_VIBE_MIN_GAP_MS 130

// ---------------------------------------------------------------------------
// Audio -- see audio.c for the synthesis itself
//
// Format is 16kHz 8-bit signed mono:
//   - 8-bit because the output stage is 8-bit anyway and the speaker's usable
//     dynamic range is nowhere near 8 bits; it also halves the byte rate.
//   - 16kHz rather than 8kHz because the "ka" rim click is a noise burst with
//     real energy well above 4kHz. At 8kHz that folds back as aliasing and the
//     click loses its snap. 16kHz costs 16 bytes/ms -- trivial against 128KB.
// ---------------------------------------------------------------------------

#define RB_AUDIO_RATE_HZ 16000
#define RB_AUDIO_VOLUME 70

// Samples rendered per pump, ceiling. 40ms at 16kHz; one frame is 33ms, so this
// covers a frame plus a late one without ever needing two writes.
#define RB_AUDIO_CHUNK_SAMPLES 640

// How far ahead of the clock the stream is kept filled. This IS the audio
// latency knob: whatever sits queued in the firmware buffer is delay between the
// press and the sound. The goal is a shallow-but-never-empty buffer, not a full
// one -- so this is one frame of slack, no more.
#define RB_AUDIO_LEAD_MS 40

// Silence written at stream open so the very first hit cannot underrun.
#define RB_AUDIO_PRIME_MS 40

// Three lanes at once plus one spare. Static pool; no malloc.
#define RB_AUDIO_VOICES 4

// phase_inc = freq * 2^32 / RATE. 2^32 / 16000 = 268435.456, so the integer form
// is off by 1.7e-6 -- about 0.002Hz at 1kHz, inaudible.
// Overflow check: 4000 * 268435 = 1.07e9, comfortably inside uint32.
#define RB_PHASE_INC(freq_hz) ((uint32_t)((uint32_t)(freq_hz) * 268435u))

// Voice recipes. Envelopes decay by `env -= env >> SHIFT` per sample, an
// exponential with a time constant of 2^SHIFT samples. At 16kHz, shift 8 reaches
// -60dB in ~111ms (the taiko thump) and shift 5 in ~14ms (the rim snap).
#define RB_DON_F1 180          // low taiko body
#define RB_DON_F2 270          // 1.5x -- the inharmonic partial that stops it sounding like a test tone
#define RB_DON_ENV_SHIFT 8
#define RB_DON_MS 160
#define RB_KA_F1 900           // rim tone
#define RB_KA_F2 1350
#define RB_KA_ENV_SHIFT 7
#define RB_KA_NOISE_SHIFT 5
#define RB_KA_MS 90
#define RB_BIG_DON_F1 150
#define RB_BIG_DON_F2 225
#define RB_BIG_DON_ENV_SHIFT 9
#define RB_BIG_DON_MS 300
#define RB_BIG_KA_F1 780
#define RB_BIG_KA_F2 1170
#define RB_BIG_KA_MS 160

// Out of 255. Normal leaves headroom so a big note is audibly louder rather than
// just longer.
#define RB_ENV_NORMAL 200
#define RB_ENV_BIG 255

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

// Bumped to 2 when save_load() started writing every key explicitly on first
// run. Version 1 installs trusted flag keys they had never written, and on
// emery persist_exists() reports those as present -- so a v1 store can hold a
// bogus "sound off". Re-initialising is the migration.
#define RB_SAVE_VERSION 2
#define RB_PERSIST_KEY_VERSION 1
#define RB_PERSIST_KEY_HIGH_SCORE 2
#define RB_PERSIST_KEY_BEST_COMBO 3
#define RB_PERSIST_KEY_SOUND 4
#define RB_PERSIST_KEY_HAPTICS 5

// Results ranks, in accuracy percent.
#define RB_RANK_S_PCT 95
#define RB_RANK_A_PCT 85
#define RB_RANK_B_PCT 70

// ---------------------------------------------------------------------------
// Debug harness -- all default to 0, all compiled out when off.
//
// These exist because of a documented emulator hazard: `pebble emu-button` can
// permanently wedge the emulator's screenshot service, after which every
// screenshot returns a libpebble2 TimeoutError until `pebble kill` plus a
// reinstall. Screenshots taken BEFORE any button press are reliable.
//
// So every interesting frame has to be reachable WITHOUT pressing a button.
// Autoplay drives the whole judgment path -- score, combo, flash, and later
// haptics and audio -- from a cold boot with zero input.
// ---------------------------------------------------------------------------

// Start the song immediately instead of showing the title, so the playing and
// results screens are reachable without a button press.
#define RB_DEBUG_AUTOSTART 0

#define RB_DEBUG_AUTOPLAY 0  // auto-hit every note as it reaches its hit time

// Offset applied to the synthetic press. 0 gives all Perfects; a value between
// RB_PERFECT_MS and RB_GOOD_MS forces Goods, which is how the Good path gets
// screenshotted.
#define RB_DEBUG_AUTOPLAY_OFFSET_MS 0

// Drop every Nth autoplay note so the miss path and the combo reset are
// visible. 0 disables.
#define RB_DEBUG_AUTOPLAY_MISS_EVERY 0

// Freeze the song clock once elapsed reaches this value. `pebble screenshot` is
// a ~1s round trip, so an interesting frame is otherwise long gone before the
// capture lands. 0 disables.
#define RB_DEBUG_FREEZE_AT_MS 0

// Log every judged press. This is how real button input gets verified: once
// `pebble emu-button` has been used, screenshots on that emulator may be dead,
// but `pebble logs` keeps working.
#define RB_DEBUG_LOG_JUDGMENTS 0

// Log frame pacing and the PCM stream's accepted-vs-requested byte counts. The
// firmware buffer depth is undocumented, so this is the only way to find where
// speaker_stream_write() starts pushing back.
#define RB_DEBUG_LOG_AUDIO 0
