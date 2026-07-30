#!/usr/bin/env python3
"""Build the Rockbeat chart and the watch's note-sequencer music from one MIDI.

    python3 tools/make_chart.py
    python3 tools/make_chart.py path/to/song.mid

Writes two generated files:

    watch/src/c/chart.c   the playable note chart (2 lanes)
    watch/src/c/music.c   SpeakerNote tracks for speaker_play_tracks()

Pebble has no MIDI file parser, but it DOES have a note sequencer:
speaker_play_notes()/speaker_play_tracks() take arrays of {midi_note, waveform,
duration_ms, velocity}.  So the MIDI is parsed here, at build time, and emitted
as those arrays -- the watch plays notes, not a recording.

That is worth ~130x: the previous build pre-rendered this same section to 16 kHz
PCM and shipped 911,160 bytes of it.  The note tables are a few KB.

Limits measured on the emulator (see music.h for the full set); two of them are
sharp edges:
  * SPEAKER_MAX_NOTES (256) is a PER-TRACK cap, and exceeding it FAULTS the app
    rather than returning false, so chunks are asserted against it here.
  * SPEAKER_MAX_TRACKS is 4, but this section peaks at 14 simultaneous MIDI
    notes -- hence the voice reduction below, which is lossy by necessity.

Standard library only: no numpy, no ffmpeg, no soundfont.
"""

from __future__ import annotations


from collections import Counter, defaultdict
from dataclasses import dataclass

from pathlib import Path
import struct
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MIDI = ROOT / "watch/resources/data/Never-Gonna-Give-You-Up.mid"
CHART_C = ROOT / "watch/src/c/chart.c"
MUSIC_C = ROOT / "watch/src/c/music.c"

TICKS_PER_BEAT_REQUIRED = 384
START_BAR = 12                 # Skip the count-in and begin on a downbeat.
BARS = 28                      # ~57 seconds at this section's 118 BPM.
BEATS_PER_BAR = 4
START_TICK = START_BAR * BEATS_PER_BAR * TICKS_PER_BEAT_REQUIRED
END_TICK = START_TICK + BARS * BEATS_PER_BAR * TICKS_PER_BEAT_REQUIRED
LEAD_MS = 2000
TAIL_MS = 2500

# A sixteenth is used only to collect simultaneous MIDI events.  The actual
# event time is kept, then spacing rules make the resulting chart playable.
SLOT_TICKS = TICKS_PER_BEAT_REQUIRED // 4
SAME_LANE_MIN_MS = 333          # > 2 * RB_MISS_MS
# Must sit BELOW an eighth note (254ms at 118 BPM) or offbeats are structurally
# impossible: with 300 here, accepting every beat forbade everything between
# them, and the chart collapsed to a rigid kick/snare alternation on two lanes
# with the hi-hat lane completely unused.
GLOBAL_MIN_MS = 240

# Density ceiling, notes per second.  It is a safety ceiling for other songs,
# NOT the thing that sets this chart's density -- at two lanes and 118 BPM the
# spacing rules bind first and this value does not bind at all.
#
# Worth understanding before tuning it, because it looks like the density knob
# and is not.  A beat here is 508ms, so an eighth offbeat sits 254ms from the
# beats either side of it, which is inside SAME_LANE_MIN_MS (333) and therefore
# illegal in its own lane.  With three lanes an offbeat could take a third lane;
# with two, both lanes are already carrying beat notes, so it is illegal in both.
# Once all 112 beats of the section are placed nothing else can fit ANYWHERE:
# sweeping this from 2.2 to 3.4 and the beat bonus from 70 to 30 produced exactly
# 112 notes every time.
#
# The consequence is that the two-lane chart is precisely the beat grid -- fully
# alternating and easy, but with no syncopation available. Getting offbeats back
# needs either a third lane or a smaller RB_MISS_MS (which sets the 333 floor).
NOTES_PER_SEC = 2.2

# Lane 0 is the LOWER of the two lanes on screen.  Only the top two lanes are
# used now, so lane 0 = MIDDLE (SELECT) and lane 1 = TOP (UP); the DOWN button
# is not a gameplay button any more.  Order matters -- it must stay
# bottom-most-first so the vertical position keeps matching the physical button.
LANE_NAMES = ("RB_LANE_MID", "RB_LANE_TOP")

# ---------------------------------------------------------------------------
# Music -- speaker_play_tracks() parameters.
#
# Every constant here was measured on the emery emulator by tools/spike (since
# removed); the SDK header documents none of the behaviour except the two caps.
# ---------------------------------------------------------------------------

