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
DATA = ROOT / "watch/resources/data"
CHART_C = ROOT / "watch/src/c/chart.c"
MUSIC_C = ROOT / "watch/src/c/music.c"

TICKS_PER_BEAT_REQUIRED = 384
BEATS_PER_BAR = 4
LEAD_MS = 2000
TAIL_MS = 2500


@dataclass(frozen=True)
class Song:
    """One playable song: which MIDI, and which bars of it.

    The section is per song and hand-picked. It is the one thing here that
    cannot be derived: which 14 bars are the chorus is a musical judgement, and
    picking the wrong ones produces a technically valid chart of the wrong part
    of the song.

    `midi` is a melody-only file written by tools/extract_melody.py. Feeding the
    full arrangement instead produces byte-identical output -- the extraction
    below is idempotent on an already-extracted line -- but the melody file is
    the one that can be auditioned, so it is the one that is kept.
    """
    title: str          # shown on the title screen; keep it short enough to fit
    ident: str          # C identifier stem
    midi: Path
    start_bar: int
    bars: int

    @property
    def start_tick(self) -> int:
        return self.start_bar * BEATS_PER_BAR * TICKS_PER_BEAT_REQUIRED

    @property
    def end_tick(self) -> int:
        return self.start_tick + self.bars * BEATS_PER_BAR * TICKS_PER_BEAT_REQUIRED


SONGS = (
    # 118 BPM. Bar 12 skips the count-in and begins on a downbeat; 28 bars is
    # ~57s. The melody is on an exact sixteenth grid, which is what lets every
    # note be charted -- see build_chart().
    Song("Never Gonna Give You Up", "ngg", DATA / "melody.mid", 12, 28),
    # 59 BPM, so a bar is 4.1s and 14 bars is ~57s -- the same length as above
    # from a quarter of the bars. The last 14 bars are the final chorus, and the
    # section ends where the song does rather than being cut mid-phrase.
    Song("You Are Not Alone", "yana", DATA / "you-are-not-alone.mid", 58, 14),
)

# tools/extract_melody.py defaults to re-deriving the first song's melody file.
DEFAULT_MIDI = SONGS[0].midi

# ---------------------------------------------------------------------------
# The chart is EVERY melody note. One note heard, one note to hit.
#
# Nothing is selected, weighted or dropped any more. The previous generator chose
# 89 of the 157 melody notes by weight, subject to a density ceiling and a global
# spacing rule, which meant 68 notes sounded with nothing to press -- the tune
# and the chart told different stories.
#
# What makes charting all of them possible is that this melody sits on an exact
# sixteenth grid. Measured over the charted section, EVERY interval between
# consecutive notes is one of:
#
#     127/128ms  (sixteenth)   31 of them
#     254/255ms  (eighth)      82
#     >= 320ms                 43
#
# There is nothing awkward in between, so the whole problem reduces to a single
# question: what happens when two notes are only 127ms apart?
#
# They go to different lanes -- see the lane rule in build_chart(). A 127ms
# same-lane repeat is roughly eight presses a second on one button, which is not
# playable, whereas alternating hands at that rate is exactly what a Taiko-style
# game is for. Forcing that alternation also has a second effect that matters
# more: it makes 254ms the smallest possible gap BETWEEN TWO NOTES IN ONE LANE,
# and that is the number the judgment windows have to live inside.
# ---------------------------------------------------------------------------

# Two notes in one lane closer than this would have overlapping judgment windows,
# so the lane assignment guarantees it and the unit tests assert it against the
# shipped chart.
#
# It is a hard floor of 2 * RB_MISS_MS, and it is why RB_MISS_MS is 125: an
# eighth is 254ms at 118 BPM, so 2 * 125 = 250 fits underneath it with 4ms to
# spare, and 160 (the old value) would not have. Change one and you must change
# the other -- in this direction, RB_MISS_MS <= SAME_LANE_MIN_MS / 2.
SAME_LANE_MIN_MS = 250

