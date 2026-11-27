#!/usr/bin/env python3
"""A title's D3D device layout (any360::xdk::D3dLayout) read from the evidence of its own D3D runtime.

    python tools/d3d_layout_map.py <title> [--reference crackdown] [--out titles/<title>/d3d_layout.json]

The reference title's layout is D3dLayout's defaults (layer/xdk/include/any360/xdk/d3d_state.h). Every function of the
reference's XDK D3D runtime (its modules.json range) whose mnemonic sequence is the same as exactly one function of
the title's D3D range is paired with it, and the two are read instruction by instruction:

  - a memory operand N(rX) whose N differs between the two gives the pair (reference N, title N): a device field
    that moved (the operands' registers must be the same in both);
  - an address built from lis + a 16-bit offset (addi, or N(rX) after lis) whose value differs gives the pair
    (reference address, title address): a constant of the runtime in the image.

For each field of the layout, the title's value is the one its reference value is paired with: kept when every
pairing that names it agrees (and the count of agreeing pairings is the evidence), reported as conflicting
otherwise, missing when no paired function reads it. A field whose value equals its reference value in the paired
code (the operand is the same in both) counts as evidence for an unchanged field.

Writes the title's layout as JSON: each field, its value, how many pairings back it and in which functions.
"""
import argparse
import difflib
import hashlib
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import shared_functions as sf  # noqa: E402

MEM = re.compile(r'^(\S+)\s+(.*)$')
DISP = re.compile(r'(-?\d+)\((r\d+)\)')
LIS = re.compile(r'^lis (r\d+),(-?\d+)$')
ADDI = re.compile(r'^addi (r\d+),(r\d+),(-?\d+)$')


def raw_bodies(title):
    """Each function's listing as generated (operands kept), cut after its last return."""
    generated = None
    for config in ('progress.json', 'pipeline.json'):
        path = ROOT / 'titles' / title / config
        if path.exists():
            generated = json.loads(path.read_text(encoding='utf-8')).get('generated')
            if generated:
                break
    bodies = {}
    for path in sorted(Path(generated).glob('*recomp*.cpp')):
        name, asm = None, []
        for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
            m = sf.DEFINE.match(line)
            if m:
                if name:
                    bodies[name] = sf.cut(asm)
                name, asm = m.group(1) or m.group(2), []
                continue
            s = line.strip()
            if name and s.startswith('// ') and not s.startswith('// ---'):
                asm.append(s[3:])
        if name:
            bodies[name] = sf.cut(asm)
    return bodies


def d3d_range(title):
    mods = json.loads((ROOT / 'titles' / title / 'modules.json').read_text(encoding='utf-8'))['modules']
    return [(int(m['start'], 16), int(m['end'], 16)) for m in mods if m['name'] == 'XDK D3D runtime']


def in_ranges(name, ranges):
    a = int(name[4:], 16)
    return any(s <= a < e for s, e in ranges)


def mnemonics(asm):
    return [x.split()[0] if x.split() else x for x in asm]


def constants(asm):
    """Per instruction: the image address it forms from a lis earlier in the function (or None)."""
    high = {}
    out = []
    for ins in asm:
        value = None
        m = LIS.match(ins)
        if m:
            high[m.group(1)] = (int(m.group(2)) & 0xFFFF) << 16
        else:
            m = ADDI.match(ins)
            if m and m.group(2) in high:
                value = (high[m.group(2)] + int(m.group(3))) & 0xFFFFFFFF
            else:
                d = DISP.search(ins)
                if d and d.group(2) in high:
                    value = (high[d.group(2)] + int(d.group(1))) & 0xFFFFFFFF
            mm = MEM.match(ins)
            if mm:  # a register written by this instruction loses its lis value
                dest = mm.group(2).split(',')[0]
                if dest in high and not ins.startswith(('st', 'cmp', 'b')):
                    del high[dest]
        out.append(value)
    return out


def pair_functions(ref, tit, min_ratio):
    """Each title function with the reference function whose mnemonics align best (ratio >= min_ratio), when that
    reference function picks it back (mutual best)."""
    rm = {n: mnemonics(a) for n, a in ref.items() if len(a) >= 6}
    tm = {n: mnemonics(a) for n, a in tit.items() if len(a) >= 6}
    def best(src, dst):
        out = {}
        for n, a in src.items():
            ca = Counter(a)
            top = []
            for m, b in dst.items():
                if not 0.8 <= len(b) / len(a) <= 1.25:
                    continue
                inter = sum((ca & Counter(b)).values())
                top.append((2 * inter / (len(a) + len(b)), m))
            top.sort(reverse=True)
            score, pick = 0.0, None
            for _, m in top[:4]:
                r = difflib.SequenceMatcher(None, a, dst[m], autojunk=False).ratio()
                if r > score:
                    score, pick = r, m
            if pick and score >= min_ratio:
                out[n] = pick
        return out
    fwd, back = best(rm, tm), best(tm, rm)
    return [(r, t) for r, t in fwd.items() if back.get(t) == r]


