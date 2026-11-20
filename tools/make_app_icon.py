"""Render the SimTransferRT application icon (Rod of Asclepius).

The GUI draws the same mark with QPainter (src/ui/MedicalBackdrop.h). This
script keeps the Windows .ico reproducible: it renders every size separately
with a small-size stroke boost, then assembles a PNG-compressed ICO by hand so
each entry can carry its own geometry.

The same sizes are also written as PNG next to the .ico. Qt loads those through
its built-in PNG reader, so the window/taskbar icon does not depend on the
optional ICO image-format plugin being deployed.

Usage: python tools/make_app_icon.py resources/app.ico
"""

import math
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw

TEAL_TOP = (15, 124, 153)
TEAL_BOTTOM = (10, 67, 88)
INK = (240, 252, 255, 236)
# 20 and 40 cover the 125% / 250% Windows title-bar sizes; QIcon picks the
# nearest entry instead of rescaling a 16 or 32 pixel bitmap.
SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)


def gradient(size):
    small = 96
    img = Image.new("RGB", (small, small))
    px = img.load()
    for y in range(small):
        for x in range(small):
            t = (x + y) / (2 * (small - 1))
            px[x, y] = tuple(
                round(a + (b - a) * t) for a, b in zip(TEAL_TOP, TEAL_BOTTOM)
            )
    return img.resize((size, size), Image.LANCZOS)


def rounded_mask(size, radius):
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle(
        [0, 0, size - 1, size - 1], radius=radius, fill=255
    )
    return mask


def render(size):
    ss = 8 if size <= 32 else 4
    n = size * ss
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    img.paste(gradient(n), (0, 0), rounded_mask(n, round(n * 0.28)))

    d = ImageDraw.Draw(img)
    box_w, box_h = n * 0.66, n * 0.80
    s = min(box_w, box_h)
    cx = n * 0.5
    top = n * 0.5 - s * 0.44
    bottom = n * 0.5 + s * 0.46
    # Fewer, wider coils at small sizes; the full coil only survives at 128+.
    if size <= 24:
        coils, amp_ratio, boost = 1.0, 0.205, 1.85
    elif size <= 32:
        coils, amp_ratio, boost = 1.25, 0.205, 1.60
    elif size <= 48:
        coils, amp_ratio, boost = 1.75, 0.175, 1.45
    elif size <= 64:
        coils, amp_ratio, boost = 2.10, 0.155, 1.25
    else:
        coils, amp_ratio, boost = 3.25, 0.118, 1.0
    amp = s * amp_ratio
    staff_w = max(1.05, 0.020 * s) * boost
    snake_w = max(1.35, 0.032 * s) * boost
    head = snake_w * (0.62 if size >= 48 else 0.80)

    def point(t):
        return (
            cx + math.sin(t * coils * 2 * math.pi + 0.35) * amp,
            top + (bottom - top) * t,
        )

    steps = 240 if size >= 48 else 120
    back, front = [], []
    for i in range(steps + 1):
        t = i / steps
        p = point(t)
        (front if math.sin(t * coils * 2 * math.pi + 0.35) >= 0 else back).append(p)

    def stroke(points, width):
        if len(points) > 1:
            d.line(points, fill=INK, width=max(1, round(width)), joint="curve")
            r = width / 2
            for p in (points[0], points[-1]):
                d.ellipse([p[0] - r, p[1] - r, p[0] + r, p[1] + r], fill=INK)

    stroke(back, snake_w)
    staff_top = top - s * (0.02 if size >= 48 else 0.075)
    stroke([(cx, staff_top), (cx, bottom)], staff_w)
    stroke(front, snake_w)
    hx, hy = point(0)
    d.ellipse(
        [hx + snake_w * 0.55 - head, hy - snake_w * 0.55 - head * 0.68,
         hx + snake_w * 0.55 + head, hy - snake_w * 0.55 + head * 0.68],
        fill=INK,
    )
    return img.resize((size, size), Image.LANCZOS)


def write_ico(path, images):
    blobs = []
    for img in images:
        from io import BytesIO

        buf = BytesIO()
        img.save(buf, format="PNG", optimize=True)
        blobs.append(buf.getvalue())
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, data = b"", b""
    for img, blob in zip(images, blobs):
        w = 0 if img.width >= 256 else img.width
        h = 0 if img.height >= 256 else img.height
        entries += struct.pack("<BBBBHHII", w, h, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
        data += blob
    path.write_bytes(header + entries + data)


def main():
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "resources/app.ico")
    out.parent.mkdir(parents=True, exist_ok=True)
    images = {size: render(size) for size in SIZES}
    write_ico(out, [images[size] for size in SIZES])
    png_dir = out.parent / "icons"
    png_dir.mkdir(parents=True, exist_ok=True)
    for size, img in images.items():
        img.save(png_dir / f"app-{size}.png", format="PNG", optimize=True)
    print(
        f"wrote {out} ({out.stat().st_size} bytes, {len(SIZES)} sizes) "
        f"and {len(SIZES)} PNG renditions in {png_dir}"
    )


if __name__ == "__main__":
    main()
