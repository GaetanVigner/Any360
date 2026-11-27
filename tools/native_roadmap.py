#!/usr/bin/env python3
"""Roadmap of everything not yet native in Any360, most called first: docs/ROADMAP.md and db/roadmap.json.

A unit is anything the progress page counts: a system call, a CPU instruction id, a named XDK library function, a GPU
unit. One that runs through ReXGlue (state `reference` / `handled`) counts as not native, the same as one that runs
nowhere (`missing`, `stub`, `todo`). Each unit is ranked by how often titles use it:
- measured, where a run counted it: a GPU unit by its uses in the reference captures (db/gpu_usage.json, made by
  `--features <dir>` from `gfx_replay <capture> --features` outputs), an XDK function by its calls a second in a title's
  function profile (titles/<title>/hot_functions.csv);
- static otherwise: an instruction id by its occurrences in the titles' recompiled code (db/instructions.json), an XDK
  function by its call sites in the recompiled code of the title that has it, a system call by its call sites.
Areas are ordered by their total use; within an area, units by their own.

  python -I tools/native_roadmap.py [--features <dir of gfx_replay --features outputs named <title>-<n>.txt>]
"""
import argparse
import csv
import json
import re
from collections import Counter, defaultdict
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import ppc_translate  # noqa: E402
DB = ROOT / 'db'
TITLES = ROOT / 'titles'

NOT_NATIVE_INSTRUCTION = {'handled': 'reference', 'missing': 'missing'}
TEXTURE = re.compile(r'^feature: (?:ps|vs) texture format (\d+) .* x(\d+)$')
RESOLVE_FORMAT = re.compile(r'^feature: resolve source \d+ msaa \d+ dest format (\d+) .* x(\d+)$')
VERTEX = re.compile(r'^feature: vertex format (\d+) x(\d+)$')
PRIMITIVE = re.compile(r'^feature: draw mode \d+ msaa \d+ prim (\d+) source \d+ x(\d+)$')
TESSELLATION = re.compile(r'^feature: .*tessellat.* x(\d+)$')
PACKET = re.compile(r'^\s+(\d+)\s+([A-Z_0-9]+)$')

OWNERS = {
    'CPU instructions': 'this toolkit: an Any360 translator from the XEX\'s PowerPC words (layer/cpu)',
    'XDK library functions': 'the title\'s D3D / library natives (any360::gfx, layer/xdk), bound from the title repository',
    'GPU units': 'the GPU sessions (any360::gfx)',
    'System calls': 'any360::sys',
}


def load(path, default=None):
    return json.loads(path.read_text(encoding='utf-8')) if path.exists() else default


def gpu_usage_from_features(folder):
    """{gpu id: {title: uses}} from gfx_replay --features outputs named <title>-<n>.txt."""
    gpu = load(DB / 'gpu.json', [])
    by_value = defaultdict(dict)
    for unit in gpu:
        if unit.get('value') is not None:
            by_value[unit['family']][unit['value']] = unit['id']
    by_name = {unit['name']: unit['id'] for unit in gpu}
    usage = defaultdict(Counter)
    for path in sorted(Path(folder).glob('*.txt')):
        title = path.stem.rsplit('-', 1)[0]
        in_packets = False
        for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
            if line.startswith('gfx_replay: PM4 packets executed'):
                in_packets = True
                continue
            if in_packets:
                m = PACKET.match(line)
                if not m:
                    in_packets = False
                elif m.group(2) in by_name:
                    usage[by_name[m.group(2)]][title] += int(m.group(1))
                continue
            for pattern, family in ((TEXTURE, 'texture formats'), (RESOLVE_FORMAT, 'texture formats'),
                                    (VERTEX, 'vertex formats'), (PRIMITIVE, 'primitive types')):
                m = pattern.match(line)
                if m and int(m.group(1)) in by_value[family]:
                    usage[by_value[family][int(m.group(1))]][title] += int(m.group(2))
            m = TESSELLATION.match(line)
            if m and 'tessellation' in by_name:
                usage[by_name['tessellation']][title] += int(m.group(1))
    captures = sorted({p.stem.rsplit('-', 1)[0] for p in Path(folder).glob('*.txt')})
    return {'_comment': 'Uses of each GPU unit in the reference captures (gfx_replay --features), by title. Made by '
                        'tools/native_roadmap.py --features; counts only.',
            'titles': captures, 'units': {k: dict(v) for k, v in sorted(usage.items())}}


def call_sites(generated, address):
    """Calls of sub_<address> in a title's recompiled code."""
    pattern = re.compile(rf'\bsub_{address[2:].upper() if address.startswith("0x") else address.upper()}\(')
    count = 0
    for path in Path(generated).glob('*_recomp.*.cpp'):
        count += len(pattern.findall(path.read_text(encoding='utf-8', errors='replace')))
    return count


