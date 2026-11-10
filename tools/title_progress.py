#!/usr/bin/env python3
"""Progress report of one title's rewrite, written into the title's own repository (docs/progress/).

A title is done when nothing of the Xbox 360 or of ReXGlue's runtime is left under it: its system calls on
any360::sys, its graphics on any360::gfx, Any360 as the host, and its game code as portable C++. This tool measures
each of those from what is on disk and writes, for humans first:

  progress.svg    bars for the four axes, then two maps: every function by module, every system call by group
  PROGRESS.md     the same as tables: every system call with its state, every module, every rewritten function
  ROADMAP.md      what is missing, in the order it is worked on, with the functions and calls each step still needs
  functions.csv   one row per function of the title: address, module, kind, instructions, state, handler, file
  progress.json   the counts (history.json keeps one entry a day)
and refreshes the block between <!-- progress:begin --> and <!-- progress:end --> in the title's README.

Sources: the title's generated code (the function list and the import table), its port sources (the functions bound
to C++), its golden tests (the functions verified against the recompiled original), titles/<title>/modules.json
(named address ranges), db/system_calls.json and db/gpu.json (what Any360 provides), the binder's
system_exports.inc (the group each call switches with), README.md's State table (the host's steps), and
titles/<title>/progress.json:

  {"name": "Crackdown", "github": "GaetanVigner/CrackdownRewrite", "out": "<title repo>/docs/progress",
   "generated": "<recomp>/generated/default", "port_src": ["<recomp>/src"], "golden": "<recomp>/tests/golden",
   "sys": {"groups": ["stateless", "files", ...], "where": "branch any360-sys-test, --any360_sys_groups=..."},
   "gfx": {"bound": true, "where": "branch any360-graphics-test, --any360_gpu=true"},
   "open": ["known problems"], "next": ["title-specific work that is not a missing function"]}

Addresses, counts and public API names only: no game code or data.

  python tools/title_progress.py crackdown [--out <dir>]      (after tools/build_db.py)
"""
import argparse
import csv
import json
import re
import sys
from collections import Counter, defaultdict
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import inventory  # noqa: E402
from gen_host_binding import GROUPS  # noqa: E402

EXPORTS_INC = ROOT / 'layer' / 'host' / 'rexglue' / 'include' / 'any360' / 'host' / 'system_exports.inc'
GROUP_ORDER = ['stateless', 'files', 'objects', 'threads', 'memory', 'xam', 'input', 'audio', 'video', 'other']
CALL_STATES = [
    ('any360', '#2da44e', 'on any360::sys in this title'),
    ('ready', '#1f6feb', 'native in Any360, not switched on in this title yet'),
    ('runtime', '#6e7681', "ReXGlue's runtime"),
    ('stub', '#bf8700', 'a stub in every runtime'),
    ('missing', '#cf222e', 'provided by nothing'),
]
FUNCTION_STATES = [
    ('native', '#2da44e', 'rewritten: bound to C++'),
    ('verified', '#1f6feb', 'checked against the original in the sandbox'),
    ('library', '#8957e5', 'SDK or middleware code, replaced as a unit'),
    ('recompiled', '#30363d', 'recompiled PowerPC, still to rewrite'),
]
STATUS_NAME = {'N': 'native', 'V': 'verified', 'L': 'library', 'R': 'recompiled'}


def read(path):  # newlines as they are on disk
    return Path(path).read_bytes().decode('utf-8', errors='replace').replace('\r\n', '\n')


def binder_groups():
    """call name -> the binder group it switches with (stateless, files, ...)."""
    return {name: group[1:].lower()
            for name, group in re.findall(r'ANY360_REXGLUE_BIND_[A-Z_]+\(([A-Za-z0-9_]+), (k[A-Za-z]+)', read(EXPORTS_INC))}


