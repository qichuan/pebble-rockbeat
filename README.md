# Rockbeat

A Taiko-style rhythm game for the Pebble Time 2.

Horizontal lanes are stacked to match the physical buttons down the right edge
of the watch. Notes ("pebbles") scroll left to right toward a fixed
hit target beside the buttons; press that lane's button as a note arrives.

```
  TOP lane    ->  UP button       (blue)
  MIDDLE lane ->  SELECT button   (red)     <- screen centre, where SELECT is
  DOWN        ->  unused          (two lanes, not three)
  BACK        ->  exit            (never used for gameplay)
```

Two lanes rather than three: it is easier to play, and it turns the game into a
two-handed alternation. The lanes keep the exact screen positions they had as
part of a three-lane layout, because the buttons did not move -- see "Layout"
below for why that is not the same as splitting the screen in half.

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

**Waf's dependency tracking is not reliable here.** A build can report success
having rebuilt nothing, leaving a stale `.pbw` that then installs cleanly — which
looks exactly like a code change having no effect. If a change does not appear on
the emulator, run `pebble clean` before concluding anything about the code.

## Testing

The judgment windows, scoring and combo logic are unit-tested on the host, with
no emulator involved:

```bash
./tools/run_tests.sh      # expect: OK: 2651 checks passed
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
what the tick period actually is. Three details matter:

- The step is **measured, not assumed**. A timer asked for 10 ms fires at ~14 ms
  on the emulator; hardcoding 10 left the clock 15% slow. The real rate also
  moves with load — the title screen runs one timer, gameplay runs three — so it
  has to be re-measured continuously rather than calibrated once.
- The rate is measured by **counting ticks between successive increments of
  `time()`**, not by comparing elapsed time against the accumulator. This is the
  detail that took the longest to get right. `time()` has one-second resolution,
  so every scheme built on elapsed time is comparing against a value quantised to
  whole seconds, and each one hunted: a cumulative average could not follow a
  rate change and settled at 0.82×; a feedback controller overshot to 1.6×, then
  to 0.66× when detuned. Counting between increments sidesteps the problem
  entirely — each increment is an exact one-second boundary, so the count is an
  exact ticks-per-second with **no quantisation error at all**, re-measured every
  second.
- The step is held in **Q8 fixed point**. An integer step cannot express a true
  period of ~13.5 ms, and rounding it to 13 or 14 is a 4–7% rate error — a whole
  beat of drift every twenty seconds.

The step is only ever adjusted, never the counter, so the clock stays monotonic —
one that stepped backwards would drag pending notes back through their hit
windows. Measured after the rewrite: **frame deltas hold steady at 33–34 ms
against the 33 ms nominal, and dropped audio is zero.** The cost is that
timestamps quantise to `RB_CLOCK_TICK_MS` (10 ms) — a bounded, predictable error
well inside the 60 ms Perfect window.

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
      chart.{c,h}         note-chart format, the generated song, forward-compat loader
      game.{c,h}          song clock, judgment, score, combo -- PEBBLE-FREE
      render.{c,h}        all drawing; the only file with graphics_* calls
      input.{c,h}         raw-click handlers -> timestamped hit events
      feedback.{c,h}      haptics policy + hit-flash state
      audio.{c,h}         drives the note sequencer; chains music chunks
      music.{c,h}         GENERATED SpeakerNote tables + the measured limits
      save.{c,h}          high score and toggles; the only persist_* caller
    resources/data/
      Never-Gonna-Give-You-Up.mid   the source of BOTH the chart and the music
  tools/make_chart.py     regenerates chart.c AND music.c from the .mid
```

### Why the lanes are not half the screen each

The lane bands are sized against a **three**-lane split even though two lanes are
played, and `render.c` divides by `RB_LANE_SLOTS` (3) rather than by
`RB_LANE_COUNT` (2). That looks like a leftover; it is not.

The premise of the whole design is that a lane sits at the vertical position of
the button that plays it, and **the buttons do not move when the game drops a
lane**. Dividing the playfield by the lane count gives two 90 px bands centred at
y=69 and y=159 — so the SELECT lane would sit 45 px below the SELECT button, and
the game would be pointing at the wrong hardware. Keeping the 60 px bands leaves
TOP on the UP button and MIDDLE on the screen's exact vertical centre where
SELECT is, and the bottom third is simply left dark.

## Controls

