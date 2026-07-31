#!/usr/bin/env python3
"""A tiny RGBA canvas and PNG writer, stdlib only.

Shared by make_icon.py and make_banner.py, which both need circles, rings and
rounded rectangles at sizes where a hard-edged circle looks obviously wrong.

Stdlib only, like the rest of tools/: a PNG is a zlib stream in a container with
a CRC-32 per chunk, and both of those are in the standard library. Pillow would
be one more thing a fresh checkout has to install before it can produce
artwork, for shapes that fit in a hundred lines.

Antialiasing is by SUPERSAMPLING -- everything is drawn at `ss` times the final
resolution with hard edges, then box-filtered down. That is why the drawing
primitives can stay this simple: none of them needs to compute coverage, and
they compose correctly with each other for free. It costs memory (a 720x320
banner at ss=3 is an 8MB buffer), which is irrelevant on a build host and is the
reason none of this belongs on the watch.

Note that the watch itself does NOT work this way -- render.c has antialiasing
switched OFF for the playfield because it is paid per drawn pixel 25 times a
second. This module is for artwork rendered once, on a laptop.
"""

from __future__ import annotations

from pathlib import Path
import math
import struct
import zlib

# Design palette, as (r, g, b). These are the true design hexes, not the 64
# colour approximations the watch quantises them to -- store artwork is viewed
# on a phone, so it should use the colours as drawn.
BLACK = (0x00, 0x00, 0x00)
WHITE = (0xFF, 0xFF, 0xFF)
LANE_TOP_BED = (0x55, 0x55, 0x00)
LANE_TOP_RAIL = (0xAA, 0x55, 0x00)
LANE_TOP_ACC = (0xFF, 0xAA, 0x00)
LANE_BOT_BED = (0x00, 0x55, 0x55)
LANE_BOT_RAIL = (0x00, 0x55, 0xAA)
LANE_BOT_ACC = (0x00, 0xAA, 0xFF)
ART_TILE = (0xAA, 0x00, 0x55)
YELLOW = (0xFF, 0xFF, 0x00)
LIGHT_GRAY = (0xAA, 0xAA, 0xAA)


