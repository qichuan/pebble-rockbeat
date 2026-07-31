#!/usr/bin/env python3
"""Encode a directory of PNG frames into an animated GIF.

    python3 tools/make_gif.py FRAME_DIR OUT.gif [--delay-ms 50]

Used for the appstore's gameplay loop. The frames come from `pebble screenshot`
against an emulator built with RB_DEBUG_TIME_SCALE, which runs the song in slow
motion so that a plain capture loop samples it at even intervals -- see that
flag's comment in rb_config.h for why the obvious alternative (one
RB_DEBUG_FREEZE_AT_MS per frame) is unaffordable.

Stdlib only, like the rest of tools/. That means both halves are written out
here: a PNG reader (zlib plus the five row filters) and a GIF LZW encoder. The
alternative is Pillow or ffmpeg, and this repo's whole toolchain works from a
bare checkout with nothing installed -- the chart and music generators already
parse MIDI by hand for the same reason.

Quantisation is a NON-issue here and that is not luck: emery has a 64-colour
palette, so a screenshot of it cannot contain more than 64 distinct colours, and
GIF allows 256. The palette is therefore exact -- every frame is reproduced
pixel-for-pixel, with no dithering and no colour loss. This asserts that rather
than assuming it, because a frame from anywhere else would silently degrade.
"""

from __future__ import annotations

from pathlib import Path
import argparse
import struct
import sys
import zlib

GIF_MAX_COLOURS = 256


# ---------------------------------------------------------------------------
# PNG reading -- enough of the format for what `pebble screenshot` writes
# (8-bit RGBA), plus the greyscale/RGB/palette cases so this is not a trap for
# the next caller.
# ---------------------------------------------------------------------------

CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def read_png(path: Path) -> tuple[int, int, list[tuple[int, int, int]]]:
    """Return (width, height, pixels) with pixels as flat RGB triples."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit(f"{path}: not a PNG")

    idat = bytearray()
    plte: list[tuple[int, int, int]] = []
    pos, width, height, depth, colour = 8, 0, 0, 0, 0
    while pos < len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        tag = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if tag == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            if body[12] != 0:
                raise SystemExit(f"{path}: interlaced PNGs are not supported")
        elif tag == b"PLTE":
            plte = [tuple(body[i:i + 3]) for i in range(0, len(body), 3)]
        elif tag == b"IDAT":
            idat += body
        pos += 12 + length

    if depth != 8:
        raise SystemExit(f"{path}: only 8-bit channels are supported, got {depth}")
    if colour not in CHANNELS:
        raise SystemExit(f"{path}: unsupported colour type {colour}")

    n = CHANNELS[colour]
    raw = zlib.decompress(bytes(idat))
    stride = width * n
    out = bytearray(height * stride)

    # Undo the per-row filters. Each row's filter type byte precedes its data,
    # and filters 2..4 refer to the row above, which by then is already decoded.
    prev = bytearray(stride)
    src = 0
    for y in range(height):
        ftype = raw[src]
        src += 1
        row = bytearray(raw[src:src + stride])
        src += stride
        if ftype == 1:
            for i in range(n, stride):
                row[i] = (row[i] + row[i - n]) & 0xFF
        elif ftype == 2:
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif ftype == 3:
            for i in range(stride):
                left = row[i - n] if i >= n else 0
                row[i] = (row[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:
            for i in range(stride):
                a = row[i - n] if i >= n else 0
                b = prev[i]
                c = prev[i - n] if i >= n else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                row[i] = (row[i] + pred) & 0xFF
        elif ftype != 0:
            raise SystemExit(f"{path}: bad filter type {ftype} on row {y}")
        out[y * stride:(y + 1) * stride] = row
        prev = row

    pixels: list[tuple[int, int, int]] = []
    for i in range(0, len(out), n):
        if colour == 3:
            pixels.append(plte[out[i]])
        elif colour in (0, 4):
            pixels.append((out[i], out[i], out[i]))
        else:
            pixels.append((out[i], out[i + 1], out[i + 2]))
    return width, height, pixels


# ---------------------------------------------------------------------------
# GIF writing
# ---------------------------------------------------------------------------

class _BitWriter:
    """LSB-first bit packer -- the order GIF's variable-width codes use."""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.acc = 0
        self.nbits = 0

    def write(self, code: int, size: int) -> None:
        self.acc |= code << self.nbits
        self.nbits += size
        while self.nbits >= 8:
            self.buf.append(self.acc & 0xFF)
            self.acc >>= 8
            self.nbits -= 8

    def flush(self) -> bytes:
        if self.nbits:
            self.buf.append(self.acc & 0xFF)
            self.acc = 0
            self.nbits = 0
        return bytes(self.buf)


