#!/usr/bin/env python3
"""Draw the Rockbeat icons straight from the design's geometry.

    python3 tools/make_icon.py

Writes three files from ONE definition of the mark:

    watch/resources/images/menu_icon.png    25x25   ships in the app
    developer-portal/app_icon/app_icon_80.png    80x80    store artwork
    developer-portal/app_icon/app_icon_144.png   144x144  store artwork

Only the 25x25 is a resource -- see package.json's media[], which is the whole
of what gets bundled. The other two are uploaded to the Pebble developer portal
by hand and cost the app nothing.

The mark is the game itself in miniature: two lanes as horizontal bars, each
running into the hit target at its end. That is why the bars are different
lengths -- the two targets sit at different points across the mark, which is
what gives it a diagonal rather than the stacked look of a list row.

The targets are HOLLOW rings, and that is the whole point of the silhouette. A
horizontal bar ending in a SOLID knob is the universal settings-sliders glyph,
so the earlier mark -- two bars with a filled dot on each -- read as a settings
icon in the launcher list and vanished among the system apps. Hollow is what
distinguishes it, because a slider knob is never hollow.

The two sizes are drawn DIFFERENTLY on purpose, because they are seen
differently. At 25px the icon is a silhouette on the watch's own launcher, so it
is one flat colour with no background plate. At 80/144 it is an app tile in a
list on a phone, so it gets the design's black rounded plate and the lane accent
colours, which is what makes it recognisable next to the screenshots.
"""

from __future__ import annotations

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))

import pngkit
from pngkit import Canvas

ROOT = Path(__file__).resolve().parents[1]
MENU_ICON = ROOT / "watch/resources/images/menu_icon.png"
STORE_DIR = ROOT / "developer-portal/app_icon"

# ---------------------------------------------------------------------------
# The mark, in the design's own 25-unit coordinates. Everything else scales
# from these four numbers so the large icons cannot drift from the small one.
# ---------------------------------------------------------------------------

BARS = ((1, 6, 8, 3), (1, 16, 11, 3))     # (x, y, w, h) -- the two lanes
RINGS = ((13, 7, 6, 2), (17, 17, 6, 2))   # (cx, cy, r, width) -- the hit target

# The white outline the design puts round each target in the large icons, in the
# same 25-unit space. 0.9 units lands on 5px at 144, which is what the design
# says.
RING_U = 0.9

MENU_SIZE = 25

# The mark is inset this fraction of the tile on every side. App tiles are
# viewed at thumbnail size next to other tiles, so the mark wants air around it
# rather than filling the plate edge to edge.
STORE_INSET = 0.14

# Plate corner radius as a fraction of the tile -- the standard rounded-square
# app tile, not a circle and not a hard square.
STORE_CORNER = 0.22


def _mark_bounds() -> tuple[float, float, float, float]:
    """The mark's extent in unit space, rings included."""
    xs, ys = [], []
    for bx, by, bw, bh in BARS:
        xs += [bx, bx + bw]
        ys += [by, by + bh]
    for cx, cy, r, _width in RINGS:
        xs += [cx - r - RING_U, cx + r + RING_U]
        ys += [cy - r - RING_U, cy + r + RING_U]
    return min(xs), min(ys), max(xs), max(ys)


def draw_menu_icon() -> Canvas:
    """25x25, BLACK on transparent, at exactly the design's pixel coordinates.

    Black rather than white: the launcher composites this over its own
    background, and on the light tiles that background is, a white mark
    disappears entirely.

    Transparent rather than a filled plate for the same reason -- a plate would
    show as a box in whatever colour it was, against a background this icon does
    not get to choose.

    ss=1, i.e. no antialiasing: at 25px a soft edge just looks muddy, and the
    SDK quantises this to 1-bit greyscale plus a tRNS anyway, so no grey would
    survive to be muddy with. `bias=r` on the rings reproduces the watch's own
    circle fill so the shape matches what render.c draws -- and without it the
    pole of each ring is a single pixel, which reads as a speck.
    """
    c = Canvas(MENU_SIZE, MENU_SIZE, ss=1)
    for bx, by, bw, bh in BARS:
        c.rect(bx, by, bw, bh, pngkit.BLACK)
    for cx, cy, r, width in RINGS:
        c.ring(cx, cy, r, width, pngkit.BLACK, bias=r)
    return c


def draw_store_icon(size: int) -> Canvas:
    """80x80 or 144x144: the design's black plate, lane accents, white rings."""
    c = Canvas(size, size, ss=4)
    c.round_rect(0, 0, size, size, round(size * STORE_CORNER), pngkit.BLACK)

    # Fit the mark into the inset box and centre it. Scaling from the measured
    # bounds rather than from size/25 is what keeps the margins even -- the mark
    # is wider than it is tall, and is not centred in its own 25x25 artboard.
    min_x, min_y, max_x, max_y = _mark_bounds()
    avail = size * (1.0 - 2.0 * STORE_INSET)
    scale = avail / (max_x - min_x)
    ox = (size - (max_x - min_x) * scale) / 2.0 - min_x * scale
    oy = (size - (max_y - min_y) * scale) / 2.0 - min_y * scale

    def sx(v: float) -> int:
        return round(ox + v * scale)

    def sy(v: float) -> int:
        return round(oy + v * scale)

    accents = (pngkit.LANE_TOP_ACC, pngkit.LANE_BOT_ACC)
    ring = max(1, round(RING_U * scale))

    for accent, (bx, by, bw, bh) in zip(accents, BARS):
        # Rounded caps: at this size a square-ended bar reads as a crop mark.
        h = sy(by + bh) - sy(by)
        c.round_rect(sx(bx), sy(by), sx(bx + bw) - sx(bx), h, h // 2, accent)

    # Hollow here too. The geometry describes a hit target, not a note dot, so
    # filling it would make the tile a fat blob with no relation to the 25px
    # silhouette -- and the two are meant to be the same mark.
    for accent, (cx, cy, r, width) in zip(accents, RINGS):
        radius = round(r * scale)
        wpx = max(1, round(width * scale))
        # White first, wider on both edges, so the accent annulus keeps an
        # outline against the black plate exactly as the bars keep theirs.
        c.ring(sx(cx), sy(cy), radius + ring, wpx + 2 * ring, pngkit.WHITE)
        c.ring(sx(cx), sy(cy), radius, wpx, accent)

    return c


def main() -> None:
    draw_menu_icon().write(MENU_ICON)
    print(f"wrote {MENU_ICON.relative_to(ROOT)} "
          f"({MENU_ICON.stat().st_size} bytes, {MENU_SIZE}x{MENU_SIZE}, black on transparent)")

    for size in (80, 144):
        out = STORE_DIR / f"app_icon_{size}.png"
        draw_store_icon(size).write(out)
        print(f"wrote {out.relative_to(ROOT)} ({out.stat().st_size} bytes, {size}x{size})")


if __name__ == "__main__":
    main()
