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
import math

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

# Sine, because it has no harmonics at all.
#
# Every other waveform available here is defined by its harmonic series, and
# harmonics are exactly what a small speaker exaggerates -- the square-wave
# accompaniment in the previous build was the second-worst noise source after
# the sub-bass. With one voice there is no need for a bright timbre to cut
# through anything, so the cleanest option is also the right one.
MELODY_WAVE = WAVE_SINE
MELODY_VELOCITY = 100

# Transpose the melody, as a whole, so its median lands at least here.
# MIDI 72 = 523Hz, comfortably inside a watch speaker's usable band; this MIDI's
# melody sits at 208-415Hz, where the driver is weak and mostly emits harmonics.
MELODY_TARGET_MEDIAN = 72

# Nothing sustains longer than this; the remainder of a longer note becomes rest.
#
# speaker_play_tracks() has no envelope -- a note plays at constant amplitude for
# its whole duration -- so a long note is a drone rather than a decaying tone,
# and a drone on a small speaker reads as buzz.
MAX_SUSTAIN_MS = 260

# Silence inserted at the end of every sounding note.
#
# Without it, consecutive notes butt directly against each other and the waveform
# steps discontinuously at the boundary -- a click per note boundary, which reads
# as a continuous crackle. Taken out of the note rather than off the next one's
# start, so the rhythm is untouched.
NOTE_GAP_MS = 18


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


def parse_midi(path: Path) -> tuple[int, list[tuple[int, int]], list[MidiNote], dict[int, int]]:
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
    programs: dict[int, int] = {}
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
            if kind == 0xC0 and channel not in programs:
                programs[channel] = payload[0]
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
    return division, compact_tempos, sorted(notes, key=lambda note: note.start), programs


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


# ---------------------------------------------------------------------------
# Melody extraction
#
# The arrangement is reduced to a single melodic line before anything else
# happens, and the game plays only that.
#
# This is the "skyline" algorithm -- within each moment, the highest sounding
# note is the melody -- following the approach in
# https://github.com/xinyiguan/MIDI_Melody_Extraction. That script is not used
# directly for two reasons: it depends on `mido`, and this generator is
# deliberately stdlib-only; and it hard-filters to MIDI channel 0, which this
# file does not have (its channels are 2,3,4,7,8,9,13,15). So the algorithm is
# reimplemented here against the parser above, with the channel chosen rather
# than assumed.
#
# Why melody-only at all: four fixed-amplitude waveforms on a watch speaker
# cannot carry a dense pop arrangement. Every previous attempt to clean it up --
# thinning percussion, octave-lifting the bass, capping sustains -- reduced the
# noise without removing its cause, which is simply too many simultaneous tones
# for the hardware. One clean line is something the speaker CAN reproduce.
# ---------------------------------------------------------------------------

# GM program families that are never the melody, whatever their pitch.
NON_MELODY_PROGRAMS = set(range(32, 40)) | set(range(88, 96))   # basses, pads


# A melody must carry at least this many notes per second to be a candidate.
MELODY_MIN_RATE = 1.0


