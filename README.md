# Rockbeat

A Taiko-style rhythm game for the Pebble Time 2.

Three horizontal lanes are stacked to match the three physical buttons down the
right edge of the watch. Notes ("pebbles") scroll left to right toward a fixed
hit target beside the buttons; press that lane's button as a note arrives.

```
  TOP lane    ->  UP button       (blue)
  MIDDLE lane ->  SELECT button   (red)     <- screen centre, where SELECT is
  BOTTOM lane ->  DOWN button     (yellow)
  BACK        ->  exit            (never used for gameplay)
```

Hits are judged Perfect / Good / Miss on timing accuracy, with a running score
and combo counter.

## Requirements

- **emery** — Pebble Time 2, 200x228, 64 colours. This is the only target
  platform; see "Platform" below.
- Pebble SDK 4.17 with pebble-tool 5.0.39 or newer.

Verify the toolchain with:

```bash
pebble --version          # expect: Pebble Tool v5.0.39 (active SDK: v4.17)
```

## Building and running

All `pebble` commands must be run from `watch/` — the tool fails elsewhere.

```bash
cd watch

pebble build                              # builds for emery
pebble install --emulator emery           # launches the emulator and installs

pebble screenshot --emulator emery --no-open shot.png
pebble logs --emulator emery
pebble kill                               # stop the emulator
```

`pebble clean` is required after editing `package.json`.

Note this tool version does **not** accept `--scale`. Add `--vnc` in a headless
environment — but be aware that disables emulator audio (see below).

## Testing

The judgment windows, scoring and combo logic are unit-tested on the host, with
no emulator involved:

```bash
./tools/run_tests.sh      # expect: OK: 2612 checks passed
```

This works because `game.c` and `chart.c` do not include `<pebble.h>` — they are
compiled with the system `cc`. Keep it that way; it is the fastest way to catch a
timing regression, and it is why `game_judge_hit()` returns a judgment instead of
firing sound and haptics itself.

## How the timing works

The authoritative clock is the game's own timeline: elapsed milliseconds since
the song started. Every note's on-screen x position derives from
`(note.hit_time_ms - elapsed_ms)`, and scoring compares the button-press
timestamp to `note.hit_time_ms`.

Sound and vibration are fire-and-forget **outputs** triggered by judged events.
The timing and scoring loop never reads audio state, so the game stays perfectly
playable and correctly scored with sound muted, disabled, or absent — and is
immune to speaker latency.

The frame rate does not bound timing precision either. Press timestamps are taken
inside the button handler, on a different code path from the 30 fps redraw, so a
late or dropped frame changes what you see but never what you score.

### The clock

The clock is driven by a **fixed-rate tick timer that calibrates itself**, not by
`time_ms()`.

The obvious implementation — accumulate deltas of `seconds*1000 + ms` — is what
this project shipped first, and it made the game unplayable. On the emulator
those two halves disagree (see "Emulator quirks"), so the clock crawled and then
jumped 1010 ms about once a second. At 95 px/s that teleports every note ~96 px,
which is why notes appeared at random and could not be hit. Correcting the
average *rate* did not help: rhythm games are ruined by jitter, not by drift.

So the time base is an `AppTimer` tick, and the only thing taken from the wall
clock is the **seconds** field — the half that is trustworthy — used to measure
what the tick period actually is. Two details matter:

- The step is **measured, not assumed**. A timer asked for 10 ms fires at ~14 ms
  on the emulator; hardcoding 10 left the clock 15% slow.
- The step is held in **Q8 fixed point**. An integer step cannot express a true
  period of ~13.5 ms, and rounding it to 13 or 14 is a 4–7% rate error — a whole
  beat of drift every twenty seconds.

The step is only ever adjusted, never the counter, so the clock stays monotonic.
Measured after the rewrite: **1.008× real time, with every frame delta even.**
The cost is that timestamps quantise to `RB_CLOCK_TICK_MS` (10 ms) — a bounded,
predictable error well inside the 60 ms Perfect window.

## Layout

```
pebble-rockbeat/
  tools/
    run_tests.sh          host-side test runner (system cc, not the ARM toolchain)
    game_test.c           judgment / scoring / combo / chart-validity tests
  watch/
    package.json          the only app config; no appinfo.json
    wscript               stock SDK output -- never edit (already globs src/c/**/*.c)
    resources/images/menu_icon.png
    src/c/
      rb_config.h         every tunable; no magic numbers in the logic files
      main.c              window lifecycle, frame timer, hit path, debug harness
      clock.{c,h}         self-calibrating tick clock (see "the clock" below)
      chart.{c,h}         note-chart format, the demo song, forward-compat loader
      game.{c,h}          song clock, judgment, score, combo -- PEBBLE-FREE
      render.{c,h}        all drawing; the only file with graphics_* calls
      input.{c,h}         raw-click handlers -> timestamped hit events
      feedback.{c,h}      haptics policy + hit-flash state
      audio.{c,h}         PCM voice pool, music streaming, mixer, own pump timer
      save.{c,h}          high score and toggles; the only persist_* caller
    resources/data/music.pcm   58.7s of 16kHz 8-bit PCM (917KB)
  tools/make_chart.py     regenerates the chart AND the audio from an mp3
```