def profile_calls():
    """{title: {address: calls a second}} from the titles' function profiles."""
    out = {}
    for path in TITLES.glob('*/hot_functions.csv'):
        with path.open(encoding='utf-8') as f:
            out[path.parent.name] = {row['address'].upper(): float(row['calls_per_s'] or 0) for row in csv.DictReader(f)}
    return out


def areas():
    pipelines = {p.parent.name: load(p) for p in TITLES.glob('*/pipeline.json')}
    profiles = profile_calls()
    out = []

    units = []
    for i in load(DB / 'instructions.json', []):
        if i['any360'] == 'native':
            continue
        uses = sum(i['titles'].values())
        state = NOT_NATIVE_INSTRUCTION[i['reference']]
        if ppc_translate.COVERED.fullmatch(i['mnemonic']):
            state += ', translated (not bound)'
        units.append({'id': i['mnemonic'], 'family': i['family'], 'state': state,
                      'uses': uses, 'source': 'static occurrences' if uses else 'unused', 'titles': i['titles']})
    out.append(('CPU instructions', units))

    units = []
    for x in load(DB / 'xdk.json', []):
        if x['any360'] == 'native':
            continue
        measured, static, titles = 0.0, 0, {}
        for title, address in x['titles'].items():
            a = address[2:].upper() if address.startswith('0x') else address.upper()
            if a in profiles.get(title, {}):
                measured += profiles[title][a]
            generated = pipelines.get(title, {}).get('generated')
            sites = call_sites(generated, address) if generated and Path(generated).exists() else 0
            static += sites
            titles[title] = sites
        units.append({'id': x['name'], 'family': x['library'], 'state': 'missing', 'uses': measured or static,
                      'source': 'calls a second (profile)' if measured else 'static call sites' if static else 'unused',
                      'titles': titles})
    out.append(('XDK library functions', units))

    usage = load(DB / 'gpu_usage.json', {'units': {}})['units']
    units = []
    for g in load(DB / 'gpu.json', []):
        if g['state'] == 'native':
            continue
        titles = usage.get(g['id'], {})
        uses = sum(titles.values())
        units.append({'id': g['name'], 'family': g['family'], 'state': g['state'], 'uses': uses,
                      'source': 'uses in the reference captures' if uses else 'unused in the reference captures',
                      'titles': titles})
    out.append(('GPU units', units))

    sort = {x['name']: x['bucket'] for x in load(DB / 'sys_stub_sort.json', {'exports': []})['exports']}
    units = []
    for s in load(DB / 'system_calls.json', []):
        if s['any360'] == 'native':
            continue
        uses = sum(s['sites'].values()) if s['sites'] else 0
        state = 'reference' if s['reference'] == 'implemented' else s['reference']
        units.append({'id': s['name'], 'family': s['group'], 'state': state, 'uses': uses,
                      'source': 'static call sites' if uses else 'imported by no known title',
                      'titles': s['sites'] or {}, 'evidence': sort.get(s['name'], '')})
    out.append(('System calls', units))

    for _, units in out:
        units.sort(key=lambda u: (-u['uses'], u['id']))
    out.sort(key=lambda area: -sum(u['uses'] for u in area[1] if 'calls a second' not in u['source']))
    return out


def platform_functions_section():
    """The Xbox platform functions of each title, ported and remaining by category (db/platform_functions.json)."""
    data = load(DB / 'platform_functions.json')
    if not data:
        return []
    titles = data['titles']
    names = list(titles)
    md = ['', '## Xbox platform functions by title', '',
          f'Counted on {data["date"]} from each title\'s refresh: what the title imports or links from the Xbox platform, '
          'not the game\'s own code nor its middleware. Each cell is ported / in the title (ported: native in Any360 '
          'and bound in the title). Titles marked ~ have no `modules.json` yet: their library ranges come from '
          'landmarks matched against the other titles, +-10-20%. Source: `db/platform_functions.json`.', '',
          '| category | ' + ' | '.join(f'{n}{" ~" if titles[n]["estimated"] else ""}' for n in names) + ' |',
          '|---|' + '---:|' * len(names)]
    totals = {n: [0, 0] for n in names}
    for category in data['categories']:
        cells = []
        for n in names:
            pair = titles[n]['counts'].get(category)
            if pair:
                totals[n][0] += pair[0]
                totals[n][1] += pair[1]
                cells.append(f'{pair[1]:,} / {pair[0]:,}')
            else:
                cells.append('-')
        md.append(f'| {category} | ' + ' | '.join(cells) + ' |')
    md.append('| **all** | ' + ' | '.join(f'**{t[1]:,} / {t[0]:,} ({100 * t[1] / t[0]:.1f}%)**'
                                           for t in (totals[n] for n in names)) + ' |')
    md += ['', 'System calls are done on every title; the XDK libraries the titles link (D3D runtime first, then the '
           'CRT, XAPI and audio) are what remains. Cheapest next steps: functions Any360 already has native but a '
           'title does not bind yet, each bound after its in-game check (docs/NATIVE_FUNCTIONS.md):', '']
    for n in names:
        ready = titles[n].get('ready', [])
        line = f'- {n} ({len(ready)}): ' + (', '.join(ready) if ready else 'none') + '.'
        if titles[n].get('note'):
            line += ' ' + titles[n]['note']
        md.append(line)
    return md