def pick_melody_channel(notes: list[MidiNote], programs: dict[int, int], tick_to_ms) -> int:
    """Score each channel on how much it behaves like a lead line.

    Density is a HARD filter, not a scoring term. Scoring it alongside pitch
    picked the wrong channel here: a 25-note high string counter-line (0.44
    notes/sec) outscored the 158-note alto sax carrying the actual tune, because
    it sat an octave higher and was perfectly monophonic. A melody has to have
    enough notes to BE the melody, so anything under MELODY_MIN_RATE is not a
    candidate at all; only then does pitch-versus-polyphony decide.
    """
    section = [n for n in notes if START_TICK <= n.start < END_TICK and n.channel != 9]
    best, best_score = None, None
    for channel in sorted({n.channel for n in section}):
        group = [n for n in section if n.channel == channel]
        program = programs.get(channel)
        if program is not None and program in NON_MELODY_PROGRAMS:
            continue
        events = []
        for note in group:
            events += [(note.start, 1), (note.end, -1)]
        events.sort()
        depth = peak = 0
        for _tick, delta in events:
            depth += delta
            peak = max(peak, depth)
        span_ms = tick_to_ms(END_TICK) - tick_to_ms(START_TICK)
        if len(group) / (span_ms / 1000.0) < MELODY_MIN_RATE:
            continue
        pitches = sorted(n.pitch for n in group)
        median = pitches[len(pitches) // 2]
        # Among channels dense enough to be a tune, prefer the one that is high
        # and plays one note at a time. Chords and pads lose on polyphony.
        score = (median - 48) / 12.0 - (peak - 1) * 1.5
        if best_score is None or (score, len(group)) > (best_score, 0):
            best, best_score = channel, score
    if best is None:
        raise ValueError("no melodic channel found in the selected MIDI section")
    return best


def extract_melody(notes: list[MidiNote], channel: int) -> list[MidiNote]:
    """Skyline within the chosen channel, forced monophonic.

    Where notes overlap, the higher one wins and the lower is dropped rather
    than shortened -- a melody that ducks to an inner voice for 40ms reads as a
    glitch, not as counterpoint.
    """
    group = sorted((n for n in notes
                    if n.channel == channel and START_TICK <= n.start < END_TICK),
                   key=lambda n: (n.start, -n.pitch))
    melody: list[MidiNote] = []
    for note in group:
        if melody and note.start < melody[-1].end:
            if note.pitch <= melody[-1].pitch:
                continue                      # covered by a higher note
            melody[-1] = MidiNote(melody[-1].start, note.start, melody[-1].channel,
                                  melody[-1].pitch, melody[-1].velocity)
            if melody[-1].end <= melody[-1].start:
                melody.pop()
        melody.append(note)
    return melody


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


def melody_octave_shift(melody: list[MidiNote]) -> int:
    """Transpose the WHOLE line by whole octaves into the speaker's range.

    Per-note lifting (which is what PITCH_FLOOR does for accompaniment) would
    destroy a melody: raising some notes an octave and not their neighbours
    breaks the contour and the tune stops being recognisable. A constant shift
    preserves every interval exactly.
    """
    pitches = sorted(n.pitch for n in melody)
    median = pitches[len(pitches) // 2]
    shift = 0
    while median + shift < MELODY_TARGET_MEDIAN:
        shift += 12
    return shift


def build_chart(melody: list[MidiNote], tick_to_ms) -> list[tuple[int, int, int]]:
    """One chart note per melody note, subject to the playability spacing rules.

    Now that the game plays only the melody, the chart is built from the SAME
    notes -- so every note the player hits is a note they can hear. Previously
    the chart came from the drum part while the music played everything, which
    was defensible then and would be incoherent now.

    Lane follows PITCH: notes above the melody's median go to the upper lane,
    below it to the lower one. That makes the lane mapping mean something on
    screen -- the line's shape is the pattern the hands play.
    """
    origin = tick_to_ms(START_TICK)
    pitches = sorted(n.pitch for n in melody)
    split = pitches[len(pitches) // 2]

    candidates: list[tuple[int, int, int, int]] = []
    for note in melody:
        time_ms = LEAD_MS + int(round(tick_to_ms(note.start) - origin))
        offset = note.start - START_TICK
        lane = 1 if note.pitch >= split else 0
        held = tick_to_ms(note.end) - tick_to_ms(note.start)
        big = int(offset % (TICKS_PER_BEAT_REQUIRED * BEATS_PER_BAR) == 0
                  or note.velocity >= 116 or held >= 700)
        candidates.append((time_ms, lane, big, note.velocity + beat_bonus(offset)))

    if not candidates:
        raise ValueError("no melody notes in selected MIDI section")

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
        raise ValueError("no playable notes survived the spacing rules")
    return chart


def build_music(melody: list[MidiNote], shift: int, tick_to_ms):
    """One monophonic melody track, split into chunks.

    Chunks exist for two reasons. The hard one is SPEAKER_MAX_NOTES: a track may
    not exceed 256 notes, and going over faults the app instead of failing. The
    useful one is that chaining chunks from the finish callback is the only place
    the music can be re-anchored to the game clock.
    """
    origin = tick_to_ms(START_TICK)
    total_ms = int(round(tick_to_ms(END_TICK) - origin))
    chunk_ticks = CHUNK_BARS * BEATS_PER_BAR * TICKS_PER_BEAT_REQUIRED

    placed: list[tuple[int, int, int, int]] = []
    for note in melody:
        start_ms = int(round(tick_to_ms(note.start) - origin))
        held = int(round(tick_to_ms(note.end) - tick_to_ms(note.start)))
        end_ms = start_ms + max(60, min(held, MAX_SUSTAIN_MS))
        if placed and start_ms < placed[-1][1]:
            end_prev = min(placed[-1][1], start_ms)
            if end_prev <= placed[-1][0]:
                placed.pop()
            else:
                placed[-1] = (placed[-1][0], end_prev, placed[-1][2], placed[-1][3])
        placed.append((start_ms, end_ms, note.pitch + shift, note.velocity))

    bounds = []
    tick = START_TICK
    while tick < END_TICK:
        nxt = min(tick + chunk_ticks, END_TICK)
        bounds.append((int(round(tick_to_ms(tick) - origin)),
                       int(round(tick_to_ms(nxt) - origin))))
        tick = nxt

    chunks = []
    for chunk_start, chunk_end in bounds:
        seq: list[tuple[int, int, int, int]] = []
        cursor = chunk_start
        for start_ms, end_ms, pitch, velocity in placed:
            if start_ms >= chunk_end or end_ms <= chunk_start:
                continue
            start_ms = max(start_ms, chunk_start)
            end_ms = min(end_ms, chunk_end)
            if end_ms - start_ms < 25:
                continue
            if start_ms > cursor:
                seq.append((0, MELODY_WAVE, start_ms - cursor, 0))
            sounding = end_ms - start_ms
            if sounding > NOTE_GAP_MS + 25:
                sounding -= NOTE_GAP_MS
            seq.append((pitch, MELODY_WAVE, sounding, min(MELODY_VELOCITY, velocity)))
            if end_ms - start_ms > sounding:
                seq.append((0, MELODY_WAVE, (end_ms - start_ms) - sounding, 0))
            cursor = end_ms
        if cursor < chunk_end:
            seq.append((0, MELODY_WAVE, chunk_end - cursor, 0))
        if len(seq) > MAX_NOTES_PER_TRACK:
            raise ValueError(f"chunk needs {len(seq)} notes, over the "
                             f"{MAX_NOTES_PER_TRACK} cap -- lower CHUNK_BARS")
        chunks.append((chunk_start, chunk_end - chunk_start, [seq]))

    return chunks, total_ms


def write_music(chunks, total_ms: int) -> int:
    parts = ['#include "music.h"\n',
             "// GENERATED FILE -- do not hand-edit. Regenerate with tools/make_chart.py.\n"
             "//\n"
             "// A single monophonic melody line for speaker_play_tracks(). See music.h for\n"
             "// the measured sequencer limits, and audio.c for the chunk chaining.\n"]

    total_notes = 0
    for chunk_index, (_start, _duration, tracks) in enumerate(chunks):
        for track_index, seq in enumerate(tracks):
            total_notes += len(seq)
            body = "\n".join(f"  {{ {note:3d}, {wave}, {ms:5d}, {vel:3d}, 0 }},"
                              for note, wave, ms, vel in seq)
            parts.append(f"static const SpeakerNote s_c{chunk_index}_t{track_index}[] = {{\n"
                         f"{body}\n}};\n")

    entries = []
    for chunk_index, (start, duration, tracks) in enumerate(chunks):
        names = ", ".join(f"s_c{chunk_index}_t{t}" for t in range(len(tracks)))
        counts = ", ".join(str(len(seq)) for seq in tracks)
        entries.append(f"  {{ {{ {names} }}, {{ {counts} }}, {len(tracks)}, {start}, {duration} }},")
    parts.append("static const MusicChunk s_chunks[] = {\n" + "\n".join(entries) + "\n};\n")

    parts.append(f"""uint16_t music_chunk_count(void) {{
  return (uint16_t)(sizeof(s_chunks) / sizeof(s_chunks[0]));
}}

const MusicChunk *music_chunk(uint16_t index) {{
  return (index < music_chunk_count()) ? &s_chunks[index] : NULL;
}}

uint32_t music_total_ms(void) {{ return {total_ms}; }}
""")
    MUSIC_C.write_text("\n".join(parts))
    return total_notes


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
    ticks_per_beat, tempos, notes, programs = parse_midi(midi)
    tick_to_ms = make_tick_to_ms(tempos, ticks_per_beat)

    channel = pick_melody_channel(notes, programs, tick_to_ms)
    melody = extract_melody(notes, channel)
    shift = melody_octave_shift(melody)

    chart = build_chart(melody, tick_to_ms)
    chunks, total_ms = build_music(melody, shift, tick_to_ms)

    # duration_ms is uint16 and the SDK caps a note at 10000ms; a chunk-long
    # rest is the longest value emitted, so checking the chunk covers every note.
    longest = max(duration for _start, duration, _tracks in chunks)
    if longest > 10000:
        raise ValueError(f"chunk of {longest}ms exceeds the 10000ms note cap -- lower CHUNK_BARS")

    duration_s = total_ms / 1000.0
    note_count = write_music(chunks, total_ms)
    write_chart(chart, duration_s, tick_to_ms)

    per_lane = Counter(lane for _t, lane, _b in chart)
    pitches = [n.pitch + shift for n in melody]
    lo = 440 * 2 ** ((min(pitches) - 69) / 12)
    hi = 440 * 2 ** ((max(pitches) - 69) / 12)
    print(f"{midi.name}: {len(notes)} MIDI notes in file, {duration_s:.1f}s section")
    print(f"melody: channel {channel} (GM program {programs.get(channel)}), "
          f"{len(melody)} notes, transposed +{shift // 12} octave(s) "
          f"-> {lo:.0f}-{hi:.0f}Hz")
    print(f"chart:  {len(chart)} notes ({len(chart) / duration_s:.2f}/s), "
          f"lanes {LANE_NAMES[0]}={per_lane[0]} {LANE_NAMES[1]}={per_lane[1]}")
    print(f"music:  {len(chunks)} chunks, {note_count} SpeakerNotes, "
          f"~{note_count * 6} bytes of flash (was 911160 as PCM)")
    print(f"wrote {CHART_C}\n      {MUSIC_C}")


if __name__ == "__main__":
    main()
