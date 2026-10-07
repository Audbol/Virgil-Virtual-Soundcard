"""Generates the Virgil logo (branding/virgil.svg) and its raster variants.

Concept: Virgil, the Roman poet who guides Dante through the Inferno, is
shown by his laurel wreath; inside it burns the Inferno, a flame built from
audio meter bars.  Run: python3 branding/make_logo.py
"""
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
S = 512
CX, CY = 256, 262


def leaf(x, y, angle, length, width, fill):
    # Pointed leaf: two quadratic curves, base at (x, y), pointing along angle.
    a = math.radians(angle)
    tx, ty = x + length * math.cos(a), y + length * math.sin(a)
    nx, ny = -math.sin(a), math.cos(a)
    mx, my = x + 0.5 * length * math.cos(a), y + 0.5 * length * math.sin(a)
    c1 = (mx + nx * width, my + ny * width)
    c2 = (mx - nx * width, my - ny * width)
    return (f'<path d="M{x:.1f},{y:.1f} Q{c1[0]:.1f},{c1[1]:.1f} {tx:.1f},{ty:.1f} '
            f'Q{c2[0]:.1f},{c2[1]:.1f} {x:.1f},{y:.1f}Z" fill="{fill}"/>')


def branch(side):
    # side = -1 (left) or +1 (right); arc from the bottom up to near the top.
    out = []
    r = 178
    stem = []
    n = 9
    for i in range(n + 1):
        t = i / n
        ang = math.radians(90 + side * (-6 + 156 * t))  # from bottom towards top
        stem.append((CX + r * math.cos(ang), CY + r * math.sin(ang), ang))
    d = "M" + " L".join(f"{p[0]:.1f},{p[1]:.1f}" for p in stem)
    out.append(f'<path d="{d}" fill="none" stroke="url(#gold)" stroke-width="7" stroke-linecap="round"/>')
    for i, (x, y, ang) in enumerate(stem[2:], start=2):
        tangent = math.degrees(ang) + side * 90  # direction of travel along the arc
        size = 50 - 2.2 * i
        out.append(leaf(x, y, tangent - side * 38, size, size * 0.34, "url(#gold)"))   # outer
        out.append(leaf(x, y, tangent + side * 42, size * 0.9, size * 0.31, "url(#gold2)"))  # inner
    tip_x, tip_y, ang = stem[-1]
    out.append(leaf(tip_x, tip_y, math.degrees(ang) + side * 90, 44, 14, "url(#gold)"))
    return "\n".join(out)


def flame_path():
    # Flame silhouette: a tall main tongue licking left, a smaller one right.
    return ("M262,74 C266,118 300,140 318,176 C326,160 326,146 322,132 "
            "C350,168 356,224 342,268 C330,308 300,336 256,340 "
            "C212,336 182,308 170,268 C158,224 170,186 196,160 "
            "C196,184 204,200 216,208 C212,160 236,120 262,74 Z")


def core_path():
    # Brighter inner flame.
    return ("M256,170 C262,204 294,224 298,262 C302,298 282,322 256,324 "
            "C230,322 210,298 214,262 C218,232 238,214 244,190 "
            "C248,204 252,212 258,216 C254,202 252,188 256,170 Z")


def bars():
    out = []
    w, gap, n = 21, 6, 7
    x0 = 256 - (n * w + (n - 1) * gap) / 2
    heights = [110, 175, 245, 270, 230, 160, 105]
    for i, h in enumerate(heights):
        x = x0 + i * (w + gap)
        out.append(f'<rect x="{x:.1f}" y="{344 - h}" width="{w}" height="{h}" rx="4"/>')
    return "\n".join(out)


def svg(background=True):
    bg = ""
    if background:
        bg = ('<rect width="512" height="512" rx="112" fill="url(#bg)"/>'
              '<circle cx="256" cy="268" r="200" fill="url(#glow)"/>')
    return f'''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512">
<defs>
  <radialGradient id="bg" cx="50%" cy="38%" r="75%">
    <stop offset="0" stop-color="#3a1410"/><stop offset="0.6" stop-color="#1c0b0a"/><stop offset="1" stop-color="#0b0606"/>
  </radialGradient>
  <radialGradient id="glow" cx="50%" cy="60%" r="50%">
    <stop offset="0" stop-color="#ff6a1a" stop-opacity="0.35"/><stop offset="1" stop-color="#ff6a1a" stop-opacity="0"/>
  </radialGradient>
  <linearGradient id="fire" x1="0" y1="1" x2="0" y2="0">
    <stop offset="0" stop-color="#9c1c12"/><stop offset="0.35" stop-color="#e2401b"/>
    <stop offset="0.7" stop-color="#ff8a1f"/><stop offset="1" stop-color="#ffd76a"/>
  </linearGradient>
  <linearGradient id="gold" x1="0" y1="0" x2="1" y2="1">
    <stop offset="0" stop-color="#f6d98a"/><stop offset="0.5" stop-color="#d4a23f"/><stop offset="1" stop-color="#9a6a1f"/>
  </linearGradient>
  <linearGradient id="gold2" x1="1" y1="0" x2="0" y2="1">
    <stop offset="0" stop-color="#e7c06a"/><stop offset="1" stop-color="#8a5c1a"/>
  </linearGradient>
  <linearGradient id="corefire" x1="0" y1="1" x2="0" y2="0">
    <stop offset="0" stop-color="#ffb341"/><stop offset="1" stop-color="#fff2b8"/>
  </linearGradient>
  <clipPath id="flame"><path d="{flame_path()}"/></clipPath>
  <clipPath id="core"><path d="{core_path()}"/></clipPath>
</defs>
{bg}
<g>{branch(-1)}</g>
<g>{branch(1)}</g>
<path d="M246,440 L232,470 L246,464 L252,474 L256,446 Z M266,440 L280,470 L266,464 L260,474 L256,446 Z" fill="#b3261e"/>
<circle cx="256" cy="440" r="10" fill="url(#gold)"/>
<g clip-path="url(#flame)" fill="url(#fire)">{bars()}</g>
<g clip-path="url(#core)" fill="url(#corefire)">{bars()}</g>
</svg>
'''


if __name__ == "__main__":
    import cairosvg
    with open(os.path.join(HERE, "virgil.svg"), "w") as f:
        f.write(svg())
    with open(os.path.join(HERE, "virgil-mark.svg"), "w") as f:
        f.write(svg(background=False))
    cairosvg.svg2png(bytestring=svg().encode(), write_to=os.path.join(HERE, "virgil-512.png"),
                     output_width=512, output_height=512)
    # Platform icons: Windows .ico, macOS .icns, Linux hicolor PNG.
    from PIL import Image
    import io
    sizes = [16, 24, 32, 48, 64, 128, 256, 512, 1024]
    imgs = {}
    for n in sizes:
        png = cairosvg.svg2png(bytestring=svg().encode(), output_width=n, output_height=n)
        imgs[n] = Image.open(io.BytesIO(png)).convert("RGBA")
    imgs[256].save(os.path.join(HERE, "virgil.ico"),
                   sizes=[(n, n) for n in (16, 24, 32, 48, 64, 128, 256)])
    imgs[1024].save(os.path.join(HERE, "virgil.icns"))
    imgs[256].save(os.path.join(HERE, "virgil-256.png"))
    print("wrote branding/virgil.svg, virgil-mark.svg, virgil-{256,512}.png, virgil.ico, virgil.icns")
