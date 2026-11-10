#!/usr/bin/env python3
"""Library recognition by comparison, the evidence behind titles/<title>/modules.json.

Titles built with the same SDK link the same libraries (the XDK's D3D runtime, audio, Xbox Live, the CRT) and often
the same middleware (Havok, Bink). A function of one title is "shared" when a function of another has the same
sequence of instruction mnemonics (operands ignored) and is long enough not to be a coincidence; shared functions
that sit together are a library. For a range of the title's code this prints, bucket by bucket:

  the functions, how many are shared with each other title (and around which address there),
  the system calls made from the bucket (Vd* says D3D, XAudio* audio, XMsg* Xbox Live ...),
  the longest strings the bucket refers to (lis / addi pairs into the decrypted image, when --image is given):
  source paths and messages name a library outright.

Titles are those with a titles/<title>/progress.json (its "generated" path). The scan of each title is cached in
out/library_scan. Addresses, counts, public API names and the strings quoted as evidence only: no game code.

  python tools/library_scan.py tdu                                          (64 KB buckets, the whole image)
  python tools/library_scan.py tdu --image <default_image.bin> --start 0x82660000 --end 0x826A0000 --bucket 0x1000
  python tools/library_scan.py tdu --functions --start 0x82663000 --end 0x82664800    (function by function)
"""
import argparse
import hashlib
import json
import pickle
import re
import struct
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CACHE = ROOT / 'out' / 'library_scan'
FUNC = re.compile(r'DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)')
MNEMONIC = re.compile(r'^[ \t]*// ([a-z][a-z0-9_.]*[+-]?)(?: [^\n]*)?\n(?![ \t]*//)', re.M)
IMPORT = re.compile(r'__imp__([A-Za-z0-9_]+)\(')
MIN = 14  # instructions a function needs for a match to count
BASE = 0x82000000


def scan(name, generated):
    """address -> (instructions, hash of the mnemonic sequence, system calls made)."""
    cache = CACHE / f'{name}.pkl'
    if cache.exists():
        return pickle.loads(cache.read_bytes())
    functions = {}
    for path in sorted(Path(generated).glob('*_recomp.*.cpp')):
        text = path.read_text(encoding='utf-8', errors='replace')
        starts = [(m.start(), int(m.group(1), 16)) for m in FUNC.finditer(text)]
        for i, (start, address) in enumerate(starts):
            body = text[start:starts[i + 1][0] if i + 1 < len(starts) else len(text)] + '\n'
            mnemonics = MNEMONIC.findall(body)
            functions[address] = (len(mnemonics), hashlib.md5(' '.join(mnemonics).encode()).hexdigest(),
                                  tuple(sorted(set(IMPORT.findall(body)))))
    CACHE.mkdir(parents=True, exist_ok=True)
    cache.write_bytes(pickle.dumps(functions))
    return functions


def referenced_strings(image, start, end, bucket):
    """bucket -> Counter of the printable strings its code refers to."""
    found = defaultdict(Counter)
    for address in range(start & ~3, min(end, BASE + len(image) - 28), 4):
        word = struct.unpack_from('>I', image, address - BASE)[0]
        if word >> 26 != 15 or (word >> 16) & 31 != 0:  # lis rX,hi
            continue
        reg, hi = (word >> 21) & 31, word & 0xFFFF
        for k in range(1, 7):
            nxt = struct.unpack_from('>I', image, address - BASE + 4 * k)[0]
            if nxt >> 26 == 14 and (nxt >> 16) & 31 == reg:  # addi rY,rX,lo
                lo = nxt & 0xFFFF
                target = ((hi << 16) + (lo - 0x10000 if lo & 0x8000 else lo)) & 0xFFFFFFFF
                if BASE <= target < BASE + len(image):
                    m = re.match(rb'[\x20-\x7e]{6,80}', image[target - BASE:target - BASE + 81])
                    if m:
                        found[address // bucket * bucket][m.group().decode('ascii')] += 1
                break
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('title')
    parser.add_argument('--image', type=Path, help='the decrypted image, for the strings')
    parser.add_argument('--start', type=lambda v: int(v, 0), default=BASE)
    parser.add_argument('--end', type=lambda v: int(v, 0), default=0x90000000)
    parser.add_argument('--bucket', type=lambda v: int(v, 0), default=0x10000)
    parser.add_argument('--functions', action='store_true', help='one entry a function instead of buckets')
    args = parser.parse_args()

    data = {}
    for path in sorted((ROOT / 'titles').glob('*/progress.json')):
        cfg = json.loads(path.read_text(encoding='utf-8'))
        if Path(cfg['generated']).exists():
            data[path.parent.name] = scan(path.parent.name, cfg['generated'])
    target = data[args.title]
    others = {name: defaultdict(list) for name in data if name != args.title}
    for name in others:
        for address, (count, digest, _) in data[name].items():
            if count >= MIN:
                others[name][digest].append(address)
    inside = [a for a in sorted(target) if args.start <= a < args.end]

    if args.functions:
        for address in inside:
            count, digest, imports = target[address]
            marks = [f'{name} {others[name][digest][0]:08X}' for name in others
                     if count >= MIN and digest in others[name]]
            print(f'{address:08X} {count:5d}  {"; ".join(marks)}{"  calls " + " ".join(imports) if imports else ""}')
        return

    strings = referenced_strings(args.image.read_bytes(), args.start, min(args.end, inside[-1] + 0x1000),
                                 args.bucket) if args.image and inside else {}
    rows = defaultdict(lambda: {'n': 0, 'imports': Counter(), **{name: Counter() for name in others}})
    for address in inside:
        count, digest, imports = target[address]
        r = rows[address // args.bucket * args.bucket]
        r['n'] += 1
        r['imports'].update(imports)
        if count >= MIN:
            for name in others:
                if digest in others[name]:
                    r[name][others[name][digest][0] >> 16] += 1
    print(f'{args.title}: {len(inside):,} functions in range; shared with '
          + ', '.join(f'{name} {sum(sum(r[name].values()) for r in rows.values()):,}' for name in others))
    for b, r in sorted(rows.items()):
        line = f'{b:08X} {r["n"]:4d} functions'
        for name in others:
            top = r[name].most_common(1)
            line += f' | {name} {sum(r[name].values()):3d}' + (f' near {top[0][0]:04X}0000' if top else '')
        if r['imports']:
            line += ' | calls ' + ' '.join(k for k, _ in r['imports'].most_common(4))
        longest = sorted(strings.get(b, {}), key=len, reverse=True)[:2]
        if longest:
            line += ' | strings ' + ' ## '.join(s[:60] for s in longest)
        print(line)


if __name__ == '__main__':
    main()