def layout_defaults():
    """D3dLayout's fields and their defaults, read from its declaration."""
    text = (ROOT / 'layer/xdk/include/any360/xdk/d3d_state.h').read_text(encoding='utf-8')
    body = text[text.index('struct D3dLayout {'):]
    body = body[:body.index('\n};')]
    fields = {}
    for line in body.splitlines():
        line = line.split('//')[0].strip()
        if not line.startswith('uint32_t'):
            continue
        for decl in re.findall(r'(\w+)(\[\d+\])?\s*=\s*(\{[^}]*\}|[0-9xXa-fA-Fu* ]+)', line[len('uint32_t'):]):
            name, array, value = decl
            if array:
                vals = [eval(v.strip().rstrip('uU')) for v in value.strip('{} ').split(',')]
                for i, v in enumerate(vals):
                    fields[f'{name}[{i}]'] = v
            else:
                fields[name] = eval(value.strip().replace('u', '').replace('U', ''))
    return fields


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('title')
    ap.add_argument('--reference', default='crackdown')
    ap.add_argument('--out')
    ap.add_argument('--min-ratio', type=float, default=0.85)
    ap.add_argument('--range', help='START-END (hex): the D3D range of the title when it has no modules.json yet')
    args = ap.parse_args()
    ref_all, tit_all = raw_bodies(args.reference), raw_bodies(args.title)
    ref = {n: a for n, a in ref_all.items() if in_ranges(n, d3d_range(args.reference))}
    title_range = ([tuple(int(x, 16) for x in args.range.split('-'))] if args.range else d3d_range(args.title))
    tit = {n: a for n, a in tit_all.items() if in_ranges(n, title_range)}
    pairs = pair_functions(ref, tit, args.min_ratio)
    disp = defaultdict(Counter)    # reference offset -> title offsets
    image = defaultdict(Counter)   # reference address -> title addresses
    where = defaultdict(set)
    for rn, tn in pairs:
        ra, ta = ref[rn], tit[tn]
        rc, tc = constants(ra), constants(ta)
        sm = difflib.SequenceMatcher(None, mnemonics(ra), mnemonics(ta), autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag != 'equal':
                continue
            for i, j in zip(range(i1, i2), range(j1, j2)):
                x, y = ra[i], ta[j]
                if rc[i] is not None and tc[j] is not None:
                    image[rc[i]][tc[j]] += 1
                    where[('image', rc[i], tc[j])].add(tn)
                    continue
                dx, dy = DISP.findall(x), DISP.findall(y)
                if len(dx) == len(dy):
                    for (ox, rx), (oy, ry) in zip(dx, dy):
                        if rx != 'r1' and ry != 'r1':
                            disp[int(ox)][int(oy)] += 1
                            where[('disp', int(ox), int(oy))].add(tn)
    fields = layout_defaults()
    # The device's offsets move by segments: per reference offset, the shift most of its pairings give (two at least,
    # three quarters of them); a field between two such offsets with the same shift takes that shift. Only offsets past
    # the device's first 1 KB are inferred so: below it the layout also holds register indexes and other objects'
    # offsets (float_constant_base, the query's fields), which only direct pairings decide. A pairing that disagrees
    # with its segment counts only with three pairings or more.
    shifts = []
    for o in sorted(disp):
        c = disp[o]
        value, n = c.most_common(1)[0]
        if o >= 0 and n >= 2 and n >= 0.75 * sum(c.values()):
            shifts.append((o, value - o, n))

    def segment(v):
        below = [s for s in shifts if s[0] <= v]
        above = [s for s in shifts if s[0] >= v]
        if below and above and below[-1][1] == above[0][1]:
            return below[-1][1], below[-1][0], above[0][0]
        return None

    out, unknown = {}, []
    for f, v in fields.items():
        kind = 'image' if v >= 0x80000000 else 'disp'
        c = (image if kind == 'image' else disp).get(v, Counter())
        seg = segment(v) if kind == 'disp' else None
        entry = None
        if c:
            value, n = c.most_common(1)[0]
            agrees = seg is not None and value == v + seg[0]
            against = seg is not None and not agrees and n < 3
            if ((n >= 2 and n >= 0.6 * sum(c.values())) or agrees) and not against:
                entry = {'value': value, 'evidence': f'{n} of {sum(c.values())} pairings',
                         'functions': sorted(where[(kind, v, value)])[:4]}
        if entry is None and seg is not None and v >= 1024:
            entry = {'value': v + seg[0], 'evidence': f'segment {seg[0]:+d} (fields {seg[1]} and {seg[2]} of the reference)'}
        if entry is None:
            unknown.append(f)
            continue
        entry['reference'] = v
        out[f] = entry
    print(f'{len(pairs)} functions paired ({len(ref)} in the D3D range of {args.reference}, {len(tit)} in that of {args.title})')
    print('segments (reference offset: shift, pairings): ' + ' '.join(f'{o}:{s:+d}({n})' for o, s, n in shifts))
    print(f'{len(out)} of {len(fields)} fields read, unknown: {unknown}')
    for f, d in out.items():
        mark = '' if d['value'] == d['reference'] else ' MOVED'
        print(f"  {f}: {d['reference']} -> {d['value']}{mark}  [{d['evidence']}]")
    if args.out:
        Path(args.out).write_text(json.dumps({'_comment': f'D3dLayout of {args.title} (any360::xdk::D3dLayout), read by '
                                                          f'tools/d3d_layout_map.py from the functions of its D3D runtime '
                                                          f'paired with those of {args.reference} (whose values are the '
                                                          f'defaults). unknown: no evidence; the natives that read those '
                                                          f'fields are not bound in this title.',
                                              'reference': args.reference, 'fields': out, 'unknown': unknown},
                                             indent=1) + '\n', encoding='utf-8')
    return 0


if __name__ == '__main__':
    sys.exit(main())
