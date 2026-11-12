#!/usr/bin/env python3
"""Inventory of a title's XDK D3D runtime, from its generated code: every function in the runtime's range with its
size, the call sites reaching it from outside the runtime (entry points) and from inside, the runtime functions it
calls, the PM4 packets it builds itself (type-3 headers: lis 0xC0xx then ori with the opcode), the kernel exports it
calls (Vd*), and a first class for the native replacement:

  flush     builds PM4 (draws, clears, resolves, swaps, waits, state flushes): to become native renderer calls
  entry     called by the title, builds no PM4: state setters, resources, getters (to become native code that keeps
            the device's memory as the title reads it)
  internal  called only from inside the runtime

Also the functions OUTSIDE the runtime that build PM4 themselves (game-engine code writing GPU commands), which a
complete replacement has to take too.

    python tools/d3d_inventory.py <title> [--json out.json]
Reads titles/<title>/progress.json or pipeline.json for the generated folder and titles/<title>/modules.json for
the range named "XDK D3D runtime". Standard library only.
"""
import argparse
import json
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
START = re.compile(r'^(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL|REX_FUNC)\((?:__imp__)?(?:guest_[a-z0-9]+_)?sub_([0-9A-F]{8})\)')
CALL = re.compile(r'\b(?:guest_[a-z0-9]+_)?sub_([0-9A-F]{8})\(ctx, base\)')
IMPORT = re.compile(r'\b__imp__(Vd[A-Za-z0-9]+|Ke[A-Za-z0-9]+|Mm[A-Za-z0-9]+)\(ctx, base\)')
LIS = re.compile(r'^// lis (r\d+),(-?\d+)$')
ORI = re.compile(r'^// ori (r\d+),(r\d+),(\d+)$')
OPS = {0x10: 'NOP', 0x21: 'REG_RMW', 0x22: 'DRAW_INDX', 0x25: 'SET_STATE', 0x26: 'VIZ_QUERY', 0x27: 'IM_LOAD',
       0x2B: 'IM_LOAD_IMMEDIATE', 0x2D: 'SET_CONSTANT', 0x2E: 'SET_CONSTANT2', 0x2F: 'LOAD_ALU_CONSTANT',
       0x36: 'DRAW_INDX_2', 0x37: 'IM_STORE', 0x3A: 'WAIT_FOR_IDLE', 0x3B: 'INVALIDATE_STATE', 0x3C: 'WAIT_REG_MEM',
       0x3E: 'REG_TO_MEM', 0x3F: 'INDIRECT_BUFFER', 0x41: 'MEM_WRITE_CNTR', 0x45: 'COND_WRITE', 0x46: 'EVENT_WRITE',
       0x48: 'ME_INIT', 0x50: 'MEM_WRITE', 0x54: 'INTERRUPT', 0x58: 'EVENT_WRITE_SHD', 0x5A: 'EVENT_WRITE_EXT',
       0x5B: 'EVENT_WRITE_ZPD', 0x5C: 'CONTEXT_UPDATE', 0x60: 'SET_BIN_MASK_LO', 0x61: 'SET_BIN_MASK_HI',
       0x62: 'SET_BIN_SELECT_LO', 0x63: 'SET_BIN_SELECT_HI'}


def generated_dir(title):
    for name in ('progress.json', 'pipeline.json'):
        p = ROOT / 'titles' / title / name
        if p.exists():
            data = json.loads(p.read_text(encoding='utf-8'))
            for key in ('generated', 'generated_dir'):
                if key in data:
                    return Path(data[key])
    sys.exit('no generated folder named in titles/%s' % title)


