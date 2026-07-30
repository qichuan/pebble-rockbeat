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

// 40ms is 25fps. This does NOT bound timing precision: press timestamps are
// sampled in the click handler, not on the frame tick, so judgment is unaffected
// by the redraw rate -- a late or dropped frame changes what is seen, never what
// is scored.
//
// Raised from 33ms (30fps) because the game was laggy on real hardware, where a
// full-screen redraw of a 200x228 colour framebuffer costs far more than it does
// on the emulator. At 95px/s a note moves 3.8px per frame, which is still well
// inside "smooth"; the app task is the scarce resource here, not the eye.
#define RB_FRAME_MS 40

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

// Travel from x=-16 to the target at x=168 is 184px; at 120px/s that is a ~1.53s
// read-ahead.
//
// Speed is now a LEGIBILITY constraint, not a taste one. The chart carries every
// melody note, so notes come as close as 127ms apart, and two notes 127ms apart
// at 95px/s were 12px apart -- closer than one note's diameter, so a run of
// sixteenths merged into a smear. At 120px/s the same pair is 15px apart and the
// tightest SAME-lane pair (an eighth, 254ms) is 30px, which clears a normal
// note's 22px diameter.
//
// Raising it further would fix nothing and cost read-ahead, which at this
// density is the scarcer resource: 1.53s is about four notes of runway.
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

// RB_MISS_MS is not a free choice -- it is set by the chart, and the chart is now
// every melody note. The tightest interval between two notes in one lane is an
// eighth, 254ms at 118 BPM, and two judgment windows must fit inside that or a
// single press sits in both and the notes stop being individually hittable. So
// 2 * RB_MISS_MS <= 254, and 125 takes it with 4ms to spare.
//
// It came down from 160, which was possible only while the chart was a selected
// subset with 333ms of clearance. Charting every note bought exactness at the
// cost of some of the timing budget; that is the trade, and it is the right way
// round for a rhythm game. Anything here must move together with
// SAME_LANE_MIN_MS in tools/make_chart.py, which asserts the gap it produces.
//
// Perfect stays generous. The press timestamp is exact and the clock no longer
// quantises in direct mode, but app-task dispatch latency between the physical
// button and the click handler is still not measurable from inside the app, so
// the window has to absorb an unknown few milliseconds.
#define RB_PERFECT_MS 55
#define RB_GOOD_MS 100
#define RB_MISS_MS 125  // beyond this a note auto-misses; presses become strays

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

// The clock's tick interval, and therefore its resolution. Timestamps quantise
// to this -- a bounded, predictable error, unlike the once-a-second 1000ms lurch
// the previous time_ms()-derived clock produced. See clock.c.
//
// Raised from 10ms, which asked the app task for 100 wakeups a second on top of
// the frame timer. That is affordable on the emulator and expensive on a watch,
// and it showed up as lag. 20ms halves the wakeup rate while staying at a third
// of the 60ms Perfect window, so the quantisation is still not what limits
// accuracy -- button travel and dispatch latency are.
//
// This is only the INTERPOLATION resolution. It does not affect the clock's rate
// or its long-run accuracy, both of which come from real second boundaries.
#define RB_CLOCK_TICK_MS 20

// Sanity bounds on the measured ticks-per-second. A 20ms tick should give ~50;
// these bounds reject a nonsense reading (a stalled or storming timer) without
// rejecting the slower rate the emulator actually delivers under load.
#define RB_CLOCK_MIN_TICKS_PER_SEC 12
#define RB_CLOCK_MAX_TICKS_PER_SEC 400

// Smoothing factor for the measured tick period: new = (old*(N-1) + measured)/N.
// This shapes the interpolation WITHIN a second only -- real second boundaries
// carry the time, so this no longer has to converge fast enough to stop an error
// accumulating, because an error can no longer accumulate. 8 keeps one jittery
// second from visibly swinging the scroll.
#define RB_CLOCK_SMOOTH 8

// The tick interpolates between real one-second boundaries and may never reach
// past the one it is filling towards. 1000 is not a tuning choice: it is what
// makes the clock monotonic by construction, since the next second's base is
// exactly 1000 higher. Raising it would let the clock overshoot a boundary and
// then step backwards across it.
#define RB_CLOCK_MAX_INTERP_MS 1000

