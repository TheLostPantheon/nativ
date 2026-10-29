#!/usr/bin/env python3
"""Project Nativ's brand assets, drawn from one geometry (the logo canvas's boards 10 and 11):

  the icon      a Vita in outline, its screen lit amber, "nativ" beneath in white -- drawn per size: from 48 px down
                the lines thicken and the letters spread; at 32 and 16 px the name goes (unreadable there) and the
                Vita fills the square
  the logo      the Vita with its screen empty, "nativ" beneath: the n white, "ativ" amber
  the wordmark  "nativ", monoline lowercase, the same colouring

  python3 make.py     writes the SVGs, renders the PNGs (headless Chrome), builds nativ.icns (iconutil, macOS's
                      inset grid) and nativ.ico. Only our own artwork: nothing of a game or of Sony's.
"""
import os
import shutil
import struct
import subprocess
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
INK, BODY, SCREEN, BG, MUTED = '#E9E6DF', '#1A1D24', '#0D0F13', '#121419', '#9097A3'
AMBER, AMBER_ON_LIGHT = '#F0B44C', '#B4791C'
CHROME = '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome'


def word_inner(n, ativ, stroke=14, sp=0):
    """'nativ' in its own units (x -6..282+4sp, y 14..110): stroke weight, and sp more room per letter for heavy strokes"""
    return (f'<g fill="none" stroke-width="{stroke}" stroke-linecap="round" stroke-linejoin="round">'
            f'<path d="M4 46 V100 M4 72 C4 54 14 46 26 46 C38 46 48 54 48 72 V100" stroke="{n}"/>'
            f'<circle cx="{101 + sp}" cy="73" r="27" stroke="{ativ}"/><path d="M{128 + sp} 46 V100" stroke="{ativ}"/>'
            f'<path d="M{161 + 2 * sp} 24 V84 Q{161 + 2 * sp} 100 {177 + 2 * sp} 100 M{149 + 2 * sp} 46 H{175 + 2 * sp}" stroke="{ativ}"/>'
            f'<path d="M{203 + 3 * sp} 46 V100" stroke="{ativ}"/>'
            f'<path d="M{225 + 4 * sp} 46 L{242 + 4 * sp} 92 Q{247 + 4 * sp} 104 {252 + 4 * sp} 92 L{269 + 4 * sp} 46" stroke="{ativ}"/></g>'
            f'<circle cx="{203 + 3 * sp}" cy="{25 - (stroke - 14) * 0.3:g}" r="{stroke * 0.6:g}" fill="{ativ}"/>')


def device_inner(body_w, controls, screen, line=INK):
    """the Vita in its own units (600 x 280): body, screen (960:544, ~60% of the width), controls full | sticks | none"""
    s = (f'<rect x="8" y="8" width="584" height="264" rx="132" fill="none" stroke="{line}" stroke-width="{body_w}"/>'
         f'<rect x="125" y="41" width="350" height="198" rx="12" fill="{screen}"/>')
    if controls == 'full':
        s += (f'<g fill="none" stroke="{line}" stroke-width="12" stroke-linecap="round"><path d="M50 108 H82 M66 92 V124"/>'
              f'<circle cx="74" cy="190" r="14" stroke-width="10"/><circle cx="526" cy="190" r="14" stroke-width="10"/></g>'
              f'<g fill="{line}"><circle cx="534" cy="90" r="8"/><circle cx="516" cy="108" r="8"/><circle cx="552" cy="108" r="8"/><circle cx="534" cy="126" r="8"/></g>')
    elif controls == 'sticks':
        s += f'<g fill="{line}"><circle cx="70" cy="140" r="22"/><circle cx="530" cy="140" r="22"/></g>'
    return s


# per drawn size: the Vita's line weight, its controls, the name's stroke and spread (None: no name)
LEVELS = {192: (14, 'full', 14, 0), 128: (16, 'full', 15, 2), 96: (20, 'sticks', 17, 5), 64: (26, 'sticks', 20, 9),
          48: (32, 'sticks', 24, 14), 32: (34, 'sticks', None, 0), 16: (46, 'none', None, 0)}


def level_for(px):   # the drawing for a size: the nearest level at or above it (1024, 512, 256: the full one)
    return min((lv for lv in LEVELS if lv >= px), default=192) if px <= 192 else 192


def icon_svg(level, size, inset=1.0):
    """the app icon (512 grid) drawn at `level`'s detail, `size` px; inset < 1: macOS's grid (the square smaller, centred)"""
    body_w, controls, ws, sp = LEVELS[level]
    pad = 512 * (1 - inset) / 2
    s = f'<svg xmlns="http://www.w3.org/2000/svg" width="{size}" height="{size}" viewBox="0 0 512 512">'
    s += f'<g transform="translate({pad:g} {pad:g}) scale({inset:g})"><rect width="512" height="512" rx="112" fill="{BODY}"/>'
    if ws is None:
        s += f'<svg x="28" y="135" width="456" height="213" viewBox="0 0 600 280">{device_inner(body_w, controls, AMBER)}</svg>'
    else:
        s += f'<svg x="80" y="70" width="352" height="164" viewBox="0 0 600 280">{device_inner(body_w, controls, AMBER)}</svg>'
        vbw = 288 + 4 * sp
        ww = 352 if sp < 10 else 380
        wh = ww * 104 / vbw
        s += f'<svg x="{256 - ww / 2:g}" y="{300 + (110 - wh) / 2:.1f}" width="{ww}" height="{wh:.1f}" viewBox="-8 10 {vbw + 4} 104">{word_inner(INK, INK, ws, sp)}</svg>'
    return s + '</g></svg>'


