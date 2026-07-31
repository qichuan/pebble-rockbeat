#!/usr/bin/env python3
"""Draw the 720x320 Pebble appstore banner.

    python3 tools/make_banner.py

Writes developer-portal/banner/banner_720x320.png -- the header image the store
listing shows above the description. It is store artwork only; nothing here
ships in the .pbw.

It is DRAWN rather than screenshotted for two reasons. The watch screen is
200x228, so any screenshot has to be scaled 1.4x vertically and letterboxed
horizontally to reach 720x320, which turns crisp 2px rails into mush. And the
banner wants the design's true colours, which the emulator's screenshot does not
report faithfully -- #555500 comes back as #564E36.

The picture is the gameplay screen's own vocabulary at banner scale: two lane
beds in the design colours, notes travelling right along their rails, and the
two target rings with the badges that name their buttons. Anyone who has seen
the banner recognises the screen, and vice versa.

Text is drawn from the 5x7 bitmap font below rather than a system font: no
stdlib module can rasterise a TrueType face, and a chunky pixel face is the
right register for the game anyway.
"""

from __future__ import annotations

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import pngkit
from pngkit import Canvas

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "developer-portal/banner/banner_720x320.png"

W, H = 720, 320

# ---------------------------------------------------------------------------
# Layout. The proportions are the watch's, scaled up: a lane is 50 of 228 on
# screen (22%) and 70 of 320 here (22%), and the rail sits just under half way
# down its bed in both.
# ---------------------------------------------------------------------------

WORDMARK_Y = 30
WORDMARK_SCALE = 8         # 5x7 cells -> 40x56 glyphs
WORDMARK_TRACK = 8         # pixels between glyphs

LANE_TOP_Y = 116
LANE_BOT_Y = 200
LANE_H = 70
LANE_GAP = LANE_BOT_Y - (LANE_TOP_Y + LANE_H)
RAIL_DY = 34               # 24/50 of the way down, as on the watch
RAIL_H = 4

TARGET_ZONE_X = 556        # lane bed stops, plain black behind the targets
DIVIDER_X = 552
DIVIDER_W = 4
TARGET_CX = 632
TARGET_R = 40
TARGET_RING_W = 10
ARROW_LEN = 24
ARROW_HALF = 16

NOTE_R = 28
NOTE_RING_W = 6
NOTE_DOT_R = 8

# Where the notes sit along each lane. Staggered between the lanes rather than
# aligned, because the chart alternates: the same-lane spacing rule in
# make_chart.py pushes a repeat onto the other lane, so a real run of sixteenths
# looks exactly like this.
NOTES_TOP = (96, 232, 368, 486)
NOTES_BOT = (164, 300, 424)

TAGLINE_Y = 286
TAGLINE_SCALE = 3
TAGLINE_TRACK = 3

WORDMARK = "ROCKBEAT"
TAGLINE = "TAP EVERY NOTE OF THE MELODY"

# ---------------------------------------------------------------------------
# A 5x7 bitmap font, uppercase only -- every character the two strings above
# use, plus the rest of the alphabet and the digits so a future tagline does not
# have to come back here. '#' is ink, '.' is paper.
# ---------------------------------------------------------------------------

