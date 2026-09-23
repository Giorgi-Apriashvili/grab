"""Draws grab's app icon (src/gui/grab.ico): a white arrow dropping into a tray on the
accent-blue rounded square. Run once after changing the design; the .ico is committed.

    python tools/make_icon.py        (needs Pillow)
"""

from pathlib import Path

from PIL import Image, ImageDraw

SIZES = [16, 20, 24, 32, 40, 48, 64, 128, 256]
ACCENT_TOP = (59, 130, 246)
ACCENT_BOTTOM = (29, 78, 216)
WHITE = (255, 255, 255, 255)


def draw(size: int) -> Image.Image:
    # Draw large and scale down for clean edges at every size.
    scale = 8 if size <= 64 else 4
    s = size * scale
    img = Image.new("RGBA", (s, s), (0, 0, 0, 0))

    # Rounded square with a vertical gradient.
    gradient = Image.new("RGBA", (s, s))
    gd = ImageDraw.Draw(gradient)
    for y in range(s):
        t = y / (s - 1)
        c = tuple(round(a + (b - a) * t) for a, b in zip(ACCENT_TOP, ACCENT_BOTTOM))
        gd.line([(0, y), (s, y)], fill=c + (255,))
    mask = Image.new("L", (s, s), 0)
    margin = round(s * (0.02 if size <= 20 else 0.04))
    ImageDraw.Draw(mask).rounded_rectangle([margin, margin, s - margin, s - margin],
                                           radius=round(s * 0.22), fill=255)
    img.paste(gradient, (0, 0), mask)

    d = ImageDraw.Draw(img)
    # Small sizes get thicker strokes so the glyph stays readable.
    stroke = s * (0.13 if size <= 20 else 0.11 if size <= 32 else 0.095)
    cx = s / 2

    # Arrow: shaft and head.
    top = s * 0.20
    head_base = s * 0.47
    tip = s * 0.64
    half_head = s * 0.21
    d.rectangle([cx - stroke / 2, top, cx + stroke / 2, head_base + 1], fill=WHITE)
    d.polygon([(cx - half_head, head_base), (cx + half_head, head_base), (cx, tip)], fill=WHITE)

    # Tray: a U under the arrow.
    left, right = s * 0.22, s * 0.78
    tray_top, bottom = s * 0.58, s * 0.80
    d.rounded_rectangle([left, bottom - stroke, right, bottom], radius=stroke / 2, fill=WHITE)
    d.rounded_rectangle([left, tray_top, left + stroke, bottom], radius=stroke / 2, fill=WHITE)
    d.rounded_rectangle([right - stroke, tray_top, right, bottom], radius=stroke / 2, fill=WHITE)

    return img.resize((size, size), Image.LANCZOS)


def main() -> None:
    out = Path(__file__).resolve().parent.parent / "src" / "gui" / "grab.ico"
    images = [draw(n) for n in SIZES]
    images[-1].save(out, format="ICO", sizes=[(n, n) for n in SIZES], append_images=images[:-1])
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
