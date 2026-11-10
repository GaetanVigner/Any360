#!/usr/bin/env python3
"""Turns a title's evidence windows into named ranges: titles/<title>/library_evidence.json lists, for each library
or middleware, a window of the image and what proves a function belongs to it (a regular expression over the
function's line as tools/library_scan.py --functions prints it: the address of the identical function in another
title, a system call it makes, a string it refers to). The range runs from the first to the last proven function
of the window, up to the start of the function after it; {"exact": true} takes the window as it is. Prints the
ranges with how many of their functions are proven, and writes them as a "modules" list to paste into
titles/<title>/modules.json (whose _comment says in words what names each).

  python tools/library_ranges.py <title> <decrypted image> titles/<title>/library_evidence.json <out.json>
"""
import bisect
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))  # library_scan
import library_scan as ls

title, image_path, spec_path, out_path = sys.argv[1:5]
data = {}
for path in sorted((ROOT / 'titles').glob('*/progress.json')):
    cfg = json.loads(path.read_text(encoding='utf-8'))
    data[path.parent.name] = ls.scan(path.parent.name, cfg['generated'])
target = data[title]
starts = sorted(target)
others = {name: defaultdict(list) for name in data if name != title}
for name in others:
    for address, (count, digest, _) in data[name].items():
        if count >= ls.MIN:
            others[name][digest].append(address)
image = Path(image_path).read_bytes()
texts = defaultdict(list)
for at, text in ls.string_references(image, starts[0], starts[-1] + 4 * target[starts[-1]][0]):
    texts[starts[bisect.bisect_right(starts, at) - 1]].append(text)


def line(address):
    count, digest, imports = target[address]
    marks = [f'{name} {others[name][digest][0]:08X}' for name in others if count >= ls.MIN and digest in others[name]]
    return f'{"; ".join(marks)} calls {" ".join(imports)} ' + ' '.join(f'"{t}"' for t in texts[address])


modules = []
for entry in json.loads(Path(spec_path).read_text(encoding='utf-8')):
    lo, hi = int(entry['from'], 16), int(entry['to'], 16)
    if entry.get('exact'):
        first, end, proven, n = lo, hi, 0, sum(1 for a in starts if lo <= a < hi)
    else:
        pattern = re.compile(entry['proof'])
        inside = [a for a in starts if lo <= a < hi]
        hits = [a for a in inside if pattern.search(line(a))]
        if not hits:
            print(f'!! no proof for {entry["name"]} in {entry["from"]}-{entry["to"]}')
            continue
        first = hits[0]
        after = bisect.bisect_right(starts, hits[-1])
        end = starts[after] if after < len(starts) else hits[-1] + 4 * target[hits[-1]][0]
        proven, n = len(hits), sum(1 for a in starts if first <= a < end)
    print(f'{first:08X}-{end:08X} {entry["name"]:42s} {entry["kind"]:10s} {proven:4d} proven of {n:5d} functions')
    modules.append({'start': f'0x{first:08X}', 'end': f'0x{end:08X}', 'name': entry['name'], 'kind': entry['kind']})
Path(out_path).write_text(json.dumps({'modules': modules}, indent=1), encoding='utf-8')
