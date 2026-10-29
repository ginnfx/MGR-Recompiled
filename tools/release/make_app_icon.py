#!/usr/bin/env python3
"""Build cmake/macos/AppIcon.icns from a square artwork image.

Follows the macOS app icon grid: the art fills an 824 px rounded square on a
1024 px canvas, with a soft drop shadow, so it sits with the system's icons.

    python3 tools/release/make_app_icon.py cmake/macos/AppIcon-source.jpg cmake/macos/AppIcon.icns
"""
import argparse
import pathlib
import subprocess
import sys
import tempfile

from PIL import Image, ImageChops, ImageDraw, ImageFilter

CANVAS = 1024
BODY = 824
RADIUS = 185
SHADOW_OFFSET = 10
SHADOW_BLUR = 14
SHADOW_ALPHA = 90
SUPERSAMPLE = 4
SIZES = (16, 32, 128, 256, 512)


def rounded_mask(size: int, radius: int) -> Image.Image:
    big = Image.new("L", (size * SUPERSAMPLE,) * 2, 0)
    ImageDraw.Draw(big).rounded_rectangle(
        (0, 0, size * SUPERSAMPLE - 1, size * SUPERSAMPLE - 1), radius * SUPERSAMPLE, fill=255)
    return big.resize((size, size), Image.LANCZOS)


def build_master(source: pathlib.Path) -> Image.Image:
    art = Image.open(source).convert("RGB")
    side = min(art.size)
    left, top = (art.width - side) // 2, (art.height - side) // 2
    art = art.crop((left, top, left + side, top + side)).resize((BODY, BODY), Image.LANCZOS)
    mask = rounded_mask(BODY, RADIUS)
    origin = (CANVAS - BODY) // 2

    shadow = Image.new("L", (CANVAS, CANVAS), 0)
    shadow.paste(mask, (origin, origin + SHADOW_OFFSET))
    shadow = shadow.filter(ImageFilter.GaussianBlur(SHADOW_BLUR))
    shadow = ImageChops.multiply(shadow, Image.new("L", shadow.size, SHADOW_ALPHA))
    icon = Image.new("RGBA", (CANVAS, CANVAS), (0, 0, 0, 0))
    icon.putalpha(shadow)
    body = art.convert("RGBA")
    body.putalpha(mask)
    icon.alpha_composite(body, (origin, origin))
    return icon


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    args = parser.parse_args()
    if args.output.suffix != ".icns":
        sys.exit("output must be an .icns file")

    master = build_master(args.source)
    with tempfile.TemporaryDirectory() as tmp:
        iconset = pathlib.Path(tmp) / "AppIcon.iconset"
        iconset.mkdir()
        for size in SIZES:
            master.resize((size, size), Image.LANCZOS).save(iconset / f"icon_{size}x{size}.png")
            master.resize((size * 2,) * 2, Image.LANCZOS).save(iconset / f"icon_{size}x{size}@2x.png")
        subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(args.output)], check=True)


if __name__ == "__main__":
    main()
