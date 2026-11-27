#!/usr/bin/env python3
"""Index of every function measured in docs/perf/archive, by name: its latest time, bytes and allocations a call.

Reads the bench outputs kept in the archive folders (docs/perf/archive/<date>-<step>/):
- sys_abi_bench_run*.txt: a system call through the PowerPC ABI against the reference runtime's;
- sys_step12_bench_run*.txt: a system call against what the host offers for the same work;
- D3D_PERF.md: a D3D native against the title's own function;
- cpu_e2e_bench_run*.txt: a title's functions translated by Any360 against the reference runtime's recompilation (the
  set's total a call of each function).
For each function (and measure: input size, path), the latest folder that measured it wins; within a folder the fastest
of its runs is kept. Writes docs/perf/FUNCTIONS.md and docs/perf/functions.json.

  python -I tools/perf_index.py
"""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ARCHIVE = ROOT / 'docs' / 'perf' / 'archive'
NUMBER = r'([0-9.]+)'
ABI_ROW = re.compile(rf'^  (.+?)\s+\|\s+{NUMBER}\s+\|\s+{NUMBER}\s+{NUMBER}\s+{NUMBER}\s+\|\s+{NUMBER}x\s*$')
HOST_ROW = re.compile(rf'^  (.+?)\s+([0-9]+) \|\s+{NUMBER}\s+{NUMBER}\s+{NUMBER}\s+\|'
                      rf'(?:\s+{NUMBER}\s+{NUMBER}\s+{NUMBER}\s+\|\s+{NUMBER}x|\s+-\s+-\s+-\s+\|)\s*$')


def number(text):
    return float(text) if text not in (None, '') else None


def abi_rows(path):
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        m = ABI_ROW.match(line)
        if m:
            name, original, ns, size, allocs, _ = m.groups()
            yield (name.strip(), 'through the ABI'), {'ns': float(ns), 'bytes': float(size), 'allocs': float(allocs),
                                                      'versus': 'original', 'versus_ns': float(original)}


def host_rows(path):
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        m = HOST_ROW.match(line)
        if m:
            name, size, ns, b, allocs, host_ns, _, _, _ = m.groups()
            yield (name.strip(), f'{size} bytes, without the ABI'), {
                'ns': float(ns), 'bytes': float(b), 'allocs': float(allocs), 'versus': 'host' if host_ns else None,
                'versus_ns': number(host_ns)}


def d3d_rows(path):
    columns = None
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        if not line.startswith('|'):
            columns = None
            continue
        cells = [c.strip() for c in line.strip('|').split('|')]
        if cells and cells[0] == 'Case':
            columns = cells
            continue
        if columns is None or set(cells[0]) <= set('-: '):
            continue
        row = dict(zip(columns, cells))
        try:
            ns = float(row['Any360 (ns)'])
            title_ns = float(row['Title (ns)'])
        except (KeyError, ValueError):
            continue
        allocated = re.match(r'([0-9.]+) B / ([0-9.]+)', row.get('Any360 allocated', ''))
        yield (row['Case'], 'D3D native against the title'), {
            'ns': ns, 'bytes': float(allocated.group(1)) if allocated else None,
            'allocs': float(allocated.group(2)) if allocated else None, 'versus': 'title', 'versus_ns': title_ns}


CPU_TOTAL = re.compile(rf'^cpu_e2e_bench: total a call of each function: Any360 {NUMBER} ns, ReXGlue {NUMBER} ns')
CPU_SET = re.compile(r'^cpu_e2e_bench: (\w+), (\d+) functions')
CPU_ALLOCATED = re.compile(rf'^cpu_e2e_bench: allocated per call {NUMBER} B in {NUMBER} allocations')


def cpu_rows(path):
    text = path.read_text(encoding='utf-8', errors='replace').splitlines()
    found = {}
    for line in text:
        for key, pattern in (('set', CPU_SET), ('total', CPU_TOTAL), ('allocated', CPU_ALLOCATED)):
            m = pattern.match(line)
            if m:
                found[key] = m.groups()
    if len(found) == 3:
        title, count = found['set']
        yield (f'CPU translation ({title})', f'{count} functions, total a call of each'), {
            'ns': float(found['total'][0]), 'bytes': float(found['allocated'][0]),
            'allocs': float(found['allocated'][1]), 'versus': 'reference recompilation',
            'versus_ns': float(found['total'][1])}


SOURCES = [('sys_abi_bench_run*.txt', abi_rows), ('sys_step12_bench_run*.txt', host_rows), ('D3D_PERF.md', d3d_rows),
           ('cpu_e2e_bench_run*.txt', cpu_rows)]


def collect():
    latest, first = {}, {}
    for folder in sorted(p for p in ARCHIVE.iterdir() if p.is_dir()):
        found = {}
        for pattern, reader in SOURCES:
            for path in sorted(folder.glob(pattern)):
                for key, row in reader(path):
                    if key not in found or row['ns'] < found[key]['ns']:
                        found[key] = row
        for key, row in found.items():
            latest[key] = dict(row, archive=folder.name)
            first.setdefault(key, folder.name)
    return latest, first


def main():
    latest, first = collect()
    rows = []
    for (name, measure), r in sorted(latest.items(), key=lambda kv: (kv[0][0].lower(), kv[0][1])):
        speed = r['versus_ns'] / r['ns'] if r.get('versus_ns') and r['ns'] else None
        rows.append({'function': name, 'measure': measure, 'ns': r['ns'], 'bytes': r['bytes'], 'allocs': r['allocs'],
                     'versus': r['versus'], 'versus_ns': r['versus_ns'], 'speed': speed, 'archive': r['archive'],
                     'first': first[(name, measure)]})
    out = ROOT / 'docs' / 'perf'
    (out / 'functions.json').write_text(json.dumps(rows, indent=0, separators=(',', ':')) + '\n', encoding='utf-8')

    def fmt(v, digits=1):
        return '' if v is None else f'{v:.{digits}f}'

    md = ['# Measured functions', '',
          'Generated by `tools/perf_index.py` from the bench outputs in [archive/](archive/): each function with its '
          'latest measure (the fastest run of the latest folder that measured it). `versus` is what it was compared '
          'with: the original through the PowerPC ABI, the host doing the same work, or the title\'s own function. '
          'Speed = versus / Any360 (> 1: Any360 faster). `first` is the folder that measured it first: where it was '
          'ported or last changed in its own step, with its NOTES.md.', '',
          f'{len({r["function"] for r in rows})} functions, {len(rows)} measures.', '',
          '| function | measure | ns | B a call | allocs a call | versus | versus ns | speed | latest | first |',
          '|---|---|---:|---:|---:|---|---:|---:|---|---|']
    for r in rows:
        md.append(f'| {r["function"]} | {r["measure"]} | {fmt(r["ns"])} | {fmt(r["bytes"])} | {fmt(r["allocs"], 2)} | '
                  f'{r["versus"] or ""} | {fmt(r["versus_ns"])} | {fmt(r["speed"], 2) + "x" if r["speed"] else ""} | '
                  f'[{r["archive"]}](archive/{r["archive"]}/) | [{r["first"]}](archive/{r["first"]}/) |')
    (out / 'FUNCTIONS.md').write_text('\n'.join(md) + '\n', encoding='utf-8')
    print(f'perf index: {len({r["function"] for r in rows})} functions, {len(rows)} measures')


if __name__ == '__main__':
    main()