def system_calls(cfg):
    db = {c['name']: c for c in json.loads(read(ROOT / 'db' / 'system_calls.json'))}
    groups = binder_groups()
    names = set()
    for path in Path(cfg['generated']).glob('*_init.cpp'):
        names |= set(re.findall(r'__imp__([A-Za-z0-9_]+) \}', read(path)))
    on = set(cfg.get('sys', {}).get('groups', []))
    gfx = cfg.get('gfx', {}).get('bound', False)
    calls = []
    for name in sorted(names):
        c = db.get(name, {})
        # A call the binder does not override yet goes with the group its reference group maps to.
        group = groups.get(name) or ('video' if c.get('group') == 'video'
                                     else (GROUPS.get(c.get('group')) or 'kOther')[1:].lower())
        native = c.get('any360') == 'native'
        if native and (group in on or (group == 'video' and gfx)):
            state = 'any360'
        elif native:
            state = 'ready'
        elif c.get('reference') == 'implemented':
            state = 'runtime'
        elif c.get('reference') == 'stub':
            state = 'stub'
        else:
            state = 'missing'
        calls.append({'name': name, 'library': c.get('library', '?'), 'group': group, 'state': state})
    return calls


def functions(cfg, title):
    sizes = inventory.functions(Path(cfg['generated']))
    bound = {}
    for src in cfg.get('port_src', []):
        for address, entry in inventory.bindings(Path(src)).items():
            bound.setdefault(address, entry)
    verified = inventory.goldens(Path(cfg['golden'])) if cfg.get('golden') else set()
    modules = inventory.load_modules(title)
    rows = []
    for address in sorted(sizes):
        module, kind = inventory.module_of(address, modules)
        status = 'N' if address in bound else 'V' if address in verified else 'L' if kind == 'library' else 'R'
        b = bound.get(address, {})
        rows.append({'address': f'{address:08X}', 'module': module, 'kind': kind, 'instructions': sizes[address],
                     'state': STATUS_NAME[status], 'handler': b.get('handler', ''),
                     'file': b.get('impl') or b.get('file', '')})
    return rows


def any360_steps():
    """The State table of Any360's README: [(step, what, state)]."""
    text = read(ROOT / 'README.md')
    section = text[text.index('## State'):]
    # Links relative to Any360's README point into Any360 from a title's repository.
    section = section.replace('](docs/', '](https://github.com/GaetanVigner/Any360/blob/main/docs/')
    return [(int(n), what.strip(), state.strip())
            for n, what, state in re.findall(r'^\| (\d+) \| (.*?) \| (.*?) \|$', section, re.M)]


# --- drawing -----------------------------------------------------------------------------------------------------

def squarify(items, x, y, w, h):
    """items: [(key, weight)] sorted by weight descending -> [(key, x, y, w, h)]."""
    total = sum(weight for _, weight in items)
    if not items or total <= 0 or w <= 0 or h <= 0:
        return []
    scale = w * h / total
    areas = [(key, weight * scale) for key, weight in items]
    out = []

    def worst(row, side):
        s = sum(a for _, a in row)
        return max(max(side * side * a / (s * s), (s * s) / (side * side * a)) for _, a in row)

    while areas:
        side = min(w, h)
        row = [areas.pop(0)]
        while areas and worst(row + [areas[0]], side) <= worst(row, side):
            row.append(areas.pop(0))
        s = sum(a for _, a in row)
        if w >= h:
            cw = s / h
            cy = y
            for key, a in row:
                out.append((key, x, cy, cw, a / cw))
                cy += a / cw
            x += cw
            w -= cw
        else:
            ch = s / w
            cx = x
            for key, a in row:
                out.append((key, cx, y, a / ch, ch))
                cx += a / ch
            y += ch
            h -= ch
    return out


