"""Chorus Cut identity, concept 2a "Sliced chorus".

Writes the logo SVGs beside this script and resources/icon/ChorusCut.icns (needs rsvg-convert
and macOS's iconutil). Usage: python3 resources/logo/make_icon.py
"""
import math, os, subprocess, tempfile

LOGO = os.path.dirname(os.path.abspath(__file__))
ICNS = os.path.join(LOGO, "..", "icon", "ChorusCut.icns")
ICONSET = os.path.join(tempfile.mkdtemp(), "ChorusCut.iconset")
os.makedirs(ICONSET)

MINT, AMBER, CORAL, PAPER, INK = "#87dec7", "#e7b75f", "#f0a08f", "#edf1ef", "#0c1216"
# Deeper versions of the same hues, for light backgrounds.
LIGHT = {"mint": "#3f9c84", "amber": "#d99a3a", "coral": "#e07f6f", "ink": "#1d2930"}
CX = CY = 512


def pt(r, deg):
    t = math.radians(deg)
    return CX + r * math.cos(t), CY - r * math.sin(t)


def arc(r, a):
    """C-shaped arc of radius r, open on the right between +a and -a degrees."""
    (x1, y1), (x2, y2) = pt(r, a), pt(r, -a)
    return f"M{x1:.1f},{y1:.1f} A{r},{r} 0 1 0 {x2:.1f},{y2:.1f}"


def play(x, y, s, fill):
    return (f'<path d="M{x - s * 0.45:.1f},{y - s * 0.6:.1f} L{x + s * 0.6:.1f},{y:.1f} L{x - s * 0.45:.1f},{y + s * 0.6:.1f} Z" '
            f'fill="{fill}" stroke="{fill}" stroke-width="{s * 0.25:.1f}" stroke-linejoin="round"/>')


def mark(colors=(MINT, AMBER, CORAL), play_fill=PAPER, small=False, uid="m"):
    """The sliced chorus: three voices as a C, cut on a slant with the tips slipped up.

    The small version (16 and 32 px icons) keeps two thicker voices and a wider cut so the slice
    survives at a few pixels."""
    if small:
        voices = [(268, 50, colors[0], 112), (120, 64, colors[2], 112)]
        cut, gap, shift, tri = (612, 492), 52, (26, -64), None
    else:
        voices = [(292, 48, colors[0], 58), (200, 54, colors[1], 58), (108, 62, colors[2], 58)]
        cut, gap, shift, tri = (596, 476), 36, (18, -44), (676, 512, 84)
    arcs = "".join(
        f'<path d="{arc(r, a)}" fill="none" stroke="{c}" stroke-width="{w}" stroke-linecap="round"/>'
        for r, a, c, w in voices)
    defs = (f'<clipPath id="{uid}L"><polygon points="0,0 {cut[0]},0 {cut[1]},1024 0,1024"/></clipPath>'
            f'<clipPath id="{uid}R"><polygon points="{cut[0] + gap},0 1024,0 1024,1024 {cut[1] + gap},1024"/></clipPath>')
    right = arcs + (play(*tri, play_fill) if tri else "")
    body = (f'<g clip-path="url(#{uid}L)">{arcs}</g>'
            f'<g clip-path="url(#{uid}R)"><g transform="translate({shift[0]},{shift[1]})">{right}</g></g>')
    return defs, body


def icon_svg(small=False, shadow=True):
    """macOS app icon: the mark on a dark tile on Apple's 1024 grid (824 tile, 100 margin)."""
    defs, body = mark(small=small)
    shadow_def = ('<filter id="shadow" x="-10%" y="-10%" width="120%" height="125%">'
                  '<feDropShadow dx="0" dy="12" stdDeviation="14" flood-color="#000" flood-opacity="0.35"/></filter>') if shadow else ""
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1024 1024" width="1024" height="1024">
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#22313a"/><stop offset="1" stop-color="#0c1216"/></linearGradient>
    <clipPath id="tile"><rect x="100" y="100" width="824" height="824" rx="185"/></clipPath>
    {shadow_def}
    {defs}
  </defs>
  <rect x="100" y="100" width="824" height="824" rx="185" fill="url(#bg)"{' filter="url(#shadow)"' if shadow else ''}/>
  <rect x="101" y="101" width="822" height="822" rx="184" fill="none" stroke="#fff" stroke-opacity="0.07" stroke-width="2"/>
  <g clip-path="url(#tile)">{body}</g>
