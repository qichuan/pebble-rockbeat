#!/usr/bin/env python3
"""Draw the Rockbeat menu icon straight from the design's geometry.

    python3 tools/make_icon.py

Writes watch/resources/images/menu_icon.png -- 25x25, white on transparent,
which is what Pebble's launcher and app list use. The design also specifies
144x144 and 80x80 versions; those are store artwork and the watch never loads
them, so they are not generated here.

The mark is the game itself in miniature: two lanes as horizontal bars, with a
note sitting at the end of each. That is why the bars are different lengths --
the notes are at different points along their lanes, which is what the screen
always looks like.

Stdlib only, like the rest of tools/: a PNG is a zlib stream in a container with
CRC-32 per chunk, and both are in the standard library. Pillow would be one more
thing a fresh checkout has to install before it can build.
"""

from __future__ import annotations

from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "watch/resources/images/menu_icon.png"

SIZE = 25

# Straight from the design's 25x25 artboard: (x, y, w, h) for the two lane bars.
BARS = ((3, 8, 11, 3), (3, 15, 15, 3))

# (cx, cy, r) for the two notes. The design gives 7x7 circles at (13,6) and
# (17,13), i.e. centres at +3 with r=3.
DOTS = ((16, 9, 3), (20, 16, 3))


def draw() -> bytearray:
    """RGBA rows, white where the mark is and fully transparent everywhere else.

    Transparent rather than black: the launcher composites the icon over its own
    background, which is not always dark, and a black plate would show as a box.
    """
    px = bytearray(SIZE * SIZE * 4)

    def put(x: int, y: int) -> None:
        if 0 <= x < SIZE and 0 <= y < SIZE:
            i = (y * SIZE + x) * 4
            px[i:i + 4] = b"\xFF\xFF\xFF\xFF"

    for bx, by, bw, bh in BARS:
        for y in range(by, by + bh):
            for x in range(bx, bx + bw):
                put(x, y)

    for cx, cy, r in DOTS:
        # Filled circle by the same distance test the watch uses. No
        # antialiasing: the icon is 25px and a soft edge would just look muddy.
        for y in range(cy - r, cy + r + 1):
            for x in range(cx - r, cx + r + 1):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r + r:
                    put(x, y)

    return px


def write_png(path: Path, px: bytearray) -> None:
    raw = bytearray()
    for y in range(SIZE):
        raw.append(0)                                  # filter type 0 per row
        raw += px[y * SIZE * 4:(y + 1) * SIZE * 4]

    def chunk(tag: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", SIZE, SIZE, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b""))


def main() -> None:
    write_png(OUT, draw())
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes, {SIZE}x{SIZE} RGBA)")


if __name__ == "__main__":
    main()
