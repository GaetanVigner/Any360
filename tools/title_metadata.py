#!/usr/bin/env python3
"""A title's metadata from its XEX: name, icon and achievements, into its bundle.

    python tools/title_metadata.py <title> <default.xex> [--out <bundle folder>]
    python pipeline/any360.py <title> metadata            (xex from titles/<title>/pipeline.json)

Every Xbox 360 executable (default.xex) carries a resource named after its
title id: the SPA, an XDBF database the dashboard read for the title's name,
icon and achievements. The ReXGlue runtime reads the same resource when the
game loads (rex/system/util/xdbf_utils.h, from Xenia) — that is where the
ports' in-game achievements come from. This step reads it once, from the file,
so a launcher has it without running the game:

    title.json            title id, version, name in every language, default language
    achievements.json     id, name, description, locked description, gamerscore, secret, icon
    icons/title.png       the title's icon
    icons/achievements/<image id>.png

in the title's bundle folder (docs/TITLE_BUNDLE.md); achievementsTotal and
titleId are set in its any360.game.json when there is one. Nothing written
here is committed: it is the publisher's.

Only the resource is decrypted, not the whole executable: XEX images are
AES-128-CBC (the retail key, public in every Xbox 360 tool) over a stream
that "basic" compression splits into data runs and zero runs, and CBC
decrypts any block from the one before it. pycryptodome is used when it is
installed; a small pure-Python AES does the job otherwise (a few seconds for
a 500 KB resource). LZX-compressed executables ("normal" compression) are not
supported yet.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
from title_art import bundle_dir  # noqa: E402

RETAIL_KEY = bytes.fromhex('20B185A59D28FDC340583FBB0896BF91')
DEVKIT_KEY = bytes(16)

LANGUAGES = {1: 'en', 2: 'ja', 3: 'de', 4: 'fr', 5: 'es', 6: 'it', 7: 'ko', 8: 'zh-Hant', 9: 'pt', 10: 'zh-Hans',
             11: 'pl', 12: 'ru'}
TITLE_STRING_ID = 0x8000
TITLE_IMAGE_ID = 0x8000
ACHIEVEMENT_SHOW_UNACHIEVED = 0x8


# ---- AES-128 decryption ---------------------------------------------------------

def _xtime(a: int) -> int:
    a <<= 1
    return (a ^ 0x11B) if a & 0x100 else a


def _mul(a: int, b: int) -> int:
    result = 0
    while b:
        if b & 1:
            result ^= a
        a = _xtime(a)
        b >>= 1
    return result


def _make_sbox() -> list[int]:
    sbox = [0] * 256
    p = q = 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ (((q << 1) | (q >> 7)) & 0xFF) ^ (((q << 2) | (q >> 6)) & 0xFF) ^ (((q << 3) | (q >> 5)) & 0xFF) ^ (((q << 4) | (q >> 4)) & 0xFF)
        sbox[p] = x ^ 0x63
        if p == 1:
            break
    sbox[0] = 0x63
    return sbox


_SBOX = _make_sbox()
_INV_SBOX = [0] * 256
for _i, _v in enumerate(_SBOX):
    _INV_SBOX[_v] = _i
_MUL = {k: [_mul(x, k) for x in range(256)] for k in (9, 11, 13, 14)}


class Aes128:
    """AES-128 block decryption (FIPS-197), pycryptodome when available."""

    def __init__(self, key: bytes):
        self.key = key
        try:
            from Crypto.Cipher import AES  # type: ignore
            self._fast = AES.new(key, AES.MODE_ECB)
        except ImportError:
            self._fast = None
            self._round_keys = self._expand(key)

    @staticmethod
    def _expand(key: bytes) -> list[list[int]]:
        words = [list(key[i:i + 4]) for i in range(0, 16, 4)]
        rcon = 1
        for i in range(4, 44):
            t = list(words[i - 1])
            if i % 4 == 0:
                t = t[1:] + t[:1]
                t = [_SBOX[b] for b in t]
                t[0] ^= rcon
                rcon = _xtime(rcon) & 0xFF
            words.append([a ^ b for a, b in zip(words[i - 4], t)])
        return [sum(words[r * 4:r * 4 + 4], []) for r in range(11)]

    def decrypt_block(self, block: bytes) -> bytes:
        if self._fast is not None:
            return self._fast.decrypt(block)
        s = [b ^ k for b, k in zip(block, self._round_keys[10])]
        m9, m11, m13, m14 = _MUL[9], _MUL[11], _MUL[13], _MUL[14]
        for rnd in range(9, -1, -1):
            # inverse shift rows (column-major state)
            s = [s[0], s[13], s[10], s[7], s[4], s[1], s[14], s[11], s[8], s[5], s[2], s[15], s[12], s[9], s[6], s[3]]
            s = [_INV_SBOX[b] for b in s]
            rk = self._round_keys[rnd]
            s = [b ^ k for b, k in zip(s, rk)]
            if rnd:
                out = []
                for c in range(4):
                    a0, a1, a2, a3 = s[4 * c:4 * c + 4]
                    out += [m14[a0] ^ m11[a1] ^ m13[a2] ^ m9[a3], m9[a0] ^ m14[a1] ^ m11[a2] ^ m13[a3],
                            m13[a0] ^ m9[a1] ^ m14[a2] ^ m11[a3], m11[a0] ^ m13[a1] ^ m9[a2] ^ m14[a3]]
                s = out
        return bytes(s)

    def decrypt_cbc(self, data: bytes, iv: bytes) -> bytes:
        if self._fast is not None:
            from Crypto.Cipher import AES  # type: ignore
            return AES.new(self.key, AES.MODE_CBC, iv).decrypt(data)
        out = bytearray()
        previous = iv
        for i in range(0, len(data), 16):
            block = data[i:i + 16]
            out += bytes(a ^ b for a, b in zip(self.decrypt_block(block), previous))
            previous = block
        return bytes(out)


# ---- XEX2 ---------------------------------------------------------------------

class XexError(Exception):
    pass


class Xex:
    """The parts of an XEX2 file needed to read a resource of its image."""

    def __init__(self, data: bytes):
        if data[:4] != b'XEX2':
            raise XexError('not an XEX2 file')
        self.data = data
        self.image_offset = struct.unpack_from('>I', data, 0x08)[0]
        security = struct.unpack_from('>I', data, 0x10)[0]
        count = struct.unpack_from('>I', data, 0x14)[0]
        self.headers = {}
        for i in range(count):
            key, value = struct.unpack_from('>II', data, 0x18 + 8 * i)
            self.headers[key] = value
        self.load_address = struct.unpack_from('>I', data, security + 0x110)[0]
        self.file_key = data[security + 0x150:security + 0x160]

        execution = self.headers.get(0x00040006)
        if execution is None:
            raise XexError('no execution info header')
        _, self.version, _, self.title_id = struct.unpack_from('>IIII', data, execution)

        fmt = self.headers.get(0x000003FF)
        if fmt is None:
            raise XexError('no file format header')
        info_size, self.encryption, self.compression = struct.unpack_from('>IHH', data, fmt)
        # (image offset, length, stream offset or None for zeros)
        self.runs: list[tuple[int, int, int | None]] = []
        if self.compression == 0:
            self.runs.append((0, len(data) - self.image_offset, 0))
        elif self.compression == 1:
            image = stream = 0
            for i in range((info_size - 8) // 8):
                size, zeros = struct.unpack_from('>II', data, fmt + 8 + 8 * i)
                if size:
                    self.runs.append((image, size, stream))
                if zeros:
                    self.runs.append((image + size, zeros, None))
                image += size + zeros
                stream += size
        else:
            raise XexError('LZX-compressed executable (normal compression) not supported yet')
        self._aes = None

    def resources(self) -> list[tuple[str, int, int]]:
        """(name, address, size) of every resource of the image."""
        at = self.headers.get(0x000002FF)
        if at is None:
            return []
        size = struct.unpack_from('>I', self.data, at)[0]
        out = []
        for i in range((size - 4) // 16):
            name = self.data[at + 4 + 16 * i:at + 12 + 16 * i].rstrip(b'\0').decode('ascii', 'replace')
            address, length = struct.unpack_from('>II', self.data, at + 12 + 16 * i)
            out.append((name, address, length))
        return out

    def _stream(self, start: int, length: int, key: bytes) -> bytes:
        """Decrypted bytes [start, start + length) of the image data stream."""
        begin = start - start % 16
        end = start + length
        end += -end % 16
        cipher = self.data[self.image_offset + begin:self.image_offset + end]
        if self.encryption == 0:
            return cipher[start - begin:start - begin + length]
        iv = bytes(16) if begin == 0 else self.data[self.image_offset + begin - 16:self.image_offset + begin]
        session = Aes128(key).decrypt_block(self.file_key)
        plain = Aes128(session).decrypt_cbc(cipher, iv)
        return plain[start - begin:start - begin + length]

    def read(self, address: int, length: int, key: bytes = RETAIL_KEY) -> bytes:
        """Bytes of the loaded image at a guest address."""
        offset = address - self.load_address
        out = bytearray()
        for image, size, stream in self.runs:
            lo, hi = max(offset, image), min(offset + length, image + size)
            if lo >= hi:
                continue
            if len(out) != lo - offset:
                raise XexError('image range not covered by the file')
            out += bytes(hi - lo) if stream is None else self._stream(stream + lo - image, hi - lo, key)
        if len(out) != length:
            raise XexError('image range beyond the file')
        return bytes(out)

    def spa(self) -> bytes:
        """The title's SPA (XDBF) resource, decrypted."""
        name = f'{self.title_id:08X}'
        for res_name, address, size in self.resources():
            if res_name.upper() == name:
                for key in (RETAIL_KEY, DEVKIT_KEY):
                    blob = self.read(address, size, key)
                    if blob[:4] == b'XDBF':
                        return blob
                raise XexError('the SPA resource does not decrypt with the retail or devkit key')
        raise XexError(f'no resource named {name}')