| Screen | UP | SELECT | DOWN | BACK |
|---|---|---|---|---|
| Title | toggle sound | start the song | toggle haptics | exit the app |
| Playing | TOP lane | MIDDLE lane | (unused) | pause |
| Paused | restart | resume | quit to title | quit to title |
| Results | to title | to title | to title | to title |

Sound and haptics settings persist, as do the high score and best combo.

## The song: "Never Gonna Give You Up"

The chart and the music are both generated from one MIDI file,
`watch/resources/data/Never-Gonna-Give-You-Up.mid`. Nothing is hand-placed and
there is no audio recording anywhere in the project.

**56.9 seconds, 118 BPM, 89 notes (1.56/s).** A bar-aligned 28-bar section
starting at bar 12, which skips the count-in and begins on a downbeat.

### Why MIDI, and not the mp3

This project first charted an mp3 with a spectral-flux onset detector: STFT,
per-band flux, peak-picking, comb-filter tempo estimation. It worked in the sense
that it found onsets, and it still produced a chart the player described as
following no rhythm at all — because onset detection *infers* a grid that the
score already states exactly.

MIDI removes all of that inference. Note-on ticks and tempo meta-events give the
exact grid. The whole analysis pipeline was deleted; `make_chart.py` is now
**Python stdlib only** — no numpy, no ffmpeg, no soundfont, nothing to install.

### How the chart is chosen

The chart is built from **the melody notes themselves** — the same notes the
watch plays — so every note the player hits is a note they can hear. (An earlier
version charted the drum part while the music played the whole arrangement, which
was defensible then and would be incoherent now.)

**Lane follows pitch**: melody notes above the line's median go to the upper
lane, below it to the lower one. So the lane pattern is the shape of the tune,
and the mapping means something on screen rather than being arbitrary.

Notes are weighted by velocity plus a beat bonus (90 on a bar downbeat, 70 on a
beat, 26 on an eighth, 0 on a sixteenth) and taken in **weight order, not time
order**. Selecting in time order inverts a chart: spacing rules mean accepting a
note forbids its neighbours, so whoever is considered first wins, and walking the
song chronologically hands that priority to whatever happens to come first.
Weight-ordered selection lays the pulse down before filler can compete.

Three spacing rules then apply: 333 ms minimum in the same lane, 240 ms globally,
and a ceiling of 2.2 notes/sec. The 333 ms figure is not musical — it is
deliberately just above `2 * RB_MISS_MS` = 320 ms, so no two notes in one lane
can ever have overlapping judgment windows. `tools/run_tests.sh` asserts that
invariant against the shipped chart.

### Regenerating

```bash
python3 tools/make_chart.py                     # uses the bundled .mid
python3 tools/make_chart.py path/to/song.mid    # any 384-tick/beat format 0/1 SMF
```

That writes **both** `watch/src/c/chart.c` and `watch/src/c/music.c` from the
same tempo map, so the notes and the music cannot drift apart. Both are generated
— do not hand-edit either. Section and density live at the top of the script
(`START_BAR`, `BARS`, `NOTES_PER_SEC`).

Charts are `{ hit_time_ms, lane, type }` arrays sorted ascending by time. The
loader is written so a chart can later come from a resource file instead of being
compiled in: `chart_load_from_resource()` is a documented stub returning `false`,
and `main.c` already calls it first and falls back to the built-in chart. The
intended binary layout is documented at the top of `chart.h`.

## How the music plays

The watch has no MIDI file parser — but it does have a **note sequencer**.
`speaker_play_tracks()` takes arrays of `SpeakerNote {midi_note, waveform,
duration_ms, velocity}`. So the MIDI is parsed at build time and emitted as those
arrays in `music.c`. The watch plays notes; it does not play a recording.

**It plays one line: the melody.** Everything else in the arrangement is
discarded, and the chart is built from the same melody notes — so every note the
player hits is a note they can hear.

| | bytes |
|---|---|
| original build: pre-rendered 16 kHz PCM resource | 911,160 |
| four-track reduction of the full arrangement | ~13,100 |
| **melody only** | **~2,800** |

Total app resources are **4,213 bytes** against the 256 KB app-store limit. The
original PCM build was 915,373 — three and a half times over it.

### Melody extraction