FONT = {
    "A": (".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"),
    "B": ("####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."),
    "C": (".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."),
    "D": ("####.", "#...#", "#...#", "#...#", "#...#", "#...#", "####."),
    "E": ("#####", "#....", "#....", "####.", "#....", "#....", "#####"),
    "F": ("#####", "#....", "#....", "####.", "#....", "#....", "#...."),
    "G": (".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".###."),
    "H": ("#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"),
    "I": ("#####", "..#..", "..#..", "..#..", "..#..", "..#..", "#####"),
    "J": ("..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."),
    "K": ("#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"),
    "L": ("#....", "#....", "#....", "#....", "#....", "#....", "#####"),
    "M": ("#...#", "##.##", "#.#.#", "#...#", "#...#", "#...#", "#...#"),
    "N": ("#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#", "#...#"),
    "O": (".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."),
    "P": ("####.", "#...#", "#...#", "####.", "#....", "#....", "#...."),
    "Q": (".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"),
    "R": ("####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"),
    "S": (".####", "#....", "#....", ".###.", "....#", "....#", "####."),
    "T": ("#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."),
    "U": ("#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."),
    "V": ("#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."),
    "W": ("#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"),
    "X": ("#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"),
    "Y": ("#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."),
    "Z": ("#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"),
    "0": (".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."),
    "1": ("..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."),
    "2": (".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"),
    "3": ("#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."),
    "4": ("...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."),
    "5": ("#####", "#....", "####.", "....#", "....#", "#...#", ".###."),
    "6": ("..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."),
    "7": ("#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."),
    "8": (".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."),
    "9": (".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."),
    " ": (".....", ".....", ".....", ".....", ".....", ".....", "....."),
}

GLYPH_W, GLYPH_H = 5, 7


def text_width(s: str, scale: int, track: int) -> int:
    return len(s) * (GLYPH_W * scale + track) - track


def draw_text(c: Canvas, x: int, y: int, s: str, scale: int, track: int,
              rgb: tuple[int, int, int]) -> None:
    for ch in s:
        glyph = FONT[ch]
        for row in range(GLYPH_H):
            for col in range(GLYPH_W):
                if glyph[row][col] == "#":
                    c.rect(x + col * scale, y + row * scale, scale, scale, rgb)
        x += GLYPH_W * scale + track


def draw_lane(c: Canvas, y: int, bed, rail, accent, up: bool,
              note_xs: tuple[int, ...]) -> None:
    c.rect(0, y, W, LANE_H, bed)
    c.rect(0, y + RAIL_DY, TARGET_ZONE_X, RAIL_H, rail)

    # The target zone is plain black so a note crossing the target is never read
    # against a coloured bed -- the same reason it is black on the watch.
    c.rect(TARGET_ZONE_X, y, W - TARGET_ZONE_X, LANE_H, pngkit.BLACK)
    c.rect(DIVIDER_X, y, DIVIDER_W, LANE_H, pngkit.LIGHT_GRAY)

    cy = y + LANE_H // 2
    c.ring(TARGET_CX, cy, TARGET_R, TARGET_RING_W, accent)
    c.arrow(TARGET_CX, cy, ARROW_LEN, ARROW_HALF, up, accent)

    for x in note_xs:
        c.circle(x, cy, NOTE_R, pngkit.WHITE)
        c.circle(x, cy, NOTE_R - NOTE_RING_W, accent)
        c.circle(x, cy, NOTE_DOT_R, bed)


def main() -> None:
    c = Canvas(W, H, ss=3)
    c.rect(0, 0, W, H, pngkit.BLACK)

    draw_text(c, (W - text_width(WORDMARK, WORDMARK_SCALE, WORDMARK_TRACK)) // 2,
              WORDMARK_Y, WORDMARK, WORDMARK_SCALE, WORDMARK_TRACK, pngkit.LANE_TOP_ACC)

    draw_lane(c, LANE_TOP_Y, pngkit.LANE_TOP_BED, pngkit.LANE_TOP_RAIL,
              pngkit.LANE_TOP_ACC, True, NOTES_TOP)
    draw_lane(c, LANE_BOT_Y, pngkit.LANE_BOT_BED, pngkit.LANE_BOT_RAIL,
              pngkit.LANE_BOT_ACC, False, NOTES_BOT)

    draw_text(c, (W - text_width(TAGLINE, TAGLINE_SCALE, TAGLINE_TRACK)) // 2,
              TAGLINE_Y, TAGLINE, TAGLINE_SCALE, TAGLINE_TRACK, pngkit.LIGHT_GRAY)

    c.write(OUT)
    print(f"wrote {OUT.relative_to(ROOT)} ({OUT.stat().st_size} bytes, {W}x{H}) "
          f"lane gap {LANE_GAP}px")


if __name__ == "__main__":
    main()
