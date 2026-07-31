# Rockbeat — project guide

Taiko-style rhythm game for Pebble Time 2 (emery), C on Pebble SDK 4.17.

## What this is

| Path | Language | Role |
|---|---|---|
| `watch/` | C | the whole app. All `pebble` commands run from here. |
| `tools/` | C + sh | host-side unit tests, no emulator needed |

## Commands

All from `watch/`; the pebble tool fails elsewhere.

```bash
pebble build
pebble clean                              # needed after editing package.json
pebble install --emulator emery
pebble screenshot --emulator emery --no-open shot.png
pebble emu-button --emulator emery click select
pebble logs --emulator emery
pebble kill
```

From the repo root: `./tools/run_tests.sh` (expect `OK: 3484 checks passed`).

This tool version does **not** accept `--scale`. `--vnc` disables emulator audio.

## Architecture

| File | Owns |
|---|---|
| `rb_config.h` | every tunable constant, with the reasoning for each |
| `main.c` | window lifecycle, frame timer, the hit path, debug harness |
| `clock.{c,h}` | real seconds, interpolated by a tick; see the gotcha below |
| `chart.{c,h}` | chart format, the generated song, forward-compatible loader |
| `game.{c,h}` | song timeline, judgment, score, combo. **No Pebble APIs.** |
| `render.{c,h}` | all drawing. **The only file with `graphics_*` calls.** |
| `input.{c,h}` | raw-click handlers producing timestamped hit events |
| `feedback.{c,h}` | hit-flash state and the haptics drop policy |
| `audio.{c,h}` | drives the note sequencer; chains music chunks |
| `music.{c,h}` | GENERATED `SpeakerNote` tables + the measured sequencer limits |
| `save.{c,h}` | high score and toggles; the only `persist_*` caller |

## Key constraints & gotchas

- **The song clock is authoritative.** Note x comes from
  `(hit_time_ms - elapsed_ms)`; scoring compares the press timestamp to
  `hit_time_ms`. Audio and haptics are outputs only. Never let the timing loop
  read audio state — that is the property that makes the game immune to speaker
  latency, and it is currently enforced by the module graph, not by discipline.

- **`game.c` and `chart.c` must never include `<pebble.h>`.** That is the only
  reason `tools/run_tests.sh` can compile them with the system `cc`. The moment
  either calls `time_ms()`, `persist_*` or `graphics_*`, the tests stop building.
  This is why `game_judge_hit()` *returns* a judgment rather than firing effects,
  and why `game.c` takes `now_ms` as a parameter instead of reading a clock.

- **`pebble emu-button` can permanently wedge the emulator's screenshot
  service.** Afterwards every `screenshot` and `emu-button` on that instance
  returns a libpebble2 `TimeoutError` and never recovers — only `pebble kill`
  plus a reinstall does. Screenshots taken before any button press are reliable.
  Hence the debug flags in `rb_config.h`: use `RB_DEBUG_AUTOPLAY` +
  `RB_DEBUG_FREEZE_AT_MS` to reach any frame with zero presses, and
  `RB_DEBUG_LOG_JUDGMENTS` + `pebble logs` to verify real input (logs survive the
  wedge).

- **`pebble wipe` can leave the emulator permanently unbootable**, and the
  symptom looks like an app problem: every `install` fails with a libpebble2
  `TimeoutError`. It is not app size — an 84 KB build failed identically. Recover
  by deleting **both** `~/Library/Application Support/Pebble SDK/4.17/emery` and
  `$TMPDIR/pb-emulator.json` (the latter goes stale claiming QEMU is still
  running), then reinstall.

- **The emulator's first frames after app launch are hundreds of ms apart.** This
  bit twice: it made a naive "stop stepping past the threshold" freeze overshoot
  by ~600 ms, and it made autoplay lose the opening notes to auto-miss when it ran
  *after* `game_step()`. Autoplay now runs before the step, and the freeze clamps
  the clock reading that both of them consume.

- **A vibration issued while another is ongoing is dropped, not queued**
  (`pebble.h` Vibes doc). Naive one-pulse-per-hit silently loses most buzzes on a
  dense chart. `feedback.c` rate-limits normal pulses and lets only big notes
  cancel an in-flight one.

