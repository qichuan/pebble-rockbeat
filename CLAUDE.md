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

From the repo root: `./tools/run_tests.sh` (expect `OK: 2651 checks passed`).

This tool version does **not** accept `--scale`. `--vnc` disables emulator audio.

## Architecture

| File | Owns |
|---|---|
| `rb_config.h` | every tunable constant, with the reasoning for each |
| `main.c` | window lifecycle, frame timer, the hit path, debug harness |
| `clock.{c,h}` | self-calibrating tick clock; see the gotcha below |
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

- **The music is MIDI played by the watch's note sequencer, not a recording.**
  `speaker_play_tracks()` takes `SpeakerNote` arrays; `tools/make_chart.py`
  parses the `.mid` at build time and emits them as `music.c`. This replaced a
  911 KB PCM resource with ~10 KB of tables — a 90× cut that took total
  resources from 915,373 bytes to 4,213 and made the app publishable.

- **Everything about the sequencer is measured; the SDK documents almost none of
  it.** All of this is in `music.h`, and all of it was found the hard way:
  - `SPEAKER_MAX_NOTES` (256) is a **per-track** cap, not per-call — 4 tracks of
    128 plays fine.
  - **Exceeding it faults the app**, it does not return false. The generator
    refuses to emit an over-long chunk; there is no runtime guard because by
    then it is too late.
  - Chaining the next chunk from the finish callback costs **no audible gap**,
    and drift against the game clock is jitter (+17/−5/−12/+41/−30 ms per 4 s
    chunk), not a rate error.
  - The **first** `speaker_play_tracks()` call costs ~200 ms of startup latency
    that later calls do not — hence `RB_MUSIC_LATENCY_MS`.
  - **A PCM stream cannot coexist with the sequencer**: `speaker_stream_open()`
    returns false while tracks play. The music survives (finishes `Done`, not
    `Preempted`). This is why there are no reactive hit sounds any more — the
    two audio sources are mutually exclusive and the music won.

- **Measure sequencer timing only after the clock has settled.** The first
  attempt reported 2–8% drift and looked like a fatal rate error; it was
  measuring `clock.c` calibrating during its first seconds. Waiting 15 s before
  sampling turned the same numbers into ±40 ms of jitter.

- **The clock is a self-calibrating tick, and must stay that way.** `time_ms()`
  has one trustworthy half and one useless half on the emulator: the seconds
  field is exact, but the ms field advances only ~150-190 per real second while
  wrapping at 1000. A `s*1000 + ms` clock therefore crawls and then lurches
  ~1010ms once a second, which teleports every note half a screen and makes the
  game unhittable. `clock.c` instead ticks on an AppTimer and uses ONLY the
  seconds field, to measure what the tick period actually is. Four things are
  load-bearing:
  - The step is **measured, not assumed** — a 10ms timer fires at ~14ms here, and
    the true rate changes with load (title screen: one timer; gameplay: three).
  - The rate is measured by **counting ticks between successive increments of
    `time()`**, never by comparing elapsed time against the accumulator. `time()`
    has one-second resolution, so any scheme built on elapsed time is chasing up
    to a second of quantisation noise: a cumulative average could not follow a
    rate change (settled at 0.82x), and a feedback controller oscillated (1.6x,
    then 0.66x). Each `time()` increment is an exact one-second boundary, so a
    tick count between two of them is an exact ticks-per-second with no
    quantisation error at all.
  - The step is held in **Q8 fixed point** — an integer step cannot express
    13.5ms, and rounding is a 4-7% rate error.
  - **Only the step is ever adjusted, never the counter**, because
    `clock_now_ms()` must stay monotonic; a clock that steps backwards drags
    pending notes back through their hit windows.

  Do NOT "simplify" this back to `s*1000 + ms`, and do not re-derive the rate
  from elapsed time.

- **`sleep N` in a capture script does not reliably reach a given point in the
  song**; use `RB_DEBUG_FREEZE_AT_MS` instead.

- **The chart AND the music are generated from one MIDI file.** Do not hand-edit
  `chart.c` or `music.c` — both are generated. Re-run `python3
  tools/make_chart.py`, which reads
  `watch/resources/data/Never-Gonna-Give-You-Up.mid` and writes both from the
  same tempo map, so the notes and the music cannot drift apart. Stdlib only: no
  numpy, no ffmpeg, no soundfont, nothing to install. (This replaced an MP3 +
  spectral-flux onset detector. The MIDI grid is exact where onset detection only
  approximated it, which is what fixed "the notes don't follow any rhythm".)

- **Two lanes, and the layout must NOT be divided by `RB_LANE_COUNT`.** The
  premise is that a lane sits at the vertical position of the button that plays
  it, and the buttons do not move when a lane is dropped. Splitting the playfield
  in two gives 90px bands centred at y=69/159, putting the SELECT lane 45px below
  the SELECT button. `render.c` therefore divides by `RB_LANE_SLOTS` (3) and
  leaves the bottom band empty. Do not "tidy" that back to `RB_LANE_COUNT`.

- **At two lanes the chart is exactly the beat grid, and no generator knob
  changes that.** A beat is 508ms at 118 BPM, so an eighth offbeat sits 254ms
  from its neighbours — inside `SAME_LANE_MIN_MS` (333, itself forced by
  `2 * RB_MISS_MS`) and therefore illegal in its own lane. With three lanes an
  offbeat could take the third; with two, both are already carrying beat notes,
  so it is illegal in both. Sweeping `NOTES_PER_SEC` 2.2→3.4 and the beat bonus
  70→30 produced exactly 112 notes every time. Syncopation needs a third lane or
  a smaller `RB_MISS_MS`.

- **Split the kit at pitch 38, not 42.** "Membranes vs metal" is the intuitive
  two-lane split and it fails: kick and snare alternate on every beat, so putting
  them in the same lane saturates it — measured 112/12. Cutting between kick and
  snare puts that alternation *across* the lanes: 56/56 and 100% hand
  alternation.

- **Resources are 4,213 bytes** — the menu icon. The app is now publishable
  (previously 893.9 KB against the 256 KB store limit). Keep it that way: do not
  re-add a PCM music resource.

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
