#!/usr/bin/env python3
"""make.py <folder>: prompts.png, the touch card's input icons (runtime/ui/overlay.cpp), from Kenney's Input Prompts.

<folder> is the pack unzipped: Input Prompts 1.5A from https://kenney.nl/assets/input-prompts (CC0, License.txt here
is its own; the zip we used had SHA-256 ac2fcf599080b0f3ba2d174c9474db6df1a0e96ff0662580e2da79a122ab78a1).

prompts.png is one 8-bit grey image: a grid of 128-pixel cells, COLS across, in ICONS' order (overlay.cpp's Prompt
enum is the same list), each cell an icon's coverage. Kenney's icons are white, with their own red where a press is
marked (a mouse button): a "red" entry keeps only that part, so the overlay can draw the plain icon and light the
part in its own accent colour."""
import os
import struct
import sys
import zlib

KM, PS, XB, NS, TO = "Keyboard & Mouse", "PlayStation Series", "Xbox Series", "Nintendo Switch", "Touch"
ICONS = [   # (folder, file, part)
    (KM, "keyboard_option", "white"), (KM, "keyboard_option_outline", "white"),
    (KM, "keyboard_alt", "white"), (KM, "keyboard_alt_outline", "white"),
    (KM, "mouse", "white"), (KM, "mouse_left", "red"), (KM, "mouse_right", "red"),
    (TO, "touch_finger_one", "white"),
    # each pad family: the left bumper held and up, the right bumper held and up, the south button pressed and up, the left stick
    (PS, "playstation_trigger_l1", "white"), (PS, "playstation_trigger_l1_outline", "white"),
    (PS, "playstation_trigger_r1", "white"), (PS, "playstation_trigger_r1_outline", "white"),
    (PS, "playstation_button_cross", "white"), (PS, "playstation_button_cross_outline", "white"), (PS, "playstation_stick_l", "white"),
    (XB, "xbox_lb", "white"), (XB, "xbox_lb_outline", "white"), (XB, "xbox_rb", "white"), (XB, "xbox_rb_outline", "white"),
    (XB, "xbox_button_a", "white"), (XB, "xbox_button_a_outline", "white"), (XB, "xbox_stick_l", "white"),
    (NS, "switch_button_l", "white"), (NS, "switch_button_l_outline", "white"), (NS, "switch_button_r", "white"),
    (NS, "switch_button_r_outline", "white"), (NS, "switch_button_b", "white"), (NS, "switch_button_b_outline", "white"),
    (NS, "switch_stick_l", "white"),
]
CELL, COLS = 128, 8


def read_png(path):   # (w, h, [(r, g, b, a)] row by row): Kenney's are palette images of 1 to 8 bits, the rest handled too
    b = open(path, "rb").read()
    assert b[:8] == b"\x89PNG\r\n\x1a\n", path
    p, idat, plte, trns = 8, b"", None, b""
    while p < len(b):
        n, t = struct.unpack(">I4s", b[p:p + 8]); d = b[p + 8:p + 8 + n]; p += 12 + n
        if t == b"IHDR": w, h, depth, ct, _, _, interlace = struct.unpack(">IIBBBBB", d); assert not interlace, path
        elif t == b"PLTE": plte = [tuple(d[i:i + 3]) for i in range(0, len(d), 3)]
        elif t == b"tRNS": trns = d
        elif t == b"IDAT": idat += d
    raw = zlib.decompress(idat)
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]; bpp = max(1, ch * depth // 8); stride = (w * ch * depth + 7) // 8
    out, prev, q = [], bytearray(stride), 0
    for _ in range(h):
        f, line = raw[q], bytearray(raw[q + 1:q + 1 + stride]); q += 1 + stride
        for i in range(stride):   # (PNG's per-row filters undone)
            a = line[i - bpp] if i >= bpp else 0; u = prev[i]; c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + u) & 255
            elif f == 3: line[i] = (line[i] + (a + u) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(u - c), abs(a - c), abs(a + u - 2 * c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else u if pb <= pc else c)) & 255
        prev = line
        if depth < 8: v = [(line[x * depth // 8] >> (8 - depth - x * depth % 8)) & ((1 << depth) - 1) for x in range(w * ch)]
        else: v = list(line)
        for x in range(w):
            if ct == 3: i = v[x]; out.append(plte[i] + ((trns[i] if i < len(trns) else 255),))
            elif ct == 6: out.append(tuple(v[x * 4:x * 4 + 4]))
            elif ct == 4: out.append((v[x * 2],) * 3 + (v[x * 2 + 1],))
            elif ct == 2: out.append(tuple(v[x * 3:x * 3 + 3]) + (255,))
            else: out.append((v[x],) * 3 + (255,))
    return w, h, out


def main(argv):
    if len(argv) != 2: sys.exit(__doc__)
    rows = (len(ICONS) + COLS - 1) // COLS; W, H = COLS * CELL, rows * CELL
    img = bytearray(W * H)
    for k, (folder, name, part) in enumerate(ICONS):
        w, h, px = read_png(os.path.join(argv[1], folder, "Double", name + ".png"))
        assert (w, h) == (CELL, CELL), name
        ox, oy = k % COLS * CELL, k // COLS * CELL
        for y in range(h):
            for x in range(w):
                r, g, b, a = px[y * w + x]
                red = r > 150 and g < 120 and b < 140   # (Kenney's highlight red, 231 50 70)
                img[(oy + y) * W + ox + x] = a if red == (part == "red") else 0
    raw = b"".join(b"\0" + bytes(img[y * W:(y + 1) * W]) for y in range(H))
    chunk = lambda t, d: struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 0, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "prompts.png")
    open(out, "wb").write(png)
    print(f"{out}: {len(ICONS)} icons, {W}x{H}, {len(png)} bytes")


if __name__ == "__main__":
    main(sys.argv)
