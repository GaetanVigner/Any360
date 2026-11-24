"""Test vectors for any360::sys's inflate and PNG decoder (layer/sys/tests/png_vectors.h), made with Python's zlib:
every deflate block kind and strategy over several kinds of data, and small PNGs of each 8-bit colour type with each
row filter, written here by hand (no imaging library). Run: python -I tools/make_png_vectors.py"""
import os
import random
import struct
import zlib

OUT = os.path.join(os.path.dirname(__file__), '..', 'layer', 'sys', 'tests', 'png_vectors.h')


def c_bytes(b):
    rows = []
    for i in range(0, len(b), 24):
        rows.append('    ' + ' '.join('0x%02X,' % v for v in b[i:i + 24]))
    return '\n'.join(rows) if rows else '    0'


def datas():
    rnd = random.Random(360)
    text = (b'Any360 rewrites Xbox 360 titles; the glyphs follow the bindings and the pad in hand. ' * 20)[:1200]
    yield 'empty', b''
    yield 'one', b'A'
    yield 'text', text
    yield 'runs', b'\0' * 300 + b'\xFF' * 100 + bytes(range(256)) * 2
    yield 'random', bytes(rnd.randrange(256) for _ in range(700))
    yield 'mixed', bytes(rnd.randrange(4) for _ in range(500)) + text[:300]


def compress(data, level, strategy):
    c = zlib.compressobj(level, zlib.DEFLATED, 15, 9, strategy)
    return c.compress(data) + c.flush()


def png(width, height, color, rows, palette=None, trns=None):
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d))
    out = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, color, 0, 0, 0))
    if palette:
        out += chunk(b'PLTE', b''.join(bytes(p) for p in palette))
    if trns:
        out += chunk(b'tRNS', bytes(trns))
    raw = b''.join(rows)
    z = zlib.compress(raw, 9)
    out += chunk(b'tEXt', b'Comment\0ancillary chunk, skipped')  # an ancillary chunk between the others
    out += chunk(b'IDAT', z[:len(z) // 2]) + chunk(b'IDAT', z[len(z) // 2:])  # data split over two chunks
    return out + chunk(b'IEND', b'')


def paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else b if pb <= pc else c


def filtered(lines, bpp):
    """Rows of raw samples, row y filtered with filter y % 5."""
    out, prev = [], bytes(len(lines[0]))
    for y, cur in enumerate(lines):
        f = y % 5
        row = bytearray()
        for i, v in enumerate(cur):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            pred = [0, a, b, (a + b) >> 1, paeth(a, b, c)][f]
            row.append((v - pred) & 0xFF)
        out.append(bytes([f]) + bytes(row))
        prev = cur
    return out


def images():
    rnd = random.Random(1950)
    w, h = 7, 10  # 10 rows: every filter twice
    for color, channels in ((0, 1), (2, 3), (4, 2), (6, 4)):
        lines = [bytes(rnd.randrange(256) if rnd.random() < 0.6 else (x * 37 + y * 11) & 0xFF for x in range(w * channels))
                 for y in range(h)]
        rgba = []
        for line in lines:
            for x in range(w):
                s = line[x * channels:(x + 1) * channels]
                r, g, b, a = {0: lambda: (s[0], s[0], s[0], 255), 2: lambda: (s[0], s[1], s[2], 255),
                              4: lambda: (s[0], s[0], s[0], s[1]), 6: lambda: tuple(s)}[color]()
                rgba.append(r | g << 8 | b << 16 | a << 24)
        yield 'color%d' % color, w, h, png(w, h, color, filtered(lines, channels)), rgba
    palette = [(rnd.randrange(256), rnd.randrange(256), rnd.randrange(256)) for _ in range(12)]
    trns = [rnd.randrange(256) for _ in range(5)]
    lines = [bytes(rnd.randrange(12) for _ in range(w)) for _ in range(h)]
    rgba = []
    for line in lines:
        for i in line:
            r, g, b = palette[i]
            rgba.append(r | g << 8 | b << 16 | (trns[i] if i < len(trns) else 255) << 24)
    yield 'palette', w, h, png(w, h, 3, filtered(lines, 1), palette, trns), rgba


def main():
    out = ['// Made by tools/make_png_vectors.py (Python zlib %s): do not edit.' % zlib.ZLIB_VERSION, '#pragma once', '',
           '#include <cstdint>', '', 'namespace png_vectors {', '']
    streams = []
    strategies = (('default', zlib.Z_DEFAULT_STRATEGY), ('filtered', zlib.Z_FILTERED),
                  ('huffman', zlib.Z_HUFFMAN_ONLY), ('rle', zlib.Z_RLE), ('fixed', zlib.Z_FIXED))
    for name, data in datas():
        out.append('inline const uint8_t %s_data[] = {\n%s\n};' % (name, c_bytes(data)))
        for level in (0, 1, 6, 9):
            for sname, strategy in strategies:
                if level == 0 and sname != 'default':
                    continue
                z = compress(data, level, strategy)
                v = '%s_%d_%s' % (name, level, sname)
                out.append('inline const uint8_t %s_z[] = {\n%s\n};' % (v, c_bytes(z)))
                streams.append((v, name, len(data), len(z)))
    out.append('')
    out.append('struct Stream { const char* name; const uint8_t* data; uint32_t size; const uint8_t* z; uint32_t z_size; };')
    out.append('inline const Stream kStreams[] = {')
    for v, name, n, zn in streams:
        out.append('    {"%s", %s_data, %d, %s_z, %d},' % (v, name, n, v, zn))
    out.append('};')
    out.append('')
    imgs = list(images())
    for name, w, h, data, rgba in imgs:
        out.append('inline const uint8_t %s_png[] = {\n%s\n};' % (name, c_bytes(data)))
        rows = ['    ' + ' '.join('0x%08Xu,' % v for v in rgba[i:i + 8]) for i in range(0, len(rgba), 8)]
        out.append('inline const uint32_t %s_rgba[] = {\n%s\n};' % (name, '\n'.join(rows)))
    out.append('')
    out.append('struct Image { const char* name; uint32_t width, height; const uint8_t* png; uint32_t png_size; const uint32_t* rgba; };')
    out.append('inline const Image kImages[] = {')
    for name, w, h, data, rgba in imgs:
        out.append('    {"%s", %d, %d, %s_png, %d, %s_rgba},' % (name, w, h, name, len(data), name))
    out.append('};')
    out.append('')
    out.append('}  // namespace png_vectors')
    with open(OUT, 'w', newline='\n') as f:
        f.write('\n'.join(out) + '\n')
    print('wrote %s: %d streams, %d images' % (os.path.normpath(OUT), len(streams), len(imgs)))


if __name__ == '__main__':
    main()
