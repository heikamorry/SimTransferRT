"""Render the combo/spin arrow glyphs used by resources/styles/light.qss.

Qt stylesheets cannot draw CSS triangles (a zero-size box with borders renders
as a filled rectangle), and the native arrows disappear as soon as a subcontrol
is styled. Ship small black chevrons instead, at 1x and 2x.

Usage: python tools/make_ui_arrows.py resources/icons
"""

import sys
from pathlib import Path

from PIL import Image, ImageDraw

BLACK = (0, 0, 0, 255)
SUPERSAMPLE = 8


def chevron(width, height, up):
    w, h = width * SUPERSAMPLE, height * SUPERSAMPLE
    img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    pad = 1.5 * SUPERSAMPLE
    if up:
        points = [(pad, h - pad), (w / 2, pad), (w - pad, h - pad)]
    else:
        points = [(pad, pad), (w / 2, h - pad), (w - pad, pad)]
    stroke = max(2, round(1.55 * SUPERSAMPLE))
    d.line(points, fill=BLACK, width=stroke, joint="curve")
    r = stroke / 2
    for x, y in (points[0], points[-1]):
        d.ellipse([x - r, y - r, x + r, y + r], fill=BLACK)
    return img.resize((width, height), Image.LANCZOS)


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "resources/icons")
    out.mkdir(parents=True, exist_ok=True)
    for name, width, height, up in (
        ("chevron-down", 9, 6, False),
        ("spin-up", 8, 5, True),
        ("spin-down", 8, 5, False),
    ):
        image = chevron(width, height, up)
        image.save(out / f"{name}.png", optimize=True)
        image.resize((width * 2, height * 2), Image.LANCZOS).save(
            out / f"{name}@2x.png", optimize=True
        )
    print(f"wrote 6 arrow files to {out}")


if __name__ == "__main__":
    main()