## Controls

| Screen | UP | SELECT | DOWN | BACK |
|---|---|---|---|---|
| Title | toggle sound | start the song | toggle haptics | exit the app |
| Playing | TOP lane | MIDDLE lane | BOTTOM lane | pause |
| Paused | restart | resume | quit to title | quit to title |
| Results | to title | to title | to title | to title |

Sound and haptics settings persist, as do the high score and best combo.

## The song: "Prelude Drive"

The stage is charted from a real track — a remix of Chopin's Prelude Op. 28 No. 4
— and the game plays that audio while you're hitting the notes.

**58.7 seconds, 90.000 BPM, 97 notes (1.65/s).** A bar-aligned 22-bar excerpt
from 69.5 s to 128.2 s of the source, taken from its sustained high-energy
section.

The length is set by audio quality, not musical taste — see "How the music
plays" below.

### The chart is generated from the audio, not hand-placed

`tools/make_chart.py` does the analysis:

1. Decode to mono 22.05 kHz; 1024-point STFT with a 256-sample hop (11.6 ms).
2. Positive spectral flux per frequency band — each band's flux averaged **per
   bin** and then z-scaled. That normalisation is the part that matters: the
   1200 Hz+ band has 456 FFT bins against the sub-200 Hz band's 10, so comparing
   raw summed flux reports "treble" for essentially every onset. The first
   attempt did exactly that and split 1478 onsets as low=0 / mid=38 / high=811.
   Normalised, the same audio splits 627 / 567 / 284.
3. Peak-pick against a moving-average threshold → 1478 onsets.
4. Tempo by comb-filtering the onset envelope → **90.000 BPM exactly**, first
   beat at 191.6 ms. The median distance from a detected onset to the nearest
   sixteenth is **15.7 ms**, which is what confirms the grid is real rather than
   fitted.
5. Snap to **eighths** (333 ms), keep the strongest onset per slot, then drop
   the quietest notes **per band**.

Both of those last choices were corrections. Sixteenths (167 ms) put notes on
subdivisions a listener does not feel as the beat, so the chart read as noise
rather than rhythm — and at 167 ms apart the circles nearly touch. And a single
global loudness threshold keeps whichever band happens to be loudest, which
starved the bass lane to a 19/46/56 split; thresholding within each band gives
25/41/31.

**Lanes follow the frequency band**, so the game reads the way the music sounds:
bass and kick on BOTTOM, piano body on MIDDLE, melody and sparkle on TOP. Big
notes are the loudest 6% of onsets.

One rule is enforced at generation time: two notes in the **same** lane are never
closer than 333 ms (one eighth at 90 BPM). That is deliberately just above
`2 * RB_MISS_MS` = 320 ms, so no two notes in a lane can have overlapping
judgment windows. Where the music wanted a faster same-lane run the generator
moves the note to a neighbouring lane rather than dropping it — which is also
what makes the dense passages alternate hands. `tools/run_tests.sh` asserts this
invariant against the shipped chart.

### Regenerating

```bash
python3 -m venv venv && ./venv/bin/pip install numpy    # ffmpeg also required
./venv/bin/python tools/make_chart.py path/to/track.mp3
```

That writes **both** `watch/resources/data/music.pcm` and
`watch/src/c/chart.c`. Both come from the same excerpt offsets, so the chart and
the audio cannot drift apart. `chart.c` is generated — do not hand-edit it.

Charts are `{ hit_time_ms, lane, type }` arrays sorted ascending by time. The
loader is written so a chart can later come from a resource file instead of being
compiled in: `chart_load_from_resource()` is a documented stub returning `false`,
and `main.c` already calls it first and falls back to the built-in chart. The
intended binary layout is documented at the top of `chart.h`.

## How the music plays

**PebbleOS has no audio decoder**, and the Speaker API accepts raw PCM only
(8/16 kHz, 8/16-bit, mono). So the mp3 cannot be shipped as-is — the track is
pre-decoded to **raw signed 8-bit 16 kHz mono PCM** and stored as a `raw`
resource, streamed a frame at a time with `resource_load_byte_range()`. Only a
few hundred bytes are ever in RAM; the 917 KB of audio stays in flash. Source and
stream run at the same rate, so there is no resampling in the mixer.