def wordmark_svg(n=INK, ativ=AMBER, width=288):
    h = round(width * 96 / 288)
    return f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{h}" viewBox="-6 14 288 96">{word_inner(n, ativ)}</svg>'


def logo_svg(n=INK, ativ=AMBER, line=INK):
    """the logo: the Vita, its screen empty, "nativ" beneath"""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="600" height="420" viewBox="0 0 600 420">'
            f'<svg x="0" y="0" width="600" height="280" viewBox="0 0 600 280">{device_inner(14, "full", SCREEN if line == INK else BG, line)}</svg>'
            f'<svg x="150" y="310" width="300" height="100" viewBox="-6 14 288 96">{word_inner(n, ativ)}</svg></svg>')


def banner_svg():
    """the README's banner: the logo side by side on the dark ground, the line under the name"""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="1200" height="360" viewBox="0 0 1200 360">'
            f'<rect width="1200" height="360" rx="32" fill="{BG}"/>'
            f'<svg x="110" y="70" width="470" height="219" viewBox="0 0 600 280">{device_inner(14, "full", SCREEN)}</svg>'
            f'<svg x="650" y="106" width="420" height="140" viewBox="-6 14 288 96">{word_inner(INK, AMBER)}</svg>'
            f'<text x="656" y="292" fill="{MUTED}" font-family="-apple-system, Segoe UI, Helvetica, Arial, sans-serif" font-size="26" '
            f'letter-spacing="0.5">Your PS Vita games, rebuilt natively</text></svg>')


def render(svg_text, png, size):
    with tempfile.NamedTemporaryFile('w', suffix='.svg', delete=False) as f:
        f.write(svg_text)
    try:
        subprocess.run([CHROME, '--headless=new', '--disable-gpu', '--hide-scrollbars', f'--screenshot={png}',
                        f'--window-size={size},{size}', '--default-background-color=00000000', 'file://' + f.name],
                       check=True, capture_output=True)
    finally:
        os.unlink(f.name)


def write_ico(pngs, path):   # an .ico of PNG-compressed images (Windows Vista and up), each entry its own size
    entries, data = [], b''
    offset = 6 + 16 * len(pngs)
    for size, png in pngs:
        blob = open(png, 'rb').read()
        entries.append(struct.pack('<BBBBHHII', size % 256, size % 256, 0, 0, 1, 32, len(blob), offset + len(data)))
        data += blob
    with open(path, 'wb') as f:
        f.write(struct.pack('<HHH', 0, 1, len(pngs)) + b''.join(entries) + data)


def main():
    out = HERE
    for level in LEVELS:
        open(os.path.join(out, f'icon-{level}.svg'), 'w').write(icon_svg(level, level))
    open(os.path.join(out, 'icon.svg'), 'w').write(icon_svg(192, 512))
    open(os.path.join(out, 'wordmark.svg'), 'w').write(wordmark_svg())
    open(os.path.join(out, 'wordmark-on-light.svg'), 'w').write(wordmark_svg(BG, AMBER_ON_LIGHT))
    open(os.path.join(out, 'logo.svg'), 'w').write(logo_svg())
    open(os.path.join(out, 'logo-on-light.svg'), 'w').write(logo_svg(BG, AMBER_ON_LIGHT, BG))
    open(os.path.join(out, 'banner.svg'), 'w').write(banner_svg())
    png_dir = os.path.join(out, 'png')
    os.makedirs(png_dir, exist_ok=True)
    for px in (16, 32, 48, 64, 96, 128, 256, 512, 1024):   # the PNG set (Linux's hicolor sizes, the launcher's window icon)
        render(icon_svg(level_for(px), px), os.path.join(png_dir, f'icon-{px}.png'), px)
    # macOS: the iconset at Apple's grid (the square at 824/1024, centred), each point size's own drawing at 1x and 2x
    iconset = os.path.join(tempfile.mkdtemp(), 'nativ.iconset')
    os.makedirs(iconset)
    for pt in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            px = pt * scale
            name = f'icon_{pt}x{pt}' + ('@2x' if scale == 2 else '') + '.png'
            render(icon_svg(level_for(pt), px, inset=824 / 1024), os.path.join(iconset, name), px)
    shutil.copy(os.path.join(iconset, 'icon_512x512.png'), os.path.join(png_dir, 'icon-macos-512.png'))
    subprocess.run(['iconutil', '-c', 'icns', iconset, '-o', os.path.join(out, 'nativ.icns')], check=True)
    write_ico([(px, os.path.join(png_dir, f'icon-{px}.png')) for px in (16, 32, 48, 64, 128, 256)], os.path.join(out, 'nativ.ico'))
    print('assets written to', out)


if __name__ == '__main__':
    main()