- **`persist_exists()` returns true for keys this app has never written**, and
  `persist_read_int()` then hands back a stale value. This cost real debugging
  time: the sound toggle read back as 1, which the flag encoding means "off", so
  audio silently never opened even though nothing had ever disabled it.
  `save_load()` therefore writes *every* key explicitly during first-run
  initialisation. Never trust `persist_exists()` as proof a key is unset — and
  keep the +1 flag encoding (1 = false, 2 = true) so a zero is distinguishable.

- **The game plays ONE line: the melody, extracted from the MIDI.** The chart is
  built from the same notes, so every note hit is a note heard. Do not reintroduce
  the full arrangement without reading the README section on why it was dropped:
  four fixed-amplitude waveforms cannot carry a dense pop arrangement on a driver
  this small, and three rounds of tuning (drum sample, waveform choice, octave
  lifting, sustain caps) reduced the noise without removing its cause.

- **`melody.mid` is the generator's input, and it is NOT a bundled resource.**
  `watch/resources/data/melody.mid` (4,755 bytes, format 0, channel 0, 538 notes)
  is what `make_chart.py` reads by default. Only entries in `package.json`'s
  `media[]` ship, so it costs the app nothing.

  It is the extracted melody, written by `tools/extract_melody.py` from the full
  arrangement. Feeding either file to `make_chart.py` produces **byte-identical**
  `chart.c`/`music.c` (verified by hash) because the same extraction runs
  internally and is idempotent on an already-extracted line. So swapping the
  input changed nothing audible -- the win is that the melody can now be
  auditioned on a real synth before it reaches the watch.

  The source arrangement is no longer in the repo. `extract_melody.py` therefore
  takes it as an argument; it only needs re-running to change which line is the
  melody.

- **Melody extraction is the skyline algorithm** (highest sounding note wins),
  after picking a channel. **Density is a hard filter, not a scoring term** --
  scoring it alongside pitch picked a 25-note counter-line (0.44/s) over the
  158-note alto sax carrying the tune, because it sat higher and was monophonic.
  Selected here: channel 15, GM program 65.

- **Transpose a melody as a UNIT, never per note.** Per-note octave lifting is
  fine for a bass pulse but destroys a melody -- raising some notes and not their
  neighbours breaks the contour and the tune stops being recognisable.

- **These are speaker properties, not synthesis ones, and they bit hardest:**
  - A watch speaker cannot move below a few hundred Hz. This MIDI's bass sat at
    **39-69Hz** and 84% of pitched notes were under 400Hz; driven there the driver
    emits only upper harmonics, i.e. a buzz with no pitch. The melody is
    transposed to 415-831Hz. Check a part's FREQUENCY before assuming it is fine.
  - **`speaker_play_tracks()` has no envelope.** A note holds at constant
    amplitude for its whole duration, so a 1907ms note is a drone, which reads as
    buzz. `MAX_SUSTAIN_MS` caps them into plucks.
  - **Monophonic tracks click at every note boundary** unless a release gap is
    left. `NOTE_GAP_MS` takes it out of the note, not off the next one's start.
  - **Sine has no harmonics**, and harmonics are what a small speaker
    exaggerates. Square as an accompaniment was the second-worst noise source.

- **The music is MIDI played by the watch's note sequencer, not a recording.**
  `speaker_play_tracks()` takes `SpeakerNote` arrays; `tools/make_chart.py`
  parses the `.mid` at build time and emits them as `music.c`. This replaced a
  911 KB PCM resource with under 3 KB of tables -- total resources went from
  915,373 bytes to 4,213 and the app became publishable.