def lzw_encode(indices: list[int], min_code_size: int) -> bytes:
    clear, eoi = 1 << min_code_size, (1 << min_code_size) + 1
    out = _BitWriter()

    table: dict[tuple[int, ...], int] = {}
    code_size = 0
    next_code = 0

    def reset() -> None:
        nonlocal table, code_size, next_code
        table = {(i,): i for i in range(1 << min_code_size)}
        code_size = min_code_size + 1
        next_code = eoi + 1

    reset()
    out.write(clear, code_size)

    prefix: tuple[int, ...] = ()
    for value in indices:
        candidate = prefix + (value,)
        if candidate in table:
            prefix = candidate
            continue
        out.write(table[prefix], code_size)
        table[candidate] = next_code
        next_code += 1
        # The decoder's table runs exactly ONE entry behind this one: it cannot
        # add the entry for a pair until it has seen the code that follows it.
        # So the width must grow one entry LATER than "the table just filled" --
        # `> (1 << code_size)`, not `==`. Getting this wrong desynchronises the
        # two a few hundred pixels in, and the symptom is a GIF that every
        # viewer renders as garbage from that point on rather than an error.
        if code_size < 12:
            if next_code > (1 << code_size):
                code_size += 1
        elif next_code == (1 << 12):
            # 12 bits is the ceiling, so start the table over. Emitted while the
            # decoder still has room for its own lagging entry.
            out.write(clear, code_size)
            reset()
        prefix = (value,)

    if prefix:
        out.write(table[prefix], code_size)
    out.write(eoi, code_size)
    return out.flush()


def _sub_blocks(data: bytes) -> bytes:
    """GIF carries image data as length-prefixed blocks of at most 255 bytes."""
    out = bytearray()
    for i in range(0, len(data), 255):
        chunk = data[i:i + 255]
        out.append(len(chunk))
        out += chunk
    out.append(0)
    return bytes(out)


def write_gif(path: Path, width: int, height: int,
              frames: list[list[int]], palette: list[tuple[int, int, int]],
              delay_ms: int) -> None:
    bits = max(1, (len(palette) - 1).bit_length())
    size = 1 << bits

    out = bytearray(b"GIF89a")
    # Logical screen descriptor: global colour table present, `bits` deep.
    out += struct.pack("<HHBBB", width, height, 0xF0 | (bits - 1), 0, 0)
    for rgb in palette:
        out += bytes(rgb)
    out += bytes(3 * (size - len(palette)))

    # NETSCAPE2.0 with a loop count of 0 -- loop forever, which is what a store
    # listing wants.
    out += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00"

    # GIF delays are in hundredths of a second, so the frame interval has to be
    # a multiple of 10ms; anything finer is silently rounded by every viewer.
    delay_cs = max(1, round(delay_ms / 10))
    min_code_size = max(2, bits)
    for frame in frames:
        out += b"\x21\xF9\x04\x00" + struct.pack("<H", delay_cs) + b"\x00\x00"
        out += b"\x2C" + struct.pack("<HHHHB", 0, 0, width, height, 0)
        out += bytes([min_code_size]) + _sub_blocks(lzw_encode(frame, min_code_size))

    out += b"\x3B"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(out))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("frames", type=Path, help="directory of PNG frames, sorted by name")
    ap.add_argument("out", type=Path)
    ap.add_argument("--delay-ms", type=int, default=50,
                    help="per-frame delay; rounded to 10ms by the format")
    args = ap.parse_args()

    paths = sorted(args.frames.glob("*.png"))
    if not paths:
        raise SystemExit(f"no PNG frames in {args.frames}")

    palette: list[tuple[int, int, int]] = []
    index_of: dict[tuple[int, int, int], int] = {}
    frames: list[list[int]] = []
    width = height = 0

    for path in paths:
        w, h, pixels = read_png(path)
        if width == 0:
            width, height = w, h
        elif (w, h) != (width, height):
            raise SystemExit(f"{path}: {w}x{h} does not match {width}x{height}")
        frame = []
        for rgb in pixels:
            i = index_of.get(rgb)
            if i is None:
                if len(palette) == GIF_MAX_COLOURS:
                    # Would need quantisation, which this does not do -- and for
                    # a 64-colour platform it should never come up. Failing is
                    # honest; silently dithering a screenshot is not.
                    raise SystemExit(
                        f"{path}: more than {GIF_MAX_COLOURS} distinct colours. "
                        "These frames are not from a 64-colour Pebble screen.")
                i = len(palette)
                index_of[rgb] = i
                palette.append(rgb)
            frame.append(i)
        frames.append(frame)

    write_gif(args.out, width, height, frames, palette, args.delay_ms)
    print(f"wrote {args.out} ({args.out.stat().st_size} bytes, {width}x{height}, "
          f"{len(frames)} frames, {len(palette)} colours, {args.delay_ms}ms/frame)")


if __name__ == "__main__":
    main()
