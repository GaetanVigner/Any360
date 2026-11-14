#!/usr/bin/env python3
"""Title artwork in Any360's standard format, from a box cover.

    python tools/title_art.py <title> <cover image> [--focus 0.4] [--art-top auto]

Writes, in the title's bundle folder (docs/TITLE_BUNDLE.md):

    art/cover.jpg    600x900 portrait (2:3), the box cover centred and cropped to 2:3
    art/hero.jpg     1920x620 wide banner, built from the cover's artwork
    art/source.<ext> the cover it was made from
    art/art.json     the settings used, so `--rebuild` makes the same files again

The banner: the cover's artwork (without the console template's header) is
blurred and darkened to fill the banner, and the part around `focus` is laid
sharp on the right, its left edge fading into the blur. The left side stays
quiet for the title text a launcher draws there.

The console header (Xbox 360 template: grey band with a green stripe) is found
by looking for that stripe in the top fifth of the cover; `--art-top` gives the
fraction by hand when a cover has none or it is not found.

Covers are the user's: nothing here is committed. Generic: every option is a
parameter, a title only chooses its focus.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageEnhance, ImageFilter

COVER_SIZE = (600, 900)
HERO_SIZE = (1920, 620)
JPEG_QUALITY = 90

# The sharp band covers this share of the banner's width, the rest is blur.
FOREGROUND_WIDTH = 0.6
# Width of the fade from blur to sharp, as a share of the banner's width.
FADE_WIDTH = 0.12


def bundles_root() -> Path:
    """%ANY360_HOME% or the per-user data folder: <LOCALAPPDATA>/Any360 or ~/.local/share/Any360."""
    home = os.environ.get('ANY360_HOME')
    if home:
        return Path(home)
    if os.name == 'nt':
        return Path(os.environ.get('LOCALAPPDATA', Path.home() / 'AppData' / 'Local')) / 'Any360'
    return Path(os.environ.get('XDG_DATA_HOME', Path.home() / '.local' / 'share')) / 'Any360'


def store_python() -> bool:
    """The Microsoft Store's Python, which silently redirects files written under AppData into its own package folder."""
    return os.name == 'nt' and ('WindowsApps' in sys.executable or 'PythonSoftwareFoundation' in sys.prefix)


def bundle_dir(title: str) -> Path:
    return bundles_root() / 'titles' / title


