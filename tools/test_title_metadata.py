"""Tests for tools/title_metadata.py: python -m unittest discover -s tools -p "test_*.py"."""
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).parent))
import title_metadata as tm  # noqa: E402

PNG = b'\x89PNG\r\n\x1a\nfake'


def pure_aes(key):
    """The pure-Python AES even where pycryptodome is installed."""
    with mock.patch.dict(sys.modules, {'Crypto': None, 'Crypto.Cipher': None}):
        return tm.Aes128(key)


def xstr(strings):
    body = struct.pack('>H', len(strings))
    for sid, text in strings.items():
        raw = text.encode('utf-8')
        body += struct.pack('>HH', sid, len(raw)) + raw
    return b'XSTR' + struct.pack('>II', 1, len(body)) + body


def xach(rows):
    body = struct.pack('>H', len(rows))
    for aid, label, desc, locked, image, score, flags in rows:
        body += struct.pack('>HHHHIHHI', aid, label, desc, locked, image, score, 0, flags) + bytes(16)
    return b'XACH' + struct.pack('>II', 1, len(body)) + body


def xdbf(entries):
    """entries: [(namespace, id, bytes)] -> an XDBF database with one free slot."""
    table, data = b'', b''
    for namespace, entry_id, blob in entries:
        table += struct.pack('>HQII', namespace, entry_id, len(data), len(blob))
        data += blob
    free = struct.pack('>II', len(data), 0)
    header = b'XDBF' + struct.pack('>5I', 0x10000, len(entries), len(entries), 1, 1)
    return header + table + free + data


def spa(title_id=0x4D5307DC):
    return xdbf([
        (1, int.from_bytes(b'XSTC', 'big'), b'XSTC' + struct.pack('>III', 1, 4, 1)),
        (1, int.from_bytes(b'XACH', 'big'), xach([
            (1, 10, 11, 12, 5, 20, 0x8),
            (2, 13, 14, 15, 6, 30, 0x0),     # secret
            (3, 16, 17, 18, 99, 50, 0x8),    # icon missing
        ])),
        (2, 0x8000, PNG + b'title'),
        (2, 5, PNG + b'5'),
        (2, 6, PNG + b'6'),
        (3, 1, xstr({0x8000: 'Demo™', 10: 'First', 11: 'Do it', 12: 'Do it first', 13: 'Hidden',
                     14: 'Shh', 15: 'Secret achievement', 16: 'Last', 17: 'End', 18: 'Finish'})),
        (3, 4, xstr({0x8000: 'Démo'})),
    ])


def xex(image, title_id=0x4D5307DC, load=0x82000000, resource_at=0x100, runs=None):
    """An unencrypted XEX2 with basic compression: runs [(data, zeros)] splitting `image`."""
    runs = runs or [(len(image), 0)]
    stream = b''
    at = 0
    for size, zeros in runs:
        stream += image[at:at + size]
        assert image[at + size:at + size + zeros] == bytes(zeros)
        at += size + zeros
    headers = []
    blobs = b''
    base = 0x18 + 8 * 4  # after 4 optional header slots
    security_at = 0x200
    fmt = struct.pack('>IHH', 8 + 8 * len(runs), 0, 1) + b''.join(struct.pack('>II', s, z) for s, z in runs)
    res = struct.pack('>I', 4 + 16) + f'{title_id:08X}'.encode() + struct.pack('>II', load + resource_at, len(spa_blob))
    execution = struct.pack('>IIIIBBBBI', 0, 0x10000001, 0, title_id, 0, 0, 1, 1, 0)
    offsets = {}
    for key, blob in ((0x000003FF, fmt), (0x000002FF, res), (0x00040006, execution)):
        offsets[key] = base + len(blobs)
        blobs += blob
    header = bytearray(0x400)
    header[0:4] = b'XEX2'
    struct.pack_into('>I', header, 0x08, 0x400)
    struct.pack_into('>I', header, 0x10, security_at)
    struct.pack_into('>I', header, 0x14, len(offsets))
    for i, (key, value) in enumerate(offsets.items()):
        struct.pack_into('>II', header, 0x18 + 8 * i, key, value)
    header[base:base + len(blobs)] = blobs
    struct.pack_into('>I', header, security_at + 0x110, load)
    return bytes(header) + stream


spa_blob = spa()


