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
./tools/run_tests.sh      # expect: OK: 1545 checks passed
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
      clock.{c,h}         the only caller of time_ms(); monotonic ms accumulator
      chart.{c,h}         note-chart format, the demo song, forward-compat loader
      game.{c,h}          song clock, judgment, score, combo -- PEBBLE-FREE
      render.{c,h}        all drawing; the only file with graphics_* calls
      input.{c,h}         raw-click handlers -> timestamped hit events
      feedback.{c,h}      haptics policy + hit-flash state
      audio.{c,h}         PCM voice pool, mixer and pump; the only speaker caller
      save.{c,h}          high score and toggles; the only persist_* caller
```

## Controls

| Screen | UP | SELECT | DOWN | BACK |
|---|---|---|---|---|
| Title | toggle sound | start the song | toggle haptics | exit the app |
| Playing | TOP lane | MIDDLE lane | BOTTOM lane | pause |
| Paused | restart | resume | quit to title | quit to title |
| Results | to title | to title | to title | to title |

Sound and haptics settings persist, as do the high score and best combo.

## The demo song

"Granite Groove" — 120 BPM, 4/4, 60 notes over 16 bars, about 35 seconds.
Bars 1-4 are quarter notes that teach the lane-to-button mapping, 5-8 introduce
eighths, 9-12 add syncopation and big notes on downbeats, and 13-16 are a dense
finale closing on two big hits.

Charts are `{ hit_time_ms, lane, type }` arrays sorted ascending by time. The
loader is written so a chart can later come from a resource file instead of being
compiled in: `chart_load_from_resource()` is a documented stub returning `false`,
and `main.c` already calls it first and falls back to the built-in chart. The
intended binary layout is documented at the top of `chart.h`.

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

- `speaker_stream_open(SpeakerPcmFormat_16kHz_8bit, 70)` **returns true** on the
  emery emulator, with `speaker_is_muted()` and `quiet_time_is_active()` both
  false. The real audio path runs; it is not silently falling back to the stub.
- **No backpressure at all.** Over a full song, `speaker_stream_write` accepted
  every byte requested — zero short writes. So a 40 ms lead (`RB_AUDIO_LEAD_MS`)
  sits comfortably inside the firmware buffer, and the drop-don't-retry path,
  while still correct to keep, never triggered.
- Frame pacing held at 33–39 ms with audio running, i.e. the ~30 fps target is
  met and the synth is not starving the frame loop.

**Could not be verified, and how each is handled:**

1. **The PCM stream's buffer *depth*.** Knowing that 40 ms fits does not reveal
   the ceiling, and the SDK documents no size. Still handled by design — the pump
   never retries and never blocks, and a short write drops the remainder rather
   than queueing stale audio. Raising `RB_AUDIO_LEAD_MS` until short writes
   appear would find the limit if it ever matters.
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

- **The emulator's clock runs roughly twice real time.** Logging showed the song
  clock advancing ~2000 ms per ~1000 ms of host wall time, with occasional 0 ms
  and 1000 ms frame deltas (the latter being `RB_CLOCK_MAX_STEP_MS` saturating).
  This does not affect correctness — everything drawn and everything scored comes
  from that same clock — but it means a `sleep` in a capture script is not a
  reliable way to reach a given moment in the song. Use `RB_DEBUG_FREEZE_AT_MS`.
- **`persist_exists()` returns true for keys the app has never written**, and
  `persist_read_int()` then returns a stale value. See the note in `CLAUDE.md`;
  `save_load()` writes every key explicitly on first run because of it.

## Status

Complete and playable: lanes, the deterministic song clock, scrolling notes,
button input, judgment, score and combo, on-screen hit feedback, haptics, the
PCM drum synth, and the title / pause / results screens with high-score
persistence.

Possible next steps: a chart loaded from a resource file (the format is already
documented in `chart.h` and the loader hook is in place), more songs, a
difficulty selector, and an input-latency calibration screen if hardware testing
shows a systematic offset.