TRACKS = 4                      # SPEAKER_MAX_TRACKS
MAX_NOTES_PER_TRACK = 256       # SPEAKER_MAX_NOTES -- exceeding this FAULTS
CHUNK_BARS = 4                  # ~8.1s at 118 BPM; keeps each track far under the cap

WAVE_SINE, WAVE_SQUARE, WAVE_TRIANGLE, WAVE_SAWTOOTH = 0, 1, 2, 3

# Which register each melodic track takes.  The section peaks at 14 simultaneous
# notes against 4 mono tracks, so this reduction drops notes by design; splitting
# by register keeps the bass line and the top line intact, which is what carries
# the tune, and sacrifices inner harmony where it collides.
TRACK_BASS, TRACK_MID, TRACK_LEAD, TRACK_PERC = 0, 1, 2, 3
REGISTER_SPLITS = (48, 70)      # < 48 bass, < 70 mid, else lead

TRACK_WAVE = {
    TRACK_BASS: WAVE_TRIANGLE,  # rounder than square; the tiny speaker has no
                                # low end, so a bright bass reads as a buzz
    TRACK_MID: WAVE_SQUARE,
    TRACK_LEAD: WAVE_SINE,
}

# Percussion is played by pitch-shifting one short noise burst rather than by
# sounding a pitch, because the four waveforms have no noise between them and a
# "drum" built from a sine is just a low blip.  SPEAKER_MAX_SAMPLE_BYTES_TOTAL
# is 16KB across all tracks; this uses ~1.5KB of it.
DRUM_SAMPLE_MS = 95
DRUM_SAMPLE_RATE = 16000
DRUM_SAMPLE_BASE_NOTE = 72      # shifting DOWN stretches the burst, which is
                                # exactly the kick/hat relationship

# Where each drum lands on that sample's keyboard.  Lower = longer and duller.
DRUM_PITCH = {
    35: 46, 36: 46,             # kick
    38: 62, 39: 64, 40: 62,     # snare / clap
    41: 50, 43: 52, 45: 55, 47: 58, 48: 60, 50: 62,   # toms
    42: 84, 44: 82, 46: 79,     # hats
    49: 74, 51: 78, 52: 74, 53: 78, 55: 74, 57: 74, 59: 78,   # cymbals
}
DRUM_MS = {"kick": 150, "snare": 130, "hat": 70, "cymbal": 260}


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
    """Kick on the lower lane, everything else on the upper one.

    This is the backbeat split, and with two lanes it is the only one that
    balances.  Cutting at 42 instead (membranes vs metal, the intuitive
    "drums and cymbals" reading) puts kick AND snare together, and since those
    two alternate on every beat they saturate that lane by themselves: measured
    112 notes on the lower lane against 12 on the upper.  Cutting at 38 puts the
    kick/snare alternation ACROSS the lanes, which is both a 56/56 balance and
    100% hand alternation -- the pattern a drummer actually plays.
    """
    return 0 if pitch < 38 else 1


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
        lane = drum_lane(note.pitch) if note.channel == 9 else int(note.pitch >= 60)
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


def drum_ms(pitch: int) -> int:
    if pitch < 38:
        return DRUM_MS["kick"]
    if pitch < 42:
        return DRUM_MS["snare"]
    if pitch in (49, 51, 52, 53, 55, 57, 59):
        return DRUM_MS["cymbal"]
    return DRUM_MS["hat"]


def render_drum_sample() -> bytes:
    """One decaying noise burst, pitch-shifted by the sequencer into a whole kit.

    Deterministic LCG rather than `random`, so regenerating the project byte-for
    byte does not depend on Python's seeding.
    """
    length = DRUM_SAMPLE_MS * DRUM_SAMPLE_RATE // 1000
    out = bytearray(length)
    state = 0x13579BDF
    for i in range(length):
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        # Fast exponential-ish decay: sharp attack, short tail, which survives a
        # watch speaker better than a linear fade.
        envelope = (1.0 - i / length) ** 3
        value = int((((state >> 16) & 0xFF) - 128) * envelope)
        out[i] = (max(-127, min(127, value)) + 256) & 0xFF
    return bytes(out)


