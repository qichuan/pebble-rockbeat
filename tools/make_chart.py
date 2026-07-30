#!/usr/bin/env python3
"""Build the Rockbeat chart and watch-playable PCM from the bundled MIDI file.

    python3 tools/make_chart.py
    python3 tools/make_chart.py path/to/song.mid

Pebble's Speaker API consumes PCM, not MIDI.  This script is intentionally the
only conversion step: it reads MIDI timing (including tempo changes), creates
the chart from the drum pattern, and renders a compact 16 kHz PCM backing track
that audio.c can stream straight from the watch resource.

It uses only the Python standard library.  In particular, no audio decoder,
soundfont, or onset detector is involved, so generated notes stay aligned with
the musical grid instead of merely approximating it from an MP3 waveform.
"""

from __future__ import annotations

from array import array
from collections import defaultdict
from dataclasses import dataclass
import math
from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MIDI = ROOT / "watch/resources/data/Never-Gonna-Give-You-Up.mid"
CHART_C = ROOT / "watch/src/c/chart.c"
PCM = ROOT / "watch/resources/data/music.pcm"

TICKS_PER_BEAT_REQUIRED = 384
START_BAR = 12                 # Skip the count-in and begin on a downbeat.
BARS = 28                      # ~57 seconds at this section's 118 BPM.
BEATS_PER_BAR = 4
START_TICK = START_BAR * BEATS_PER_BAR * TICKS_PER_BEAT_REQUIRED
END_TICK = START_TICK + BARS * BEATS_PER_BAR * TICKS_PER_BEAT_REQUIRED
LEAD_MS = 2000
TAIL_MS = 2500
MUSIC_RATE = 16000

# A sixteenth is used only to collect simultaneous MIDI events.  The actual
# event time is kept, then spacing rules make the resulting chart playable.
SLOT_TICKS = TICKS_PER_BEAT_REQUIRED // 4
SAME_LANE_MIN_MS = 333          # > 2 * RB_MISS_MS
# Must sit BELOW an eighth note (254ms at 118 BPM) or offbeats are structurally
# impossible: with 300 here, accepting every beat forbade everything between
# them, and the chart collapsed to a rigid kick/snare alternation on two lanes
# with the hi-hat lane completely unused.
GLOBAL_MIN_MS = 240

# Density ceiling, notes per second. Spacing rules alone would allow ~6/s here,
# which is the "too fast to hit anything" failure again. Because candidates are
# taken in weight order, this budget keeps the beats and spends whatever is left
# on the strongest offbeats, rather than thinning uniformly.
NOTES_PER_SEC = 2.6

LANE_NAMES = ("RB_LANE_BOT", "RB_LANE_MID", "RB_LANE_TOP")


@dataclass(frozen=True)
class MidiNote:
    start: int
    end: int
    channel: int
    pitch: int
    velocity: int


def read_vlq(data: bytes, pos: int) -> tuple[int, int]:
    value = 0
    while True:
        if pos >= len(data):
            raise ValueError("truncated MIDI variable-length quantity")
        byte = data[pos]
        pos += 1
        value = (value << 7) | (byte & 0x7F)
        if not byte & 0x80:
            return value, pos