</svg>
'''


# ---- Wordmark ---------------------------------------------------------------
# Monoline lettering drawn to match the mark: round caps, geometric bowls, a C cut from the same
# arc. Units: baseline y=188, x-height centre line 112, cap/ascender top 60; stroke 24.
SW, H = 24, 12
GLYPHS = {
    # path, advance width (outer stroke edges start at x=0)
    "C": ("M107.3,85.6 A58,58 0 1 0 107.3,174.4", 119),
    "h": ("M12,60 V188 M12,150 A38,38 0 0 1 88,150 V188", 100),
    "o": ("M88,150 A38,38 0 1 0 12,150 A38,38 0 1 0 88,150 Z", 100),
    "r": ("M12,188 V150 A38,38 0 0 1 50,112 H62", 74),
    "u": ("M12,112 V150 A38,38 0 0 0 88,150 M88,112 V188", 100),
    "s": ("M64.2,121.5 A28,19 0 1 0 40,150 A28,19 0 1 1 15.8,178.5", 80),
    "t": ("M8,112 H62 M30,76 V166 A22,22 0 0 0 52,188 H62", 74),
    " ": ("", 34),
}
TRACK = 18


def wordmark(text, ink, accent, x0=0.0, y0=0.0, scale=1.0):
    """Return (svg group, width, height) for the lettering; capital Cs take the accent colour."""
    parts, x = [], 0.0
    for i, ch in enumerate(text):
        d, adv = GLYPHS[ch]
        if d:
            color = accent if ch == "C" else ink
            parts.append(f'<path transform="translate({x:.1f},0)" d="{d}" stroke="{color}"/>')
        x += adv + (TRACK if i < len(text) - 1 and text[i + 1] != " " and ch != " " else 0)
    group = (f'<g transform="translate({x0:.1f},{y0:.1f}) scale({scale})" fill="none" stroke-width="{SW}" '
             f'stroke-linecap="round" stroke-linejoin="round">{"".join(parts)}</g>')
    return group, x * scale, 200 * scale


def lockup(light=False):
    """Horizontal lockup: the mark (no tile) beside the wordmark, on a transparent background."""
    if light:
        colors, ink, accent, tri = (LIGHT["mint"], LIGHT["amber"], LIGHT["coral"]), LIGHT["ink"], LIGHT["mint"], LIGHT["ink"]
    else:
        colors, ink, accent, tri = (MINT, AMBER, CORAL), PAPER, MINT, PAPER
    defs, body = mark(colors, tri, uid="k")
    # The mark's ink spans x 191..754, y 147..838 in its 1024 box (the slipped tips rise 44 above
    # the C). Scale it to about 1.3 times the cap height and centre it on the C of the lettering.
    s, pad = 0.29, 12
    mark_g = f'<g transform="translate({pad - 191 * s:.1f},{pad + 8 - 147 * s:.1f}) scale({s})">{body}</g>'
    text_x = pad + (754 - 191) * s + 52
    word_g, ww, _ = wordmark("Chorus Cut", ink, accent, x0=text_x, y0=pad - 4, scale=1.0)
    width, height = text_x + ww + pad, 2 * pad + 8 + (838 - 147) * s
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width:.0f} {height}" width="{width:.0f}" height="{height}">
  <defs>{defs}</defs>
  {mark_g}
  {word_g}
</svg>
'''


def mark_only(light=False):
    colors = (LIGHT["mint"], LIGHT["amber"], LIGHT["coral"]) if light else (MINT, AMBER, CORAL)
    defs, body = mark(colors, LIGHT["ink"] if light else PAPER, uid="o")
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="171 127 603 731" width="603" height="731">
  <defs>{defs}</defs>
  {body}
</svg>
'''


def write(path, text):
    open(path, "w").write(text)
    return path


write(os.path.join(LOGO, "icon.svg"), icon_svg())
write(os.path.join(LOGO, "icon-small.svg"), icon_svg(small=True, shadow=False))
write(os.path.join(LOGO, "mark.svg"), mark_only())
write(os.path.join(LOGO, "mark-light.svg"), mark_only(light=True))
write(os.path.join(LOGO, "lockup-dark.svg"), lockup())
write(os.path.join(LOGO, "lockup-light.svg"), lockup(light=True))

# macOS icon set: the simplified mark below 64 px, the full one above.
for pts in (16, 32, 128, 256, 512):
    for scale in (1, 2):
        px = pts * scale
        src = "icon-small.svg" if px <= 32 else "icon.svg"
        name = f"icon_{pts}x{pts}{'@2x' if scale == 2 else ''}.png"
        subprocess.run(["rsvg-convert", "-w", str(px), "-h", str(px), os.path.join(LOGO, src),
                        "-o", os.path.join(ICONSET, name)], check=True)
os.makedirs(os.path.dirname(ICNS), exist_ok=True)
subprocess.run(["iconutil", "-c", "icns", ICONSET, "-o", ICNS], check=True)
print("wrote", os.path.normpath(ICNS))
