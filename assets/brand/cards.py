#!/usr/bin/env python3
"""Project Nativ's trailer cards: python3 cards.py [<folder>] (default ~/Movies/Nativ trailer cards)

4K PNGs (3840 x 2160) in the brand's look (make.py's palette, logo and wordmark), set in Outfit (fonts/, SIL Open Font
License): a title card, "open beta out now", one card per feature, a feature list, an end card, and lower thirds with a
transparent ground for naming a game over its footage. Rendered by headless Chrome, as make.py renders the icons. The
controls card's input icons are Kenney's Input Prompts (CC0, third_party/kenney-prompts). Edit the copy below and run it
again. Only our own artwork and text: nothing of a game's or of Sony's.
"""
import os
import subprocess
import sys
import tempfile

import make as M

INK, MUTED, AMBER, BG = M.INK, M.MUTED, M.AMBER, M.BG
W, H = 3840, 2160
FONT = os.path.join(M.HERE, "fonts", "Outfit[wght].ttf")
REPO = "github.com/TheLostPantheon/nativ"

# Kenney Input Prompts 1.5A (CC0), Vector/: the paths as drawn on their 64-unit grid; "lit" is the part Kenney marks red
ICONS = {
    "option": ["M48 8 Q56 8 56 16 L56 48 Q56 56 48 56 L16 56 Q8 56 8 48 L8 16 Q8 8 16 8 L48 8 M29.25 22 L20 22 20 26 26.75 26 34.8 42 44 42 44 38 37.25 38 29.25 22 M44 22 L36 22 36 26 44 26 44 22"],
    "mouse": ["M34 10 L44 10 Q45.75 10 46.9 10.6 49 11.75 49 15 L49 25 38 25 38 19 Q38 16.5 36.25 14.75 35.25 13.75 34 13.35 L34 10 M29.9 16.9 Q30.75 16 32 16 33.25 16 34.15 16.9 35 17.75 35 19 L35 27 34.85 28 34.15 29.15 Q33.25 30 32 30 30.75 30 29.9 29.15 29.4 28.65 29.2 28 L29 27 29 19 Q29 17.75 29.9 16.9 M15 28 L26.1 28 Q26.35 29.85 27.75 31.25 29.5 33 32 33 34.5 33 36.25 31.25 37.65 29.85 37.95 28 L49 28 49 37.5 Q48.8 44.25 44 49 39.25 53.8 32.5 54 L31.5 54 Q24.75 53.8 19.95 49 15.2 44.25 15 37.5 L15 28",
              "lit:M20 10 L30 10 30 13.35 Q28.75 13.75 27.75 14.75 26 16.5 26 19 L26 25 15 25 15 15 Q15 11.75 17.15 10.6 18.25 10 20 10"],
    "tap": ["M37.55 30.55 L36.95 29.05 37 28 Q37 24.8 34.8 22.55 L34.7 22.45 34.6 22.35 Q32.3 20 29 20 25.7 20 23.35 22.4 21 24.7 21 28 21 31.35 23.3 33.6 L23.35 33.65 Q24.75 35.05 26.5 35.65 L27.75 38.95 Q24 38.55 21.2 35.75 18 32.6 18 28 18 23.45 21.2 20.25 24.45 17 29 17 33.55 17 36.75 20.25 L36.95 20.45 Q40 23.55 40 28 L39.85 30 Q38.6 30.1 37.55 30.55 M36 34.75 L36.8 34.8 Q37.65 33.8 38.65 33.35 39.4 33 40.3 33 42.65 32.95 43.65 34.65 44.75 33.75 46.25 33.75 47.75 33.75 48.65 34.95 50.7 37.75 52.4 41.8 54 47.75 51.1 51.55 49.45 53.45 46.95 54.35 L41.7 54.75 Q39.65 54.2 37.8 53.1 34.1 50.85 30.45 50.65 24.95 51 25 47.65 25 46.5 25.7 45.65 26.45 44.95 27.55 44.7 29.05 44.5 30.75 44.7 32.85 44.6 32.4 43 L30.9 38.85 29.8 36 27.85 30.65 Q27.35 29.35 27.85 28.3 28.25 27.55 29.95 27.15 31.6 26.75 32.5 27.45 L33.7 29.05 36 34.75"],
}


def icon(name, x, y, size, lit=AMBER):
    paths = "".join(f'<path fill="{lit if d.startswith("lit:") else INK}" d="{d[4:] if d.startswith("lit:") else d}"/>' for d in ICONS[name])
    return f'<svg x="{x}" y="{y}" width="{size}" height="{size}" viewBox="0 0 64 64">{paths}</svg>'


