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
// Vertical layout -- taken directly from the design, composed at 200x228.
//
//    0..56    HUD          score left, combo right
//   56..106   lane TOP     UP button
//  106..112   gutter
//  112..162   lane BOT     SELECT button
//  162..168   gutter
//  168..220   song band    art tile + title/artist
//  220..228   progress
//
// These are absolute because the design is: it was drawn at this exact size for
// this exact screen, and deriving them from bounds would only invent a layout
// nobody drew. The one thing still read from bounds at draw time is the WIDTH,
// so the right-hand furniture stays anchored if the canvas ever differs.
// ---------------------------------------------------------------------------

#define RB_HUD_H 56          // score + combo band across the top
#define RB_LANE_TOP_Y 56
#define RB_LANE_BOT_Y 112
#define RB_LANE_H 50
#define RB_LANE_RAIL_DY 24   // the rail sits this far down inside a lane
#define RB_LANE_RAIL_H 2

// The lane bed stops here and the target zone behind it is plain black, so a
// note crossing the target is never read against a coloured bed.
#define RB_TARGET_ZONE_X 148
#define RB_TARGET_DIVIDER_X 146
#define RB_TARGET_DIVIDER_W 2

#define RB_BAND_Y 168        // song band: art tile + title/artist plate
#define RB_BAND_H 52
#define RB_BAND_ART_W 74
#define RB_BAND_RULE_W 2     // yellow rule separating art from text
#define RB_BAND_DISC_R 15    // the record on the art tile
#define RB_BAND_HOLE_R 4

#define RB_PROGRESS_Y 220
#define RB_PROGRESS_H 8

// ---------------------------------------------------------------------------
// Horizontal layout
// ---------------------------------------------------------------------------

// Target centre. The design puts a 40x40 ring at x=156, so its centre is 176.
#define RB_TARGET_CX 176
#define RB_TARGET_R 20
#define RB_TARGET_RING_W 5

// The arrow badge inside a resting target ring: 12px along its axis, 8px either
// side of it. Sized to sit inside RB_TARGET_R - RB_TARGET_RING_W (15px) with a
// little air, and the same both ways round so the UP and RIGHT badges read as
// the same mark rotated rather than as two different shapes.
#define RB_ARROW_LEN 12
#define RB_ARROW_HALF 8

// Notes are 28px across in the design, so r=14, with a 3px white ring and an
// inner dot in the lane's own bed colour.
#define RB_NOTE_R_NORMAL 14
#define RB_NOTE_R_BIG 18
#define RB_NOTE_RING_W 3
#define RB_NOTE_DOT_R 4

// Notes are culled outside this margin either side of the screen. Slightly
// larger than RB_NOTE_R_BIG so a big note slides off smoothly rather than
// popping.
#define RB_CULL_MARGIN 24

// Travel from x=-18 to the target at x=176 is 194px; at 120px/s that is a ~1.6s
// read-ahead.
//
// Speed is a LEGIBILITY constraint, not a taste one. The chart carries every
// melody note, so notes come as close as 127ms apart, and two notes 127ms apart
// at 95px/s were 12px apart -- closer than one note's diameter, so a run of
// sixteenths merged into a smear. At 120px/s the same pair is 15px apart and the
// tightest SAME-lane pair (an eighth, 254ms) is 30px, which clears a normal
// note's 28px diameter.
#define RB_SCROLL_PX_PER_SEC 120

// ---------------------------------------------------------------------------
// Judgment popup -- the yellow plate the design puts inside the hit lane.
// ---------------------------------------------------------------------------

#define RB_POPUP_W 68
#define RB_POPUP_H 30
#define RB_POPUP_INSET_X 6   // from the lane's left edge

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

// How long a target acknowledges a BUTTON PRESS, whether or not it hit anything.
// A press that judges lights the hit flash instead, which outlasts this and is
// checked first, so the two never fight.
//
// Deliberately shorter than RB_FLASH_MS: this is "the pad went down", not "you
// scored", and it must never be mistaken for the latter. But it cannot go below
// two frames (2 * RB_FRAME_MS = 80ms) or a press can land and expire between
// redraws and never be seen at all -- nothing forces a repaint for it, by
// design; see prv_lane_hit() in main.c.
#define RB_PRESS_FLASH_MS 100

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

