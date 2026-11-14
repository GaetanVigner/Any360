"""Tests for tools/title_art.py: python -m unittest discover -s tools -p "test_*.py"."""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).parent))
import title_art  # noqa: E402


def xbox_cover(width=400, height=570, header=0.11, stripe=6, art=(200, 40, 40)):
    """A synthetic Xbox 360 style cover: grey header, green stripe, coloured artwork with a white mark at 40%."""
    image = Image.new('RGB', (width, height), art)
    draw = ImageDraw.Draw(image)
    header_px = round(height * header)
    draw.rectangle((0, 0, width, header_px - stripe - 1), fill=(225, 226, 230))
    draw.rectangle((0, header_px - stripe, width, header_px - 1), fill=(60, 95, 20))
    mark = round(height * 0.4)
    draw.rectangle((width // 3, mark - 10, width * 2 // 3, mark + 10), fill=(255, 255, 255))
    return image


class HeaderDetection(unittest.TestCase):
    def test_finds_the_end_of_the_green_stripe(self):
        cover = xbox_cover(header=0.11)
        self.assertAlmostEqual(title_art.platform_header_end(cover), round(570 * 0.11) / 570, places=3)

    def test_a_cover_without_header_starts_at_zero(self):
        self.assertEqual(title_art.platform_header_end(Image.new('RGB', (300, 450), (30, 30, 200))), 0.0)

    def test_green_artwork_lower_down_is_not_a_header(self):
        cover = xbox_cover(header=0.1)
        ImageDraw.Draw(cover).rectangle((0, 300, 400, 330), fill=(40, 160, 40))
        self.assertLess(title_art.platform_header_end(cover), 0.12)


class Shapes(unittest.TestCase):
    def test_crop_to_ratio_never_stretches(self):
        wide = title_art.crop_to_ratio(Image.new('RGB', (1000, 100)), 2.0)
        tall = title_art.crop_to_ratio(Image.new('RGB', (100, 1000)), 0.5, focus_y=0.0)
        self.assertEqual(wide.size, (200, 100))
        self.assertEqual(tall.size, (100, 200))

    def test_the_cover_is_600x900(self):
        self.assertEqual(title_art.make_cover(xbox_cover()).size, title_art.COVER_SIZE)

    def test_the_banner_is_1920x620_with_the_focus_sharp_on_the_right(self):
        hero = title_art.make_hero(xbox_cover(), focus=0.4, art_top=0.11)
        self.assertEqual(hero.size, title_art.HERO_SIZE)
        # The white mark at 40% of the cover lands in the sharp part, at mid-height.
        right = hero.getpixel((round(1920 * 0.7), 310))
        self.assertGreater(min(right), 230)
        # The left side is the darkened blur: no bright pixel.
        self.assertLess(max(hero.getpixel((40, 310))), 160)

    def test_a_focus_at_the_edge_stays_inside_the_artwork(self):
        for focus in (0.0, 1.0):
            self.assertEqual(title_art.make_hero(xbox_cover(), focus=focus, art_top=0.11).size, title_art.HERO_SIZE)


class Build(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.source = self.root / 'box.png'
        xbox_cover().save(self.source)

    def tearDown(self):
        self.tmp.cleanup()

    def test_writes_the_standard_files_and_their_settings(self):
        out = self.root / 'bundle'
        settings = title_art.build('demo', self.source, 0.4, None, out)
        art = out / 'art'
        self.assertEqual(sorted(p.name for p in art.iterdir()), ['art.json', 'cover.jpg', 'hero.jpg', 'source.png'])
        for name, size in (('cover.jpg', (600, 900)), ('hero.jpg', (1920, 620))):
            with Image.open(art / name) as image:
                self.assertEqual(image.size, size)
        self.assertTrue(settings['art_top_detected'])
        self.assertEqual(json.loads((art / 'art.json').read_text(encoding='utf-8'))['focus'], 0.4)

    def test_rebuild_makes_the_same_files_from_the_kept_source(self):
        out = self.root / 'bundle'
        title_art.main(['demo', str(self.source), '--focus', '0.3', '--out', str(out)])
        first = (out / 'art' / 'hero.jpg').read_bytes()
        (out / 'art' / 'hero.jpg').unlink()
        self.source.unlink()
        title_art.main(['demo', '--rebuild', '--out', str(out)])
        self.assertEqual((out / 'art' / 'hero.jpg').read_bytes(), first)

    def test_a_manual_art_top_is_kept(self):
        settings = title_art.build('demo', self.source, 0.4, 0.2, self.root / 'b')
        self.assertEqual(settings['art_top'], 0.2)
        self.assertFalse(settings['art_top_detected'])


class Locations(unittest.TestCase):
    def test_any360_home_wins(self):
        with mock.patch.dict(os.environ, {'ANY360_HOME': '/x/Any360'}):
            self.assertEqual(title_art.bundle_dir('crackdown'), Path('/x/Any360') / 'titles' / 'crackdown')

    def test_default_is_the_per_user_data_folder(self):
        env = {k: v for k, v in os.environ.items() if k != 'ANY360_HOME'}
        with mock.patch.dict(os.environ, env, clear=True):
            root = title_art.bundles_root()
        self.assertEqual(root.name, 'Any360')
        self.assertIn('Local' if os.name == 'nt' else 'share', str(root))


if __name__ == '__main__':
    unittest.main()