def stick(x, y, size):   # a thumbstick from above, in the brand's line (Kenney's stick is drawn side-on)
    s = size / 2
    return (f'<g transform="translate({x + s} {y + s})" fill="none" stroke="{INK}" stroke-width="{size * 0.05:g}">'
            f'<circle r="{s * 0.86:g}"/><circle r="{s * 0.5:g}" fill="{INK}" stroke="none"/>'
            f'<circle r="{s * 0.5:g}" cx="{s * 0.2:g}" cy="{-s * 0.14:g}" fill="none" stroke="{AMBER}" stroke-dasharray="{size * 0.04:g} {size * 0.05:g}"/></g>')


def vita(x, y, w, screen=AMBER, glow=True):
    h = w * 280 / 600
    g = f'<ellipse cx="{x + w / 2}" cy="{y + h / 2}" rx="{w * 0.42}" ry="{h * 0.5}" fill="url(#glow)"/>' if glow else ""
    return g + f'<svg x="{x}" y="{y}" width="{w}" height="{h:g}" viewBox="0 0 600 280">{M.device_inner(14, "full", screen)}</svg>'


def wordmark(x, y, w):
    return f'<svg x="{x}" y="{y}" width="{w}" height="{w * 96 / 288:g}" viewBox="-6 14 288 96">{M.word_inner(INK, AMBER)}</svg>'


GLOW = (f'<defs><radialGradient id="glow"><stop offset="0" stop-color="{AMBER}" stop-opacity="0.16"/>'
        f'<stop offset="1" stop-color="{AMBER}" stop-opacity="0"/></radialGradient></defs>')


def page(body, transparent=False):
    ground = "transparent" if transparent else f"radial-gradient(120% 120% at 70% 45%, #191C24 0%, {BG} 55%, #0C0E12 100%)"
    return f"""<!doctype html><meta charset="utf-8"><style>
@font-face {{ font-family: Outfit; src: url("file://{FONT}"); font-weight: 100 900; }}
html, body {{ margin: 0; width: {W}px; height: {H}px; overflow: hidden; background: {ground}; }}
body {{ font-family: Outfit, sans-serif; color: {INK}; position: relative; font-kerning: normal; }}
.label {{ font-size: 58px; font-weight: 600; letter-spacing: 0.26em; color: {AMBER}; text-transform: uppercase; }}
.head {{ text-wrap: balance; font-size: 232px; font-weight: 600; line-height: 1.0; letter-spacing: -0.02em; margin-top: 44px; }}
.sub {{ text-wrap: balance; font-size: 84px; font-weight: 300; line-height: 1.3; color: {MUTED}; margin-top: 64px; max-width: 1700px; }}
.left {{ position: absolute; left: 300px; top: 50%; transform: translateY(-50%); width: 1900px; }}
svg.art {{ position: absolute; left: 0; top: 0; }}
</style><body>{body}</body>"""


def feature(label, head, sub, art=""):
    return page(f'<svg class="art" width="{W}" height="{H}">{GLOW}{art}{wordmark(300, 1890, 250)}</svg>'
                f'<div class="left"><div class="label">{label}</div><div class="head">{head}</div><div class="sub">{sub}</div></div>')


def frames_art():   # 30 and 60 frames over the same second
    x0, x1, out = 2380, 3540, ""
    for row, (n, col, lab, y) in enumerate(((15, MUTED, "30", 830), (30, AMBER, "60", 1170))):
        step = (x1 - x0) / n
        for i in range(n):
            out += f'<rect x="{x0 + i * step:g}" y="{y}" width="{step * 0.62:g}" height="190" rx="10" fill="{col}" opacity="{0.9 if row else 0.55}"/>'
        out += f'<text x="{x0 - 50}" y="{y + 132}" text-anchor="end" font-family="Outfit" font-weight="600" font-size="110" fill="{col}">{lab}</text>'
    return out + f'<text x="{x1}" y="1500" text-anchor="end" font-family="Outfit" font-weight="300" font-size="62" fill="{MUTED}">one second</text>'


def scale_art():   # 1x, 2x, 4x and 6x the Vita's 960 x 544, from one corner
    out, x, yb = "", 2300, 1660
    for k, col, sw, dash in ((6, AMBER, 9, ""), (4, MUTED, 5, ' stroke-dasharray="22 18"'), (2, MUTED, 5, ' stroke-dasharray="22 18"'), (1, INK, 0, "")):
        w, h = 1260 * k / 6, 1260 * k / 6 * 544 / 960
        fill = INK if k == 1 else "none"
        out += f'<rect x="{x}" y="{yb - h:g}" width="{w:g}" height="{h:g}" rx="{6 + 2 * k}" fill="{fill}" stroke="{col}" stroke-width="{sw}"{dash}/>'
        out += f'<text x="{x + w - 26:g}" y="{yb - h + 84:g}" text-anchor="end" font-family="Outfit" font-weight="600" font-size="{70 if k > 1 else 54}" fill="{BG if k == 1 else col}">{k}×</text>'
    return out


