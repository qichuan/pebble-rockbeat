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

// How far the clock may disagree with the whole-second field before its rate is
// trimmed. Must exceed 1000: the seconds field is quantised to whole seconds, so
// the clock legitimately sits up to a second either side of it. 1200 leaves
// ~200ms of genuine error tolerance.
#define RB_CLOCK_SYNC_SLACK_MS 1200

// Wait this long before trusting the measured tick rate. `coarse` is quantised
// to whole seconds, so a shorter window would calibrate against up to 1s of
// quantisation error.
#define RB_CLOCK_CAL_MIN_MS 3000

// Sanity bounds on the measured tick period. A 10ms request fires at ~14ms on
// the emulator; these bounds allow for far worse without letting a bad reading
// run the clock away.
#define RB_CLOCK_TICK_MIN_MS 4
#define RB_CLOCK_TICK_MAX_MS 40

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

// MEASURED, not documented: speaker_stream_write() accepts at most 512 samples
// in a single call. Asking for 640 returned exactly 512 every time, on every
// pump. The SDK says only "may be less if the buffer is full" and gives no size.
#define RB_AUDIO_WRITE_MAX 512

// Render buffer, one slice at a time.
#define RB_AUDIO_CHUNK_SAMPLES RB_AUDIO_WRITE_MAX

// Bounded slice count per pump, for when a pump runs late. Not a retry loop: it
// never spins waiting for space, and anything still unwritten is dropped.
#define RB_AUDIO_SLICES_PER_PUMP 2

// The pump runs on its OWN timer, not on the render frame.
//
// This is not a preference, it is arithmetic. A write accepts at most 512
// samples (32ms of audio), while a render frame is 33ms and in practice ~37ms.
// Pumping once per frame therefore delivers at most 512 samples per 37ms =
// ~13.8k/s against the 16k/s the speaker consumes, so the stream starves no
// matter how many slices are attempted -- measured as a steady ~10% of audio
// dropped. At 20ms a pump needs only 320 samples, comfortably under the cap.
//
// Decoupling it from the frame rate is also the more honest structure: audio is
// an independent output, and now its cadence no longer depends on how fast the
// game happens to be drawing.
#define RB_AUDIO_PUMP_MS 20

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
// just longer. Both were cut from 200/255 once the backing track arrived: the
// drums now sit ON TOP of music rather than in silence, and at the old levels a
// don over a loud bar clipped the sum. At 150/200 a single don peaks around 55
// and a big note around 74, which leaves room beside the ~50 the music occupies.
#define RB_ENV_NORMAL 150
#define RB_ENV_BIG 200

// ---------------------------------------------------------------------------
// Backing music
//
// The stage audio is a raw signed-8-bit 8kHz mono PCM resource -- 88s of the
// source track, 688KB, against emery's 1024KB resource budget. It cannot be an
// mp3: PebbleOS exposes no decoder, and the Speaker API takes raw PCM only.
//
// 8kHz halves the byte rate against the 16kHz mixer and is what makes 88s fit
// at all; the cost is a 4kHz ceiling, so the track sounds like AM radio. The
// mixer upsamples 2x with linear interpolation into its 16kHz stream.
//
// Music position is derived from the STREAM position, not from a counter of its
// own -- see audio.c. That is what keeps it locked to the notes, and it means
// audio still cannot influence the game clock.
// ---------------------------------------------------------------------------

#define RB_MUSIC_RATE_HZ 16000

// Music starts at the same offset as the first note, so the audio and the chart
// share one origin. Also gives the player a beat of runway before note one.
#define RB_MUSIC_START_MS 2000

// Music is halved so the drums cut through it. >>1 rather than a multiply
// because this runs per sample.
#define RB_MUSIC_GAIN_SHIFT 1

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
#define RB_DEBUG_AUTOSTART 1

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