class Aes(unittest.TestCase):
    def test_fips197_block(self):
        aes = pure_aes(bytes(range(16)))
        self.assertEqual(aes.decrypt_block(bytes.fromhex('69c4e0d86a7b0430d8cdb78070b4c55a')).hex(),
                         '00112233445566778899aabbccddeeff')

    def test_nist_cbc_vectors(self):
        aes = pure_aes(bytes.fromhex('2b7e151628aed2a6abf7158809cf4f3c'))
        cipher = bytes.fromhex('7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2')
        plain = aes.decrypt_cbc(cipher, bytes.fromhex('000102030405060708090a0b0c0d0e0f'))
        self.assertEqual(plain.hex(), '6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51')

    def test_cbc_from_the_middle_needs_only_the_previous_block(self):
        aes = pure_aes(bytes.fromhex('2b7e151628aed2a6abf7158809cf4f3c'))
        second = aes.decrypt_cbc(bytes.fromhex('5086cb9b507219ee95db113a917678b2'), bytes.fromhex('7649abac8119b246cee98e9b12e9197d'))
        self.assertEqual(second.hex(), 'ae2d8a571e03ac9c9eb76fac45af8e51')


class XdbfReading(unittest.TestCase):
    def setUp(self):
        self.db = tm.Xdbf(spa_blob)

    def test_languages_strings_and_default(self):
        self.assertEqual(self.db.languages(), [1, 4])
        self.assertEqual(self.db.default_language(), 1)
        self.assertEqual(self.db.strings(1)[0x8000], 'Demo™')
        self.assertEqual(self.db.strings(4)[0x8000], 'Démo')
        self.assertEqual(self.db.strings(9), {})

    def test_achievements_and_images(self):
        rows = self.db.achievements()
        self.assertEqual([(a['id'], a['gamerscore'], a['image']) for a in rows], [(1, 20, 5), (2, 30, 6), (3, 50, 99)])
        self.assertTrue(self.db.image(5).startswith(PNG))
        self.assertIsNone(self.db.image(99))

    def test_not_an_xdbf(self):
        with self.assertRaises(tm.XexError):
            tm.Xdbf(b'NOPE' + bytes(40))


class XexReading(unittest.TestCase):
    def test_header_fields_and_resource(self):
        image = bytes(0x100) + spa_blob
        x = tm.Xex(xex(image))
        self.assertEqual(x.title_id, 0x4D5307DC)
        self.assertEqual(x.load_address, 0x82000000)
        self.assertEqual(x.resources(), [('4D5307DC', 0x82000100, len(spa_blob))])
        self.assertEqual(x.spa(), spa_blob)

    def test_basic_compression_zero_runs_are_restored(self):
        # The SPA lies across a zero run: data, zeros, data.
        cut = 40
        zeros = 32
        spa_with_gap = spa_blob[:cut] + bytes(zeros) + spa_blob[cut:]
        image = bytes(0x100) + spa_with_gap
        x = tm.Xex(xex(image, runs=[(0x100 + cut, zeros), (len(spa_blob) - cut, 0)]))
        self.assertEqual(x.read(0x82000100, len(spa_with_gap)), spa_with_gap)

    def test_errors_say_what_is_wrong(self):
        with self.assertRaises(tm.XexError):
            tm.Xex(b'MZ' + bytes(100))
        x = tm.Xex(xex(bytes(0x100) + spa_blob, title_id=0x4D5307DC))
        x.title_id = 0x12345678
        with self.assertRaisesRegex(tm.XexError, '12345678'):
            x.spa()


class Extract(unittest.TestCase):
    def test_writes_title_achievements_icons_and_updates_the_manifest(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / 'default.xex'
            path.write_bytes(xex(bytes(0x100) + spa_blob))
            bundle = root / 'bundle'
            bundle.mkdir()
            (bundle / 'any360.game.json').write_text(json.dumps({'id': 'demo', 'title': 'Demo'}), encoding='utf-8')

            summary = tm.extract(path, bundle)

            self.assertEqual(summary, {'title': 'Demo™', 'titleId': '4D5307DC', 'languages': 2, 'achievements': 3, 'gamerscore': 100})
            title = json.loads((bundle / 'title.json').read_text(encoding='utf-8'))
            self.assertEqual(title['names'], {'en': 'Demo™', 'fr': 'Démo'})
            self.assertEqual(title['icon'], 'icons/title.png')
            rows = json.loads((bundle / 'achievements.json').read_text(encoding='utf-8'))
            self.assertEqual(rows[0], {'id': 1, 'name': 'First', 'description': 'Do it', 'lockedDescription': 'Do it first',
                                       'gamerscore': 20, 'secret': False, 'icon': 'icons/achievements/5.png'})
            self.assertTrue(rows[1]['secret'])
            self.assertIsNone(rows[2]['icon'])
            self.assertTrue((bundle / 'icons' / 'achievements' / '6.png').exists())
            game = json.loads((bundle / 'any360.game.json').read_text(encoding='utf-8'))
            self.assertEqual((game['titleId'], game['achievementsTotal'], game['title']), ('4D5307DC', 3, 'Demo'))


if __name__ == '__main__':
    unittest.main()