# What makes a note "big" -- drawn larger, worth double, and given a heavier
# buzz. Bar downbeats always qualify; beyond that, a note is an accent if it is
# struck harder than the line's median by this margin, or held this long.
#
# The margin is relative for a reason: see the note in build_chart().
BIG_VELOCITY_MARGIN = 8
BIG_HELD_MS = 700

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
MAX_NOTE_MS = 10000             # SDK cap on a single note's duration_ms

# Chunks are bounded in MILLISECONDS, not bars.
#
# Bars were the unit until a second song arrived at half the tempo, where 8 bars
# is 32.5s rather than 16.3s -- and a chunk that long never reported finishing at
# all. The music stopped after the first chunk, with no error and nothing in the
# log, because the finish callback that drives the handover never came. Note
# count was not the difference: the chunk that failed held 123 notes, and one
# that works holds 159.
#
# So the ceiling is expressed in the unit the limit actually lives in. Bars are
# still the split points -- a boundary should fall on a downbeat, where a
# handover gap is least audible -- but bars are accumulated until adding another
# would cross this.
#
# What is firmly established: a 32.5s chunk NEVER reports finishing. Reproduced
# every run, and it takes the rest of the song with it, because the handover is
# driven by that callback. 16.3s chunks always complete.
#
# What is NOT established is anything finer. Boundary error at 16.3s measured
# +33/+17/+10ms on one run and +4/-189/+320ms on another with identical inputs,
# so the emulator's own variance is larger than any difference between 16s and
# 18s. Do not read a trend into those numbers, and do not tune this against
# them; the watch is the only place a real figure could come from.
#
# 16500 therefore buys margin, not precision. It admits exactly the 16.27s chunk
# that always works at both tempos -- 8 bars at 118 BPM and 4 bars at 59 BPM are
# both 16271ms -- and excludes the next bar up at either. Boundaries are not
# free (each is a handover costing a frame's gap and a driver restart), so the
# pressure is towards longer; this resists it until there is a measurement worth
# trusting.
CHUNK_MAX_MS = 16500

# Independently of duration: going over MAX_NOTES_PER_TRACK does not fail, it
# FAULTS the app, so the chunker respects this too.
CHUNK_MAX_NOTES = 200

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


def pick_melody_channel(notes: list[MidiNote], programs: dict[int, int], tick_to_ms,
                        song: Song) -> int:
    """Score each channel on how much it behaves like a lead line.

    Density is a HARD filter, not a scoring term. Scoring it alongside pitch
    picked the wrong channel here: a 25-note high string counter-line (0.44
    notes/sec) outscored the 158-note alto sax carrying the actual tune, because
    it sat an octave higher and was perfectly monophonic. A melody has to have
    enough notes to BE the melody, so anything under MELODY_MIN_RATE is not a
    candidate at all; only then does pitch-versus-polyphony decide.
    """
    section = [n for n in notes
               if song.start_tick <= n.start < song.end_tick and n.channel != 9]
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
        span_ms = tick_to_ms(song.end_tick) - tick_to_ms(song.start_tick)
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