def allocate_tracks(notes: list[MidiNote], tick_to_ms) -> list[list[tuple[int, int, int, int]]]:
    """Reduce polyphonic MIDI to TRACKS monophonic voices.

    Returns, per track, a list of (start_ms, end_ms, midi_note, velocity) with
    no overlaps -- which is what a SpeakerTrack is: one note at a time.

    Notes are considered loudest-first within each register so that when two
    collide the quieter one is the one dropped.  A note that would overlap an
    already-placed note in its own track is discarded rather than bumped to a
    neighbouring track: moving a bass note into the lead voice would be audible
    as a wrong note, which is worse than a thinner texture.
    """
    lanes: list[list[tuple[int, int, int, int]]] = [[] for _ in range(TRACKS)]

    def register(pitch: int) -> int:
        if pitch < REGISTER_SPLITS[0]:
            return TRACK_BASS
        return TRACK_MID if pitch < REGISTER_SPLITS[1] else TRACK_LEAD

    section = [n for n in notes if START_TICK <= n.start < END_TICK]
    origin = tick_to_ms(START_TICK)
    ordered = sorted(section, key=lambda n: (n.start, -n.velocity))

    for note in ordered:
        start = int(round(tick_to_ms(note.start) - origin))
        if note.channel == 9:
            track = TRACK_PERC
            pitch = DRUM_PITCH.get(note.pitch, 70)
            end = start + drum_ms(note.pitch)
        else:
            track = register(note.pitch)
            pitch = note.pitch
            end = start + max(60, int(round(tick_to_ms(note.end) - tick_to_ms(note.start))))
        if end <= start:
            continue
        placed = lanes[track]
        if placed and start < placed[-1][1]:
            # Overlaps the note already sounding. Truncating the previous note
            # is preferable to dropping this one when the overlap is slight --
            # legato in the source should not silence the next note.
            prev_start, prev_end, prev_pitch, prev_vel = placed[-1]
            if start - prev_start >= 40:
                placed[-1] = (prev_start, start, prev_pitch, prev_vel)
            else:
                continue
        placed.append((start, end, pitch, note.velocity))

    return lanes


def build_music(notes: list[MidiNote], tick_to_ms):
    """Turn the allocated voices into per-chunk SpeakerNote sequences.

    Chunks exist for two reasons.  The hard one is SPEAKER_MAX_NOTES: a track
    may not exceed 256 notes, and going over faults the app instead of failing.
    The useful one is that chaining chunks from the finish callback is the only
    place the music can be re-anchored to the game clock, so a chunk boundary is
    a resync point.

    Every track in a chunk is padded to the SAME duration.  Without that a short
    track ends early and the next chunk cannot start until the longest finishes,
    so the voices would walk apart from each other a little more each chunk.
    """
    lanes = allocate_tracks(notes, tick_to_ms)
    origin = tick_to_ms(START_TICK)
    total_ms = int(round(tick_to_ms(END_TICK) - origin))
    chunk_ticks = CHUNK_BARS * BEATS_PER_BAR * TICKS_PER_BEAT_REQUIRED

    bounds = []
    tick = START_TICK
    while tick < END_TICK:
        nxt = min(tick + chunk_ticks, END_TICK)
        bounds.append((int(round(tick_to_ms(tick) - origin)),
                       int(round(tick_to_ms(nxt) - origin))))
        tick = nxt

    chunks = []
    for chunk_start, chunk_end in bounds:
        tracks = []
        for track_index, placed in enumerate(lanes):
            seq: list[tuple[int, int, int, int]] = []   # note, wave, ms, velocity
            cursor = chunk_start
            wave = TRACK_WAVE.get(track_index, WAVE_SAWTOOTH)
            for start, end, pitch, velocity in placed:
                if start >= chunk_end or end <= chunk_start:
                    continue
                start = max(start, chunk_start)
                end = min(end, chunk_end)
                if end - start < 15:
                    continue
                if start > cursor:
                    seq.append((0, wave, start - cursor, 0))   # rest
                seq.append((pitch, wave, end - start, min(127, velocity)))
                cursor = end
            if cursor < chunk_end:
                seq.append((0, wave, chunk_end - cursor, 0))
            if len(seq) > MAX_NOTES_PER_TRACK:
                raise ValueError(
                    f"track {track_index} needs {len(seq)} notes in one chunk, "
                    f"over the {MAX_NOTES_PER_TRACK} cap -- lower CHUNK_BARS")
            tracks.append(seq)
        chunks.append((chunk_start, chunk_end - chunk_start, tracks))

    return chunks, total_ms


