#!/usr/bin/env python3
"""Convert a PNG/JPG wallpaper into a raw RGB C array embedded in a .c file.

Scales+crops (cover-fit) to a fixed target resolution baked into the
kernel, since the freestanding kernel has no image decoder of its own.
"""
import sys
from PIL import Image

def cover_fit(img, target_w, target_h):
    src_w, src_h = img.size
    src_ratio = src_w / src_h
    dst_ratio = target_w / target_h
    if src_ratio > dst_ratio:
        # source is wider than target -> crop left/right
        new_h = target_h
        new_w = int(new_h * src_ratio)
    else:
        new_w = target_w
        new_h = int(new_w / src_ratio)
    img = img.convert("RGB").resize((new_w, new_h), Image.LANCZOS)
    left = (new_w - target_w) // 2
    top = (new_h - target_h) // 2
    return img.crop((left, top, left + target_w, top + target_h))

def main():
    if len(sys.argv) != 5:
        print("usage: img_to_c.py <input.png> <symbol_name> <width> <height>", file=sys.stderr)
        sys.exit(1)

    in_path, symbol, w, h = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    img = Image.open(in_path)
    img = cover_fit(img, w, h)

    pixels = img.tobytes()  # RGB, 3 bytes/pixel, row-major

    print(f"/* Auto-generated from {in_path} by tools/img_to_c.py. Do not edit by hand. */")
    print('#include "kernel.h"')
    print()
    print(f"const int {symbol}_width = {w};")
    print(f"const int {symbol}_height = {h};")
    print(f"const unsigned char {symbol}_rgb[{w * h * 3}] = {{")

    line = []
    for i, b in enumerate(pixels):
        line.append(str(b))
        if len(line) == 24:
            print(",".join(line) + ",")
            line = []
    if line:
        print(",".join(line) + ",")

    print("};")

if __name__ == "__main__":
    main()