def esc(text):
    return str(text).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def treemap(out, x0, y0, w, h, title, groups, states):
    """groups: [(name, {state: count})]: a block per group, sized by its count, its cells coloured by state."""
    out.append(f'<text class="h" x="{x0}" y="{y0 - 10}">{esc(title)}</text>')
    out.append(f'<rect x="{x0}" y="{y0}" width="{w}" height="{h}" fill="#0d1117"/>')
    weights = sorted(((name, sum(cells.values())) for name, cells in groups), key=lambda i: -i[1])
    cells_by = dict(groups)
    color = {s: c for s, c, _ in states}
    # One cell stands for the same number of items everywhere in the map (about 1,200 cells in all).
    per = max(1, sum(weight for _, weight in weights) // 1200)
    for name, bx, by, bw, bh in squarify(weights, x0, y0, w, h):
        cells = [(s, cells_by[name].get(s, 0)) for s, _, _ in states if cells_by[name].get(s, 0)]
        units = []
        for state, c in cells:
            units += [state] * max(1, round(c / per))
        cols = max(1, round((len(units) * bw / max(bh, 1)) ** 0.5))
        rows = max(1, -(-len(units) // cols))
        cw, ch = bw / cols, bh / rows
        for i, state in enumerate(units):
            out.append(f'<rect x="{bx + (i % cols) * cw:.1f}" y="{by + (i // cols) * ch:.1f}" '
                       f'width="{max(cw - .6, .3):.1f}" height="{max(ch - .6, .3):.1f}" fill="{color[state]}"/>')
        detail = ', '.join(f'{c:,} {s}' for s, c in cells)
        out.append(f'<rect x="{bx:.1f}" y="{by:.1f}" width="{bw:.1f}" height="{bh:.1f}" fill="none" stroke="#0d1117" '
                   f'stroke-width="2"><title>{esc(name)}: {detail}</title></rect>')
        if bw > 6.2 * len(name) + 8 and bh > 14:
            out.append(f'<text x="{bx + 4:.1f}" y="{by + 12:.1f}" fill="#ffffff" font-size="10" '
                       f'paint-order="stroke" stroke="#0d1117" stroke-width="3">{esc(name)}</text>')


def legend(out, x, y, entries, per_row=2, spacing=320):
    for i, (label, color) in enumerate(entries):
        lx, ly = x + (i % per_row) * spacing, y + (i // per_row) * 17
        out.append(f'<rect x="{lx}" y="{ly}" width="11" height="11" rx="2" fill="{color}"/>')
        out.append(f'<text class="t" x="{lx + 16}" y="{ly + 10}">{esc(label)}</text>')


def bar(out, y, label, segments, total, text):
    """One axis: segments [(count, color)] over total, the label left and the numbers right."""
    x0, w = 300, 480
    out.append(f'<text class="b" x="20" y="{y + 15}">{esc(label)}</text>')
    out.append(f'<rect x="{x0}" y="{y}" width="{w}" height="20" rx="4" fill="#30363d"/>')
    x = x0
    for count, color in segments:
        sw = w * count / max(total, 1)
        if count and sw < 2:
            sw = 2  # a sliver that stays visible
        out.append(f'<rect x="{x:.1f}" y="{y}" width="{sw:.1f}" height="20" rx="4" fill="{color}"/>')
        x += sw
    out.append(f'<text class="v" x="{x0 + w + 12}" y="{y + 15}">{esc(text)}</text>')


def percent(part, total):
    share = part / max(total, 1) * 100
    return f'{share:.0f}%' if share >= 10 or share == 0 else f'{share:.1f}%' if share >= 0.1 else f'{share:.2f}%'


# --- report ------------------------------------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('title')
    parser.add_argument('--out', type=Path, help='where to write (default: the "out" of titles/<title>/progress.json)')
    parser.add_argument('--readme', type=Path, help='a README whose <!-- progress:begin/end --> block to refresh')
    args = parser.parse_args()
    cfg = json.loads(read(ROOT / 'titles' / args.title / 'progress.json'))
    out_dir = args.out or Path(cfg['out'])
    out_dir.mkdir(parents=True, exist_ok=True)
    today = date.today().isoformat()
    name = cfg['name']

    calls = system_calls(cfg)
    rows = functions(cfg, args.title)
    gpu = json.loads(read(ROOT / 'db' / 'gpu.json'))
    steps = any360_steps()
    sys_cfg, gfx_cfg = cfg.get('sys', {}), cfg.get('gfx', {})

    call_count = Counter(c['state'] for c in calls)
    state_count = Counter(r['state'] for r in rows)
    instructions = Counter()
    for r in rows:
        instructions[r['state']] += r['instructions']
    total_instructions = sum(instructions.values())
    gpu_native = sum(1 for u in gpu if u['state'] == 'native')
    game_rows = [r for r in rows if r['kind'] != 'library']
    game_total = len(game_rows)
    game_native = sum(1 for r in game_rows if r['state'] == 'native')
    game_verified = sum(1 for r in game_rows if r['state'] == 'verified')
    library_rows = [r for r in rows if r['kind'] == 'library']
    library_native = sum(1 for r in library_rows if r['state'] == 'native')

    modules = defaultdict(lambda: {'kind': 'game', 'functions': 0, 'instructions': 0, 'native': 0, 'verified': 0,
                                   'library': 0, 'recompiled': 0})
    for r in rows:
        m = modules[r['module']]
        m['kind'] = r['kind']
        m['functions'] += 1
        m['instructions'] += r['instructions']
        m[r['state']] += 1
    by_group = defaultdict(Counter)
    for c in calls:
        by_group[c['group']][c['state']] += 1
    groups = [g for g in GROUP_ORDER if g in by_group]

    summary = {
        'title': name, 'date': today,
        'system_calls': {'total': len(calls), **{s: call_count.get(s, 0) for s, _, _ in CALL_STATES}},
        'gpu_units': {'total': len(gpu), 'native': gpu_native, 'bound': bool(gfx_cfg.get('bound'))},
        'functions': {'total': len(rows), **{s: state_count.get(s, 0) for s, _, _ in FUNCTION_STATES}},
        'game_functions': {'total': game_total, 'native': game_native, 'verified': game_verified},
        'instructions': {'total': total_instructions, **{s: instructions.get(s, 0) for s, _, _ in FUNCTION_STATES}},
    }
    (out_dir / 'progress.json').write_text(json.dumps(summary, indent=1) + '\n', encoding='utf-8')
    history_path = out_dir / 'history.json'
    history = json.loads(read(history_path)) if history_path.exists() else []
    history = [h for h in history if isinstance(h, dict) and h.get('date') != today and 'system_calls' in h]
    history.append({k: summary[k] for k in ('date', 'system_calls', 'gpu_units', 'functions', 'game_functions')})
    history_path.write_text(json.dumps(history, indent=1) + '\n', encoding='utf-8')

    with open(out_dir / 'functions.csv', 'w', newline='', encoding='utf-8') as f:
        writer = csv.writer(f, lineterminator='\n')
        writer.writerow(['address', 'module', 'kind', 'instructions', 'state', 'handler', 'file'])
        for r in rows:
            writer.writerow([r['address'], r['module'], r['kind'], r['instructions'], r['state'], r['handler'], r['file']])

    # --- the picture
    width = 1100
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="760" viewBox="0 0 {width} 760" '
           'font-family="Segoe UI, Helvetica, Arial, sans-serif" font-size="12">',
           '<style>.t{fill:#8b949e}.h{fill:#c9d1d9;font-size:14px;font-weight:600}'
           '.b{fill:#c9d1d9;font-size:14px}.v{fill:#c9d1d9;font-size:13px}</style>',
           '<rect width="100%" height="100%" fill="#161b22"/>',
           f'<text class="h" x="20" y="30" style="font-size:19px">{esc(name)}: how much of the game runs on its own '
           'code</text>',
           f'<text class="t" x="20" y="50">{today}. Green is done. Generated by Any360\'s tools/title_progress.py; '
           'tables in PROGRESS.md, what is missing in ROADMAP.md.</text>']
    sys_note = f'{call_count["any360"]} of {len(calls)}'
    if call_count['ready']:
        sys_note += f' (+{call_count["ready"]} ready in Any360)'
    if call_count['any360'] and sys_cfg.get('default') is False:
        sys_note += ', opt-in'
    bar(svg, 70, 'System calls on any360::sys', [(call_count['any360'], '#2da44e'), (call_count['ready'], '#1f6feb')],
        len(calls), sys_note)
    gfx_note = f'{gpu_native} of {len(gpu)} GPU features'
    gfx_note += (', opt-in' if gfx_cfg.get('default') is False else '') if gfx_cfg.get('bound') else ', not bound here'
    bar(svg, 100, 'Graphics on any360::gfx', [(gpu_native, '#2da44e' if gfx_cfg.get('bound') else '#1f6feb')], len(gpu),
        gfx_note)
    host_done = sum(1 for n, _, s in steps if n in (13, 14) and s.startswith('done'))
    host_partial = sum(1 for n, _, s in steps if n in (13, 14) and s.startswith('partial'))
    bar(svg, 130, 'Host: loader, threads, window', [(host_done, '#2da44e'), (host_partial * 0.25, '#bf8700')], 2,
        "ReXGlue's host still" if not host_done else f'{host_done} of 2 parts on Any360')
    bar(svg, 160, 'Game code rewritten in C++', [(game_native, '#2da44e'), (game_verified, '#1f6feb')], game_total,
        f'{game_native:,} of {game_total:,} functions ({percent(game_native, game_total)})'
        + (f', {game_verified:,} verified' if game_verified else ''))

    function_groups = [(m, {s: v[s] for s, _, _ in FUNCTION_STATES}) for m, v in modules.items()]
    treemap(svg, 20, 232, 640, 400, f'The game\'s {len(rows):,} functions, by module', function_groups, FUNCTION_STATES)
    legend(svg, 20, 644, [(f'{s}: {d}', c) for s, c, d in FUNCTION_STATES])
    call_groups = [(g, dict(by_group[g])) for g in groups]
    treemap(svg, 690, 232, 390, 400, f'Its {len(calls)} Xbox system calls, by group', call_groups, CALL_STATES)
    legend(svg, 690, 644, [(f'{s}: {d}', c) for s, c, d in CALL_STATES], per_row=1)
    svg.append('</svg>')
    (out_dir / 'progress.svg').write_text('\n'.join(svg) + '\n', encoding='utf-8')

    # --- the tables
    github = cfg.get('github', '')
    md = [f'# {name}: rewrite progress', '',
          f'Generated on {today} by [Any360](https://github.com/GaetanVigner/Any360)\'s `tools/title_progress.py`. '
          'What is still missing, step by step: [ROADMAP.md](ROADMAP.md). Every function: '
          '[functions.csv](functions.csv). Counts: [progress.json](progress.json).', '',
          '![Progress](progress.svg)', '',
          '## Where it stands', '',
          '| Axis | Done | Of | Share | Notes |', '|---|---|---|---|---|',
          f'| System calls on `any360::sys` | {call_count["any360"]} | {len(calls)} | '
          f'{percent(call_count["any360"], len(calls))} | {sys_cfg.get("where", "not switched on in this title yet")} |',
          f'| Graphics on `any360::gfx` (GPU features Any360 handles) | {gpu_native} | {len(gpu)} | '
          f'{percent(gpu_native, len(gpu))} | {gfx_cfg.get("where", "not bound in this title yet")} |',
          f'| Host (XEX loader, address space, threads, window, input, audio output) | {host_done} | 2 | '
          f'{percent(host_done, 2)} | ReXGlue\'s host runs the title; Any360 steps 13 and 14 |',
          f'| Game functions rewritten in C++ | {game_native:,} | {game_total:,} | {percent(game_native, game_total)} | '
          f'{game_verified:,} more verified against the original in the sandbox |',
          f'| Library functions (SDK, middleware) replaced | {library_native:,} | {len(library_rows):,} | '
          f'{percent(library_native, len(library_rows))} | replaced as units, not decompiled |',
          f'| Instructions of rewritten functions | {instructions["native"]:,} | {total_instructions:,} | '
          f'{percent(instructions["native"], total_instructions)} | PowerPC instructions of the original |', '']
    md += ['## System calls', '',
           f'The {len(calls)} kernel and XAM functions the title imports, by the group a title switches to '
           '`any360::sys` at once. ' + ' '.join(f'**{s}**: {d}.' for s, _, d in CALL_STATES), '',
           '| Group | ' + ' | '.join(s for s, _, _ in CALL_STATES) + ' | Total |',
           '|---|' + '---|' * (len(CALL_STATES) + 1)]
    for g in groups:
        md.append(f'| {g} | ' + ' | '.join(str(by_group[g].get(s, 0)) for s, _, _ in CALL_STATES)
                  + f' | {sum(by_group[g].values())} |')
    md += ['', '<details><summary>Every call</summary>', '', '| Group | Library | Function | State |', '|---|---|---|---|']
    for g in groups:
        md += [f'| {g} | {c["library"]} | `{c["name"]}` | {c["state"]} |' for c in calls if c['group'] == g]
    md += ['', '</details>', '']
    family = defaultdict(Counter)
    for u in gpu:
        family[u['family']][u['state']] += 1
    md += ['## Graphics', '',
           'What `any360::gfx` handles of the Xbox 360 GPU (the same for every title; a title uses a part of it). '
           + (f'In this title: {gfx_cfg["where"]}.' if gfx_cfg.get('where') else 'Not bound in this title yet.'), '',
           '| Family | Native | Not yet | Total |', '|---|---|---|---|']
    for f_name, counts in sorted(family.items()):
        total = sum(counts.values())
        md.append(f'| {f_name} | {counts.get("native", 0)} | {total - counts.get("native", 0)} | {total} |')
    md += ['', '## Game code', '',
           'Every function of the executable by module (named address ranges; the rest by 1 MB area until symbol '
           'recovery names them). ' + ' '.join(f'**{s}**: {d}.' for s, _, d in FUNCTION_STATES), '',
           '| Module | Kind | Functions | Instructions | Native | Verified | Still recompiled | Native share |',
           '|---|---|---|---|---|---|---|---|']
    for m, v in sorted(modules.items(), key=lambda kv: -kv[1]['instructions']):
        md.append(f'| {m} | {v["kind"]} | {v["functions"]:,} | {v["instructions"]:,} | {v["native"]:,} | '
                  f'{v["verified"]:,} | {v["recompiled"] + v["library"]:,} | {percent(v["native"], v["functions"])} |')
    native_rows = [r for r in rows if r['state'] == 'native']
    md += ['', f'<details><summary>The {len(native_rows):,} rewritten functions</summary>', '',
           '| Address | Module | Instructions | Handler | File |', '|---|---|---|---|---|']
    md += [f'| `{r["address"]}` | {r["module"]} | {r["instructions"]} | `{r["handler"] or "-"}` | {r["file"]} |'
           for r in native_rows]
    md += ['', '</details>', '', '## Host', '',
           'Any360\'s own steps towards hosting a title without ReXGlue\'s runtime '
           '([Any360 README](https://github.com/GaetanVigner/Any360#state)).', '',
           '| Step | What | State |', '|---|---|---|']
    md += [f'| {n} | {what} | {state} |' for n, what, state in steps if n >= 9]
    md.append('')
    (out_dir / 'PROGRESS.md').write_text('\n'.join(md), encoding='utf-8')

    # --- what is missing
    not_on = [c for c in calls if c['state'] != 'any360']
    road = [f'# {name}: roadmap', '',
            f'Generated on {today} with [PROGRESS.md](PROGRESS.md). The rewrite is done when nothing of the Xbox 360 '
            'or of ReXGlue\'s runtime is left under the game: every system call on `any360::sys`, graphics on '
            '`any360::gfx`, Any360 as the host, and the game\'s code as portable C++. Steps are in working order; '
            'each lists what it still needs.', '']
    road += ['## 1. System calls on any360::sys', '']
    if not not_on:
        road += [f'All {len(calls)} imports run on `any360::sys` ({sys_cfg.get("where", "")}).', '']
        if sys_cfg.get('default') is False:
            road += ['Still to do: make it the default build (today it is opt-in), then remove the ReXGlue kernel '
                     'from the link.', '']
    else:
        road += [f'{len(calls) - len(not_on)} of {len(calls)} imports run on `any360::sys`; {len(not_on)} do not yet.', '',
                 '| Group | Calls to switch | Of which Any360 already has | Missing in Any360 |', '|---|---|---|---|']
        for g in groups:
            left = [c for c in not_on if c['group'] == g]
            if left:
                ready = sum(1 for c in left if c['state'] == 'ready')
                road.append(f'| {g} | {len(left)} | {ready} | {len(left) - ready} |')
        road.append('')
        for g in groups:
            left = [c for c in not_on if c['group'] == g]
            if left:
                road += [f'<details><summary>{g}: {len(left)} calls</summary>', '',
                         ' '.join(f'`{c["name"]}`' + ('' if c['state'] == 'ready' else f' ({c["state"]})') for c in left),
                         '', '</details>', '']
    road += ['## 2. Graphics on any360::gfx', '',
             (f'The title draws through `any360::gfx`: {gfx_cfg["where"]}.' if gfx_cfg.get('bound')
              else 'The title is not bound to `any360::gfx` yet.'), '']
    if gfx_cfg.get('default') is False:
        road += ['Still to do: make it the default renderer (today ReXGlue\'s Xenos emulation is), then drop the '
                 'emulated GPU from the build.', '']
    for item in gfx_cfg.get('todo', []):
        road.append(f'- {item}')
    if gfx_cfg.get('todo'):
        road.append('')
    missing_gpu = [u for u in gpu if u['state'] != 'native']
    road += [f'GPU features `any360::gfx` does not handle yet ({len(missing_gpu)} of {len(gpu)}; none stops this '
             'title today, each is one a title may need):', '',
             '| Family | Features |', '|---|---|']
    for f_name in sorted({u['family'] for u in missing_gpu}):
        road.append(f'| {f_name} | ' + ', '.join(u['name'] for u in missing_gpu if u['family'] == f_name) + ' |')
    road.append('')
    road += ['## 3. Any360 as the host', '',
             'Shared by every title (Any360\'s own roadmap); the title follows once they land.', '',
             '| Any360 step | What | State |', '|---|---|---|']
    road += [f'| {n} | {what} | {state} |' for n, what, state in steps if n >= 11 and not state.startswith('done')]
    road.append('')
    road += ['## 4. Library code replaced as a unit', '']
    libraries = [(m, v) for m, v in modules.items() if v['kind'] == 'library']
    if libraries:
        road += ['SDK and middleware code linked into the executable is not decompiled: Any360 provides the same '
                 'service natively and the whole range goes.', '',
                 '| Library range | Functions | Instructions | Replaced | Verified in the sandbox | Left |',
                 '|---|---|---|---|---|---|']
        for m, v in sorted(libraries, key=lambda kv: -kv[1]['instructions']):
            road.append(f'| {m} | {v["functions"]:,} | {v["instructions"]:,} | {v["native"]:,} | {v["verified"]:,} | '
                        f'{v["functions"] - v["native"]:,} |')
    else:
        road += ['No library range is named yet for this title (titles/' + args.title + '/modules.json in Any360): '
                 'the first step is recognising the SDK code (D3D runtime, CRT, audio and video middleware) so it '
                 'is not decompiled as game code.']
    road.append('')
    left_game = game_total - game_native
    road += ['## 5. Game code as portable C++', '',
             f'{left_game:,} of {game_total:,} game functions are still recompiled PowerPC'
             + (f' ({game_verified:,} of them already have a sandbox test to check a rewrite against)'
                if game_verified else '') + '. The list, function '
             'by function, is [functions.csv](functions.csv) (state `recompiled` or `verified`). By module, largest '
             'first:', '',
             '| Module | Functions left | Instructions left | Rewritten | Verified |', '|---|---|---|---|---|']
    for m, v in sorted(modules.items(), key=lambda kv: -(kv[1]['recompiled'] + kv[1]['verified'])):
        if v['kind'] == 'library':
            continue
        left_instructions = sum(r['instructions'] for r in rows if r['module'] == m and r['state'] != 'native')
        road.append(f'| {m} | {v["recompiled"] + v["verified"]:,} | {left_instructions:,} | {v["native"]:,} | '
                    f'{v["verified"]:,} |')
    road += ['', 'Order of work: symbol recovery and library recognition first (they shrink this list), then leaf '
             'functions (no calls: each is checked alone), then their callers, module by module; every rewrite is '
             'bound over the recompiled function and proven equal in the guest sandbox before it replaces it '
             '(Any360\'s `pipeline/any360.py <title> select | decompile | bind | verify`).', '']
    if cfg.get('next'):
        road += ['## 6. This title', ''] + [f'- {item}' for item in cfg['next']] + ['']
    if cfg.get('open'):
        road += ['## Known problems', ''] + [f'- {item}' for item in cfg['open']] + ['']
    (out_dir / 'ROADMAP.md').write_text('\n'.join(road), encoding='utf-8')

    # --- the README's block: the picture and the four axes, between the markers
    readme = args.readme or (Path(cfg['readme']) if cfg.get('readme') else None)
    if readme and readme.exists():
        text = read(readme)
        begin, end = '<!-- progress:begin -->', '<!-- progress:end -->'
        if begin in text and end in text:
            rel = cfg.get('readme_docs', 'docs/progress')
            block = [begin,
                     f'[![{name}: rewrite progress]({rel}/progress.svg)]({rel}/PROGRESS.md)', '',
                     '| | Where it stands |', '|---|---|',
                     f'| **System calls** | {call_count["any360"]} of {len(calls)} on `any360::sys`'
                     + (f', {call_count["ready"]} more ready in Any360' if call_count['ready'] else '')
                     + (' (opt-in)' if call_count['any360'] and sys_cfg.get('default') is False else '') + ' |',
                     '| **Graphics** | ' + (f'`any360::gfx`, a Vulkan renderer handling {gpu_native} of {len(gpu)} Xbox 360 '
                                            'GPU features' + (' (opt-in)' if gfx_cfg.get('default') is False else '')
                                            if gfx_cfg.get('bound') else "ReXGlue's GPU emulation") + ' |',
                     "| **Host** | ReXGlue's loader, threads and window; Any360's own host is not started |",
                     f'| **Game code** | {game_native:,} of {game_total:,} functions rewritten in C++ '
                     f'({percent(game_native, game_total)})'
                     + (f', {game_verified:,} more verified against the original' if game_verified else '') + ' |', '',
                     f'Updated {today}. Every system call, module and rewritten function: [PROGRESS.md]({rel}/PROGRESS.md). '
                     f'What is missing, step by step: [ROADMAP.md]({rel}/ROADMAP.md). Function by function: '
                     f'[functions.csv]({rel}/functions.csv).',
                     end]
            text = text[:text.index(begin)] + '\n'.join(block) + text[text.index(end) + len(end):]
            if b'\r\n' in readme.read_bytes():
                text = text.replace('\n', '\r\n')
            readme.write_bytes(text.encode('utf-8'))

    print(f'{name}: system calls {call_count["any360"]}/{len(calls)} on any360::sys (+{call_count["ready"]} ready); '
          f'GPU {gpu_native}/{len(gpu)}{" bound" if gfx_cfg.get("bound") else ""}; functions {len(rows):,}: '
          f'{state_count["native"]} native, {state_count["verified"]} verified, {state_count["library"]} library; '
          f'game code {game_native}/{game_total}; wrote {out_dir}')


if __name__ == '__main__':
    main()