def extract_melody(notes: list[MidiNote], channel: int,
                   start_tick: int, end_tick: int) -> list[MidiNote]:
    """Skyline within the chosen channel, forced monophonic.

    Where notes overlap, the higher one wins and the lower is dropped rather
    than shortened -- a melody that ducks to an inner voice for 40ms reads as a
    glitch, not as counterpoint.

    The tick range is always explicit: make_chart passes the song's charted
    section, tools/extract_melody.py passes the whole file so the melody MIDI it
    writes is a general extraction rather than an excerpt.
    """
    lo, hi = start_tick, end_tick
    group = sorted((n for n in notes
                    if n.channel == channel and lo <= n.start < hi),
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


def build_chart(melody: list[MidiNote], tick_to_ms, song: Song):
    """One chart note per melody note. Every note. No selection, no dropping.

    The chart and the music come from the same list, so a note heard is always a
    note to hit and a note hit is always a note heard. Because both are placed
    from the same tempo map at the same offset, a chart note's hit time IS the
    moment its tone sounds -- pressing on the beat and pressing on the note are
    the same action.

    Lane follows PITCH where it can: notes above the melody's median go to the
    upper lane, below it to the lower one, so the lane pattern is the shape of
    the tune and means something on screen.

    Where it cannot, spacing wins. Pitch does not care how fast the line moves,
    and two notes 127ms apart in one lane are unplayable -- roughly eight presses
    a second on one button -- as well as being closer than two judgment windows
    can sit. So a note whose preferred lane is still busy takes the other one.
    That is what guarantees SAME_LANE_MIN_MS across the whole chart, and with it
    the invariant the unit tests check.
    """
    origin = tick_to_ms(song.start_tick)
    pitches = sorted(n.pitch for n in melody)
    split = pitches[len(pitches) // 2]

    if not melody:
        raise ValueError("no melody notes in selected MIDI section")

    # The velocity accent is RELATIVE to this line, not an absolute threshold.
    #
    # An absolute one was here (velocity >= 116) and it silently marked every
    # single note big: this melody's velocities run 119-124, because the part was
    # sequenced flat, so the test was true 157 times out of 157. A relative
    # margin degrades to marking nothing on a flat part -- which is the honest
    # answer, since a flat part HAS no accents -- while still finding them on an
    # expressively played one.
    velocities = sorted(n.velocity for n in melody)
    accent_velocity = velocities[len(velocities) // 2] + BIG_VELOCITY_MARGIN

    chart: list[tuple[int, int, int]] = []
    last_in_lane = [-10 ** 9, -10 ** 9]
    forced = 0
    for note in melody:
        time_ms = LEAD_MS + int(round(tick_to_ms(note.start) - origin))
        offset = note.start - song.start_tick
        held = tick_to_ms(note.end) - tick_to_ms(note.start)
        big = int(offset % (TICKS_PER_BEAT_REQUIRED * BEATS_PER_BAR) == 0
                  or note.velocity >= accent_velocity
                  or held >= BIG_HELD_MS)

        preferred = 1 if note.pitch >= split else 0
        other = 1 - preferred
        if time_ms - last_in_lane[preferred] >= SAME_LANE_MIN_MS:
            lane = preferred
        elif time_ms - last_in_lane[other] >= SAME_LANE_MIN_MS:
            lane = other
            forced += 1
        else:
            # Both lanes are still busy. With two lanes this needs three notes
            # inside SAME_LANE_MIN_MS of each other, which this melody never
            # does -- the tightest run is 127+127=254ms. Refuse rather than emit
            # a chart whose judgment windows overlap: silently dropping the note
            # would break the one-note-one-press promise, and keeping it would
            # break the tests, so neither is a quiet option.
            raise ValueError(
                f"note at {time_ms}ms cannot be placed {SAME_LANE_MIN_MS}ms clear "
                f"in either lane (lanes last used at {last_in_lane}). This song "
                f"is denser than two lanes can carry -- it needs a third lane, or "
                f"a smaller RB_MISS_MS and SAME_LANE_MIN_MS to match.")

        last_in_lane[lane] = time_ms
        chart.append((time_ms, lane, big))

    chart.sort()
    return chart, forced


def build_music(melody: list[MidiNote], shift: int, tick_to_ms, song: Song):
    """One monophonic melody track, split into chunks.

    Chunks exist for two reasons. The hard one is SPEAKER_MAX_NOTES: a track may
    not exceed 256 notes, and going over faults the app instead of failing. The
    useful one is that chaining chunks from the finish callback is the only place
    the music can be re-anchored to the game clock.
    """
    origin = tick_to_ms(song.start_tick)
    total_ms = int(round(tick_to_ms(song.end_tick) - origin))
    bar_ticks = BEATS_PER_BAR * TICKS_PER_BEAT_REQUIRED

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

    # Accumulate whole bars into a chunk until one more would cross CHUNK_MAX_MS.
    # Splitting on a bar keeps every handover on a downbeat.
    bounds = []
    tick = song.start_tick
    while tick < song.end_tick:
        nxt = min(tick + bar_ticks, song.end_tick)
        while nxt < song.end_tick:
            after = min(nxt + bar_ticks, song.end_tick)
            if tick_to_ms(after) - tick_to_ms(tick) > CHUNK_MAX_MS:
                break
            nxt = after
        bounds.append((int(round(tick_to_ms(tick) - origin)),
                       int(round(tick_to_ms(nxt) - origin))))
        tick = nxt

    def append_rest(seq, ms: int) -> None:
        """A rest longer than the SDK's per-note cap has to be split.

        Only rests can get long -- sounding notes are already capped by
        MAX_SUSTAIN_MS -- but a chunk that opens or closes on a long silence can
        exceed 10000ms on its own, and a single over-long entry is not something
        the watch reports, it is something it mis-plays.
        """
        while ms > 0:
            piece = min(ms, MAX_NOTE_MS)
            seq.append((0, MELODY_WAVE, piece, 0))
            ms -= piece

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
                append_rest(seq, start_ms - cursor)
            sounding = end_ms - start_ms
            if sounding > NOTE_GAP_MS + 25:
                sounding -= NOTE_GAP_MS
            seq.append((pitch, MELODY_WAVE, sounding, min(MELODY_VELOCITY, velocity)))
            if end_ms - start_ms > sounding:
                append_rest(seq, (end_ms - start_ms) - sounding)
            cursor = end_ms
        if cursor < chunk_end:
            append_rest(seq, chunk_end - cursor)
        if len(seq) > CHUNK_MAX_NOTES:
            raise ValueError(f"chunk needs {len(seq)} notes, over the working "
                             f"limit of {CHUNK_MAX_NOTES} (hard cap "
                             f"{MAX_NOTES_PER_TRACK}) -- lower CHUNK_MAX_MS")
        chunks.append((chunk_start, chunk_end - chunk_start, [seq]))

    return chunks, total_ms


def write_music(built) -> int:
    """Emit every song's chunks, plus a per-song index.

    `built` is a list of (song, chunks, total_ms). Arrays are named by the song's
    C identifier stem rather than by index, so adding or reordering a song does
    not silently renumber the previous one's symbols.
    """
    parts = ['#include "music.h"\n',
             "// GENERATED FILE -- do not hand-edit. Regenerate with tools/make_chart.py.\n"
             "//\n"
             "// One monophonic melody line per song, for speaker_play_tracks(). See music.h\n"
             "// for the measured sequencer limits, and audio.c for the chunk handover.\n"]

    total_notes = 0
    for song, chunks, _total_ms in built:
        for chunk_index, (_start, _duration, tracks) in enumerate(chunks):
            for track_index, seq in enumerate(tracks):
                total_notes += len(seq)
                body = "\n".join(f"  {{ {note:3d}, {wave}, {ms:5d}, {vel:3d}, 0 }},"
                                  for note, wave, ms, vel in seq)
                parts.append(
                    f"static const SpeakerNote s_{song.ident}_c{chunk_index}_t{track_index}[] "
                    f"= {{\n{body}\n}};\n")

    for song, chunks, _total_ms in built:
        entries = []
        for chunk_index, (start, duration, tracks) in enumerate(chunks):
            names = ", ".join(f"s_{song.ident}_c{chunk_index}_t{t}"
                              for t in range(len(tracks)))
            counts = ", ".join(str(len(seq)) for seq in tracks)
            entries.append(f"  {{ {{ {names} }}, {{ {counts} }}, "
                           f"{len(tracks)}, {start}, {duration} }},")
        parts.append(f"static const MusicChunk s_{song.ident}_chunks[] = {{\n"
                     + "\n".join(entries) + "\n};\n")

    rows = "\n".join(
        f"  {{ s_{song.ident}_chunks, "
        f"(uint16_t)(sizeof(s_{song.ident}_chunks) / sizeof(s_{song.ident}_chunks[0])), "
        f"{total_ms} }},"
        for song, _chunks, total_ms in built)
    parts.append("static const MusicSong s_songs[] = {\n" + rows + "\n};\n")

    parts.append("""static const MusicSong *prv_song(uint8_t song) {
  return (song < (sizeof(s_songs) / sizeof(s_songs[0]))) ? &s_songs[song] : NULL;
}

uint16_t music_chunk_count(uint8_t song) {
  const MusicSong *const s = prv_song(song);
  return (s != NULL) ? s->chunk_count : 0;
}

const MusicChunk *music_chunk(uint8_t song, uint16_t index) {
  const MusicSong *const s = prv_song(song);
  return (s != NULL && index < s->chunk_count) ? &s->chunks[index] : NULL;
}

uint32_t music_total_ms(uint8_t song) {
  const MusicSong *const s = prv_song(song);
  return (s != NULL) ? s->total_ms : 0;
}
""")
    MUSIC_C.write_text("\n".join(parts))
    return total_notes


def write_chart(built) -> None:
    """Emit every song's note array, plus the table the app indexes by song."""
    parts = ['#include "chart.h"\n',
             "// GENERATED FILE -- do not hand-edit. Regenerate with tools/make_chart.py.\n"
             "//\n"
             "// Charted directly from MIDI note-on events and tempo messages. The chart is\n"
             "// EVERY melody note, 1:1 -- a note heard is a note to hit. Lane follows pitch\n"
             "// except where spacing forces the other lane; see tools/make_chart.py.\n"
             "//\n"
             "// The same MIDI and the same tempo map generate music.c, so the notes and the\n"
             "// music share one origin and cannot drift apart.\n"]

    for song, chart, duration_s, bpm in built:
        rows = "\n".join(
            f"  {{ {time_ms:6d}, {LANE_NAMES[lane]:12s}, "
            f"{'RB_NOTE_BIG   ' if big else 'RB_NOTE_NORMAL'} }},"
            for time_ms, lane, big in chart)
        parts.append(f"// {song.title}: {len(chart)} notes, "
                     f"{len(chart) / duration_s:.2f}/s, {duration_s:.1f}s at {bpm} BPM.\n"
                     f"static const ChartNote s_{song.ident}_notes[] = {{\n{rows}\n}};\n")

    rows = "\n".join(
        f'  {{ .title = "{song.title}", .notes = s_{song.ident}_notes,\n'
        f"    .note_count = (uint16_t)(sizeof(s_{song.ident}_notes) "
        f"/ sizeof(s_{song.ident}_notes[0])),\n"
        f"    .bpm = {bpm}, .lead_in_ms = {LEAD_MS}, "
        f".end_ms = {LEAD_MS + int(round(duration_s * 1000)) + TAIL_MS} }},"
        for song, chart, duration_s, bpm in built)
    parts.append("static const Chart s_charts[] = {\n" + rows + "\n};\n")

    parts.append("""uint8_t chart_count(void) {
  return (uint8_t)(sizeof(s_charts) / sizeof(s_charts[0]));
}

const Chart *chart_get(uint8_t index) {
  return (index < chart_count()) ? &s_charts[index] : &s_charts[0];
}

bool chart_load_from_resource(uint32_t resource_id, Chart *out_chart) {
  (void)resource_id;
  (void)out_chart;
  return false;
}
""")
    CHART_C.write_text("\n".join(parts))


def build_song(song: Song):
    """Everything for one song: melody, chart, music, and a printed report."""
    ticks_per_beat, tempos, notes, programs = parse_midi(song.midi)
    tick_to_ms = make_tick_to_ms(tempos, ticks_per_beat)

    channel = pick_melody_channel(notes, programs, tick_to_ms, song)
    melody = extract_melody(notes, channel, song.start_tick, song.end_tick)
    shift = melody_octave_shift(melody)

    chart, forced = build_chart(melody, tick_to_ms, song)
    chunks, total_ms = build_music(melody, shift, tick_to_ms, song)

    # duration_ms is uint16 and the SDK caps a single note at 10000ms. Long rests
    # are split to stay under it, so this asserts that splitting actually worked
    # rather than assuming it -- checking the emitted entries, not the chunk.
    longest = max(ms for _s, _d, tracks in chunks for seq in tracks for _p, _w, ms, _v in seq)
    if longest > MAX_NOTE_MS:
        raise ValueError(f"{song.title}: emitted a {longest}ms entry, over the "
                         f"{MAX_NOTE_MS}ms note cap")

    duration_s = total_ms / 1000.0
    bpm = round(60000 / (tick_to_ms(song.start_tick + TICKS_PER_BEAT_REQUIRED)
                         - tick_to_ms(song.start_tick)))

    # Every melody note is charted, so this had better be an identity.
    assert len(chart) == len(melody), (len(chart), len(melody))
    same_lane = min((b[0] - a[0] for a, b in zip(chart, chart[1:]) if a[1] == b[1]),
                    default=0)
    tightest = min((b[0] - a[0] for a, b in zip(chart, chart[1:])), default=0)
    if same_lane < SAME_LANE_MIN_MS:
        raise ValueError(f"{song.title}: same-lane gap {same_lane}ms is under "
                         f"{SAME_LANE_MIN_MS}ms -- windows would overlap")

    per_lane = Counter(lane for _t, lane, _b in chart)
    pitches = [n.pitch + shift for n in melody]
    lo = 440 * 2 ** ((min(pitches) - 69) / 12)
    hi = 440 * 2 ** ((max(pitches) - 69) / 12)
    notes_per_chunk = max(len(seq) for _s, _d, tracks in chunks for seq in tracks)

    print(f"\n{song.title}  [{song.midi.name}, bars {song.start_bar}-"
          f"{song.start_bar + song.bars}, {bpm} BPM, {duration_s:.1f}s]")
    print(f"  melody: channel {channel} (GM program {programs.get(channel)}), "
          f"{len(melody)} notes, +{shift // 12} octave(s) -> {lo:.0f}-{hi:.0f}Hz")
    print(f"  chart:  {len(chart)} notes ({len(chart) / duration_s:.2f}/s) -- "
          f"every melody note, 1:1")
    print(f"          lanes {LANE_NAMES[0]}={per_lane[0]} {LANE_NAMES[1]}={per_lane[1]}, "
          f"{forced} placed off-pitch to keep spacing")
    print(f"          tightest gap {tightest}ms; tightest SAME-LANE gap {same_lane}ms "
          f"(needs >= {SAME_LANE_MIN_MS}, i.e. RB_MISS_MS <= {same_lane // 2})")
    print(f"  music:  {len(chunks)} chunks, up to {notes_per_chunk} notes each "
          f"(cap {MAX_NOTES_PER_TRACK})")

    return (song, chart, duration_s, bpm), (song, chunks, total_ms), len(chart)


def main() -> None:
    if len(sys.argv) > 1:
        sys.exit(__doc__)

    charts, musics, total_chart_notes = [], [], 0
    for song in SONGS:
        chart_part, music_part, n = build_song(song)
        charts.append(chart_part)
        musics.append(music_part)
        total_chart_notes += n

    note_count = write_music(musics)
    write_chart(charts)

    print(f"\n{len(SONGS)} songs: {total_chart_notes} chart notes, "
          f"{note_count} SpeakerNotes (~{note_count * 6} bytes of flash)")
    print(f"wrote {CHART_C}\n      {MUSIC_C}")


if __name__ == "__main__":
    main()