- **The clock is real seconds interpolated by a tick. The tick must never carry
  the time.** `time_ms()` has one trustworthy half and one useless half on the
  emulator: the seconds field is exact, but the ms field advances only ~150-190
  per real second while wrapping at 1000. A `s*1000 + ms` clock therefore crawls
  and then lurches ~1010ms once a second, which teleports every note half a
  screen and makes the game unhittable. So `clock.c` uses ONLY the seconds field,
  and fills the gap between seconds with an AppTimer tick:

  ```
  now = base_ms + min(ticks_since_boundary * measured_period, 1000)
  ```

  `base_ms` advances by exactly 1000 on each increment of `time()`. Everything
  important follows from the tick being an *interpolator*, not an accumulator:
  - **Error cannot accumulate.** The clock is exact at every second boundary
    whatever the tick has been doing — no startup transient to converge out of,
    no drift over a song of any length.
  - **A bad period estimate is confined to the second it happens in.** Too high
    and the interpolation saturates against the clamp; too low and the boundary
    takes up the slack. Neither leaks forward.
  - **Monotonicity is structural.** The clamp is 1000 and the next base is
    exactly 1000 higher, so the clock cannot step backwards — which matters
    because one that did would drag pending notes back through their hit windows.
  - The period is still **measured by counting ticks between successive
    increments of `time()`**, never against elapsed time — `time()` has
    one-second resolution, so anything built on elapsed time chases up to a
    second of quantisation noise. It is held in **Q8 fixed point**; an integer
    cannot express a 10.8ms tick, and rounding is a 2-7% error.

  **Two accumulator designs were tried and both failed, in opposite directions**
  — do not reintroduce either:
  - Correcting only the *rate*, smoothed at 1/8 from a 10ms seed: ~25s to
    converge, ~14% slow throughout, and the lost time was never recovered
    because nothing corrected accumulated error.
  - Correcting the accumulated error too: a period measured during a slow startup
    second and applied to a fast one drove the clock to **1.8x for several
    seconds**. Startup is exactly when the tick rate moves fastest (frame
    interval was measured falling 69ms → 37ms over six seconds), so a predictor
    is the wrong instrument no matter how its gain is tuned.

  The music is what makes any of this audible: the speaker plays in real time and
  cannot be steered, so every millisecond the clock is wrong is a millisecond the
  music sits away from the notes.

- **Keeping the music on the notes is one constant, and its sign is
  counter-intuitive.** The chart and the music come from one MIDI and are aligned
  *exactly* in the generated data — verified by reconstructing the music timeline
  from `music.c` and checking that all 89 chart notes coincide with a sounding
  note onset to 0ms. So any desync is a runtime property, and there are only two
  possible causes. Diagnose with `RB_DEBUG_LOG_AUDIO`, which logs when each chunk
  starts sounding against the song time its notes are charted at:
  - a **growing** error is a clock rate problem — fix it in `clock.c`, never by
    tuning the audio offset;
  - a **flat** error is `RB_MUSIC_OFFSET_MS`.

  The first `speaker_play_tracks()` call **swallows ~200ms of the chunk it is
  given** rather than costing latency before it, and chaining is seamless so
  every later chunk inherits the head start. The correction therefore starts the
  music *later* — the opposite of a latency compensation. Earlier builds had a
  positive "latency" constant here (200, then re-measured as 590); both were
  measuring the clock losing time, not the speaker.

