#!/usr/bin/env python3
"""Functions that are the same instructions in several titles, most call sites first, with the module each sits in.

    python tools/shared_functions.py <title> <title> [...] [--leaf] [--least N] [--most N] [--rows N] [--all]

Reads each title's generated code (the "generated" folder of titles/<title>/progress.json or pipeline.json) and
its module map
(titles/<title>/modules.json). A function is its instructions with addresses masked, cut after its last return
(the generated code of some builds carries padding after it, which hid _stricmp and strstr from a comparison of
whole functions). Call sites are the direct calls in the generated code, summed over the titles.

--leaf keeps the functions that call nothing: the ones a native replacement can take alone.
Functions that sit in middleware or game code in any title are left out unless --all: the same instructions in
every title is not a library (Havok is in all three titles at hand), and only a library's functions are replaced
(docs/NATIVE_FUNCTIONS.md).
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFINE = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\)|^(?:PPC_FUNC_IMPL|REX_FUNC)\((?:__imp__)?(sub_[0-9A-F]{8})\)')
CALL = re.compile(r'\b(sub_[0-9A-F]{8})\(ctx, base\)')
ADDRESS = re.compile(r'0x[0-9a-f]{6,8}')


def cut(asm):
    last = -1
    for i, instruction in enumerate(asm):
        if instruction == 'blr' or instruction.startswith('b ADDR') or instruction.startswith('bctr'):
            last = i
    return asm[:last + 1] if last >= 0 else asm


def load(title):
    generated = None
    for config in ('progress.json', 'pipeline.json'):
        path = ROOT / 'titles' / title / config
        if path.exists():
            generated = json.loads(path.read_text(encoding='utf-8')).get('generated')
            if generated:
                break
    if not generated:
        sys.exit(f'titles/{title}: no "generated" folder in progress.json or pipeline.json')
    bodies, calls = {}, {}
    for path in sorted(Path(generated).glob('*recomp*.cpp')):
        name, asm = None, []
        for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
            m = DEFINE.match(line)
            if m:
                if name:
                    bodies[name] = cut(asm)
                name, asm = m.group(1) or m.group(2), []
                continue
            stripped = line.strip()
            if name and stripped.startswith('// '):
                asm.append(ADDRESS.sub('ADDR', stripped[3:]))
            for target in CALL.findall(line):
                calls[target] = calls.get(target, 0) + 1
        if name:
            bodies[name] = cut(asm)
    modules_path = ROOT / 'titles' / title / 'modules.json'
    modules = json.loads(modules_path.read_text(encoding='utf-8'))['modules'] if modules_path.exists() else []
    return bodies, calls, modules


def where(modules, name):
    address = int(name[4:], 16)
    for m in modules:
        if int(m['start'], 16) <= address < int(m['end'], 16):
            return m['kind'] + ': ' + m['name']
    return 'no module'


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('titles', nargs='+')
    parser.add_argument('--leaf', action='store_true')
    parser.add_argument('--least', type=int, default=6)
    parser.add_argument('--most', type=int, default=400)
    parser.add_argument('--rows', type=int, default=60)
    parser.add_argument('--all', action='store_true')
    args = parser.parse_args()
    if len(args.titles) < 2:
        parser.error('two titles at least')
    data = {title: load(title) for title in args.titles}
    index = {}
    for title, (bodies, calls, modules) in data.items():
        for name, asm in bodies.items():
            if args.least <= len(asm) <= args.most:
                index.setdefault(hashlib.sha1('\n'.join(asm).encode()).hexdigest(), {}).setdefault(title, []).append(name)
    rows = []
    for by_title in index.values():
        if len(by_title) != len(args.titles):
            continue
        first = args.titles[0]
        asm = data[first][0][by_title[first][0]]
        text = ' '.join(asm)
        calls_out = 'bl ADDR' in text or 'bctrl' in text or 'blrl' in text
        if args.leaf and calls_out:
            continue
        places = {t: where(data[t][2], names[0]) for t, names in by_title.items()}
        if not args.all and any(p.startswith(('middleware', 'game')) for p in places.values()):
            continue
        sites = sum(data[t][1].get(n, 0) for t, names in by_title.items() for n in names)
        rows.append((sites, calls_out, by_title, places, asm))
    rows.sort(key=lambda r: -r[0])
    print(f'{len(rows)} functions are the same instructions in {", ".join(args.titles)}')
    for sites, calls_out, by_title, places, asm in rows[:args.rows]:
        print(f'\n{sites} call sites, {len(asm)} instructions, {"calls" if calls_out else "leaf"}')
        for title in args.titles:
            print(f'  {title}: {by_title[title][0][4:]}  [{places[title]}]' +
                  (f'  (+{len(by_title[title]) - 1} more)' if len(by_title[title]) > 1 else ''))
        print('  ' + ' | '.join(asm)[:600])
    return 0


if __name__ == '__main__':
    sys.exit(main())