# ---- XDBF ---------------------------------------------------------------------

class Xdbf:
    """An XDBF database: entries by (namespace, id). Namespaces: 1 metadata, 2 images, 3 strings."""

    def __init__(self, blob: bytes):
        if blob[:4] != b'XDBF':
            raise XexError('not an XDBF database')
        _, _, entry_count, used, free_count, _ = struct.unpack_from('>6I', blob, 0)
        data_start = 24 + entry_count * 18 + free_count * 8
        self.blob = blob
        self.entries: dict[tuple[int, int], bytes] = {}
        for i in range(used):
            namespace, entry_id, offset, size = struct.unpack_from('>HQII', blob, 24 + 18 * i)
            self.entries[(namespace, entry_id)] = blob[data_start + offset:data_start + offset + size]

    def section(self, magic: str) -> bytes | None:
        return self.entries.get((1, int.from_bytes(magic.encode('ascii'), 'big')))

    def image(self, image_id: int) -> bytes | None:
        return self.entries.get((2, image_id))

    def strings(self, language: int) -> dict[int, str]:
        table = self.entries.get((3, language))
        if not table or table[:4] != b'XSTR':
            return {}
        count = struct.unpack_from('>H', table, 12)[0]
        out, at = {}, 14
        for _ in range(count):
            string_id, length = struct.unpack_from('>HH', table, at)
            out[string_id] = table[at + 4:at + 4 + length].decode('utf-8', 'replace')
            at += 4 + length
        return out

    def languages(self) -> list[int]:
        return sorted(entry_id for namespace, entry_id in self.entries if namespace == 3)

    def default_language(self) -> int:
        xstc = self.section('XSTC')
        return struct.unpack_from('>I', xstc, 12)[0] if xstc else 1

    def achievements(self) -> list[dict]:
        xach = self.section('XACH')
        if not xach:
            return []
        count = struct.unpack_from('>H', xach, 12)[0]
        out = []
        for i in range(count):
            (aid, label, description, unachieved, image, gamerscore, _, flags) = struct.unpack_from('>HHHHIHHI', xach, 14 + 0x24 * i)
            out.append({'id': aid, 'label': label, 'description': description, 'unachieved': unachieved,
                        'image': image, 'gamerscore': gamerscore, 'flags': flags})
        return out