- **Never call a speaker function from the speaker's finish callback.** It runs
  in the driver's context, and `speaker_play_tracks()` / `speaker_set_finish_
  callback()` from there is re-entering the driver that just called you. The
  emulator tolerates it; real firmware does not — this app chained chunks that
  way at every boundary and **the watch rebooted after a while**. `prv_finished()`
  now only sets a flag; every speaker call is made from `audio_tick()` on the app
  task. The cost is that a handover waits for the next frame, so a boundary is no
  longer gapless — accepted deliberately, because a one-frame gap at a note
  boundary is not something a player picks out and a reboot is.

- **The app task is the scarce resource on hardware, and the emulator hides it.**
  A 10ms clock tick (100 wakeups/sec) plus a 30fps full-screen redraw was fine on
  the emulator and *laggy on the watch*. Three things fixed it, in rough order of
  effect:
  - **Antialiasing is off for the playfield** (`render.c`). It is paid per drawn
    pixel, and a gameplay frame draws a dozen-plus circles, several stroked 3px
    wide, 25 times a second. The title/pause/results screens are drawn once and
    then stared at, so they keep it. Do not "tidy" this back to one global
    setting.
  - **The clock runs no timer at all where `time_ms()` works** — see the clock
    entry below.
  - 25fps and a 20ms tick, down from 30fps and 10ms.

  None of it costs timing accuracy: press timestamps are sampled in the click
  handler, not on the frame tick.

- **Whether `time_ms()`'s millisecond field works is decided at RUNTIME, by
  measurement — not by this file.** The tick clock exists because the field was
  once recorded advancing only ~150-190 per real second. It does not reproduce on
  the current emulator, which measures 1004/982ms of advance with zero stalls, so
  the clock now tests the field and reads it directly when it passes, cancelling
  the tick entirely. Two conditions, and **both** are required:
  - **rate** — forward advance over one real second must be ~1000ms;
  - **granularity** — the field must not stall across a whole tick. A field that
    updates in coarse jumps sums to the right total per second while standing
    still in between, which on a scrolling playfield is exactly the stutter this
    is meant to remove.

  Do not replace the rate test with "how high does ms get within a second". That
  is phase-dependent — a field advancing 190/sec still peaks near 999 about one
  second in five — and it was measured passing on the emulator and switching to a
  clock the platform could not support. `RB_DEBUG_LOG_AUDIO` logs which mode was
  chosen and why; it is the only way to see this on hardware.

- **A music re-sync that fires on jitter causes the problem it corrects.**
  Chained chunks track to ±60ms, but a chunk restarted cold after the sequencer
  has idled comes back ~200ms short. So a tight threshold fires at every
  boundary, each correction causing the cold start that triggers the next — a
  stable limit cycle injecting ~170ms of silence every 8 seconds. `RB_MUSIC_
  RESYNC_MS` is a guard rail for a real runaway, not the mechanism that keeps the
  music in time; chaining is.

- **`sleep N` in a capture script does not reliably reach a given point in the
  song**; use `RB_DEBUG_FREEZE_AT_MS` instead.

- **Capturing ANIMATION needs `RB_DEBUG_TIME_SCALE`, not the freeze flag.** The
  freeze holds one moment and is a compile-time constant, so a smooth sequence
  would cost one rebuild-install-wait cycle *per frame*. Slowing the song 20x
  instead makes an ordinary `pebble screenshot` loop sample even intervals off a
  single install: a ~920ms round trip becomes ~46ms of song time, and 50 frames
  take 46 seconds. `tools/make_gif.py` encodes them. Note it scales the clock
  READING only — press timestamps are not scaled (so use AUTOPLAY) and the
  speaker cannot be slowed (so the music runs away).

- **`run_tests.sh` fails if any `RB_DEBUG_*` flag is left on**, and that guard
  exists because leaving one on is INVISIBLE — the build compiles, installs and
  looks exactly like a good one. It had already happened: the emulator ran the
  slow-motion capture build unnoticed until two screenshots 12s apart showed the
  combo advancing by one note where real time is ~47. Note `RB_DEBUG_TIME_SCALE`
  is a divisor, so its off value is **1**, not 0.
  - **Rebuilding is not reinstalling.** `pebble build` leaves whatever is on the
    emulator alone, so a capture session ends with a clean `.pbw` on disk and a
    debug build still running. Always `pebble install` after resetting the flags.

- **In GIF LZW the decoder's table is one entry behind the encoder's**, because
  it cannot add the entry for a pair until it sees the following code. The code
  width must therefore grow one entry LATER than "the table just filled"
  (`> (1 << code_size)`, not `==`). Wrong by one and every viewer renders
  garbage a few hundred pixels in with no error raised — it was found by
  round-tripping, not by eye. `make_gif.py` is checked against both a
  spec-written decoder and macOS `sips`. The palette needs no quantisation and
  that is structural: emery has 64 colours, GIF allows 256, so screenshots
  encode exactly; the tool refuses frames from elsewhere rather than dithering.

- **The chart AND the music are generated from one MIDI file.** Do not hand-edit
  `chart.c` or `music.c` — both are generated. Re-run `python3
  tools/make_chart.py`, which reads `watch/resources/data/melody.mid` and writes
  both from the same tempo map, so the notes and the music cannot drift apart.
  That shared origin is why a desync is never a data problem. Stdlib only: no
  numpy, no ffmpeg, no soundfont, nothing to install. (This replaced an MP3 +
  spectral-flux onset detector. The MIDI grid is exact where onset detection only
  approximated it, which is what fixed "the notes don't follow any rhythm".)

- **The lanes are UP and DOWN, and the layout is ABSOLUTE, from the design.**
  `rb_config.h` carries the exact y bands the design was drawn at (HUD 0-56,
  lanes 56-106 and 112-162, song band 168-220, progress 220-228). They are not
  derived from `layer_get_bounds()` any more: the design was composed at 200x228
  for this screen, and deriving them would only invent a layout nobody drew. The
  width still comes from bounds so right-hand furniture stays anchored.
  - **The lanes are UP and SELECT; DOWN is not a lane.** Three arrangements have
    shipped and the current one deliberately BREAKS the "lane sits at its
    button's vertical position" rule: SELECT is the middle button and it plays
    the lower band. Adjacent buttons won over aligned ones because alternating
    at sixteenth-note rates is the game, and the ends of the stack are easy to
    find but slow to alternate between. README's "Which buttons are the lanes"
    has the full history -- read it before changing this again.
  - Because of that, the lower lane's badge is a **RIGHT** arrow
    (`prv_draw_arrow` in render.c), never a down one: a down arrow would point at
    the one button that does nothing during play. Right is the direction the
    notes travel, which names no button at all.
  - `main.c` reads the lane value through `RB_BTN_UP`/`RB_BTN_SELECT`/
    `RB_BTN_DOWN` for its menus. `input.c` reports LANES; the menus need BUTTONS.
    One mapping in one place, so a future remap cannot silently swap "play" and
    "quit" underneath render.c's on-screen legends.
  - `RB_LANE_MID` was renamed `RB_LANE_BOT`. `LANE_NAMES` in make_chart.py must
    stay bottom-most-first: `build_chart()` puts notes above the melody's median
    in lane 1, and the higher pitch has to land in the higher lane.

- **`layer_mark_dirty()` re-renders the WHOLE WINDOW, not one layer's rect.**
  Splitting the screen into HUD / playfield / song-band layers and marking only
  the moving one dirty is the obvious way to stop re-running static text layouts
  25 times a second. It does not work here, and it was measured rather than
  assumed: a per-proc counter showed all three layers repainting 927/927/927
  times over one run, when the band should have repainted about 130. The split
  bought nothing and cost three extra full-rect background fills per frame, so
  it was reverted to one canvas. Do not reintroduce it without first proving
  partial redraw exists.

- **Build colours from their ARGB byte, not by name.** The 64-colour palette is
  two bits per channel, so a design hex on the grid transcribes mechanically:
  `0b11` then RR GG BB, each pair meaning 0/85/170/255. Grepping
  `gcolor_definitions.h` for these values returned confident WRONG answers (it
  named #555500 "Indigo"), and a wrong colour constant is not something the
  build catches. `render.c` uses `RB_ARGB(0b11010100)`.

- **The emulator screenshot does not report colours faithfully.** Black comes
  back exactly #000000 and greys within 1, but saturated colours are warm-shifted
  (#555500 reads #564E36, #0055AA reads #16638D). Judge hue relationships, not
  absolute values, and expect the watch to differ again.

- **Two songs, and adding a third is a generator-only change.** `SONGS` in
  `tools/make_chart.py` is the whole configuration: title, C identifier stem,
  melody `.mid`, start bar, bar count. `chart_count()` drives the title-screen
  selector, `save.c` allocates persist keys per song from a base, and
  `render.c`'s list scrolls — so no C file needs editing to make a new song
  appear. The section (which bars) is the one thing that cannot be derived; it
  is a musical judgement.

- **Three songs**: Never Gonna Give You Up (118 BPM, bars 12-40, 157 notes),
  You Are Not Alone (59 BPM, bars 30-44, 74), Golden (93 BPM, bars 11-39, 146).
  All ~55-57s. "I Want It That Way" and "Dancing Queen" were removed on request.

- **Golden's section starts at bar 11 = 0:25, given as a timestamp.** Convert a
  timestamp to the nearest bar and then CHECK what the melody is doing there:
  bars 10-12 repeat bars 6-8 note for note, so bar 11 is the fifth of five
  repeated notes rather than a phrase head. It opens on the moving part of the
  line, which reads fine; bar 12 is the next phrase head if it ever needs to
  change. Every technical measure is identical across bars 10, 11 and 12, so
  that choice is purely musical.

- **Velocity is normalised PER SONG** (`velocity_gain`). Arrangements are written
  at wildly different levels -- these peak at 124, 97 and 39 -- so emitting raw
  velocity makes one song a third the volume of another for no musical reason.
  A single gain per song brings the loudest note to full scale and preserves
  every ratio; do NOT stretch each melody across the range instead, which would
  turn song 1's near-flat 119-124 into a 55-100 swing that is not in the music.

- **A melody can share its channel with the accompaniment, separated only by
  VELOCITY.** "Golden" is a piano arrangement written that way: tune at 100, an
  ostinato under it at 50-63. Skyline cannot tell them apart -- wherever the tune
  rests the ostinato becomes the highest sounding note, and the player is asked
  to hit an accompaniment figure. `melody_velocity_floor()` looks for a GAP in
  the velocity values and drops everything below it, BEFORE the skyline runs;
  afterwards is too late. It returns 0 (keep everything) unless the gap is
  clear, so a merely expressive part is untouched -- measured: Golden splits at
  95, the other two songs do not split at all.

- **Anything expressed in TICKS must be scaled to the file's own division.**
  `MIN_ONSET_TICKS` was derived from `TICKS_PER_BEAT_REQUIRED` (384) but applied
  by `extract_melody.py` to the raw source BEFORE resampling. On a 48-tick file
  that made it a whole quarter note, and it collapsed the melody to one note per
  beat -- silently, producing a plausible uniform 492ms line that bore no
  relation to the arrangement. It is now `min_onset_ticks(ticks_per_beat)` and
  the caller passes the file's division. Check any new tick constant the same
  way; the pipeline reads files at 48, 120, 192, 384 and 480.

- **Onsets closer than a 32nd note are ONE melodic event** (`MIN_ONSET_TICKS`).
  Rolled chords and grace notes arrive as separate note-ons a few ms apart, and
  the skyline rule only removes them when they OVERLAP -- a chord spread 1ms at a
  time survives it intact. The FIRST arrangement of "Golden" carried six such
  pairs at 1, 25 and 31ms, and without collapsing them every 28-bar window of it
  failed to chart. The arrangement now in use needs no such help (its tightest
  gap is 491ms), but the rule is right in general and stays.

- **The same song can differ enormously between arrangements.** Two files of
  "Golden": the first cleared the same-lane floor by 1ms and only charted at all
  once near-simultaneous onsets were collapsed; the second clears it by 241ms
  with nothing special done. If a song fights the constraints, the arrangement
  is worth changing before the constraints are.

- **A section is chosen on FOUR measures, not one.** Getting any of them wrong
  ships a song that is technically valid and bad to play:
  - **Is it the chorus?** Fingerprint the melody on a half-beat grid and count
    how often each 8-bar window recurs; the chorus is the most-repeated phrase.
    Mean pitch is a decent tiebreak -- a chorus sits on top of the range.
  - **How much of it SOUNDS?** Dancing Queen's most-repeated hook contains a
    **12.1 second stretch with no melody at all** -- an instrumental break -- and
    sounded for 47% of its length. Rank by longest silence, not just by average.
  - **Velocity.** See the fade-out entry below.
  - **Grace notes.** See the entry below.

- **`extract_melody.py` resamples every source onto 384 ticks/beat.** Sources
  arrive at 120, 192 and 384. Normalising at that one boundary is deliberate:
  the alternative is threading a variable ticks-per-beat through every function
  downstream that does bar arithmetic. `parse_midi()` therefore accepts any
  metrical division, and the 384 requirement is asserted in `build_song()`, where
  the bar maths actually lives. Resampling preserves real time exactly bar
  rounding (~0.7ms).

- **Check the section's VELOCITIES, not just its notes.** Arrangements write
  fade-outs as velocity, and "You Are Not Alone" drops to **velocity 1** for its
  last eight bars. The original section (58-72) was the final chorus and ran
  straight into it, so the second half of that song charted notes the player
  could see and hit with **no sound at all** -- and nothing objected, because the
  chart is built from note positions and never looks at velocity. Symptom on the
  watch: "there's no music in the later part."
  - The section now ends two bars clear of the fade.
  - `MELODY_MIN_VELOCITY` floors what is emitted, as a backstop for the next
    arrangement that fades somewhere less obvious. Relative dynamics above the
    floor are kept.
  - The generator prints each melody's velocity range and warns when notes fall
    under the floor. That is the check that would have caught this.

- **`MAX_SUSTAIN_MS` was 260 and that was far too tight.** It exists because
  `speaker_play_tracks()` has no envelope, so a very long note is a drone that
  reads as buzz -- but 260 was set when the watch played a FOUR-TRACK reduction,
  where simultaneous fixed-amplitude tones were the real problem. One sine line
  is a different proposition. At 260 a ballad arrived as unconnected plucks:
  "You Are Not Alone" is legato 85% of its section and sounded for 26% of it.
  Now 800, which roughly doubles every song. It costs less than it looks --
  consecutive notes already cut each other off, so this only extends the last
  note before a rest.

- **Chunk boundaries are nudged off notes, never through them.** A note
  straddling a boundary used to be clipped at the end of one chunk and restarted
  at the beginning of the next: two onsets for one chart note, breaking
  one-note-one-sound. Worse, a leading fragment under the minimum length was
  dropped, leaving a charted note with NO sound. Both appeared the moment
  MAX_SUSTAIN_MS grew long enough for notes to reach a boundary -- three
  duplicated onsets and one silent note. The final edge is nudged too, for a
  note starting a few ms before the section ends.

- **Nothing may stop the music permanently.** The whole handover chain hangs off
  one finish callback, so anything that swallows it silences the rest of the song
  with no error and nothing in the log. Two defences, both in `audio.c`:
  - a **watchdog** in `audio_tick()` forces the handover if a chunk has not
    reported finishing `RB_MUSIC_STALL_MS` past its own duration;
  - a failed `speaker_play_tracks()` **arms the next chunk** instead of giving
    up, so a transient failure costs one chunk rather than the remainder.

- **Pick a section on mean melody pitch, and check for grace notes.** A chorus
  usually sits on top of the singer's range, so mean pitch per 8-bar block finds
  it. But "I Want It That Way" charts fine in most windows while containing
  ornaments **50ms apart** -- unhittable, and invisible unless looked for, since
  nothing is dropped and the chart validates. The generator now warns on any pair
  under `PLAYABLE_MIN_MS`; move the section rather than dropping the note, which
  would break the 1:1 promise.

- **Sound and haptics are always on.** The toggles used to live on UP/DOWN on the
  title screen, which is where the song selector now is. `save.c` no longer
  stores them.

- **DOWN is subscribed, but it is NOT a lane.** It reports `RB_LANE_NONE`
  (0xFF, deliberately outside the `RbLane` enum so it can never index a
  lane-sized array). Menus use it to move the selection; `main.c` ignores it
  during play. Do not alias it onto a lane — there is no third band.

- **Music chunks are bounded in MILLISECONDS, not bars** (`CHUNK_MAX_MS`). Bars
  were the unit until a song at half the tempo made 8 bars 32.5s instead of
  16.3s — and **a 32.5s chunk never reports finishing at all**. No error, nothing
  in the log; the music just stops after the first chunk, because the handover is
  driven by that callback. Note count was not the difference (123 notes failed,
  159 works). Reproduced every run.
  - Anything finer than that is emulator noise: boundary error at 16.3s measured
    +33/+17/+10ms on one run and +4/-189/+320ms on another with **identical
    inputs**. Do not tune `CHUNK_MAX_MS` against those numbers.

- **The chart is EVERY melody note, 1:1. Do not reintroduce selection.** 157
  notes for 157 melody notes, so a note heard is always a note to hit and a note
  hit is always a note heard. Both come from the same tempo map at the same
  offset (`LEAD_MS` == `RB_MUSIC_START_MS`), so a chart note's hit time *is* the
  moment its tone sounds.

  This is only possible because the melody sits on an exact sixteenth grid —
  every interval is 127ms, 254ms, or ≥320ms, with nothing awkward between. Two
  things make it playable:
  - **Lane follows pitch, except when spacing overrides it.** A note whose
    preferred lane was used within `SAME_LANE_MIN_MS` takes the other lane (29
    of 157 do). That is what guarantees the minimum same-lane gap is an eighth
    (254ms) rather than a sixteenth — a 127ms same-lane repeat is ~8 presses a
    second on one button, unplayable, whereas alternating hands at that rate is
    the whole point of a Taiko-style game.
  - **`RB_MISS_MS` is therefore 125, not a free choice.** Two judgment windows
    must fit inside 254ms. It came down from 160, which was only possible while
    the chart was a subset with 333ms of clearance. `make_chart.py` asserts the
    gap it produces and `run_tests.sh` asserts it against the shipped chart.

  The old generator chose 89 of 157 by weight under a density ceiling, so 68
  notes sounded with nothing to press. `NOTES_PER_SEC`, `GLOBAL_MIN_MS` and
  `beat_bonus()` existed only to serve that selection and are gone.

- **Accent tests on a MIDI part must be RELATIVE, not absolute.** `velocity >=
  116` marked *every* note big, because this melody was sequenced flat at
  119-124. It went unnoticed while the chart was a subset. The test is now
  median + `BIG_VELOCITY_MARGIN`, which finds accents on an expressive part and
  correctly finds none on a flat one — 24 of 157 now, all from bar downbeats and
  long holds.

- **Split the kit at pitch 38, not 42.** "Membranes vs metal" is the intuitive
  two-lane split and it fails: kick and snare alternate on every beat, so putting
  them in the same lane saturates it — measured 112/12. Cutting between kick and
  snare puts that alternation *across* the lanes: 56/56 and 100% hand
  alternation.

- **Resources are ~4.2 KB** — the menu icon. The app is now publishable
  (previously 893.9 KB against the 256 KB store limit). Keep it that way: do not
  re-add a PCM music resource.

- **The menu icon is BLACK on transparent, and that was measured.** The obvious
  choice is white — the design specifies white, and Pebble's own docs suggest it
  — but the emery launcher composites the icon over LIGHT rows, so white
  disappears. Checked by installing and screenshotting the launcher list, not by
  reading docs. The SDK converts it to a 1-bit greyscale PNG with a `tRNS`
  marking the background transparent, so the black survives intact into the pbw.

- **Store artwork is generated, and none of it ships.** `tools/make_icon.py`
  writes the 25px menu icon AND the 80/144 store tiles from one geometry;
  `tools/make_banner.py` writes the 720x320 store banner; both sit on
  `tools/pngkit.py`, a stdlib supersampling canvas. Only `package.json`'s
  `media[]` decides what is bundled, so `developer-portal/` costs the app zero
  bytes. The banner is DRAWN, not screenshotted: 200x228 stretched to 720x320
  destroys the 2px rails, and the emulator misreports the colours anyway.
  Antialiasing there is free (it runs once, on a laptop) — do not take that as
  licence to turn it on in `render.c`, where it is paid per pixel 25x a second.

- **The back button cannot take a raw, long, or repeating click handler**
  (`pebble.h:97-98`). Single-click only. Fine here — BACK is never gameplay.

- **`ClickHandler` carries no timestamp.** The press time must be sampled with
  `clock_now_ms()` as the literal first statement of the handler; everything after
  it is latency charged to the player.

- **Guard the speaker with `#if PBL_API_EXISTS(speaker_stream_open)`, never
  `#ifdef PBL_SPEAKER`.** Gabbro declares the real functions but does not get the
  `PBL_SPEAKER` define — an SDK inconsistency.

