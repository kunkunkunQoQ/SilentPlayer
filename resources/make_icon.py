# -*- coding: utf-8 -*-
"""Generate resources/app.ico for SilentPlayer.

Dark rounded-square background with a white play triangle.
Sizes: 16 / 32 / 48 / 256. Entries are PNG-compressed (supported on Windows Vista+).
Run: python resources/make_icon.py
"""
import struct
import zlib
import os

SIZES = [16, 32, 48, 256]
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "app.ico")


def rounded_square_coverage(size, radius, x, y):
    """4x4 supersampled coverage of a rounded rectangle (0..size)."""
    cover = 0
    left, top, right, bottom = 0.0, 0.0, float(size), float(size)
    for sy in range(4):
        for sx in range(4):
            px = x + (sx + 0.5) / 4.0
            py = y + (sy + 0.5) / 4.0
            inside = False
            if px < left + radius and py < top + radius:
                cx, cy = left + radius, top + radius
                inside = (px - cx) ** 2 + (py - cy) ** 2 <= radius * radius
            elif px > right - radius and py < top + radius:
                cx, cy = right - radius, top + radius
                inside = (px - cx) ** 2 + (py - cy) ** 2 <= radius * radius
            elif px < left + radius and py > bottom - radius:
                cx, cy = left + radius, bottom - radius
                inside = (px - cx) ** 2 + (py - cy) ** 2 <= radius * radius
            elif px > right - radius and py > bottom - radius:
                cx, cy = right - radius, bottom - radius
                inside = (px - cx) ** 2 + (py - cy) ** 2 <= radius * radius
            else:
                inside = (left <= px <= right) and (top <= py <= bottom)
            cover += 1 if inside else 0
    return cover / 16.0


def in_triangle(px, py, a, b, c):
    def sign(p1, p2, p3):
        return (p1[0] - p3[0]) * (p2[1] - p3[1]) - (p2[0] - p3[0]) * (p1[1] - p3[1])

    d1 = sign((px, py), a, b)
    d2 = sign((px, py), b, c)
    d3 = sign((px, py), c, a)
    neg = d1 < 0 or d2 < 0 or d3 < 0
    pos = d1 > 0 or d2 > 0 or d3 > 0
    return not (neg and pos)


def render(size):
    bg = (43, 46, 51)        # #2B2E33 dark slate
    fg = (255, 255, 255)     # white triangle
    radius = max(2.0, size * 0.20)
    tri = (
        (size * 0.44, size * 0.33),
        (size * 0.44, size * 0.67),
        (size * 0.70, size * 0.50),
    )
    pixels = [[(0, 0, 0, 0)] * size for _ in range(size)]
    for y in range(size):
        for x in range(size):
            cov_bg = rounded_square_coverage(size, radius, x + 0.5, y + 0.5)
            if cov_bg <= 0.0:
                continue
            cov_tri = 0
            for sy in range(4):
                for sx in range(4):
                    qx = x + (sx + 0.5) / 4.0
                    qy = y + (sy + 0.5) / 4.0
                    if in_triangle(qx, qy, *tri):
                        cov_tri += 1
            cov_tri /= 16.0
            frac = min(1.0, cov_tri)
            r = int(bg[0] * (1 - frac) + fg[0] * frac)
            g = int(bg[1] * (1 - frac) + fg[1] * frac)
            b = int(bg[2] * (1 - frac) + fg[2] * frac)
            pixels[y][x] = (r, g, b, int(round(cov_bg * 255)))
    return pixels


def png_chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(
        ">I", zlib.crc32(tag + data) & 0xFFFFFFFF
    )


def encode_png(size, pixels):
    def chunk(tag, data):
        return png_chunk(tag, data)

    ihdr = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)
    raw = b""
    for y in range(size):
        raw += b"\x00"
        for x in range(size):
            r, g, b, a = pixels[y][x]
            raw += bytes((r, g, b, a))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(raw, 9))
        + chunk(b"IEND", b"")
    )


def main():
    entries = []
    data_blocks = []
    offset = 6 + 16 * len(SIZES)
    for size in SIZES:
        png = encode_png(size, render(size))
        entries.append(
            struct.pack(
                "<BBBBHHII",
                size & 0xFF if size < 256 else 0,
                size & 0xFF if size < 256 else 0,
                0,
                0,
                1,
                32,
                len(png),
                offset,
            )
        )
        data_blocks.append(png)
        offset += len(png)

    with open(OUT, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(SIZES)))
        for e in entries:
            f.write(e)
        for d in data_blocks:
            f.write(d)
    print("wrote", OUT, os.path.getsize(OUT), "bytes")


if __name__ == "__main__":
    main()