def extract(xex_path: Path, folder: Path) -> dict:
    """Writes title.json, achievements.json and icons/ into the bundle folder; returns a summary."""
    xex = Xex(xex_path.read_bytes())
    db = Xdbf(xex.spa())
    default = db.default_language()
    tables = {lang: db.strings(lang) for lang in db.languages()}
    text = tables.get(default) or next(iter(tables.values()), {})

    folder.mkdir(parents=True, exist_ok=True)
    icons = folder / 'icons'
    (icons / 'achievements').mkdir(parents=True, exist_ok=True)

    title = {
        'titleId': f'{xex.title_id:08X}',
        'version': f'{xex.version >> 28}.{(xex.version >> 24) & 0xF}.{(xex.version >> 8) & 0xFFFF}.{xex.version & 0xFF}',
        'defaultLanguage': LANGUAGES.get(default, str(default)),
        'names': {LANGUAGES.get(lang, str(lang)): table[TITLE_STRING_ID] for lang, table in tables.items() if TITLE_STRING_ID in table},
        'icon': None,
    }
    if (png := db.image(TITLE_IMAGE_ID)) is not None:
        (icons / 'title.png').write_bytes(png)
        title['icon'] = 'icons/title.png'

    achievements = []
    for a in db.achievements():
        icon = None
        if (png := db.image(a['image'])) is not None:
            icon = f'icons/achievements/{a["image"]}.png'
            (folder / icon).write_bytes(png)
        achievements.append({
            'id': a['id'],
            'name': text.get(a['label'], ''),
            'description': text.get(a['description'], ''),
            'lockedDescription': text.get(a['unachieved'], ''),
            'gamerscore': a['gamerscore'],
            'secret': not a['flags'] & ACHIEVEMENT_SHOW_UNACHIEVED,
            'icon': icon,
        })

    (folder / 'title.json').write_text(json.dumps(title, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    (folder / 'achievements.json').write_text(json.dumps(achievements, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')

    manifest = folder / 'any360.game.json'
    if manifest.exists():
        game = json.loads(manifest.read_text(encoding='utf-8'))
        game['titleId'] = title['titleId']
        game['achievementsTotal'] = len(achievements)
        manifest.write_text(json.dumps(game, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')

    return {'title': title['names'].get(title['defaultLanguage']) or next(iter(title['names'].values()), '?'),
            'titleId': title['titleId'], 'languages': len(tables), 'achievements': len(achievements),
            'gamerscore': sum(a['gamerscore'] for a in achievements)}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('title')
    parser.add_argument('xex', type=Path)
    parser.add_argument('--out', type=Path, help='bundle folder (default: <ANY360_HOME>/titles/<title>)')
    args = parser.parse_args(argv)
    summary = extract(args.xex, args.out or bundle_dir(args.title))
    print(f"{args.title}: {summary['title']} ({summary['titleId']}), {summary['languages']} languages, "
          f"{summary['achievements']} achievements, {summary['gamerscore']} G")
    return 0


if __name__ == '__main__':
    sys.exit(main())