def write(result):
    data = {'_comment': 'Every unit not yet native, by area, most used first. Made by tools/native_roadmap.py.',
            'areas': [{'area': name, 'owner': OWNERS[name], 'units': units} for name, units in result]}
    (DB / 'roadmap.json').write_text(json.dumps(data, indent=0, separators=(',', ':')) + '\n', encoding='utf-8')

    md = ['# Roadmap: what is not native yet, most called first', '',
          'Generated by `tools/native_roadmap.py`. A unit that runs through ReXGlue (`reference`) is not native, the '
          'same as one that runs nowhere (`missing`, `stub`). Each is ranked by its use in the known titles: measured '
          'where a run counted it, static otherwise; the `source` column says which. Areas are ordered by their total '
          'use. Rules: CLAUDE.md (Doing the work).', '',
          '| area | not native | of which reference | used by a title | total use | owner |',
          '|---|---:|---:|---:|---:|---|']
    for name, units in result:
        used = [u for u in units if u['uses']]
        md.append(f'| {name} | {len(units)} | {sum(1 for u in units if u["state"].startswith("reference"))} | {len(used)} | '
                  f'{sum(u["uses"] for u in used):,.0f} | {OWNERS[name]} |')
    md += platform_functions_section()
    for name, units in result:
        used = [u for u in units if u['uses']]
        unused = [u for u in units if not u['uses']]
        md += ['', f'## {name}', '', f'{len(units)} not native, {len(used)} used by a known title. Owner: {OWNERS[name]}.',
               '']
        translated = [u for u in used if u['state'].endswith('translated (not bound)')]
        if translated:
            share = sum(u['uses'] for u in translated) / sum(u['uses'] for u in used)
            md += [f'Any360\'s translator (`tools/ppc_translate.py`, [CPU.md](CPU.md)) decodes {len(translated)} of them '
                   f'(state `translated`), {100 * share:.1f}% of the occurrences. A function is translated only when '
                   'all its instructions and callees are, a smaller share of the titles\' code (CPU.md gives it by '
                   'title). No title runs translated code yet, so none is native.', '']
        if used:
            total = sum(u['uses'] for u in used)
            md += ['| rank | unit | family | state | use | share | cumulative | source | by title |',
                   '|---:|---|---|---|---:|---:|---:|---|---|']
            running = 0.0
            for rank, u in enumerate(used, 1):
                running += u['uses']
                titles = ', '.join(f'{t} {v:,.0f}' for t, v in sorted(u['titles'].items()) if v)
                md.append(f'| {rank} | {u["id"]} | {u["family"]} | {u["state"]} | {u["uses"]:,.0f} | '
                          f'{100 * u["uses"] / total:.2f}% | {100 * running / total:.1f}% | {u["source"]} | {titles} |')
        if unused:
            by_family = Counter((u['family'], u['state']) for u in unused)
            md += ['', f'Not used by any known title ({len(unused)}), by family and state: ' +
                   ', '.join(f'{f} ({s}) {n}' for (f, s), n in sorted(by_family.items(), key=lambda kv: -kv[1])) + '.',
                   'The full list is in db/roadmap.json.']
    (ROOT / 'docs' / 'ROADMAP.md').write_text('\n'.join(md) + '\n', encoding='utf-8')


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--features', type=Path, help='a folder of gfx_replay --features outputs, <title>-<n>.txt')
    args = parser.parse_args()
    if args.features:
        (DB / 'gpu_usage.json').write_text(json.dumps(gpu_usage_from_features(args.features), indent=1) + '\n',
                                           encoding='utf-8')
    result = areas()
    write(result)
    print('roadmap: ' + '; '.join(f'{name} {len(units)} not native, {sum(1 for u in units if u["uses"])} used'
                                  for name, units in result))


if __name__ == '__main__':
    main()