def parse_midi(path: Path) -> tuple[int, list[tuple[int, int]], list[MidiNote]]:
    data = path.read_bytes()
    if data[:4] != b"MThd" or len(data) < 14:
        raise ValueError(f"{path} is not a Standard MIDI file")
    header_len = struct.unpack(">I", data[4:8])[0]
    fmt, tracks, division = struct.unpack(">HHH", data[8:14])
    if fmt not in (0, 1) or division & 0x8000:
        raise ValueError("only metrical format-0/1 MIDI files are supported")
    if division != TICKS_PER_BEAT_REQUIRED:
        raise ValueError(f"expected {TICKS_PER_BEAT_REQUIRED} ticks/beat, got {division}")

    pos = 8 + header_len
    tempos = [(0, 500000)]
    notes: list[MidiNote] = []
    for _ in range(tracks):
        if data[pos:pos + 4] != b"MTrk":
            raise ValueError("missing MIDI track chunk")
        size = struct.unpack(">I", data[pos + 4:pos + 8])[0]
        track = data[pos + 8:pos + 8 + size]
        pos += 8 + size
        tick = 0
        cursor = 0
        running = None
        active: dict[tuple[int, int], list[tuple[int, int]]] = defaultdict(list)
        while cursor < len(track):
            delta, cursor = read_vlq(track, cursor)
            tick += delta
            status = track[cursor]
            if status < 0x80:
                if running is None:
                    raise ValueError("running status without a prior MIDI status")
                status = running
            else:
                cursor += 1
                if status < 0xF0:
                    running = status
            if status == 0xFF:
                kind = track[cursor]
                cursor += 1
                length, cursor = read_vlq(track, cursor)
                payload = track[cursor:cursor + length]
                cursor += length
                if kind == 0x51 and len(payload) == 3:
                    tempos.append((tick, int.from_bytes(payload, "big")))
                continue
            if status in (0xF0, 0xF7):
                length, cursor = read_vlq(track, cursor)
                cursor += length
                continue
            kind, channel = status & 0xF0, status & 0x0F
            length = 1 if kind in (0xC0, 0xD0) else 2
            payload = track[cursor:cursor + length]
            cursor += length
            if kind not in (0x80, 0x90) or len(payload) != 2:
                continue
            pitch, velocity = payload
            key = (channel, pitch)
            if kind == 0x90 and velocity:
                active[key].append((tick, velocity))
            elif active[key]:
                start, start_velocity = active[key].pop(0)
                notes.append(MidiNote(start, max(start + 1, tick), channel, pitch, start_velocity))
        # A malformed file with a hanging note should still be audible briefly.
        for (channel, pitch), values in active.items():
            notes.extend(MidiNote(start, start + division, channel, pitch, velocity)
                         for start, velocity in values)
    tempos.sort()
    compact_tempos: list[tuple[int, int]] = []
    for tick, tempo in tempos:
        if compact_tempos and compact_tempos[-1][0] == tick:
            compact_tempos[-1] = (tick, tempo)
        else:
            compact_tempos.append((tick, tempo))
    return division, compact_tempos, sorted(notes, key=lambda note: note.start)


def make_tick_to_ms(tempos: list[tuple[int, int]], ticks_per_beat: int):
    anchors: list[tuple[int, float, int]] = []
    elapsed = 0.0
    previous_tick, previous_tempo = tempos[0]
    anchors.append((previous_tick, elapsed, previous_tempo))
    for tick, tempo in tempos[1:]:
        elapsed += (tick - previous_tick) * previous_tempo / ticks_per_beat / 1000.0
        anchors.append((tick, elapsed, tempo))
        previous_tick, previous_tempo = tick, tempo

    def tick_to_ms(tick: int) -> float:
        anchor = anchors[0]
        for candidate in anchors:
            if candidate[0] > tick:
                break
            anchor = candidate
        anchor_tick, anchor_ms, tempo = anchor
        return anchor_ms + (tick - anchor_tick) * tempo / ticks_per_beat / 1000.0
    return tick_to_ms


def drum_lane(pitch: int) -> int:
    if pitch <= 36:             # kick / low tom
        return 0
    if pitch <= 41:             # snare / clap / mid tom
        return 1
    return 2                    # hi-hat, cymbal, high percussion


def drum_weight(note: MidiNote) -> int:
    # Kick and snare establish the pulse; hats fill it in only if there is room.
    role_bonus = 48 if note.pitch in (35, 36, 38, 39, 40) else 14
    return note.velocity + role_bonus


