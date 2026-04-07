#!/usr/bin/env python3
"""Generates a simple 128x128 icon PNG for the Audiobook app."""
import struct, zlib, os

W, H = 128, 128

def png_bytes(pixels):
    def chunk(tag, data):
        c = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', c)
    raw = b''.join(b'\x00' + bytes([v for rgb in row for v in rgb]) for row in pixels)
    return (b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(raw))
            + chunk(b'IEND', b''))

# Build pixel grid
BG   = (26, 26, 46)    # dark navy
BLUE = (79, 195, 247)  # accent blue
WHITE = (238, 238, 238)

pixels = [[BG] * W for _ in range(H)]

# Rounded rect border
def in_rounded_rect(x, y, rx, ry, rw, rh, radius):
    if x < rx or x >= rx+rw or y < ry or y >= ry+rh:
        return False
    cx1, cy1 = rx + radius, ry + radius
    cx2, cy2 = rx + rw - 1 - radius, ry + rh - 1 - radius
    px = min(max(x, cx1), cx2)
    py = min(max(y, cy1), cy2)
    return (x - px)**2 + (y - py)**2 <= radius**2

for y in range(H):
    for x in range(W):
        if in_rounded_rect(x, y, 8, 8, W-16, H-16, 18):
            pixels[y][x] = (36, 36, 66)

# Headphone shape (simplified: two circles + arc + band)
cx, cy = W//2, H//2 + 8

# Headband arc (thick curve at top)
import math
for angle_deg in range(0, 181, 1):
    angle = math.radians(angle_deg)
    for r in range(28, 36):
        bx = int(cx - r * math.cos(angle))
        by = int(cy - r * math.sin(angle))
        if 0 <= bx < W and 0 <= by < H:
            pixels[by][bx] = BLUE

# Left ear cup
for y in range(H):
    for x in range(W):
        dx, dy = x - (cx - 30), y - (cy + 10)
        d = math.sqrt(dx*dx + dy*dy)
        if 10 <= d <= 18:
            pixels[y][x] = BLUE
        elif d < 10:
            pixels[y][x] = (50, 110, 160)

# Right ear cup
for y in range(H):
    for x in range(W):
        dx, dy = x - (cx + 30), y - (cy + 10)
        d = math.sqrt(dx*dx + dy*dy)
        if 10 <= d <= 18:
            pixels[y][x] = BLUE
        elif d < 10:
            pixels[y][x] = (50, 110, 160)

out_path = os.path.join(os.path.dirname(__file__), '..', 'assets', 'icon.png')
with open(out_path, 'wb') as f:
    f.write(png_bytes(pixels))

print(f"Icon written to {os.path.abspath(out_path)}")