**The excerpt length is set by that budget.** At 16 kHz, 8-bit the rate is 16000
bytes/s, so 58.7 s costs 917 KB of emery's **1024 KB**. An earlier build ran
88 s at 8 kHz — same size, but 8 kHz caps the audio at 4 kHz and it sounded like
AM radio. Length was traded for bandwidth deliberately.

**IMA ADPCM was tried and rejected.** At 4 bits/sample it would have bought
16 kHz *and* the full 88 s at the same file size. Measured against the source it
returned **24.4 dB SNR versus 37.5 dB for plain 8-bit** — ADPCM buys bandwidth by
spending precision, and since the speaker's output stage is 8-bit anyway the
trade lost. The code was removed rather than kept as dead weight.

> ⚠️ **The build prints a resource-size warning**: 691.6 KB exceeds the 256 KB
> **app-store** limit. It installs and runs fine via `pebble install`, but this
> build could not be published as-is. A publishable version would need a much
> shorter excerpt (~30 s) or synthesised music instead of a recording.

Because the app owns one PCM stream and mixes the drums and the music into it
itself, there is no preemption and no second audio source to keep in step. Music
position is derived from the **stream position**, which is itself driven by the
game clock — so the music is locked to the notes by construction, and the
dependency still runs one way only: audio follows the clock, never the reverse.

## Platform

`emery` only. Two reasons beyond it being the stated target:

- Gabbro (Round 2) would need a different layout — three horizontal lanes on a
  260x260 round screen get clipped by the bezel.
- The SDK's own manifest schema
  (`sdk-core/pebble/common/tools/schemas/attributes.json`) lists only
  `aplite, basalt, chalk, diorite, emery, flint` as legal `targetPlatforms`
  values. `gabbro` is absent from it.

All geometry is still derived from `layer_get_bounds()` at draw time, so nothing
hardcodes 200x228 and a second platform would be a layout pass, not a rewrite.

## Debug harness

`rb_config.h` carries a few flags, all default `0` and all compiled out when off.
They exist because of a real emulator hazard: **`pebble emu-button` can
permanently wedge the emulator's screenshot service**, after which every
`screenshot` and `emu-button` on that instance returns a libpebble2
`TimeoutError` until `pebble kill` plus a reinstall. Screenshots taken *before*
any button press are reliable.

So every interesting frame has to be reachable without pressing a button:

| Flag | Effect |
|---|---|
| `RB_DEBUG_AUTOPLAY` | auto-hits every note at its exact hit time — drives the whole judgment path from a cold boot with zero input |
| `RB_DEBUG_AUTOPLAY_OFFSET_MS` | offsets the synthetic press; a value between `RB_PERFECT_MS` and `RB_GOOD_MS` forces Goods |
| `RB_DEBUG_AUTOPLAY_MISS_EVERY` | drops every Nth note so the miss path and combo reset are visible |
| `RB_DEBUG_FREEZE_AT_MS` | clamps the song clock to a chosen elapsed value, so a `~1s` screenshot round trip cannot miss the moment |
| `RB_DEBUG_LOG_JUDGMENTS` | logs every judged press — this is how real button input gets verified, since `pebble logs` keeps working even if screenshots are wedged |

## SDK APIs: what was verified, and what was not

Every SDK call used was checked against the local headers at
`~/Library/Application Support/Pebble SDK/SDKs/4.17/sdk-core/`. Nothing was
guessed.

**Confirmed present** (emery `pebble.h` line numbers): the Speaker API at
8463-8628 — including `speaker_stream_open/write/close` at 8592/8598/8601, with
the symbols genuinely defined in `emery/lib/libpebble.a` (checked with `nm -g`);
`window_raw_click_subscribe` at 5678; `time_ms` at 8825, **not** deprecated;
`app_timer_register/reschedule/cancel` at 3016/3023/3028; the `vibes_*` family and
`VibePattern` at 8430-8459; `quiet_time_is_active` at 8713; `persist_*` at
3147-3224; and the graphics and layer calls at 4129-4265, 4978 and 5296-5427.
Emery's 200x228 / 64-colour / 128 KB figures come from
`common/tools/pebble_sdk_platform.py:133-174`.

**Measured empirically** (the emulator, via `RB_DEBUG_LOG_AUDIO`):

- `speaker_stream_open(SpeakerPcmFormat_16kHz_8bit, 70)` **returns true**, with
  `speaker_is_muted()` and `quiet_time_is_active()` both false. The real audio
  path runs; it is not silently falling back to the stub.
- **`speaker_stream_write()` accepts at most 512 samples per call.** The SDK says
  only "may be less if the buffer is full" and documents no size. Asking for 640
  returned exactly 512, every call, every time. This is now `RB_AUDIO_WRITE_MAX`.
