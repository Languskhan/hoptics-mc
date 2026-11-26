#!/usr/bin/env python3
"""Made-up 256x256 map tiles (z/x/y.png) around Sacramento for the PC harness: land, a street grid, a river and a park,
drawn in OpenStreetMap-like colours so the dark and night themes' recolouring shows. Not real map data.

usage: make_test_tiles.py <outdir>
"""
import math
import os
import sys

from PIL import Image, ImageDraw

out = sys.argv[1]
LAT0, LAT1, LON0, LON1 = 38.45, 39.0, -121.55, -120.9


def tile_xy(lat, lon, z):
    n = 2 ** z
    x = (lon + 180) / 360 * n
    s = math.sin(math.radians(lat))
    y = (0.5 - math.log((1 + s) / (1 - s)) / (4 * math.pi)) * n
    return x, y


for z in range(8, 14):
    x0, y0 = tile_xy(LAT1, LON0, z)
    x1, y1 = tile_xy(LAT0, LON1, z)
    for tx in range(int(x0), int(x1) + 1):
        for ty in range(int(y0), int(y1) + 1):
            im = Image.new("RGB", (256, 256), (242, 239, 233))  # land
            d = ImageDraw.Draw(im)
            step = 256 // max(1, 2 ** (z - 9))  # streets: a grid fixed to the world, denser when zoomed in
            step = max(16, min(128, step if step else 16))
            for k in range(0, 256, step):
                for a, b in (((k, 0), (k, 255)), ((0, k), (255, k))):
                    d.line([a, b], fill=(205, 200, 190), width=4)
                    d.line([a, b], fill=(255, 255, 255), width=2)
            # a park and a river (in world pixels, so they line up across tiles)
            wx, wy = tx * 256, ty * 256
            px, py = tile_xy(38.62, -121.32, z)
            pr = 2 ** z * 256 * 0.00004
            d.ellipse([px * 256 - wx - pr, py * 256 - wy - pr, px * 256 - wx + pr, py * 256 - wy + pr], fill=(200, 230, 190))
            pts = []
            for i in range(41):
                lat = 38.55 + i * 0.01
                lon = -121.45 + 0.08 * math.sin(i / 6)
                rx, ry = tile_xy(lat, lon, z)
                pts.append((rx * 256 - wx, ry * 256 - wy))
            d.line(pts, fill=(170, 211, 223), width=max(3, 2 ** (z - 8)))
            os.makedirs(f"{out}/{z}/{tx}", exist_ok=True)
            im.save(f"{out}/{z}/{tx}/{ty}.png")
print("tiles written to", out)
