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

// Travel from x=-16 to the target at x=168 is 184px; at 95px/s that is a ~1.94s
// read-ahead. Cut from 120px/s (1.53s) after play-testing: at 90 BPM a beat is
// 667ms, so the slower speed puts nearly three beats of runway on screen, which
// is what makes an approaching note readable rather than a surprise. The chart
// is sparse enough now that the extra dwell does not crowd the lanes.
#define RB_SCROLL_PX_PER_SEC 95

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

// Widened from 45/100 after play-testing. Part of why hitting anything felt
// impossible was the clock lurching (fixed in clock.c), but the windows were
// also tight for a wrist-mounted button with mechanical travel, and the clock
// now quantises to RB_CLOCK_TICK_MS which spends some of the budget.
//
// RB_MISS_MS stays at 160 deliberately: the chart generator guarantees 333ms
// between notes in a lane, and 2*160 = 320 is what keeps their windows from
// overlapping. Raising it past 166 would break that and must be done together
// with SAME_LANE_MIN_MS in tools/make_chart.py.
#define RB_PERFECT_MS 60
#define RB_GOOD_MS 125
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

// The clock's tick interval, and therefore its resolution. 10ms is well inside
// the 45ms Perfect window while keeping the wakeup rate sane. Timestamps
// quantise to this -- a bounded, predictable error, unlike the once-a-second
// 1000ms lurch the previous time_ms()-derived clock produced. See clock.c.
#define RB_CLOCK_TICK_MS 10

// Sanity bounds on the measured ticks-per-second. A 10ms tick should give ~100;
// these bounds reject a nonsense reading (a stalled or storming timer) without
// rejecting the ~70/s the emulator actually delivers.
#define RB_CLOCK_MIN_TICKS_PER_SEC 12
#define RB_CLOCK_MAX_TICKS_PER_SEC 400

// Smoothing factor for the measured rate: new = (old*(N-1) + measured)/N.
// 8 settles within a few seconds while stopping one jittery second from audibly
// swinging the tempo.
#define RB_CLOCK_SMOOTH 8

// ---------------------------------------------------------------------------
// Feedback
// ---------------------------------------------------------------------------

// How long a lane's hit flash stays lit. Keyed off the game clock, not the
// frame counter, so it is deterministic.
#define RB_FLASH_MS 150

// The charted excerpt has 288 notes; the pool leaves room to re-generate a
// denser one without touching code. Costs one byte of RAM per note.
#define RB_MAX_NOTES 320

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

// Clearly heavier than a normal tap, still under the 333ms gap between
// consecutive eighth notes at 90 BPM. Pebble vibration is coarse on/off with no
// amplitude control, so pulse LENGTH is the only lever for "harder".
#define RB_VIBE_BIG_MS 120

// Minimum spacing between normal pulses: pattern length plus spin-down plus
// margin. Below this the motor never fully stops and separate hits smear into
// one continuous buzz, losing the per-note feel. The charted excerpt places
// notes as little as 167ms apart across lanes, so on the densest runs some
// pulses are deliberately skipped -- that is the drop being made predictable
// rather than left to the SDK.
#define RB_VIBE_MIN_GAP_MS 130

// ---------------------------------------------------------------------------
// Audio -- the note sequencer. See music.h for everything that was measured
// about speaker_play_tracks(), and audio.c for the chunk chaining.
//
// There is no PCM synthesis here any more. The music is MIDI played by the
// watch's own sequencer, and a PCM stream cannot coexist with it -- measured:
// speaker_stream_open() returns false while tracks are playing. That removed
// the whole voice pool, the mixer, the pump timer and their constants along
// with the 911KB PCM resource they existed to mix.
// ---------------------------------------------------------------------------

#define RB_AUDIO_VOLUME 70

// Music starts at the same offset as the first note, so the audio and the chart
// share one origin. Also gives the player a beat of runway before note one.
#define RB_MUSIC_START_MS 2000

// MEASURED: the FIRST speaker_play_tracks() call costs ~200ms before sound
// appears; chained calls cost nothing (drift over five later chunks was
// +17/-5/-12/+41/-30ms, i.e. jitter). So the first call is issued this much
// early. Without it the entire track sits ~200ms -- a fifth of a beat at
// 118 BPM -- behind the notes, which is inside the Good window but audibly late.
#define RB_MUSIC_LATENCY_MS 200

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