def aspect_art():   # 16:9, 21:9 and 32:9 around one centre
    out, cx, cy, h = "", 2860, 1080, 560
    lab = {32: (1, -1, AMBER), 21: (1, 1, MUTED), 16: (0, 0, INK)}   # (right edge?, above -1 / below 1 / inside 0)
    for num, col, sw, dash in ((32, AMBER, 9, ""), (21, MUTED, 6, ' stroke-dasharray="26 20"'), (16, INK, 7, "")):
        w = h * num / 9 * 0.62
        out += f'<rect x="{cx - w / 2:g}" y="{cy - h / 2}" width="{w:g}" height="{h}" rx="14" fill="none" stroke="{col}" stroke-width="{sw}"{dash}/>'
        right, where, c = lab[num]
        x = cx + w / 2 - 24 if right else cx
        y = cy - h / 2 - 40 if where < 0 else cy + h / 2 + 96 if where > 0 else cy + 24
        out += f'<text x="{x:g}" y="{y:g}" text-anchor="{"end" if right else "middle"}" font-family="Outfit" font-weight="600" font-size="{64 if where else 84}" fill="{c}">{num}:9</text>'
    return out


def controls_art():
    y, s, out = 840, 300, ""
    groups = ((2120, [("option", 0), ("mouse", 330)], "Keyboard and mouse"), (2900, [("stick", 0)], "Controller"), (3360, [("tap", 0)], "Touch"))
    for x, items, cap in groups:
        for name, dx in items:
            out += stick(x + dx, y, s) if name == "stick" else icon(name, x + dx, y, s)
        cx = x + (330 + s) / 2 if len(items) > 1 else x + s / 2
        out += f'<text x="{cx:g}" y="{y + s + 110}" text-anchor="middle" font-family="Outfit" font-weight="400" font-size="58" fill="{MUTED}">{cap}</text>'
    return out


def online_art():
    out = vita(2120, 940, 560, glow=True) + vita(3060, 940, 560, glow=True)
    return out + (f'<path d="M2400 900 C2600 560 3140 560 3340 900" fill="none" stroke="{AMBER}" stroke-width="10" stroke-linecap="round" stroke-dasharray="2 34"/>'
                  f'<circle cx="2870" cy="645" r="16" fill="{AMBER}"/>')


CARDS = {
    "01-title": page(f'<svg class="art" width="{W}" height="{H}">{GLOW}{vita(1240, 520, 1360)}{wordmark(1460, 1250, 920)}</svg>'
                     f'<div style="position:absolute;top:1640px;width:100%;text-align:center;font-size:96px;font-weight:300;color:{MUTED}">'
                     f'Your PS Vita games, rebuilt natively</div>'),
    "02-open-beta": page(f'<svg class="art" width="{W}" height="{H}">{GLOW}<ellipse cx="1920" cy="1080" rx="1500" ry="700" fill="url(#glow)"/>{wordmark(1650, 470, 540)}</svg>'
                         f'<div style="position:absolute;top:780px;width:100%;text-align:center">'
                         f'<div class="label" style="font-size:80px">Open beta</div>'
                         f'<div style="font-size:420px;font-weight:600;line-height:1;letter-spacing:-0.03em;margin-top:30px">Out now</div>'
                         f'<div style="font-size:92px;font-weight:300;color:{MUTED};margin-top:70px">Free for Mac, Windows and Linux</div></div>'),
    "03-native": feature("Native", "Recompiled,<br>not emulated", "Each game is translated into a native program for your computer.", vita(2240, 760, 1300)),
    "04-resolution": feature("Resolution", "Up to 6×<br>resolution", "From the Vita's 960 × 544 to 5760 × 3264.", scale_art()),
    "05-frame-rate": feature("Frame rate", "60 fps", "Twice the frame rate in Killzone: Mercenary, Uncharted: Golden Abyss and Unit 13.", frames_art()),
    "06-widescreen": feature("Widescreen", "Ultrawide<br>ready", "21:9 and 32:9 views, with the HUD kept in place.", aspect_art()),
    "07-controls": feature("Controls", "Play your way", "Keyboard and mouse or a controller, with the touch screen mapped to both.", controls_art()),
    "08-online": feature("Online", "Online play,<br>revived", "Killzone: Mercenary multiplayer and Unit 13 co-op, player to player.", online_art()),
    "09-your-games": feature("Your games", "Bring your own", "Nativ builds from the games you own. It never includes any.",
                             f'<ellipse cx="2900" cy="1080" rx="900" ry="620" fill="url(#glow)"/>'
                             f'<g transform="translate(2560 640)" fill="none" stroke="{INK}" stroke-width="16" stroke-linejoin="round">'
                             f'<path d="M60 0 H560 Q620 0 620 60 V820 Q620 880 560 880 H160 L0 720 V60 Q0 0 60 0 Z"/>'
                             f'<rect x="110" y="140" width="400" height="460" rx="28" stroke="{AMBER}" stroke-width="12"/></g>'),
    "10-open-source": feature("Open source", "Free and<br>open source", f"GPL licensed, on GitHub: {REPO}",
                              f'<ellipse cx="2900" cy="1080" rx="900" ry="620" fill="url(#glow)"/>'
                              f'<text x="2900" y="1250" text-anchor="middle" font-family="Outfit" font-weight="300" font-size="560" fill="{AMBER}">{{ }}</text>'),
}
FEATURES = ["Recompiled, not emulated", "Up to 6× resolution", "60 fps", "Ultrawide 21:9 and 32:9",
            "Keyboard, mouse or controller", "Online multiplayer and co-op", "Mac, Windows and Linux", "Free and open source"]