def platform_header_end(image: Image.Image, search: float = 0.2) -> float:
    """Where the console template's header ends, as a fraction of the height (0 when there is none).

    The Xbox 360 template ends its header with a green stripe: rows whose
    middle half is mostly green. The header ends after the last such row in
    the top `search` of the cover.
    """
    rgb = image.convert('RGB')
    width, height = rgb.size
    left, right = width // 4, width * 3 // 4
    last_green = -1
    for y in range(int(height * search)):
        green = 0
        samples = 0
        for x in range(left, right, max(1, (right - left) // 64)):
            r, g, b = rgb.getpixel((x, y))
            samples += 1
            if g > 45 and g > r + 15 and g > b + 15:
                green += 1
        if samples and green / samples > 0.6:
            last_green = y
    return 0.0 if last_green < 0 else (last_green + 1) / height


def crop_to_ratio(image: Image.Image, ratio: float, focus_y: float = 0.5) -> Image.Image:
    """The largest part of the image with width/height == ratio, centred horizontally, around focus_y vertically."""
    width, height = image.size
    if width / height > ratio:
        new_width = round(height * ratio)
        left = (width - new_width) // 2
        return image.crop((left, 0, left + new_width, height))
    new_height = round(width / ratio)
    top = round(focus_y * height - new_height / 2)
    top = max(0, min(height - new_height, top))
    return image.crop((0, top, width, top + new_height))


def make_cover(image: Image.Image) -> Image.Image:
    """The box cover at 600x900: cropped to 2:3 (a few columns or the bottom), never stretched."""
    cover = crop_to_ratio(image.convert('RGB'), COVER_SIZE[0] / COVER_SIZE[1], focus_y=0.0)
    return cover.resize(COVER_SIZE, Image.LANCZOS)


def make_hero(image: Image.Image, focus: float, art_top: float) -> Image.Image:
    """The 1920x620 banner from the cover's artwork (below art_top), centred on focus (fraction of the cover's height)."""
    rgb = image.convert('RGB')
    width, height = rgb.size
    top = round(art_top * height)
    art = rgb.crop((0, top, width, height))
    # focus is given on the whole cover; move it into the artwork.
    art_focus = min(1.0, max(0.0, (focus * height - top) / art.height))
    hero_w, hero_h = HERO_SIZE

    # Backdrop: the artwork filling the banner, blurred and darkened.
    backdrop = crop_to_ratio(art, hero_w / hero_h, art_focus).resize(HERO_SIZE, Image.LANCZOS)
    backdrop = backdrop.filter(ImageFilter.GaussianBlur(radius=hero_h / 8))
    backdrop = ImageEnhance.Brightness(backdrop).enhance(0.45)

    # Foreground: the artwork at the foreground's width, the band around focus.
    fg_w = round(hero_w * FOREGROUND_WIDTH)
    scale = fg_w / art.width
    scaled = art.resize((fg_w, max(hero_h, round(art.height * scale))), Image.LANCZOS)
    band_top = round(art_focus * scaled.height - hero_h / 2)
    band_top = max(0, min(scaled.height - hero_h, band_top))
    band = scaled.crop((0, band_top, fg_w, band_top + hero_h))

    # Its left edge fades into the backdrop.
    mask = Image.new('L', band.size, 255)
    fade = max(1, round(hero_w * FADE_WIDTH))
    draw = ImageDraw.Draw(mask)
    for x in range(fade):
        draw.line([(x, 0), (x, hero_h)], fill=round(255 * x / fade))

    hero = backdrop.copy()
    hero.paste(band, (hero_w - fg_w, 0), mask)
    return hero


def build(title: str, source: Path, focus: float, art_top: float | None, out: Path | None = None) -> dict:
    """Writes the title's art; returns the settings saved in art.json."""
    folder = (out or bundle_dir(title)) / 'art'
    folder.mkdir(parents=True, exist_ok=True)

    with Image.open(source) as image:
        image.load()
    top = platform_header_end(image) if art_top is None else art_top

    kept = folder / f'source{source.suffix.lower()}'
    if source.resolve() != kept.resolve():
        for old in folder.glob('source.*'):
            old.unlink()
        shutil.copyfile(source, kept)

    make_cover(image).save(folder / 'cover.jpg', quality=JPEG_QUALITY, optimize=True, progressive=True)
    make_hero(image, focus, top).save(folder / 'hero.jpg', quality=JPEG_QUALITY, optimize=True, progressive=True)

    settings = {
        'source': kept.name,
        'focus': focus,
        'art_top': round(top, 4),
        'art_top_detected': art_top is None,
        'cover': {'file': 'cover.jpg', 'size': list(COVER_SIZE)},
        'hero': {'file': 'hero.jpg', 'size': list(HERO_SIZE)},
    }
    (folder / 'art.json').write_text(json.dumps(settings, indent=2) + '\n', encoding='utf-8')
    return settings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('title', help="the title's id (crackdown, simpsons…)")
    parser.add_argument('cover', nargs='?', help='the box cover (jpg, png, webp); omit with --rebuild')
    parser.add_argument('--focus', type=float, default=0.4, help="the banner's vertical centre, as a fraction of the cover's height")
    parser.add_argument('--art-top', default='auto', help="where the artwork starts below the console header (fraction) or 'auto'")
    parser.add_argument('--out', type=Path, help='bundle folder (default: <ANY360_HOME>/titles/<title>)')
    parser.add_argument('--rebuild', action='store_true', help='make the files again from art/source.* and art/art.json')
    args = parser.parse_args(argv)

    folder = args.out or bundle_dir(args.title)
    if args.out is None and not os.environ.get('ANY360_HOME') and store_python():
        print('warning: the Microsoft Store Python redirects writes under AppData into its package folder; '
              'the launcher will not see them. Use --out <folder> (then copy) or another Python.', file=sys.stderr)
    if args.rebuild:
        settings = json.loads((folder / 'art' / 'art.json').read_text(encoding='utf-8'))
        source = folder / 'art' / settings['source']
        focus = settings['focus']
        art_top = None if settings.get('art_top_detected') else settings['art_top']
    else:
        if not args.cover:
            parser.error('a cover image is needed (or --rebuild)')
        source = Path(args.cover)
        focus = args.focus
        art_top = None if args.art_top == 'auto' else float(args.art_top)

    settings = build(args.title, source, focus, art_top, folder)
    print(f"{args.title}: {folder / 'art'} (art from {settings['art_top']:.1%} of the cover, focus {focus:.0%})")
    return 0


if __name__ == '__main__':
    sys.exit(main())
