#!/usr/bin/env python3
"""Inventory of a recomp's ReXGlue SDK patch set: which SDK areas each patch touches, and whether it is generic.

Reads the `NNNN-name[.platform].patch` files of a patch directory (the Crackdown port keeps them in
`patches/rexglue-sdk/`) and, when present, the port's `docs/rexglue-patches.md` table for the one-line purpose of each.
A patch is classified by the SDK paths it changes (kernel, codegen, audio, GPU shared / D3D12 / Vulkan, UI, platform)
and flagged `game_named` when its text mentions the title (a candidate for the layer once renamed), so the progress page
and the design document can count what moves into the layer. Writes <out>/patches-<name>.json. No patch content is
copied: file names, touched SDK paths and purposes only.

  python tools/patch_inventory.py --name crackdown --patches <recomp>/patches/rexglue-sdk \
      [--purposes <recomp>/docs/rexglue-patches.md] [--game-words crackdown] [--out docs/coverage]
"""
import argparse
import json
import re
from collections import Counter
from datetime import date
from pathlib import Path

DIFF = re.compile(r'^diff --git a/(\S+) b/', re.M)
PURPOSE = re.compile(r'^\|\s*(\d{4})(?:-(\d{4}))?\s*\|\s*\w+\s*\|\s*(.+?)\s*\|\s*$', re.M)

AREAS = [  # first match wins
    ('HLE renderer channel', r'graphics/hle_frame|native_records'),
    ('GPU: Vulkan backend', r'graphics/vulkan/|ui/vulkan|spirv'),
    ('GPU: D3D12 backend', r'graphics/d3d12/|ui/d3d12'),
    ('GPU: shared (command processor, caches)', r'graphics/'),
    ('codegen (recompiler)', r'src/codegen/|resources/templates/codegen|include/rex/ppc'),
    ('kernel: XAM', r'kernel/xam'),
    ('kernel: xboxkrnl', r'kernel/xboxkrnl|kernel/kernel_init|system/x'),
    ('audio (XMA, mixer)', r'audio/'),
    ('input', r'input/'),
    ('file system', r'filesystem/'),
    ('UI / presenter', r'ui/'),
    ('core / platform (threads, clock, logging)', r'core/|logging|base/|math'),
    ('build', r'CMakeLists|cmake/|\.cmake'),
]


def area_of(paths):
    for area, pattern in AREAS:
        if any(re.search(pattern, p) for p in paths):
            return area
    return 'other'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--name', required=True)
    parser.add_argument('--patches', type=Path, required=True)
    parser.add_argument('--purposes', type=Path)
    parser.add_argument('--game-words', default='')
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'docs' / 'coverage')
    args = parser.parse_args()
    purposes = {}
    if args.purposes and args.purposes.exists():
        for first, last, text in PURPOSE.findall(args.purposes.read_text(encoding='utf-8', errors='replace')):
            for n in range(int(first), int(last or first) + 1):
                purposes[n] = text
    words = [w for w in args.game_words.lower().split(',') if w]
    patches = []
    for path in sorted(args.patches.glob('*.patch')):
        text = path.read_text(encoding='utf-8', errors='replace')
        number = int(path.name[:4])
        parts = path.name[:-6].split('.')
        platform = parts[1] if len(parts) > 1 else 'all'
        paths = sorted(set(DIFF.findall(text)))
        named = any(w in text.lower() for w in words)
        patches.append({'number': number, 'file': path.name, 'platform': platform, 'area': area_of(paths),
                        'paths': paths, 'purpose': purposes.get(number, ''), 'game_named': named,
                        'lines': text.count('\n')})
    areas = Counter(p['area'] for p in patches)
    out = {'name': args.name, 'date': date.today().isoformat(), 'count': len(patches),
           'game_named': sum(1 for p in patches if p['game_named']), 'areas': dict(areas.most_common()),
           'patches': patches}
    args.out.mkdir(parents=True, exist_ok=True)
    target = args.out / f'patches-{args.name}.json'
    target.write_text(json.dumps(out, indent=1) + '\n', encoding='utf-8')
    print(f'{len(patches)} patches, {out["game_named"]} name the game; by area: {dict(areas)}; wrote {target}')


if __name__ == '__main__':
    main()