items = "".join(f'<div style="display:flex;align-items:center;gap:44px;font-size:88px;font-weight:400;height:190px">'
                f'<span style="width:26px;height:26px;border-radius:13px;background:{AMBER};flex:none"></span>{f}</div>' for f in FEATURES)
CARDS["11-features"] = page(f'<svg class="art" width="{W}" height="{H}">{GLOW}<ellipse cx="2600" cy="1080" rx="1700" ry="900" fill="url(#glow)"/>{wordmark(300, 1890, 250)}</svg>'
                            f'<div style="position:absolute;left:300px;top:50%;transform:translateY(-54%)"><div class="label">The open beta</div>'
                            f'<div style="margin-top:110px;display:grid;grid-template-columns:1560px 1560px;column-gap:120px">{items}</div></div>')
CARDS["12-end"] = page(f'<svg class="art" width="{W}" height="{H}">{GLOW}{vita(1480, 360, 880)}{wordmark(1600, 830, 640)}</svg>'
                       f'<div style="position:absolute;top:1220px;width:100%;text-align:center">'
                       f'<div style="font-size:150px;font-weight:600;letter-spacing:-0.02em">Open beta out now</div>'
                       f'<div style="font-size:100px;font-weight:400;color:{AMBER};margin-top:60px">{REPO}</div>'
                       f'<div style="font-size:78px;font-weight:300;color:{MUTED};margin-top:56px">Mac · Windows · Linux</div></div>')

# lower thirds: a game's name and what to notice, over its footage (transparent ground)
LOWER = {
    "killzone": ("Killzone: Mercenary", "60 fps · 4× resolution · online multiplayer"),
    "uncharted": ("Uncharted: Golden Abyss", "60 fps · ultrawide · mouse aim"),
    "unit13": ("Unit 13", "60 fps · online co-op"),
    "shovel-knight": ("Shovel Knight", "native on Mac, Windows and Linux"),
}
for key, (name, what) in LOWER.items():
    CARDS[f"20-lower-{key}"] = page(
        f'<div style="position:absolute;left:220px;bottom:220px;display:flex;align-items:stretch;gap:0;'
        f'background:rgba(18,20,25,0.82);border-radius:28px;overflow:hidden;box-shadow:0 20px 80px rgba(0,0,0,0.45)">'
        f'<div style="width:22px;background:{AMBER}"></div>'
        f'<div style="padding:52px 90px 56px 70px"><div style="font-size:112px;font-weight:600;letter-spacing:-0.01em">{name}</div>'
        f'<div style="font-size:64px;font-weight:400;color:{AMBER};margin-top:18px">{what}</div></div></div>', transparent=True)


def render(html, png, transparent):
    with tempfile.NamedTemporaryFile("w", suffix=".html", delete=False) as f:
        f.write(html)
    try:
        args = [M.CHROME, "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                f"--window-size={W},{H}", f"--screenshot={png}", "--virtual-time-budget=3000", "--allow-file-access-from-files"]
        if transparent: args.insert(1, "--default-background-color=00000000")
        subprocess.run(args + [f"file://{f.name}"], check=True, capture_output=True)
    finally:
        os.unlink(f.name)


def main(argv):
    out = os.path.expanduser(argv[1] if len(argv) > 1 else "~/Movies/Nativ trailer cards")
    os.makedirs(out, exist_ok=True)
    for name, html in CARDS.items():
        render(html, os.path.join(out, name + ".png"), name.startswith("20-lower"))
        print(os.path.join(out, name + ".png"))


if __name__ == "__main__":
    main(sys.argv)