// How long after speaker_play_tracks() sound actually appears, plus the frame of
// release granularity that audio_tick() adds. Each chunk is handed over this much
// BEFORE the song time its first note is charted at, so the tone lands on the
// note instead of behind it.
//
// MEASURED on a real Pebble Time 2, with RB_MUSIC_OFFSET_MS at 0 and the old
// chain-on-finish scheduling still in place. Each chunk's wall duration against
// its nominal content length:
//
//                         chunk 0   chunk 1   chunk 2
//     Golden               +176      +158      +172
//     You Are Not Alone    +167      +163      +179
//     Never Gonna...       +507      +230      +131
//
// It is a FIXED PER-CALL COST, not a playback rate and not a cold-start ramp.
// Both alternatives were considered and the data rules them out:
//   - Not a rate: YANA's chunks are 16833ms against Golden's 15738ms, 7% longer,
//     yet YANA overruns by LESS (167 vs 176). A rate error scales with duration
//     and would go the other way. It is also independent of note count -- 59
//     notes and 106 notes cost the same.
//   - Not a cold start: Golden's chunk 0 was the first speaker call in a freshly
//     launched app (8.7s of title screen, nothing sounded before it) and cost
//     176ms, the same as every other call.
//
// So ONE constant, not one for the first call and one for the rest, and it is
// the measured mean DIRECTLY -- 169 over the six clean samples, rounded to 170.
//
// That the mean transfers with no correction is worth showing, because it looks
// like it should need one. Each measured overrun is (call latency + one frame of
// quantisation): the old design noticed the finish on the next frame after the
// callback. Each release now costs (call latency + one frame of quantisation)
// too: audio_tick() fires at the first frame at or after the release time. Same
// 40ms loop, same distribution, opposite sides of the subtraction -- so they
// cancel and CALL_MS is the raw mean rather than the latency alone.
//
// Residual is then about +-30ms about the note, one frame of jitter either way,
// and it does NOT accumulate across the song.
//
// THE 507 IS AN OUTLIER, and the only unexplained number here -- that run was
// the first launch immediately after installing. If the opening of a song ever
// sounds much later than the rest, it is the thing to re-measure; do not grow
// this constant to cover it, because that would put every other chunk early.
//
// The emulator does the MIRROR IMAGE of this and that is why it went unnoticed
// for so long: there the first call SWALLOWS ~200ms of the chunk it is given and
// chaining is free, so the music ran early and the correction had to push it
// later. On hardware every call costs time up front and the correction has to
// pull it earlier. One emery build serves both -- there is no compile-time way
// to tell an emulator from a watch -- so THE WATCH WINS AND THE EMULATOR IS NOW
// KNOWN-WRONG BY ROUGHLY THIS AMOUNT. Do not "fix" audio sync from an emulator
// session; it can no longer answer that question.
#define RB_MUSIC_CALL_MS 170

// Residual trim on the whole music timeline. Positive starts the music LATER.
//
// This used to carry the platform's startup behaviour and was the only knob;
// that job now belongs to RB_MUSIC_CALL_MS above, which is why this is 0. What
// is left is a place to take out a flat error that survives it.
//
// Method, so it can be repeated: enable RB_DEBUG_LOG_AUDIO and read the
// "resid=" figure logged when a chunk reaches its natural end. It is exactly
// (actual call latency - RB_MUSIC_CALL_MS), so positive means CALL_MS is too
// small. Only the last chunk of a song reports it -- every other chunk is
// deliberately stopped early -- so it is one sample per playthrough and worth
// collecting from all three songs.
//
// Which constant to move:
//   - every chunk is out by the same amount -> RB_MUSIC_CALL_MS
//   - a flat error survives fixing that     -> here
//   - only the OPENING of a song is out     -> neither; see the 507ms outlier
//     noted above, which is the only evidence a first call differs at all
//   - the error GROWS across the song       -> neither; that is a rate problem
//     and belongs in clock.c. Never paper over it with this.
//
// That last case is not hypothetical: it is what the chained design produced
// (522/752/883ms and climbing), and it is what releasing on the song clock
// removes. Each chunk is now anchored independently, so a growing error would
// mean something genuinely new.
#define RB_MUSIC_OFFSET_MS 0

