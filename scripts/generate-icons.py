#!/usr/bin/env python3
"""Generate PipeDuck icons from the master logo.

The master is assets/icons/source-pipeduck.png (1024x1024, full wordmark).
This script produces:
  * hicolor application icons (emblem for small sizes, full logo for large)
  * symbolic icons (monochrome silhouette)
  * StatusNotifierItem ARGB pixmaps for idle and active states

Requires Pillow:
    python3 -m pip install pillow
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parent.parent
ICONS = ROOT / "assets" / "icons"
SOURCE = ICONS / "source-pipeduck.png"

# Crop of the supplied logo containing the duck, headphones and waveform,
# above the "pipeduck" wordmark.
MARK_BOX = (170, 40, 854, 695)
BACKGROUND = (6, 16, 26)
KEY_LOW = 6
KEY_HIGH = 34
ACTIVE_GREEN = (46, 230, 107)
ACTIVE_CENTER = 0.815
ACTIVE_RADIUS = 0.135
ACTIVE_BORDER = 0.035

HICOLOR_SIZES = (16, 22, 24, 32, 48, 64, 128, 256, 512)
PIXMAP_SIZES = (16, 22, 24, 32, 48, 64)
SMALL_MARK_LIMIT = 64


def load_mark() -> Image.Image:
    source = Image.open(SOURCE).convert("RGB")
    if source.size != (1024, 1024):
        raise SystemExit(f"expected a 1024x1024 master, got {source.size}")
    crop = source.crop(MARK_BOX)
    side = max(crop.size)
    square = Image.new("RGB", (side, side), BACKGROUND)
    square.paste(crop, ((side - crop.width) // 2, (side - crop.height) // 2))
    return square


def key_background(square: Image.Image) -> Image.Image:
    """Remove the dark card background and decontaminate edge pixels."""
    result = Image.new("RGBA", square.size)
    source_pixels = square.load()
    output_pixels = result.load()
    for y in range(square.height):
        for x in range(square.width):
            r, g, b = source_pixels[x, y]
            distance = max(abs(r - BACKGROUND[0]), abs(g - BACKGROUND[1]), abs(b - BACKGROUND[2]))
            if distance <= KEY_LOW:
                alpha = 0.0
            elif distance >= KEY_HIGH:
                alpha = 1.0
            else:
                alpha = (distance - KEY_LOW) / (KEY_HIGH - KEY_LOW)
            if alpha <= 0.0:
                output_pixels[x, y] = (0, 0, 0, 0)
                continue
            clean = (
                min(255, max(0, round((r - (1.0 - alpha) * BACKGROUND[0]) / alpha))),
                min(255, max(0, round((g - (1.0 - alpha) * BACKGROUND[1]) / alpha))),
                min(255, max(0, round((b - (1.0 - alpha) * BACKGROUND[2]) / alpha))),
            )
            output_pixels[x, y] = (*clean, round(alpha * 255))
    return result


def make_active(mark: Image.Image) -> Image.Image:
    active = mark.copy()
    draw = ImageDraw.Draw(active)
    size = active.width
    cx = cy = ACTIVE_CENTER * size
    radius = ACTIVE_RADIUS * size
    border = ACTIVE_BORDER * size
    draw.ellipse(
        (cx - radius - border, cy - radius - border, cx + radius + border, cy + radius + border),
        fill=(*BACKGROUND, 255),
    )
    draw.ellipse((cx - radius, cy - radius, cx + radius, cy + radius), fill=(*ACTIVE_GREEN, 255))
    return active


def make_symbolic(mark: Image.Image) -> Image.Image:
    symbolic = Image.new("RGBA", mark.size, (255, 255, 255, 0))
    symbolic.putalpha(mark.getchannel("A"))
    return symbolic


def resize(image: Image.Image, size: int) -> Image.Image:
    return image.resize((size, size), Image.Resampling.LANCZOS)


def write_argb(image: Image.Image, path: Path) -> None:
    data = image.convert("RGBA").tobytes()
    payload = bytearray(len(data))
    for index in range(0, len(data), 4):
        r, g, b, a = data[index : index + 4]
        payload[index] = a
        payload[index + 1] = r
        payload[index + 2] = g
        payload[index + 3] = b
    path.write_bytes(payload)


def main() -> None:
    full = Image.open(SOURCE).convert("RGBA")
    mark = key_background(load_mark())
    active = make_active(mark)
    symbolic = make_symbolic(mark)

    ICONS.mkdir(parents=True, exist_ok=True)
    mark.save(ICONS / "source-pipeduck-mark.png", optimize=True)
    active.save(ICONS / "source-pipeduck-on.png", optimize=True)

    for size in HICOLOR_SIZES:
        directory = ICONS / "hicolor" / f"{size}x{size}" / "apps"
        directory.mkdir(parents=True, exist_ok=True)
        app_icon = full if size > SMALL_MARK_LIMIT else mark
        resize(app_icon, size).save(directory / "pipeduck.png", optimize=True)
        resize(symbolic, size).save(directory / "pipeduck-symbolic.png", optimize=True)

    pixmap_directory = ICONS / "pixmap"
    pixmap_directory.mkdir(parents=True, exist_ok=True)
    for size in PIXMAP_SIZES:
        write_argb(resize(mark, size), pixmap_directory / f"pipeduck-{size}.argb")
        write_argb(resize(active, size), pixmap_directory / f"pipeduck-on-{size}.argb")


if __name__ == "__main__":
    main()
