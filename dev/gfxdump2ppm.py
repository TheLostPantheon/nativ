#!/usr/bin/env python3
"""Decode the hardware oracle's scene dumps (the Vita agent's gfxdump plugin, vita-agent repository: fNNNNN_sSS.bin, a 32-byte header
{magic 'GXDP', frame, scene, SceGxmColorFormat, surface type, stride (pixels), width, height} and
the surface's raw bytes) into PPM images, for comparing against gfxreplay's RECOMP_REPLAY_DRAWS
output of the same scene.  usage: gfxdump2ppm.py <dump.bin> [out.ppm]

Formats: the families gfx_soft.c's targets use (U8U8U8U8 ABGR/ARGB/RGBA/BGRA, U8, U5U6U5,
F16F16F16F16, U2F10F10F10, F11F11F10, F32 and F32F32 shown as their first word's bytes). Layout:
0 linear, 1 tiled (32x32 tiles), 2 swizzled (Morton). HDR values are clamped to 0..1."""
import struct, sys


def half(h):
    s, e, m = (h >> 15) & 1, (h >> 10) & 31, h & 1023
    if e == 0: v = m / 1024.0 * 2.0 ** -14
    elif e == 31: v = float('inf')
    else: v = (1 + m / 1024.0) * 2.0 ** (e - 15)
    return -v if s else v


def f10(v):   # unsigned 10-bit float: 5 exponent, 5 mantissa
    e, m = (v >> 5) & 31, v & 31
    if e == 0: return m / 32.0 * 2.0 ** -14
    if e == 31: return float('inf')
    return (1 + m / 32.0) * 2.0 ** (e - 15)


def f11(v):   # unsigned 11-bit float: 5 exponent, 6 mantissa
    e, m = (v >> 6) & 31, v & 63
    if e == 0: return m / 64.0 * 2.0 ** -14
    if e == 31: return float('inf')
    return (1 + m / 64.0) * 2.0 ** (e - 15)


def index(x, y, w, h, stride, typ):
    if typ == 1:   # tiled: 32x32 tiles, stride/32 per tile row
        tx = max(1, stride // 32)
        return ((y // 32) * tx + x // 32) * 1024 + (y % 32) * 32 + x % 32
    if typ == 2:   # swizzled: Morton over the power-of-two square, the longer side's extra bits above
        pw = ph = 1
        while pw < w: pw <<= 1
        while ph < h: ph <<= 1
        m = min(pw, ph); idx = 0; bit = 0; k = 1
        while k < m:
            idx |= (1 << bit if y & k else 0) | (2 << bit if x & k else 0); k <<= 1; bit += 2
        rest = (x // m) if pw > ph else (y // m)
        return idx + rest * m * m
    return y * stride + x


def bpp_of(fmt):
    fam = fmt & 0xF1800000
    if fam in (0x30000000, 0x40000000, 0x50000000, 0x60000000, 0xF0000000, 0x20800000, 0x40800000, 0xA0800000, 0xB0800000, 0xC0800000): return 2
    if fam in (0x80800000, 0x90800000): return 1
    if fam in (0x01000000, 0x11000000): return 8
    return 4


def decode(fmt, data, off, bpp):
    fam = fmt & 0xF1800000
    c = lambda v: max(0, min(255, int(v * 255 + 0.5)))
    if bpp == 4 and fam == 0x00000000:   # U8U8U8U8: the swizzle (bits 12-14 of a texture format; for colour formats the low nibble of the high byte)
        b = data[off:off + 4]; sw = (fmt >> 12) & 7
        if sw == 1: return (b[2], b[1], b[0])     # ARGB
        if sw == 2: return (b[3], b[2], b[1])     # RGBA
        if sw == 3: return (b[1], b[2], b[3])     # BGRA
        return (b[0], b[1], b[2])                 # ABGR: bytes R G B A
    if fam == 0x41000000:   # U2F10F10F10 (A2 high): bits 0-9 R, 10-19 G, 20-29 B
        p = struct.unpack_from('<I', data, off)[0]
        return (c(f10(p & 1023)), c(f10((p >> 10) & 1023)), c(f10((p >> 20) & 1023)))
    if fam == 0x21000000:   # F11F11F10
        p = struct.unpack_from('<I', data, off)[0]
        return (c(f11(p & 2047)), c(f11((p >> 11) & 2047)), c(f10((p >> 22) & 1023)))
    if fam == 0x01000000:   # F16F16F16F16
        r, g, b = (half(struct.unpack_from('<H', data, off + 2 * k)[0]) for k in range(3))
        return (c(r), c(g), c(b))
    if fam == 0x80800000:   # U8 (one component)
        v = data[off]; return (v, v, v)
    if fam == 0x30000000:   # U5U6U5
        p = struct.unpack_from('<H', data, off)[0]
        return (((p >> 11) & 31) * 255 // 31, ((p >> 5) & 63) * 255 // 63, (p & 31) * 255 // 31)
    if fam == 0x10800000:   # F32
        v = struct.unpack_from('<f', data, off)[0]; return (c(v), c(v), c(v))
    b = data[off:off + 3]; return (b[0], b[1], b[2])   # F32F32 and the rest: first bytes raw


def main():
    src = sys.argv[1]; dst = sys.argv[2] if len(sys.argv) > 2 else src.rsplit('.', 1)[0] + '.ppm'
    d = open(src, 'rb').read()
    magic, frame, scene, fmt, typ, stride, w, h = struct.unpack_from('<8I', d, 0)
    assert magic == 0x50445847, 'not a gfxdump file'
    px = d[32:]; bpp = bpp_of(fmt)
    out = bytearray()
    for y in range(h):
        for x in range(w):
            off = index(x, y, w, h, stride, typ) * bpp
            out += bytes(decode(fmt, px, off, bpp)) if off + bpp <= len(px) else b'\0\0\0'
    open(dst, 'wb').write(b'P6\n%d %d\n255\n' % (w, h) + out)
    print(f'{src}: frame {frame} scene {scene} fmt {fmt:#010x} type {typ} stride {stride} {w}x{h} -> {dst}')


if __name__ == '__main__':
    main()