def write_music(chunks, total_ms: int, sample: bytes) -> tuple[int, int]:
    parts = ['#include "music.h"\n',
             "// GENERATED FILE -- do not hand-edit. Regenerate with tools/make_chart.py.\n"
             "//\n"
             "// The music is played by the watch's note sequencer, not streamed as audio:\n"
             "// these are SpeakerNote arrays for speaker_play_tracks(). See music.h for the\n"
             "// measured limits that shape them, and audio.c for the chunk chaining.\n"]

    rows = ",\n".join("  " + ", ".join(f"0x{b:02x}" for b in sample[i:i + 12])
                      for i in range(0, len(sample), 12))
    parts.append(f"// One noise burst, pitch-shifted by the sequencer into the whole kit.\n"
                 f"static const uint8_t s_drum_pcm[{len(sample)}] = {{\n{rows}\n}};\n")
    parts.append(f"""static const SpeakerSample s_drum_sample = {{
  .data = s_drum_pcm,
  .num_bytes = sizeof(s_drum_pcm),
  .format = SpeakerPcmFormat_16kHz_8bit,
  .base_midi_note = {DRUM_SAMPLE_BASE_NOTE},
  .loop = false,
}};
""")

    total_notes = 0
    for chunk_index, (_start, _duration, tracks) in enumerate(chunks):
        for track_index, seq in enumerate(tracks):
            total_notes += len(seq)
            body = "\n".join(
                f"  {{ {note:3d}, {wave}, {ms:5d}, {vel:3d}, 0 }},"
                for note, wave, ms, vel in seq)
            parts.append(f"static const SpeakerNote s_c{chunk_index}_t{track_index}[] = {{\n"
                         f"{body}\n}};\n")

    entries = []
    for chunk_index, (start, duration, tracks) in enumerate(chunks):
        names = ", ".join(f"s_c{chunk_index}_t{t}" for t in range(len(tracks)))
        counts = ", ".join(str(len(seq)) for seq in tracks)
        entries.append(f"  {{ {{ {names} }},\n    {{ {counts} }}, {start}, {duration} }},")
    parts.append("static const MusicChunk s_chunks[] = {\n" + "\n".join(entries) + "\n};\n")

    parts.append(f"""uint16_t music_chunk_count(void) {{
  return (uint16_t)(sizeof(s_chunks) / sizeof(s_chunks[0]));
}}

const MusicChunk *music_chunk(uint16_t index) {{
  return (index < music_chunk_count()) ? &s_chunks[index] : NULL;
}}

const SpeakerSample *music_drum_sample(void) {{ return &s_drum_sample; }}

uint32_t music_total_ms(void) {{ return {total_ms}; }}
""")

    MUSIC_C.write_text("\n".join(parts))
    return total_notes, len(sample)


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
// tempo messages. Two lanes: drum membranes (kick, snare, toms) on MIDDLE,
// metal (hats, cymbals) on TOP. A MIDI sixteenth collects simultaneous layers,
// then spacing rules retain a playable rhythm. The same MIDI source generates
// music.c, so the notes and the music share one tempo map and cannot drift.
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
    chunks, total_ms = build_music(notes, tick_to_ms)

    # duration_ms is uint16 and the SDK caps a note at 10000ms; a chunk-long
    # rest is the longest value emitted, so checking the chunk covers every note.
    longest = max(duration for _start, duration, _tracks in chunks)
    if longest > 10000:
        raise ValueError(f"chunk of {longest}ms exceeds the 10000ms note cap -- lower CHUNK_BARS")

    duration_s = total_ms / 1000.0
    note_count, sample_bytes = write_music(chunks, total_ms, render_drum_sample())
    write_chart(chart, duration_s, tick_to_ms)

    per_lane = Counter(lane for _t, lane, _b in chart)
    print(f"{midi.name}: {len(notes)} MIDI notes in file, {duration_s:.1f}s section")
    print(f"chart: {len(chart)} notes ({len(chart) / duration_s:.2f}/s), "
          f"lanes {LANE_NAMES[0]}={per_lane[0]} {LANE_NAMES[1]}={per_lane[1]}")
    print(f"music: {len(chunks)} chunks, {note_count} SpeakerNotes, "
          f"{sample_bytes}B drum sample")
    print(f"       ~{note_count * 6 + sample_bytes} bytes of flash "
          f"(was 911160 as PCM)")
    print(f"wrote {CHART_C}\n      {MUSIC_C}")


if __name__ == "__main__":
    main()
