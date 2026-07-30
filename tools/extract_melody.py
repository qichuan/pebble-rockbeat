#!/usr/bin/env python3
"""Write a melody-only MIDI file from a full arrangement.

    python3 tools/extract_melody.py                       # bundled song
    python3 tools/extract_melody.py in.mid out.mid

This is a PREPROCESSING step, and it is optional: make_chart.py performs the
same extraction internally, so the watch build is byte-identical whether it is
fed the full arrangement or the melody file this writes.

What it buys is inspectability.  The melody MIDI can be opened and auditioned
anywhere -- a DAW, a PC MIDI player, the reference script below -- so the
extraction can be judged on its own before anything is compiled into the game.
Debugging "is the melody right?" by rebuilding the watch app is a slow loop.

Algorithm: the "skyline" melody extraction -- within each moment, the highest
sounding note is the melody -- following
https://github.com/xinyiguan/MIDI_Melody_Extraction.

That script is not used directly for two reasons.  It depends on `mido`, and
this project is deliberately dependency-free (a fresh checkout builds the game
with nothing installed).  And it hard-filters to MIDI channel 0, which the
bundled file does not have -- its channels are 2,3,4,7,8,9,13,15, so the script
would return an empty result.  The algorithm is therefore reimplemented over
make_chart's own parser, with the melody channel CHOSEN rather than assumed.

The output is written on channel 0, which is the convention that script (and
most melody tooling) expects, so the result is usable by them in turn.
"""

from __future__ import annotations

from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

from make_chart import (  # noqa: E402  (path set above)
    MidiNote,
    extract_melody,
    make_tick_to_ms,
    parse_midi,
    pick_melody_channel,
)

ROOT = Path(__file__).resolve().parents[1]

# The full arrangement this melody was extracted FROM. It is not in the repo:
# melody.mid is what the build reads, and keeping a 37KB arrangement around only
# to re-derive a file that is already committed buys nothing. Point the tool at
# a copy (or at any other .mid) to re-run the extraction.
DEFAULT_SOURCE = ROOT / "watch/resources/data/Never-Gonna-Give-You-Up.mid"
DEFAULT_OUT = ROOT / "watch/resources/data/melody.mid"

OUT_CHANNEL = 0


def write_vlq(value: int) -> bytes:
    """MIDI variable-length quantity: 7 bits per byte, high bit = continue."""
    if value < 0:
        raise ValueError("negative delta time")
    out = bytearray([value & 0x7F])
    value >>= 7
    while value:
        out.append((value & 0x7F) | 0x80)
        value >>= 7
    return bytes(reversed(out))


def build_track(melody: list[MidiNote], tempos: list[tuple[int, int]],
                program: int | None) -> bytes:
    """Serialise one MTrk chunk holding the tempo map and the melody.

    Absolute tick positions are PRESERVED rather than re-zeroed to the first
    note. make_chart.py selects its section by absolute tick (START_BAR/BARS),
    so re-basing here would silently shift which 28 bars the game charts.
    """
    events: list[tuple[int, int, bytes]] = []   # (tick, order, bytes)

    for tick, tempo in tempos:
        events.append((tick, 0, b"\xFF\x51\x03" + tempo.to_bytes(3, "big")))
    if program is not None:
        events.append((0, 0, bytes([0xC0 | OUT_CHANNEL, program & 0x7F])))

    for note in melody:
        # Note-offs sort before note-ons at the same tick (order 1 vs 2) so a
        # repeated pitch is released before it is struck again -- otherwise the
        # off silences the note that was just started.
        events.append((note.start, 2,
                       bytes([0x90 | OUT_CHANNEL, note.pitch & 0x7F, note.velocity & 0x7F])))
        events.append((max(note.end, note.start + 1), 1,
                       bytes([0x80 | OUT_CHANNEL, note.pitch & 0x7F, 0])))

    events.sort(key=lambda e: (e[0], e[1]))

    body = bytearray()
    previous = 0
    for tick, _order, payload in events:
        body += write_vlq(tick - previous)
        body += payload
        previous = tick
    body += write_vlq(0) + b"\xFF\x2F\x00"      # end of track

    return b"MTrk" + struct.pack(">I", len(body)) + bytes(body)


def write_midi(path: Path, division: int, track: bytes) -> None:
    header = b"MThd" + struct.pack(">I", 6) + struct.pack(">HHH", 0, 1, division)
    path.write_bytes(header + track)


def main() -> None:
    if len(sys.argv) > 3:
        sys.exit(__doc__)
    source = Path(sys.argv[1]) if len(sys.argv) >= 2 else DEFAULT_SOURCE
    out = Path(sys.argv[2]) if len(sys.argv) == 3 else DEFAULT_OUT
    if not source.exists():
        sys.exit(f"{source} not found -- pass the source arrangement as an argument")

    division, tempos, notes, programs = parse_midi(source)
    tick_to_ms = make_tick_to_ms(tempos, division)

    # The channel is chosen from the charted section -- that is the part of the
    # song the game uses, so it is the part that should decide what "the melody"
    # is -- but the notes are then taken from the WHOLE file, so the output is a
    # general melody extraction rather than a 28-bar excerpt.
    channel = pick_melody_channel(notes, programs, tick_to_ms)
    last = max((n.end for n in notes), default=0)
    melody = extract_melody(notes, channel, 0, last + 1)
    if not melody:
        sys.exit(f"no melody found on channel {channel}")

    track = build_track(melody, tempos, programs.get(channel))
    write_midi(out, division, track)

    span = (tick_to_ms(melody[-1].end) - tick_to_ms(melody[0].start)) / 1000.0
    pitches = [n.pitch for n in melody]
    print(f"{source.name}: {len(notes)} notes, channels "
          f"{sorted({n.channel for n in notes})}")
    print(f"melody: channel {channel} (GM program {programs.get(channel)}), "
          f"{len(melody)} notes over {span:.1f}s, pitch {min(pitches)}-{max(pitches)}")
    print(f"wrote {out} ({out.stat().st_size} bytes, format 0, channel {OUT_CHANNEL})")


if __name__ == "__main__":
    main()