def d3d_range(title):
    mods = json.loads((ROOT / 'titles' / title / 'modules.json').read_text(encoding='utf-8'))
    mods = mods.get('modules', mods) if isinstance(mods, dict) else mods
    for m in mods:
        if m['name'] == 'XDK D3D runtime':
            return int(m['start'], 16), int(m['end'], 16)
    sys.exit('no "XDK D3D runtime" range in titles/%s/modules.json' % title)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('title')
    ap.add_argument('--json')
    args = ap.parse_args()
    gen = generated_dir(args.title)
    lo, hi = d3d_range(args.title)
    inside = lambda a: lo <= a < hi
    funcs = {}  # address -> record

    def rec(a):
        return funcs.setdefault(a, {'size': 0, 'outside_sites': 0, 'outside_callers': set(), 'inside_sites': 0,
                                    'calls': set(), 'pm4': set(), 'imports': set()})

    pm4_outside = {}
    for path in sorted(gen.rglob('*.cpp')):
        fn = None
        pending = {}
        with open(path, encoding='utf-8', errors='replace') as f:
            for raw in f:
                line = raw.strip()
                m = START.match(raw)
                if m:
                    fn = int(m.group(1), 16)
                    pending = {}
                    if inside(fn):
                        rec(fn)
                    continue
                if fn is None:
                    continue
                if line.startswith('// ') and inside(fn):
                    funcs[fn]['size'] += 1
                for c in CALL.finditer(line):
                    t = int(c.group(1), 16)
                    if not inside(t) or t == fn:
                        continue
                    r = rec(t)
                    if inside(fn):
                        r['inside_sites'] += 1
                        funcs[fn]['calls'].add(t)
                    else:
                        r['outside_sites'] += 1
                        r['outside_callers'].add(fn)
                if inside(fn):
                    for i in IMPORT.finditer(line):
                        funcs[fn]['imports'].add(i.group(1))
                m = LIS.match(line)
                if m:
                    v = int(m.group(2)) & 0xFFFF
                    if 0xC000 <= v <= 0xC040:
                        pending[m.group(1)] = v
                    else:
                        pending.pop(m.group(1), None)
                    continue
                m = ORI.match(line)
                if m and m.group(2) in pending:
                    op = (int(m.group(3)) >> 8) & 0x7F
                    if op in OPS:
                        if inside(fn):
                            funcs[fn]['pm4'].add(OPS[op])
                        else:
                            pm4_outside.setdefault(fn, set()).add(OPS[op])
                    pending.pop(m.group(2), None)

    def klass(r):
        if r['pm4'] or r['imports']:
            return 'flush'
        return 'entry' if r['outside_sites'] else 'internal'

    rows = []
    for a, r in sorted(funcs.items()):
        rows.append({'address': '0x%08X' % a, 'class': klass(r), 'instructions': r['size'],
                     'outside_sites': r['outside_sites'], 'outside_callers': len(r['outside_callers']),
                     'inside_sites': r['inside_sites'], 'calls': ['0x%08X' % c for c in sorted(r['calls'])],
                     'pm4': sorted(r['pm4']), 'imports': sorted(r['imports'])})
    by = {}
    for r in rows:
        by.setdefault(r['class'], []).append(r)
    print('%s: XDK D3D runtime 0x%08X-0x%08X, %d functions, %d instructions' %
          (args.title, lo, hi, len(rows), sum(r['instructions'] for r in rows)))
    for k in ('flush', 'entry', 'internal'):
        rs = by.get(k, [])
        print('  %-8s %4d functions, %6d instructions, %5d call sites from the title' %
              (k, len(rs), sum(r['instructions'] for r in rs), sum(r['outside_sites'] for r in rs)))
    print('  outside the runtime, building PM4 themselves: %d functions: %s' %
          (len(pm4_outside), ', '.join('sub_%08X (%s)' % (a, '/'.join(sorted(o))) for a, o in sorted(pm4_outside.items()))))
    if args.json:
        Path(args.json).write_text(json.dumps({'title': args.title, 'range': ['0x%08X' % lo, '0x%08X' % hi],
                                               'functions': rows,
                                               'pm4_outside': {'0x%08X' % a: sorted(o) for a, o in sorted(pm4_outside.items())}},
                                              indent=1), encoding='utf-8')
        print('written:', args.json)


if __name__ == '__main__':
    main()