class Canvas:
    """An RGBA image drawn at `ss`x and downsampled on output.

    Every coordinate passed in is in FINAL pixels; the scaling to the internal
    buffer happens here so callers never have to think about it.
    """

    def __init__(self, width: int, height: int, ss: int = 4) -> None:
        self.w = width
        self.h = height
        self.ss = ss
        self._w = width * ss
        self._h = height * ss
        self._px = bytearray(self._w * self._h * 4)  # transparent

    # -- primitives ---------------------------------------------------------
    # All of them are opaque writes rather than alpha blends: every shape here
    # is a flat fill, and "last one wins" is exactly the painter's model the
    # design uses. Transparency exists only as the untouched background.

    def _span(self, x0: int, x1: int, y: int, rgb: tuple[int, int, int]) -> None:
        """Fill [x0, x1) on internal row y. Clipped."""
        if y < 0 or y >= self._h:
            return
        x0 = max(0, x0)
        x1 = min(self._w, x1)
        if x1 <= x0:
            return
        i = (y * self._w + x0) * 4
        self._px[i:i + (x1 - x0) * 4] = bytes((*rgb, 0xFF)) * (x1 - x0)

    def rect(self, x: int, y: int, w: int, h: int, rgb: tuple[int, int, int]) -> None:
        s = self.ss
        for yy in range(y * s, (y + h) * s):
            self._span(x * s, (x + w) * s, yy, rgb)

    def round_rect(self, x: int, y: int, w: int, h: int, r: int,
                   rgb: tuple[int, int, int]) -> None:
        s = self.ss
        rs = r * s
        for yy in range(y * s, (y + h) * s):
            # Distance into the rounded band, measured from whichever corner
            # this row is nearest; 0 in the straight middle section.
            dy = 0
            if yy < y * s + rs:
                dy = y * s + rs - yy
            elif yy >= (y + h) * s - rs:
                dy = yy - ((y + h) * s - rs - 1)
            inset = 0
            if dy > 0:
                inset = rs - _isqrt_clamped(rs * rs - dy * dy)
            self._span(x * s + inset, (x + w) * s - inset, yy, rgb)

    def circle(self, cx: int, cy: int, r: int, rgb: tuple[int, int, int],
               bias: int = 0) -> None:
        """A filled disc: every pixel with dx^2 + dy^2 <= r^2 + bias.

        `bias` exists so the 25px menu icon can be regenerated with EXACTLY the
        shape that shipped. Passing bias=r reproduces the `<= r*r + r` test the
        watch's own fill uses, and at 25px one pixel at the poles is the
        difference between a disc and a diamond.
        """
        s = self.ss
        cxs, cys, rs = cx * s, cy * s, r * s
        limit = rs * rs + bias * s * s
        for yy in range(cys - rs, cys + rs + 1):
            half = _isqrt_clamped(limit - (yy - cys) ** 2)
            self._span(cxs - half, cxs + half + 1, yy, rgb)

    def ring(self, cx: int, cy: int, r: int, width: int,
             rgb: tuple[int, int, int]) -> None:
        """An annulus, drawn directly rather than as two circles.

        Two circles would need to know what colour sits inside the ring, which
        in the banner is a lane bed in one place and black in another.
        """
        s = self.ss
        cxs, cys, rs, inner = cx * s, cy * s, r * s, (r - width) * s
        for yy in range(cys - rs, cys + rs + 1):
            dy2 = (yy - cys) ** 2
            outer_half = _isqrt_clamped(rs * rs - dy2)
            if dy2 >= inner * inner:
                self._span(cxs - outer_half, cxs + outer_half + 1, yy, rgb)
            else:
                inner_half = _isqrt_clamped(inner * inner - dy2)
                self._span(cxs - outer_half, cxs - inner_half, yy, rgb)
                self._span(cxs + inner_half + 1, cxs + outer_half + 1, yy, rgb)

    def arrow(self, cx: int, cy: int, length: int, half: int, up: bool,
              rgb: tuple[int, int, int]) -> None:
        """The lane badge, matching prv_draw_arrow() in render.c.

        UP for the top lane, RIGHT for the bottom one -- which is played with
        SELECT, the middle button, so a DOWN arrow would name the wrong button.
        """
        s = self.ss
        ln, hf = length * s, half * s
        for i in range(ln):
            if up:
                grow = (hf * (i + 1)) // ln
                self._span(cx * s - grow, cx * s + grow, cy * s - ln // 2 + i, rgb)
            else:
                shrink = (hf * (ln - i)) // ln
                yy0 = cy * s - shrink
                xx = cx * s - ln // 2 + i
                for yy in range(yy0, cy * s + shrink):
                    self._span(xx, xx + 1, yy, rgb)

    # -- output -------------------------------------------------------------

    def _downsample(self) -> bytearray:
        """Box-filter ss x ss blocks, premultiplying so edges do not fringe.

        Averaging straight RGBA would pull the transparent background's colour
        (which is black, and meaningless) into every edge pixel, leaving a dark
        halo around the icon once it is composited onto a light background.
        """
        s = self.ss
        n = s * s
        out = bytearray(self.w * self.h * 4)
        src = self._px
        for y in range(self.h):
            for x in range(self.w):
                r = g = b = a = 0
                for yy in range(y * s, y * s + s):
                    base = (yy * self._w + x * s) * 4
                    for k in range(0, s * 4, 4):
                        pa = src[base + k + 3]
                        if pa:
                            r += src[base + k]
                            g += src[base + k + 1]
                            b += src[base + k + 2]
                            a += pa
                i = (y * self.w + x) * 4
                if a:
                    cover = a // 255            # number of opaque subpixels
                    out[i] = r // cover
                    out[i + 1] = g // cover
                    out[i + 2] = b // cover
                    out[i + 3] = a // n
        return out

    def write(self, path: Path) -> None:
        px = self._downsample()
        raw = bytearray()
        for y in range(self.h):
            raw.append(0)                                   # filter type 0
            raw += px[y * self.w * 4:(y + 1) * self.w * 4]

        def chunk(tag: bytes, data: bytes) -> bytes:
            return (struct.pack(">I", len(data)) + tag + data
                    + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(
            b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + chunk(b"IEND", b""))


def _isqrt_clamped(value: int) -> int:
    """isqrt that treats a negative radicand as 0, so callers need no guard.

    math.isqrt, not value ** 0.5: the supersampled radii run into the hundreds
    of thousands squared, where float sqrt can land a unit either side of the
    true root and leave a one-subpixel notch in an arc.
    """
    return 0 if value <= 0 else math.isqrt(value)
