#!/usr/bin/env python3
"""Phase D0 of docs/D3D.md: every entry and flush function of a title's XDK D3D runtime with a name and a role.

For each function of the inventory (titles/<title>/d3d_inventory.json, tools/d3d_inventory.py) of class entry or
flush, from its generated code:
  args            the argument registers (r3-r10) it reads before writing them (a straight scan: approximate where a
                  branch skips the first read)
  r3_reads /      the byte offsets it reads and writes in the object r3 points to (the device for the device's
  r3_writes       methods, the resource for a resource's), through r3 or a register copied from it with mr until
                  overwritten or a call, with their sizes; "indexed" when an access adds a register
  pm4, calls      from the inventory
and its name: titles/<title>/d3d_names.json (address -> [name, what it does]) for the functions read so far, a role
made from the above otherwise. Which ones the port binds (native code or a hook): tools/inventory.py.

    python tools/d3d_roles.py <title>            writes titles/<title>/d3d.json and prints a summary
Standard library only.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import inventory  # noqa: E402

START = re.compile(r'^(?:DEFINE_REX_FUNC|PPC_FUNC_IMPL|REX_FUNC)\((?:__imp__)?(?:guest_[a-z0-9]+_)?sub_([0-9A-F]{8})\)')
INSN = re.compile(r'^// ([a-z][a-z0-9.]*)\s*(.*)$')
MEM = re.compile(r'^(-?\d+)\((r\d+)\)$')
SIZES = {'b': 1, 'h': 2, 'w': 4, 'd': 8, 'f': 4}
STORE = re.compile(r'^st([bhwd]|fs|fd|v|vx|vlx|vrx)')
LOAD = re.compile(r'^l([bhwd]|fs|fd|v|vx|vlx|vrx|wa|ha)')
ARGS = [f'r{i}' for i in range(3, 11)]
# The runtime library's register save and restore routines (__savegprlr_N, __restgprlr_N), found in the generated
# function table: calls to them are the prologues and epilogues, not calls.
SAVE_RESTORE = (0, 0)
HELPER = re.compile(r'\{ 0x([0-9A-F]{8}), __(?:save|rest)gprlr_\d+ \}')


def access_size(op):
    m = re.match(r'^(?:st|l)(b|h|w|d|fs|fd|v)', op)
    if not m:
        return 4
    k = m.group(1)
    return {'b': 1, 'h': 2, 'w': 4, 'd': 8, 'fs': 4, 'fd': 8, 'v': 16}[k]


def functions(generated):
    """address -> list of (op, operands) of each generated function."""
    out = {}
    for path in sorted(Path(generated).glob('*.cpp')):
        current = None
        for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
            m = START.match(line)
            if m:
                current = out.setdefault(m.group(1), [])
                continue
            if current is None:
                continue
            if line.startswith('}'):
                current = None
                continue
            s = line.strip()
            im = INSN.match(s)
            if im:
                current.append((im.group(1), [o.strip() for o in im.group(2).split(',') if o.strip()]))
    return out


def analyse(insns):
    device = {'r3'}
    written, args = set(), []
    reads, writes, indexed = {}, {}, False

    def read(reg):
        if reg in ARGS and reg not in written and reg not in args:
            args.append(reg)

    for op, ops in insns:
        base_op = op.rstrip('.')
        if base_op == 'bl' and ops and SAVE_RESTORE[0] <= int(ops[0], 16) < SAVE_RESTORE[1]:
            continue  # __savegprlr_N / __restgprlr_N: the prologue's register saves, not a call
        if base_op in ('bl', 'bctrl', 'blrl'):
            for r in ARGS + ['r11', 'r12']:
                written.add(r)
            device -= {f'r{i}' for i in range(3, 13)}
            continue
        if base_op.startswith('b') or base_op in ('sync', 'lwsync', 'eieio', 'isync', 'db16cyc', 'nop'):
            continue
        regs = [o for o in ops if re.fullmatch(r'r\d+', o)]
        mem = next((MEM.match(o) for o in ops if MEM.match(o)), None)
        if STORE.match(base_op):
            for r in regs:
                read(r)
            if mem:
                read(mem.group(2))
                if mem.group(2) in device:
                    writes[int(mem.group(1))] = max(writes.get(int(mem.group(1)), 0), access_size(base_op))
                if base_op.endswith('u'):
                    written.add(mem.group(2))
            elif len(regs) >= 3 and (regs[1] in device or regs[2] in device):
                indexed = True
            continue
        if base_op.startswith(('cmp', 'tw', 'td', 'mt')) and not base_op.startswith('mtmsrd'):
            for r in regs:
                read(r)
            continue
        if not ops:
            continue
        dest = ops[0]
        sources = ops[1:]
        if LOAD.match(base_op) and mem:
            read(mem.group(2))
            if mem.group(2) in device:
                reads[int(mem.group(1))] = max(reads.get(int(mem.group(1)), 0), access_size(base_op))
        elif LOAD.match(base_op) and len(regs) >= 3:
            for r in regs[1:]:
                read(r)
            if regs[1] in device or regs[2] in device:
                indexed = True
        else:
            for s in sources:
                if re.fullmatch(r'r\d+', s):
                    read(s)
        if base_op == 'mr' and len(ops) == 2 and ops[1] in device:
            device.add(dest)
        elif re.fullmatch(r'r\d+', dest):
            device.discard(dest)
        if re.fullmatch(r'r\d+', dest):
            written.add(dest)
    fmt = lambda d: [f'+{k} ({v})' for k, v in sorted(d.items())]
    return {'args': sorted(args, key=lambda r: int(r[1:])), 'r3_reads': fmt(reads), 'r3_writes': fmt(writes), 'indexed': indexed}


def role(entry, a):
    if entry['class'] == 'flush':
        return 'flush: builds ' + ', '.join(entry['pm4'])
    if a['r3_writes']:
        return 'writes r3 ' + ', '.join(w.split(' ')[0] for w in a['r3_writes'][:8]) + \
            (' ...' if len(a['r3_writes']) > 8 else '')
    if a['r3_reads']:
        return 'reads r3 ' + ', '.join(r.split(' ')[0] for r in a['r3_reads'][:8])
    return 'no access through r3' + (' (calls ' + ', '.join(entry['calls'][:4]) + ')' if entry['calls'] else '')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('title')
    args = parser.parse_args()
    title_dir = ROOT / 'titles' / args.title
    inv = json.loads((title_dir / 'd3d_inventory.json').read_text(encoding='utf-8'))
    cfg = json.loads((title_dir / 'progress.json').read_text(encoding='utf-8'))
    names_path = title_dir / 'd3d_names.json'
    names = json.loads(names_path.read_text(encoding='utf-8')) if names_path.exists() else {}
    names = {k.upper().replace('0X', '0x'): v for k, v in names.items() if not k.startswith('_')}
    bound = set()
    for src in cfg.get('port_src', []):
        bound |= {f'0x{a:08X}' for a in inventory.bindings(Path(src))}
    global SAVE_RESTORE
    helpers = [int(m.group(1), 16) for path in Path(cfg['generated']).glob('*_init.cpp')
               for m in HELPER.finditer(path.read_text(encoding='utf-8', errors='replace'))]
    if helpers:
        SAVE_RESTORE = (min(helpers), max(helpers) + 16)
    code = functions(cfg['generated'])
    rows = []
    for f in inv['functions']:
        if f['class'] not in ('entry', 'flush'):
            continue
        a = analyse(code.get(f['address'][2:].upper(), []))
        name, what = names.get(f['address'], ['', ''])
        rows.append({'address': f['address'], 'class': f['class'], 'name': name or f'sub_{f["address"][2:]}',
                     'named': bool(name), 'role': what or role(f, a), 'bound': f['address'] in bound,
                     'outside_sites': f['outside_sites'], 'instructions': f['instructions'], **a,
                     'pm4': f['pm4'], 'calls': f['calls']})
    out = {'title': inv['title'], 'range': inv['range'],
           'note': 'Phase D0 of docs/D3D.md, written by tools/d3d_roles.py; names from d3d_names.json, roles of '
                   'unnamed functions made from their device accesses and PM4. args are approximate.',
           'functions': rows}
    (title_dir / 'd3d.json').write_text(json.dumps(out, indent=1) + '\n', encoding='utf-8')
    entry = [r for r in rows if r['class'] == 'entry']
    flush = [r for r in rows if r['class'] == 'flush']
    print(f'{args.title}: {len(entry)} entry, {len(flush)} flush; named {sum(r["named"] for r in rows)}, '
          f'bound by the port {sum(r["bound"] for r in rows)} ({sum(r["outside_sites"] for r in rows if r["bound"])} of '
          f'{sum(r["outside_sites"] for r in rows)} call sites); wrote {title_dir / "d3d.json"}')


if __name__ == '__main__':
    main()