- **That cap forces the pump onto its own timer.** 512 samples is 32 ms of audio
  while a render frame is 33 ms (~37 ms in practice), so pumping once per frame
  delivers ~13.8k samples/s against the 16k/s the speaker consumes — the stream
  starves no matter how many slices are attempted, measured as a steady ~10% of
  audio dropped. A dedicated 20 ms pump timer (`RB_AUDIO_PUMP_MS`) needs only 320
  samples per call and brought dropped audio to **zero**.
- Frame pacing holds at 33–39 ms with audio and music running.

**Could not be verified, and how each is handled:**

1. **Whether the 512-sample cap is the true buffer depth or a per-call limit.**
   Either way the pump is now sized under it. The drop-don't-retry path remains
   correct to keep: on a short write the remainder is discarded and
   `s_samples_written` still advances, so the music stays aligned with the notes
   instead of drifting progressively behind them.
2. **Button-press latency through the firmware.** `ClickHandler` carries **no
   timestamp** (`pebble.h:5211`; the recognizer exposes only button id, click
   count and is-repeating), so the press time is sampled with `time_ms()` as the
   first statement of the handler. Whatever dispatch delay sits between the
   physical button and that line is not measurable from inside the app. It is
   absorbed by deliberately generous judgment windows (±45 ms for Perfect). An
   emulator session measured a real press at +17 ms, which suggests the path is
   short, but that is one sample and not a calibration.
3. **`time_ms()`'s true resolution on hardware.** If the millisecond field is
   quantised to a firmware tick rather than being genuinely 1 ms, timing
   granularity is coarser than it appears — still well inside the Perfect window,
   but it should be measured rather than assumed.
4. **An SDK inconsistency around the speaker.** Gabbro declares the real speaker
   functions (`gabbro/pebble.h:8585`) but does **not** get the `PBL_SPEAKER`
   build define — only emery and flint do
   (`pebble_sdk_platform.py:151,190`). On basalt/aplite/chalk/diorite the calls
   are macros expanding to `(0)`. The correct guard is therefore
   `#if PBL_API_EXISTS(speaker_stream_open)` (`pebble_sdk_version.h:566`), not
   `#ifdef PBL_SPEAKER`.
5. **Emulator audio fidelity.** The emulator passes `-audio driver=coreaudio` on
   macOS but `driver=none` under `--vnc`
   (`pebble_tool/sdk/emulator.py:338-343`), so audio must be checked in a
   non-VNC run — and even then treat timbre, volume and buffer behaviour as
   hardware properties, not emulator ones.
6. **Haptics cannot be verified on the emulator at all** — there is no motor. The
   code paths and the rate-limit were exercised, but how the pulses actually
   *feel* is a hardware question. Note the SDK behaviour that shapes the design:
   a vibration issued while another is ongoing is *dropped, not queued*, so a
   naive one-pulse-per-hit would silently lose most buzzes on a dense chart.
7. **How the synthesised drums actually sound.** The emulator routes audio
   through coreaudio, but timbre and volume on a small watch speaker are a
   hardware property. If the "ka" noise burst does not survive the speaker's
   roll-off, raise `RB_KA_F1` and lean on the pitch difference from the "don"
   rather than on the noise.

## Emulator quirks worth knowing

- **`time_ms()`'s millisecond field is unusable on the emulator.** The seconds
  field tracks real time exactly (verified against `time()` and host log
  timestamps), but the ms part — documented as "milliseconds since the last
  second" — advances only ~150–190 per real second while still wrapping at 1000.
  Deriving the clock from `seconds*1000 + ms` therefore produced a clock that
  crawled at ~5 ms per frame and then **lurched 1010 ms roughly once a second**,
  teleporting every note half a screen. That is why the game was unhittable and
  looked random. `clock.c` no longer uses the ms field at all — see below.
- **`persist_exists()` returns true for keys the app has never written**, and
  `persist_read_int()` then returns a stale value. See the note in `CLAUDE.md`;
  `save_load()` writes every key explicitly on first run because of it.
- A `sleep` in a capture script is not a reliable way to reach a given moment in
  the song. Use `RB_DEBUG_FREEZE_AT_MS`.

## Status

Complete and playable: lanes, the deterministic song clock, scrolling notes,
button input, judgment, score and combo, on-screen hit feedback, haptics, the
PCM drum synth, backing music streamed from a resource, a chart generated from
that music's own rhythm, and the title / pause / results screens with high-score
persistence.

Possible next steps: a shorter excerpt or synthesised music so the build fits the
app-store resource limit; multiple songs (the loader hook and binary format are
already in `chart.h`); a difficulty selector driven by the generator's drop
quantile; and an input-latency calibration screen if hardware testing shows a
systematic offset.