The melody is found with the **skyline algorithm** — within each moment, the
highest sounding note is the melody — following the approach in
[xinyiguan/MIDI_Melody_Extraction](https://github.com/xinyiguan/MIDI_Melody_Extraction).
That script is not used directly: it depends on `mido` (this generator is
deliberately stdlib-only) and it hard-filters to MIDI channel 0, which this file
does not have — its channels are 2,3,4,7,8,9,13,15. So the algorithm is
reimplemented against our own parser, with the channel *chosen* rather than
assumed.

Channel selection scores each channel on how much it behaves like a lead line.
The important detail is that **density is a hard filter, not a scoring term**.
Scoring it alongside pitch picked the wrong channel: a 25-note high string
counter-line (0.44 notes/sec) outscored the 158-note alto sax carrying the tune,
because it sat an octave higher and was perfectly monophonic. A melody has to
have enough notes to *be* the melody, so anything under 1 note/sec is not a
candidate at all; only then does pitch-versus-polyphony decide.

For this file it selects **channel 15, GM program 65 (Alto Sax), 157 notes**.

The whole line is then transposed **up one octave as a unit**, to 415–831 Hz.
Transposing as a unit matters: per-note octave lifting (which is fine for a bass
pulse) would raise some notes and not their neighbours, breaking the contour so
the tune stops being recognisable.

### Why one line, and not the arrangement

The first attempt reduced the full arrangement to four tracks. It sounded noisy,
and three rounds of tuning reduced the noise without removing its cause:

- **Percussion was 45% of the song and every bit of it was white noise** (a 95 ms
  burst, pitch-shifted — downshifted for the kick that became a *427 ms rumble of
  stretched static*). Replacing it with a real drum hit and thinning the hats took
  it to 27.5%.
- **The mid track was a square wave sounding 84% of the time** — a continuous
  buzzsaw, not an accompaniment. Triangle helped.
- **The bass sat at 39–69 Hz**, with 84% of all pitched notes below 400 Hz. A
  watch speaker cannot move at those frequencies; driven at 39 Hz it emits only
  upper harmonics, a buzz with no pitch in it. Octave-lifting fixed the
  frequency but not the fundamental problem.

The fundamental problem is that **four fixed-amplitude waveforms cannot carry a
dense pop arrangement on a driver this small**. Simultaneous tones intermodulate
rather than summing cleanly. One clean line is something the speaker *can*
reproduce, so that is what it plays.

Three properties of the sequencer shape the output, all of which still apply:

- **There is no envelope.** A note sounds at constant amplitude for its whole
  duration, so this MIDI's 1907 ms notes were two seconds of unchanging tone — a
  drone, which reads as buzz. `MAX_SUSTAIN_MS` caps them into plucks.
- **Monophonic tracks click at note boundaries.** Consecutive notes butt together
  and the waveform steps discontinuously. `NOTE_GAP_MS` leaves 18 ms of silence
  at the end of each note, taken out of the note rather than off the next one's
  start, so the rhythm is untouched.
- **Sine, because it has no harmonics at all.** Every other available waveform is
  defined by its harmonic series, and harmonics are what a small speaker
  exaggerates. With one voice nothing needs a bright timbre to cut through.

### Everything below was measured, not documented

The SDK header documents the two cap constants and nothing else. These were found
with a throwaway spike on the emulator, and two of them are sharp edges:

- `SPEAKER_MAX_NOTES` (256) is a **per-track** cap, not a per-call budget — 4
  tracks of 128 plays fine.
- **Exceeding it faults the app**, rather than returning false. The generator
  refuses to emit an over-long chunk; there is deliberately no runtime guard,
  because by then it is too late.
- Chaining the next chunk from the finish callback costs **no audible gap**, and
  drift against the game clock over six consecutive 4-second chunks was
  +17/−5/−12/+41/−30 ms — jitter, not a rate error.
- The **first** `speaker_play_tracks()` call costs ~200 ms of startup latency
  that later calls do not. Hence `RB_MUSIC_LATENCY_MS`.
- **A PCM stream cannot coexist with the sequencer.** `speaker_stream_open()`
  returns false while tracks are playing (the music itself is unharmed — it
  finishes `Done`, not `Preempted`). This is why there are no reactive hit
  sounds: the two audio sources are mutually exclusive.

Measuring the sequencer needs one piece of care: sample it only **after the game
clock has settled**. The first attempt reported 2–8% drift and looked like a
fatal rate error, but it was measuring `clock.c` calibrating during its first
seconds. Waiting 15 s turned the same numbers into ±40 ms of jitter.

The music is released against **song time, not an AppTimer**, for the same
reason. The clock loses ~2 s during early calibration, so a real-time timer armed
for "1800 ms from now" fires while the song clock still reads ~1300 — starting
the music the better part of a second ahead of its notes, permanently.
`audio_tick()` takes elapsed song time as a parameter and `audio.c` does not
include `clock.h` at all, so the one-way dependency is structural.

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
| `RB_DEBUG_AUTOSTART` | skips the title screen and starts the song immediately — **check this is 0 before shipping** |
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
8463-8628 — including `speaker_play_notes/tracks/tone` and
`speaker_stream_open/write/close`, with the symbols genuinely defined in
`emery/lib/libpebble.a` (checked with `nm -g`);
`window_raw_click_subscribe` at 5678; `time_ms` at 8825, **not** deprecated;
`app_timer_register/reschedule/cancel` at 3016/3023/3028; the `vibes_*` family and
`VibePattern` at 8430-8459; `quiet_time_is_active` at 8713; `persist_*` at
3147-3224; and the graphics and layer calls at 4129-4265, 4978 and 5296-5427.
Emery's 200x228 / 64-colour / 128 KB figures come from
`common/tools/pebble_sdk_platform.py:133-174`.

**Measured empirically** on the emulator — the sequencer behaviour is in "How
the music plays" above; the short version is that `SPEAKER_MAX_NOTES` is a
per-track cap, exceeding it faults the app, chunk chaining is gapless, the first
call costs ~200 ms, and a PCM stream cannot coexist with tracks. Also:

- Frame pacing holds at **33–35 ms** against the 33 ms nominal with the music
  playing, across the full 57-second song with zero playback failures.

**Could not be verified, and how each is handled:**

1. **Whether the ~200 ms first-call latency is constant across hardware.** It is
   compensated by a single constant (`RB_MUSIC_LATENCY_MS`) measured on the
   emulator. If the real device differs, the whole track sits uniformly early or
   late against the notes — audible, but a one-number fix, and it cannot affect
   scoring, which never reads audio.
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
   `#if PBL_API_EXISTS(speaker_play_tracks)` (`pebble_sdk_version.h:566`), not
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
7. **How the sequencer's waveforms actually sound.** The emulator routes audio
   through coreaudio, but timbre and volume on a small watch speaker are a
   hardware property — and this build leans on it much harder than a recording
   would, since a square-wave bass and a pitch-shifted noise burst are pure
   synthesis. The four-voice reduction of a fourteen-voice arrangement is also a
   musical judgement that only listening on hardware can settle.

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
- **`pebble wipe` can leave the emulator permanently unbootable**, and the
  symptom looks like an app problem: every `install` fails with a libpebble2
  `TimeoutError`. It is not app size — an 84 KB build failed identically.
  Recover by deleting **both**
  `~/Library/Application Support/Pebble SDK/4.17/emery` and
  `$TMPDIR/pb-emulator.json` (which goes stale claiming QEMU is still running),
  then reinstall.

## Status

Complete and playable: two lanes, the deterministic song clock, scrolling notes,
button input, judgment, score and combo, on-screen hit feedback, haptics, backing
music played by the watch's note sequencer from MIDI, a chart generated from that
same MIDI, and the title / pause / results screens with high-score persistence.

At **4,213 bytes of resources** the build is now within the app-store limit, which
the previous PCM-based build (915,373 bytes) was not.

**Not verified on hardware.** Everything above was checked on the emery
emulator. Haptics in particular cannot be tested there at all — there is no
motor — and speaker timbre is a hardware property.

Known rough edges, in the order I would fix them:

1. **The two-lane chart has no syncopation** — it is exactly the beat grid, for
   the structural reason above. A three-lane "hard" mode would restore it, and
   the generator already supports it.
2. **No reactive hit sound.** Forced by the sequencer/stream exclusivity, but a
   very short `speaker_play_tone()` between chunks might fit; it would need
   measuring against the music, which currently owns the speaker outright.
3. **The bottom third of the playfield is dark and unused.** Honest, but it looks
   unfinished; the HUD could expand into it.
4. Multiple songs (the loader hook and binary format are already in `chart.h`),
   and an input-latency calibration screen if hardware testing shows a systematic
   offset.