// ---------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------

// Bumped to 2 when save_load() started writing every key explicitly on first
// run. Version 1 installs trusted flag keys they had never written, and on
// emery persist_exists() reports those as present -- so a v1 store can hold a
// bogus "sound off". Re-initialising is the migration.
// Bumped to 3 when bests became per song and the sound/haptics toggles were
// removed. A v2 store holds a single shared score under a key that is now song
// 0's, which would silently become song 0's best; re-initialising is the
// migration, and it costs one high score once.
#define RB_SAVE_VERSION 3
#define RB_PERSIST_KEY_VERSION 1

// Per-song keys are allocated from these bases, so adding a song needs no new
// constant. The two ranges must not overlap: keep them RB_MAX_SONGS apart.
#define RB_PERSIST_KEY_HIGH_SCORE_BASE 16
#define RB_PERSIST_KEY_BEST_COMBO_BASE 32

// Upper bound on compiled-in songs, and the size of the saved-bests arrays.
// chart_count() is the real number; this only has to be >= it, and it is
// asserted against chart_count() at startup.
#define RB_MAX_SONGS 8

// Title screen song list.
//
// Sized so all four songs are visible at once: scrolling a list this short is
// worse than a slightly tighter row, because a song you cannot see is a song you
// do not know is there. Beyond RB_TITLE_ROWS the list scrolls around the
// selection rather than running off the bottom, so a fifth song still works --
// it just stops being visible all at once.
//
// The vertical budget is exact. Panel 26..204 is 178px: header 32, four 21px
// rows, then the best line, the action hint and the control hint. Changing any
// of these means re-checking that the last line still lands above 204.
#define RB_TITLE_PANEL_Y 26
#define RB_TITLE_PANEL_H 178
#define RB_TITLE_LIST_Y 62
#define RB_TITLE_ROW_H 21
#define RB_TITLE_ROWS 4

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

// Which song autostart plays, so a second song's chart can be captured too.
#define RB_DEBUG_AUTOSTART_SONG 0

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

// Run the song this many times slower than real time. 1 disables (and compiles
// out entirely).
//
// This exists to capture ANIMATION. FREEZE_AT_MS holds one moment, and it is a
// compile-time constant, so a smooth sequence of frames would cost one
// rebuild-install-wait cycle per frame -- minutes for a couple of seconds of
// motion. Slowing the song instead means ONE install, after which a plain loop
// of `pebble screenshot` samples it at even intervals: at scale S, a ~1s
// round trip advances the song ~1000/S ms, so S=20 gives ~50ms steps, which at
// RB_SCROLL_PX_PER_SEC is about 6px of note travel per frame.
//
// It scales the clock READING inside the frame handler only. Two consequences,
// both fine for capture and both wrong for play:
//   - press timestamps are sampled in the click handler and are NOT scaled, so
//     manual input judges against a timeline it does not share. Use AUTOPLAY.
//   - the speaker plays in real time and cannot be slowed, so the music runs
//     away from the notes and the re-sync guard fights it the whole way.
// Capture with the sound off, and never leave this on.
#define RB_DEBUG_TIME_SCALE 1

// Log every judged press. This is how real button input gets verified: once
// `pebble emu-button` has been used, screenshots on that emulator may be dead,
// but `pebble logs` keeps working.
#define RB_DEBUG_LOG_JUDGMENTS 0

// Log frame pacing and the PCM stream's accepted-vs-requested byte counts. The
// firmware buffer depth is undocumented, so this is the only way to find where
// speaker_stream_write() starts pushing back.
#define RB_DEBUG_LOG_AUDIO 0