- **The Gothic system fonts have no arrow glyphs.** Lane badges are drawn as
  stacked `graphics_fill_rect` rows, not `▲`/`▼` characters, which render as tofu.
  No `GPath` either — that would mean a heap allocation.

- **Waf can report a successful build having rebuilt nothing**, leaving a stale
  `.pbw` that installs fine — which looks exactly like a source change having no
  effect on the emulator. This cost two rounds of debugging a "2-lane change that
  did not apply". `pebble clean` before concluding anything about the code.

- **`wscript` is the stock SDK default and byte-identical to the sibling repos —
  never edit it.** It already globs `src/c/**/*.c`, so new source files need no
  build change.

## Conventions

- Static functions are prefixed `prv_`, file-scope state `s_`, public functions
  `<module>_<verb>`, macros `RB_`.
- `#pragma once` in every header, never include guards.
- Fixed-size static arrays only; no `malloc`. Integer maths only; no floats.
- Every tunable lives in `rb_config.h` with a comment explaining the number.
- The frame timer only mutates state and calls `layer_mark_dirty()`; all drawing
  happens in `render_update_proc`.
- Layout is derived from `layer_get_bounds()` at draw time, never hardcoded.
- System fonts only (`FONT_KEY_GOTHIC_*`).
- 2-space indent, K&R braces, braces even on single-statement ifs.

## Verifying a change

```bash
./tools/run_tests.sh                              # judgment logic, no emulator
cd watch && pebble build && pebble install --emulator emery
pebble screenshot --emulator emery --no-open shot.png   # BEFORE any button press
```

For gameplay frames, set `RB_DEBUG_AUTOPLAY 1` and `RB_DEBUG_FREEZE_AT_MS` to the
moment you want, then rebuild — and remember the freeze is in *song* time, so you
must wait that long after install before capturing.

Always read the screenshot back rather than assuming it rendered; emery's display
squashes warm colours together, so a palette choice has to be checked by eye.