def beat_bonus(tick_offset: int) -> int:
    """Reward notes that land where a listener feels the pulse.

    Without this the chart inverts itself.  Hi-hats occur on every subdivision
    and outnumber kick and snare several times over, so a selector that walks
    the song in time order takes a hat on the "and", and the kick 300ms later
    then fails the spacing rule and is dropped.  Measured on the first attempt:
    only 19 of 117 notes fell on the four beats, 98 fell between them, and the
    lane split was 101 hat / 11 snare / 5 kick.  The chart was almost pure
    filler with the backbone removed.
    """
    beat = TICKS_PER_BEAT_REQUIRED
    bar = beat * BEATS_PER_BAR
    if tick_offset % bar == 0:
        return 90                       # bar downbeat -- never drop these
    if tick_offset % beat == 0:
        return 70                       # on the beat
    if tick_offset % (beat // 2) == 0:
        return 26                       # eighth -- the "and", worth keeping
    return 0                            # sixteenth filler


def build_chart(notes: list[MidiNote], tick_to_ms) -> list[tuple[int, int, int]]:
    drums = [note for note in notes if note.channel == 9 and START_TICK <= note.start < END_TICK]
    source = drums or [note for note in notes if START_TICK <= note.start < END_TICK]
    slots: dict[int, list[MidiNote]] = defaultdict(list)
    for note in source:
        slots[round((note.start - START_TICK) / SLOT_TICKS)].append(note)

    candidates: list[tuple[int, int, int, int]] = []
    for slot in sorted(slots):
        group = slots[slot]
        # One intent per rhythmic subdivision.  This prevents simultaneous drum
        # layers (kick + hat) becoming impossible two-button chords.
        note = max(group, key=drum_weight)
        lane = drum_lane(note.pitch) if note.channel == 9 else min(2, max(0, (note.pitch - 36) // 18))
        time_ms = LEAD_MS + int(round(tick_to_ms(note.start) - tick_to_ms(START_TICK)))
        offset = note.start - START_TICK
        downbeat = offset % (TICKS_PER_BEAT_REQUIRED * BEATS_PER_BAR) == 0
        big = int(downbeat or note.velocity >= 116)
        candidates.append((time_ms, lane, big, drum_weight(note) + beat_bonus(offset)))

    # Select by musical importance, NOT in time order.
    #
    # Spacing rules mean accepting one note forbids others nearby, so whoever is
    # considered first wins. Walking the song chronologically hands that priority
    # to whatever happens to come first -- usually a hi-hat -- and the kick or
    # snare right after it gets refused. Sorting by weight first means the pulse
    # is laid down before filler is allowed to compete for the same space.
    #
    # The preferred lane is honoured strictly rather than falling back to a
    # neighbour: the lane mapping is the thing that makes the chart readable as
    # the drum part, and a kick relocated to the hi-hat lane to dodge a spacing
    # rule is worse than no note at all.
    span_ms = max(c[0] for c in candidates) - min(c[0] for c in candidates)
    budget = max(1, int(NOTES_PER_SEC * span_ms / 1000.0))

    chosen: list[tuple[int, int, int]] = []
    for time_ms, lane, big, _weight in sorted(candidates, key=lambda c: -c[3]):
        if len(chosen) >= budget:
            break
        if any(abs(time_ms - t) < GLOBAL_MIN_MS for t, _l, _b in chosen):
            continue
        if any(abs(time_ms - t) < SAME_LANE_MIN_MS for t, l, _b in chosen if l == lane):
            continue
        chosen.append((time_ms, lane, big))

    chart = sorted(chosen)
    if not chart:
        raise ValueError("no playable notes found in selected MIDI section")
    return chart


def midi_hz(pitch: int) -> float:
    return 440.0 * 2.0 ** ((pitch - 69) / 12.0)


def render_pcm(notes: list[MidiNote], tick_to_ms) -> float:
    """Render a deliberately small General-MIDI-ish backing track to signed PCM."""
    start_ms, end_ms = tick_to_ms(START_TICK), tick_to_ms(END_TICK)
    samples = int(round((end_ms - start_ms) * MUSIC_RATE / 1000.0))
    mix = array("h", [0]) * samples
    for note in notes:
        if not START_TICK <= note.start < END_TICK:
            continue
        start = int(round((tick_to_ms(note.start) - start_ms) * MUSIC_RATE / 1000.0))
        duration_ms = tick_to_ms(note.end) - tick_to_ms(note.start)
        if note.channel == 9:
            length = min(int(MUSIC_RATE * 0.16), max(250, int(duration_ms * MUSIC_RATE / 1000.0)))
        else:
            length = min(int(MUSIC_RATE * 0.34), max(500, int(duration_ms * MUSIC_RATE / 1000.0)))
        length = min(length, samples - start)
        if length <= 0:
            continue
        amplitude = 10 + note.velocity // 7
        if note.channel == 9:
            # Deterministic noise with a quick exponential fade reads well on
            # the tiny speaker and keeps percussion recognisable.
            state = (note.start * 1103515245 + note.pitch * 12345) & 0x7fffffff
            for offset in range(length):
                state = (state * 1103515245 + 12345) & 0x7fffffff
                envelope = (length - offset) / length
                value = int((((state >> 16) & 0xFF) - 128) * amplitude * envelope / 128)
                mix[start + offset] += value
        else:
            phase_step = 2.0 * math.pi * midi_hz(note.pitch) / MUSIC_RATE
            for offset in range(length):
                envelope = 1.0 - offset / length
                value = int(math.sin(offset * phase_step) * amplitude * envelope)
                mix[start + offset] += value
    output = bytearray(samples)
    for index, value in enumerate(mix):
        output[index] = (max(-127, min(127, value // 2)) + 256) & 0xFF
    PCM.write_bytes(output)
    return samples / MUSIC_RATE


def write_chart(chart: list[tuple[int, int, int]], duration_s: float, tick_to_ms) -> None:
    rows = "\n".join(
        f"  {{ {time_ms:6d}, {LANE_NAMES[lane]:12s}, "
        f"{'RB_NOTE_BIG   ' if big else 'RB_NOTE_NORMAL'} }},"
        for time_ms, lane, big in chart)
    bpm = round(60000 / (tick_to_ms(START_TICK + TICKS_PER_BEAT_REQUIRED)
                         - tick_to_ms(START_TICK)))
    end_ms = LEAD_MS + int(round(duration_s * 1000)) + TAIL_MS
    CHART_C.write_text(f'''#include "chart.h"

// GENERATED FILE -- do not hand-edit. Regenerate with tools/make_chart.py.
//
// "Never Gonna Give You Up" is charted directly from MIDI note-on events and
// tempo messages. The chart uses percussion when present: kick/low drums map
// to BOTTOM, snare/clap to MIDDLE, and hats/cymbals to TOP. A MIDI sixteenth
// collects simultaneous layers, then spacing rules retain a playable rhythm.
// The same MIDI source is rendered to watch/resources/data/music.pcm, so the
// audio and notes use one tempo map and cannot drift.
//
// {len(chart)} notes, {len(chart) / duration_s:.2f}/s; {duration_s:.1f}s.

static const ChartNote s_demo_notes[] = {{
{rows}
}};

static const Chart s_demo_chart = {{
  .title = "Never Gonna Give You Up",
  .notes = s_demo_notes,
  .note_count = (uint16_t)(sizeof(s_demo_notes) / sizeof(s_demo_notes[0])),
  .bpm = {bpm},
  .lead_in_ms = {LEAD_MS},
  .end_ms = {end_ms},
}};

const Chart *chart_get_builtin(void) {{ return &s_demo_chart; }}

bool chart_load_from_resource(uint32_t resource_id, Chart *out_chart) {{
  (void)resource_id;
  (void)out_chart;
  return false;
}}
''')


def main() -> None:
    if len(sys.argv) > 2:
        sys.exit(__doc__)
    midi = Path(sys.argv[1]) if len(sys.argv) == 2 else DEFAULT_MIDI
    ticks_per_beat, tempos, notes = parse_midi(midi)
    tick_to_ms = make_tick_to_ms(tempos, ticks_per_beat)
    chart = build_chart(notes, tick_to_ms)
    duration_s = render_pcm(notes, tick_to_ms)
    write_chart(chart, duration_s, tick_to_ms)
    print(f"{midi.name}: {len(notes)} MIDI notes, {len(chart)} chart notes")
    print(f"wrote {CHART_C} and {PCM} ({PCM.stat().st_size / 1024:.1f} KB, {duration_s:.1f}s)")


if __name__ == "__main__":
    main()
