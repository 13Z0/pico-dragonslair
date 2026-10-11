#!/usr/bin/env python3
"""
Draws the ld2pico icon, a laserdisc with an SD card in front of it, and
writes ld2pico.ico with every size Windows asks for. Plain Python, so nothing
has to be installed; run it again after changing the drawing. The rasteriser
and the file writers are those of pico-bootLoader's updateAll/win/make_icon.py.

Usage: make_icon.py [--preview PNG]
"""

import math
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
SIZES = (16, 20, 24, 32, 40, 48, 64, 256)
SS = 8   # samples per pixel in each direction


def hexrgb(s):
    return tuple(int(s[i:i + 2], 16) / 255 for i in (1, 3, 5))


DISC_EDGE = hexrgb("#4b5563")
DISC = hexrgb("#cfd6de")
SHEEN = (hexrgb("#e9a3c9"), hexrgb("#a3d3e9"), hexrgb("#e9e3a3"))
LABEL = hexrgb("#7a2d8c")
HOLE = hexrgb("#f4f4f4")
CARD_EDGE = hexrgb("#16324f")
CARD = hexrgb("#2f6496")
CONTACT = hexrgb("#e9c25c")


# --- Shapes, in a 0..1 square with y pointing down -------------------------

def circle(cx, cy, r, n=48):
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def sector(cx, cy, r0, r1, a0, a1, n=12):
    """A piece of a ring between radii r0 and r1, angles a0..a1 in degrees."""
    outer = [(cx + r1 * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
              cy + r1 * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]
    inner = [(cx + r0 * math.cos(math.radians(a1 - (a1 - a0) * i / n)),
              cy + r0 * math.sin(math.radians(a1 - (a1 - a0) * i / n))) for i in range(n + 1)]
    return outer + inner


def card(x0, y0, x1, y1, cut):
    """An SD card: a rectangle with the top right corner cut off."""
    return [(x0, y0), (x1 - cut, y0), (x1, y0 + cut), (x1, y1), (x0, y1)]


def rect(x0, y0, x1, y1):
    return [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]


def drawing(size):
    """(polygon, colour) pairs, back to front, for one icon size."""
    small = size < 32
    cx, cy, r = 0.43, 0.43, 0.41
    shapes = [(circle(cx, cy, r + max(1 / size, 0.02)), DISC_EDGE), (circle(cx, cy, r), DISC)]
    if not small:
        # the rainbow sheen of a laserdisc, two opposite pieces of a ring
        for i, colour in enumerate(SHEEN):
            a = 200 + 14 * i
            shapes.append((sector(cx, cy, 0.17, r - 0.03, a, a + 14), colour))
            shapes.append((sector(cx, cy, 0.17, r - 0.03, a + 180, a + 194), colour))
    shapes.append((circle(cx, cy, 0.14), LABEL))
    shapes.append((circle(cx, cy, 0.035 if not small else 0.05), HOLE))
    # the SD card in front, bottom right
    x0, y0, x1, y1 = 0.56, 0.48, 0.95, 0.97
    edge = max(1 / size, 0.025)
    shapes.append((card(x0 - edge, y0 - edge, x1 + edge, y1 + edge, 0.12), CARD_EDGE))
    shapes.append((card(x0, y0, x1, y1, 0.11), CARD))
    if not small:
        for i in range(4):
            x = 0.61 + i * 0.075
            shapes.append((rect(x, 0.53, x + 0.045, 0.64), CONTACT))
    return shapes


# --- Rasteriser -------------------------------------------------------------

def coverage(poly, size):
    """Fraction of each pixel the polygon covers (non-zero winding)."""
    cov = [[0.0] * size for _ in range(size)]
    pts = [(x * size, y * size) for x, y in poly]
    edges = [(pts[i - 1], pts[i]) for i in range(len(pts))]
    w = 1 / SS
    for sy in range(size * SS):
        y = (sy + 0.5) / SS
        xs = []
        for (x0, y0), (x1, y1) in edges:
            if (y0 <= y < y1) or (y1 <= y < y0):
                xs.append((x0 + (y - y0) * (x1 - x0) / (y1 - y0), 1 if y1 > y0 else -1))
        xs.sort()
        row = cov[int(y)]
        wind, start = 0, 0.0
        for x, d in xs:
            before, wind = wind, wind + d
            if not before and wind:
                start = x
            elif before and not wind:
                a, b = max(start, 0.0), min(x, float(size))
                p = int(a)
                while p < b and p < size:
                    row[p] += (min(b, p + 1) - max(a, p)) * w
                    p += 1
    return cov


def render(size):
    """RGBA rows, premultiplied while compositing, straight alpha out."""
    img = [[[0.0, 0.0, 0.0, 0.0] for _ in range(size)] for _ in range(size)]
    for poly, (r, g, b) in drawing(size):
        cov = coverage(poly, size)
        for y in range(size):
            for x in range(size):
                a = min(cov[y][x], 1.0)
                if a <= 0:
                    continue
                p = img[y][x]
                p[0] = r * a + p[0] * (1 - a)
                p[1] = g * a + p[1] * (1 - a)
                p[2] = b * a + p[2] * (1 - a)
                p[3] = a + p[3] * (1 - a)
    rows = []
    for y in range(size):
        row = bytearray()
        for r, g, b, a in img[y]:
            if a > 0:
                r, g, b = r / a, g / a, b / a
            row += bytes(round(min(max(v, 0), 1) * 255) for v in (r, g, b, a))
        rows.append(bytes(row))
    return rows


def png(rows, width, height):
    raw = b"".join(b"\0" + row for row in rows)

    def chunk(kind, data):
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def ico(images):
    """An .ico of PNG images, which Windows reads from Vista on."""
    head = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, data = b"", b""
    for size, blob in images:
        entries += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
        data += blob
    return head + entries + data


def main():
    images, renders = [], {}
    for size in SIZES:
        rows = render(size)
        renders[size] = rows
        images.append((size, png(rows, size, size)))
    out = os.path.join(HERE, "ld2pico.ico")
    with open(out, "wb") as f:
        f.write(ico(images))
    print(f"wrote {out} ({os.path.getsize(out)} bytes)")

    if len(sys.argv) == 3 and sys.argv[1] == "--preview":
        # Every size side by side on a light and a dark background, each
        # small size also enlarged 4x, for a look at the result.
        width = sum(SIZES) + sum(s * 4 for s in SIZES if s < 64) + 10 * (2 * len(SIZES))
        height = 256 + 20
        canvas = []
        for bg in ((236, 236, 236), (32, 32, 32)):
            band = [[list(bg) for _ in range(width)] for _ in range(height)]
            x = 10
            for size in SIZES:
                for scale in ((1, 4) if size < 64 else (1,)):
                    rows = renders[size]
                    for y in range(size * scale):
                        src = rows[y // scale]
                        for xx in range(size * scale):
                            r, g, b, a = src[(xx // scale) * 4:(xx // scale) * 4 + 4]
                            d = band[10 + y][x + xx]
                            for c, v in enumerate((r, g, b)):
                                d[c] = round(v * a / 255 + d[c] * (1 - a / 255))
                    x += size * scale + 10
            canvas += band
        rows = [bytes(v for px in row for v in px + [255]) for row in canvas]
        with open(sys.argv[2], "wb") as f:
            f.write(png(rows, width, len(canvas)))
        print(f"wrote {sys.argv[2]}")


if __name__ == "__main__":
    main()