// Deciding whether time_ms()'s millisecond field can be trusted, which decides
// whether the clock needs a tick timer at all. See clock.c.
//
// The test is how far the field ADVANCES across one real second, summing forward
// deltas and counting a wrap as +1000. A field that counts milliseconds advances
// ~1000; the emulator's advances ~190. The window is generous either side
// because the sum is built from tick samples and a tick can straddle a boundary,
// but it is nowhere near wide enough to admit 190. An upper bound as well as a
// lower one, so a field that races is rejected too rather than trusted blindly.
//
// Do NOT replace this with "how high does ms get within a second". That was
// tried and it is phase-dependent: a field advancing 190/sec still spans a
// different 190-wide band each second, so about one second in five it peaks near
// 999 and passes. Measured on the emulator, which promptly switched to a clock
// it cannot support.
#define RB_CLOCK_DIRECT_MIN_ADVANCE_MS 850
#define RB_CLOCK_DIRECT_MAX_ADVANCE_MS 1150
// ...and the field must also be FINE-GRAINED, not merely correct on average. A
// field that updates in coarse steps sums to the right total per second while
// standing still in between, which on a scrolling playfield is exactly the
// stutter direct mode exists to remove. Counted as ticks across which the field
// did not move at all; a fine-grained field moves on every one. Measured on the
// emulator, whose field advances ~1000/sec and still stalls -- so it correctly
// keeps the interpolated clock, which is smoother there.
#define RB_CLOCK_DIRECT_MAX_STALLS 1

#define RB_CLOCK_DIRECT_GOOD_SECONDS 2

// Largest gap in time() that is still treated as elapsed time rather than as the
// wall clock being stepped. A few seconds means the app was descheduled and the
// song really has moved on; more than that is an NTP or timezone correction, and
// following it would teleport every note mid-song.
#define RB_CLOCK_MAX_GAP_S 5

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

// Clearly heavier than a normal tap, and still inside the 254ms gap between
// consecutive eighth notes at 118 BPM. Pebble vibration is coarse on/off with no
// amplitude control, so pulse LENGTH is the only lever for "harder".
#define RB_VIBE_BIG_MS 120

// Minimum spacing between normal pulses: pattern length plus spin-down plus
// margin. Below this the motor never fully stops and separate hits smear into
// one continuous buzz, losing the per-note feel.
//
// The chart now carries every melody note, so a sixteenth run puts hits 127ms
// apart -- under this figure, and deliberately so. Those runs get a pulse on
// roughly every other note rather than a continuous blur. That is the drop being
// made predictable instead of left to the SDK, which would drop them anyway and
// arbitrarily.
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

// Shifts the whole music timeline against the song clock. Positive starts the
// music LATER; this is the one knob for "the tune does not land on the notes".
//
// MEASURED, with the clock in its current form: chained chunks track perfectly.
// Consecutive boundaries came 8073/8186/8103/8193/8083ms apart against an 8135ms
// nominal -- +-60ms of jitter and no rate error whatsoever. But the FIRST chunk
// completes ~200ms sooner than its content should allow, so the sequencer eats
// about that much while starting up, and because chaining is seamless every
// later chunk inherits the head start for the rest of the song.
//
// The sign is worth being careful about: the music runs EARLY, so the correction
// starts it LATER. That is the opposite of a latency compensation, and earlier
// builds got it backwards -- there was a positive 200ms "latency" constant here,
// later re-measured as 590ms. Both were measuring the song clock losing time to
// its own calibration rather than anything about the speaker. With the clock
// fixed, what is left is the speaker's genuine, and negative, startup cost.
//
// Method, so it can be repeated: set this to 0, enable RB_DEBUG_LOG_AUDIO, and
// read the "late=" figures, which compare when each chunk starts sounding
// against the song time its notes are charted at. Take the mean. A flat error is
// a constant offset and belongs here; a GROWING one is a rate problem and
// belongs in clock.c -- do not paper over the second with this.
//
// At 200 the mean lateness over two runs was -21ms and +25ms, with a single
// reproducible +151ms outlier at one boundary. Chasing below ~50ms would be
// fitting to emulator noise: per-chunk jitter alone is +-60ms.
//
// This is an emulator figure. If the music sits consistently ahead of or behind
// the notes on real hardware, this is the number to re-measure, and the only one.
#define RB_MUSIC_OFFSET_MS 200

// How far the music may run AHEAD of the song clock before a chunk boundary is
// used to pull it back. Below this the next chunk is chained immediately, which
// is gapless; above it the chunk is held until the song clock reaches its start.
//
// The music cannot correct itself WITHIN a chunk -- the sequencer plays a note
// list and reports no position -- so chunk boundaries are the only re-sync
// points there are, roughly one every 8 seconds.
//
// This is deliberately well above the jitter it is meant to ignore, because
// correcting is NOT free. MEASURED: a chunk chained immediately consumes its
// nominal duration to within +-60ms, but a chunk started cold, after the
// sequencer has been left idle for a couple of hundred ms, comes back ~200ms
// short -- the same startup loss RB_MUSIC_OFFSET_MS exists to cancel.
//
// A threshold tight enough to fire on ordinary jitter is therefore self-
// defeating: it fires at every boundary, each correction causes the cold start
// that causes the next one, and the result is a stable limit cycle that injects
// ~170ms of silence every 8 seconds. 40ms did exactly that here.
//
// 250ms sits above jitter and below anything a player would hear as out of time
// (a beat is 508ms at 118 BPM). Chaining stays the normal path; this is a guard
// rail for a real runaway, not the mechanism that keeps the music in time.
#define RB_MUSIC_RESYNC_MS 250

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
