#!/usr/bin/env python3
"""Any360 progress page, in the style of AnyPS5's progress.html, for the whole Xbox 360 platform surface rather than
one title: one square per system call (xboxkrnl / xam / xbdm export), per CPU instruction id and per named XDK
library function, coloured by state; filterable tables of the database; the known titles as sources; the toolkit's
components; the history.

Reads db/*.json (tools/build_db.py), layer/status.json and docs/coverage/patches-*.json. Writes docs/progress.html,
docs/progress.svg (the overview treemaps, for the README), docs/progress.json and appends today's point to
docs/history.json. Standard library only; run tools/build_db.py then this before every commit.

  python tools/progress_page.py
"""
import json
from collections import Counter, defaultdict
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / 'docs'
DB = ROOT / 'db'
STATUS = ROOT / 'layer' / 'status.json'
LIVE = 'https://gaetanvigner.github.io/Any360/progress.html'  # where the standalone SVG's blocks link to
REPOS = {'crackdown': 'https://github.com/GaetanVigner/CrackdownRewrite',
         'simpsons': 'https://github.com/GaetanVigner/TheSimpsonsGameRewrite'}

BG, TEXT, MUTED = '#0d1117', '#c9d1d9', '#8b949e'
# Any360 states first (what this repository provides), then the reference runtime's states (what runs today).
CALL_STATES = [('native', '#2ea043'), ('ported', '#2f81f7'), ('reference', '#8957e5'), ('stub', '#d29922'), ('missing', '#f85149')]
INSTRUCTION_STATES = [('native', '#2ea043'), ('reference', '#8957e5'), ('missing', '#f85149')]
XDK_STATES = [('native', '#2ea043'), ('ported', '#2f81f7'), ('reference', '#8957e5'), ('todo', '#f85149')]


def esc(text):
    return str(text).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')


def pct(part, total):
    return f'{part / total * 100:.1f}%' if total else '-'


def load_json(path, default=None):
    return json.loads(path.read_text(encoding='utf-8')) if path.exists() else default


def call_state(row):
    """The one state a system call is drawn with: Any360's when it has one, else the reference's."""
    if row['any360'] in ('native', 'ported'):
        return row['any360']
    return 'reference' if row['reference'] == 'implemented' else row['reference']


def instruction_state(row):
    return 'native' if row['any360'] == 'native' else 'reference' if row['reference'] == 'handled' else 'missing'


# --- squarified treemap of unit squares (AnyPS5 progress.svg style) ----------------------------------------------

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


def treemap(x0, y0, w, h, title, groups, states, done_states, href='#', subtitle=''):
    """groups: [(name, {state: count})]. One unit square per item (merged when a block holds more than ~400)."""
    total = sum(sum(c.values()) for _, c in groups)
    done = sum(c.get(s, 0) for _, c in groups for s in done_states)
    out = [f'<text x="{x0}" y="{y0 - 9}" font-size="16" fill="{TEXT}" font-weight="600">{esc(title)}: '
           f'{pct(done, total)} ({done:,}/{total:,})'
           + (f' <tspan font-size="12" font-weight="400" fill="{MUTED}">{esc(subtitle)}</tspan>' if subtitle else '') + '</text>',
           f'<rect x="{x0}" y="{y0}" width="{w}" height="{h}" fill="{BG}"/>']
    color = dict(states)
    blocks = squarify(sorted(((n, sum(c.values())) for n, c in groups if sum(c.values())), key=lambda i: -i[1]), x0, y0, w, h)
    counts = dict(groups)
    for name, bx, by, bw, bh in blocks:
        cells = counts[name]
        count = sum(cells.values())
        per = max(1, count // 400)
        units = []
        for state, _ in states:
            units += [state] * max(0, round(cells.get(state, 0) / per))
        if not units:
            continue
        cols = max(1, round((len(units) * bw / max(bh, 1)) ** 0.5))
        rows = max(1, -(-len(units) // cols))
        cw, ch = bw / cols, bh / rows
        share = sum(cells.get(s, 0) for s in done_states) / count * 100
        detail = ', '.join(f'{cells[s]} {s}' for s, _ in states if cells.get(s))
        tip = f'{name}: {count:,} ({share:.0f}% ' + '/'.join(done_states) + f'; {detail})' + (f' [{per} per square]' if per > 1 else '')
        out.append(f'<a href="{href}"><g class="blk"><title>{esc(tip)}</title>')
        for i, state in enumerate(units):
            cx, cy = bx + (i % cols) * cw, by + (i // cols) * ch
            out.append(f'<rect x="{cx:.2f}" y="{cy:.2f}" width="{max(cw - .5, .3):.2f}" height="{max(ch - .5, .3):.2f}" fill="{color[state]}"/>')
        out.append(f'<rect x="{bx:.1f}" y="{by:.1f}" width="{bw:.1f}" height="{bh:.1f}" fill="none" stroke="{BG}" stroke-width="1.5"/>')
        if bw > 40 and bh > 14:
            label = name if bw > 7 * len(name) else name[:max(2, int(bw / 7))]
            out.append(f'<text x="{bx + 4:.1f}" y="{by + 13:.1f}" font-size="12" fill="#ffffff" stroke="{BG}" '
                       f'stroke-width="3" paint-order="stroke">{esc(label)}</text>')
        out.append('</g></a>')
    return out


def legend(x, y, entries):
    out = []
    for i, (label, color) in enumerate(entries):
        lx = x + i * 118
        out.append(f'<rect x="{lx}" y="{y}" width="10" height="10" rx="2" fill="{color}"/>')
        out.append(f'<text x="{lx + 14}" y="{y + 9}" font-size="11" fill="{MUTED}">{esc(label)}</text>')
    return out


def overview_svg(calls, instructions, xdk, href_base):
    """The three global treemaps stacked: system calls by library / group, instructions by family, XDK by library."""
    width, map_h, header, gap = 1000, 300, 34, 46
    by_group = defaultdict(Counter)
    for r in calls:
        by_group[f"{r['library']} {r['group']}"][call_state(r)] += 1
    by_family = defaultdict(Counter)
    for r in instructions:
        by_family[r['family']][instruction_state(r)] += 1
    by_library = defaultdict(Counter)
    for r in xdk:
        by_library[r['library']][r['any360']] += 1
    runnable = sum(1 for r in calls if call_state(r) in ('native', 'ported', 'reference'))
    handled = sum(1 for r in instructions if instruction_state(r) != 'missing')
    y = header
    out = []
    out += treemap(0, y, width, map_h, 'System calls native', sorted(by_group.items()), CALL_STATES, ['native', 'ported'],
                   f'{href_base}#calls', f'· {pct(runnable, len(calls))} run on the reference runtime · one square per export')
    out += legend(0, y + map_h + 6, [('native (Any360)', '#2ea043'), ('ported (in a title)', '#2f81f7'),
                                     ('reference (ReXGlue)', '#8957e5'), ('stub', '#d29922'), ('missing', '#f85149')])
    y += map_h + gap
    out += treemap(0, y, width, 160, 'CPU instructions native', sorted(by_family.items()), INSTRUCTION_STATES, ['native'],
                   f'{href_base}#instructions', f'· {pct(handled, len(instructions))} handled by the reference recompiler · one square per instruction id')
    out += legend(0, y + 160 + 6, [('native (Any360 rules)', '#2ea043'), ('reference (recompiler)', '#8957e5'), ('missing', '#f85149')])
    y += 160 + gap
    xdk_h = 140
    out += treemap(0, y, width, xdk_h, 'XDK library functions native', sorted(by_library.items()), XDK_STATES, ['native', 'ported'],
                   f'{href_base}#xdk', '· named by signature so far · one square per function')
    out += legend(0, y + xdk_h + 6, [('native (Any360)', '#2ea043'), ('ported (in a title)', '#2f81f7'), ('reference (hooked by name)', '#8957e5'), ('to do', '#f85149')])
    height = y + xdk_h + 24
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}" '
           f'font-family="sans-serif">\n<style>.blk:hover rect{{opacity:.8}}</style>\n<rect width="{width}" height="{height}" fill="{BG}"/>\n'
           + '\n'.join(out) + '\n</svg>')
    return svg, height


# --- history ---------------------------------------------------------------------------------------------------

def history_svg(history):
    width, height, left, bottom = 1000, 180, 50, 24
    if not history:
        return ''
    series = [(p['system_calls']['native'] + p['system_calls']['ported'], p['system_calls']['runnable'], p['system_calls']['total'],
               p['xdk']['native'] + p['xdk']['ported'], p['xdk']['total']) for p in history]
    top = max(max(s[1], 1) for s in series)
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}" font-family="sans-serif">',
           f'<rect width="{width}" height="{height}" fill="{BG}"/>']
    n = len(series)
    step = (width - left - 10) / max(n, 1)
    for i, ((done, runnable, total, xdone, xtotal), p) in enumerate(zip(series, history)):
        x = left + i * step
        hr = (height - bottom - 10) * runnable / top
        hd = (height - bottom - 10) * done / top
        tip = f"{p['date']}: {done} native or ported, {runnable} runnable of {total} system calls; XDK {xdone}/{xtotal}"
        out.append(f'<g><title>{esc(tip)}</title><rect x="{x:.1f}" y="{height - bottom - hr:.1f}" width="{max(step - 2, 1):.1f}" height="{hr:.1f}" fill="#8957e5"/>'
                   f'<rect x="{x:.1f}" y="{height - bottom - hd:.1f}" width="{max(step - 2, 1):.1f}" height="{hd:.1f}" fill="#2ea043"/></g>')
        if i in (0, n - 1):
            out.append(f'<text x="{x:.1f}" y="{height - 8}" font-size="11" fill="{MUTED}">{esc(p["date"])}</text>')
    out.append(f'<text x="0" y="14" font-size="11" fill="{MUTED}">{top:,}</text>')
    out.append('</svg>')
    return '\n'.join(out)


# --- the page ----------------------------------------------------------------------------------------------------

def main():
    calls = load_json(DB / 'system_calls.json', [])
    instructions = load_json(DB / 'instructions.json', [])
    xdk = load_json(DB / 'xdk.json', [])
    db_summary = load_json(DB / 'summary.json', {})
    status = load_json(STATUS, {'components': []})
    patches = {p.stem[len('patches-'):]: load_json(p) for p in (DOCS / 'coverage').glob('patches-*.json')}
    today = date.today().isoformat()

    for r in calls:
        r['state'] = call_state(r)
    for r in instructions:
        r['state'] = instruction_state(r)
        r['used'] = sum(r['titles'].values())
    comps = status['components']
    summary = {
        'date': today,
        'reference': db_summary.get('reference', {}),
        'system_calls': {'total': len(calls), 'used_by_titles': sum(1 for r in calls if r['titles']),
                         'native': sum(1 for r in calls if r['state'] == 'native'), 'ported': sum(1 for r in calls if r['state'] == 'ported'),
                         'runnable': sum(1 for r in calls if r['state'] in ('native', 'ported', 'reference')),
                         'by_state': dict(Counter(r['state'] for r in calls)), 'by_library': dict(Counter(r['library'] for r in calls))},
        'instructions': {'total': len(instructions), 'used_by_titles': sum(1 for r in instructions if r['titles']),
                         'native': sum(1 for r in instructions if r['state'] == 'native'),
                         'reference': sum(1 for r in instructions if r['state'] == 'reference'),
                         'by_family': {f: dict(c) for f, c in sorted(
                             ((f, Counter(r['state'] for r in instructions if r['family'] == f)) for f in {r['family'] for r in instructions}))}},
        'xdk': {'total': len(xdk), 'native': sum(1 for r in xdk if r['any360'] == 'native'),
                'ported': sum(1 for r in xdk if r['any360'] == 'ported'), 'unnamed_by_title': db_summary.get('xdk', {}).get('unnamed_by_title', {})},
        'titles': db_summary.get('titles', {}),
        'toolkit': {'components': len(comps), 'done': sum(1 for x in comps if x['state'] == 'done'),
                    'in_progress': sum(1 for x in comps if x['state'] == 'in progress'), 'patches': {k: v['count'] for k, v in patches.items()}},
    }

    history_path = DOCS / 'history.json'
    history = [p for p in load_json(history_path, []) if p.get('date') != today and 'system_calls' in p]
    history.append({'date': today,
                    'system_calls': {k: summary['system_calls'][k] for k in ('native', 'ported', 'runnable', 'total')},
                    'instructions': {k: summary['instructions'][k] for k in ('native', 'reference', 'total')},
                    'xdk': {k: summary['xdk'][k] for k in ('native', 'ported', 'total')}, 'toolkit_done': summary['toolkit']['done']})
    history_path.write_text(json.dumps(history, indent=1) + '\n', encoding='utf-8')
    (DOCS / 'progress.json').write_text(json.dumps(summary, indent=1) + '\n', encoding='utf-8')

    svg, _ = overview_svg(calls, instructions, xdk, '')
    svg_live, _ = overview_svg(calls, instructions, xdk, LIVE)
    (DOCS / 'progress.svg').write_text(svg_live + '\n', encoding='utf-8')

    page_data = {
        'date': today, 'summary': summary, 'components': comps, 'patches': {k: v['areas'] for k, v in patches.items()},
        'calls': [{k: r[k] for k in ('name', 'library', 'ordinal', 'group', 'reference', 'any360', 'state', 'titles')} for r in calls],
        'instructions': [{k: r[k] for k in ('id', 'mnemonic', 'family', 'reference', 'any360', 'state', 'used', 'titles')} for r in instructions],
        'xdk': xdk, 'repos': REPOS,
    }
    html = TEMPLATE.replace('__DATA__', json.dumps(page_data, separators=(',', ':')).replace('</', '<\\/'))
    html = html.replace('__DATE__', today).replace('__OVERVIEW__', svg).replace('__HISTORY__', history_svg(history))
    (DOCS / 'progress.html').write_text(html, encoding='utf-8')
    s = summary['system_calls']
    print(f"progress page: system calls {s['native'] + s['ported']}/{s['total']} native or ported ({s['runnable']} runnable, "
          f"{s['used_by_titles']} used by the known titles); instructions {summary['instructions']['native']}/{summary['instructions']['total']} native "
          f"({summary['instructions']['reference']} handled by the reference); XDK {summary['xdk']['native'] + summary['xdk']['ported']}/{summary['xdk']['total']}; "
          f"toolkit {summary['toolkit']['done']}/{summary['toolkit']['components']}; wrote {DOCS / 'progress.html'}")


TEMPLATE = '''<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1">
<title>Any360 progress</title>
<style>
:root{--bg:#0d1117;--panel:#161b22;--line:#30363d;--fg:#c9d1d9;--muted:#8b949e;--link:#58a6ff;--ok:#2ea043}
*{box-sizing:border-box}body{margin:0;padding:16px;background:var(--bg);color:var(--fg);font:14px/1.5 -apple-system,"Segoe UI",Helvetica,Arial,sans-serif}
main{max-width:1000px;margin:auto}h1{font-size:22px;margin:0 0 6px}h2{font-size:17px;margin:28px 0 8px;padding-bottom:4px;border-bottom:1px solid var(--line)}
h3{font-size:14px;margin:18px 0 6px;color:var(--muted)}p{color:var(--muted)}a{color:var(--link);text-decoration:none}a:hover{text-decoration:underline}
code{background:var(--panel);padding:1px 4px;border-radius:3px;font-size:12.5px}svg{max-width:100%;height:auto;display:block}.map{margin:12px 0}
nav{display:flex;gap:6px;flex-wrap:wrap;margin:14px 0}.tab{background:var(--panel);color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:5px 12px;cursor:pointer;font:inherit}
.tab.active{border-color:var(--link);color:var(--link)}.panel{display:none}.panel.active{display:block}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:8px;margin:10px 0}.card{background:var(--panel);border:1px solid var(--line);border-radius:6px;padding:8px 10px}
.card b{display:block;font-size:20px;font-weight:600}.card span{color:var(--muted);font-size:12px}
table{border-collapse:collapse;width:100%;background:var(--panel);font-size:13px;margin:6px 0 10px}th,td{border-bottom:1px solid var(--line);padding:4px 8px;text-align:left;vertical-align:top}
th{color:var(--muted);font-weight:600;cursor:pointer;user-select:none;white-space:nowrap}th.sorted::after{content:" \\25BE"}th.sorted.asc::after{content:" \\25B4"}
td.n,th.n{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}tr:last-child td{border-bottom:none}tbody tr:hover td{background:#1c2129}
.bar{display:inline-block;width:80px;height:7px;background:var(--line);border-radius:4px;vertical-align:middle;overflow:hidden;margin-right:6px}.bar i{display:block;height:100%;background:var(--ok)}
.chip{display:inline-block;padding:0 7px;border-radius:10px;font-size:11.5px;line-height:18px;border:1px solid var(--line);color:var(--muted);cursor:pointer;margin:0 4px 4px 0}.chip.on{color:var(--fg)}
.dot{display:inline-block;width:8px;height:8px;border-radius:50%;margin-right:5px}
.tools{display:flex;gap:8px;align-items:center;flex-wrap:wrap;margin:6px 0}input[type=search]{background:var(--panel);border:1px solid var(--line);color:var(--fg);border-radius:6px;padding:4px 8px;font:inherit;min-width:220px}
button.small{background:var(--panel);color:var(--fg);border:1px solid var(--line);border-radius:6px;padding:3px 10px;cursor:pointer;font:inherit}
label.t{color:var(--muted);font-size:12.5px;cursor:pointer}.muted{color:var(--muted)}footer{margin-top:36px;color:var(--muted);font-size:12px}
</style></head><body><main>
<h1>Any360 progress</h1>
<p>How much of the Xbox 360 platform surface Any360 provides natively, whatever the title: every system call a game can import (the exports of <code>xboxkrnl.exe</code>, <code>xam.xex</code>, <code>xbdm.dll</code>), every CPU instruction a recompiled or decompiled game can execute, and the XDK library functions games link statically (Direct3D, CRT, XAudio...). The reference runtime (<a href="https://github.com/rexglue/rexglue-sdk">ReXGlue</a>, stage 0) is the oracle each native function is verified against, not the goal. Known titles are sources: they say which entries matter first, and their own repositories track their own progress. Generated by <a href="https://github.com/GaetanVigner/Any360/blob/main/tools/progress_page.py">tools/progress_page.py</a> from <a href="https://github.com/GaetanVigner/Any360/tree/main/db">db/</a> on __DATE__; raw numbers in <a href="progress.json">progress.json</a>. Public API names, ordinals and counts only: no game code or data.</p>
<nav><button class="tab active" data-tab="overview">Overview</button><button class="tab" data-tab="calls">System calls</button><button class="tab" data-tab="instructions">Instructions</button><button class="tab" data-tab="xdk">XDK libraries</button><button class="tab" data-tab="sources">Sources (titles)</button><button class="tab" data-tab="toolkit">Toolkit</button><button class="tab" data-tab="history">History</button></nav>

<section class="panel active" id="panel-overview">
<div class="cards" id="overview-cards"></div>
<div class="map">__OVERVIEW__</div>
<p>One square per entry of the database, grouped by library and area; hover a block for its numbers, click it for the table. <b>Native</b> is implemented in this repository (<code>any360::sys</code>, <code>any360::gfx</code>, the decompilation rules) and verified against the reference; <b>ported</b> exists as native code in a known title's repository and is to be moved here; <b>reference</b> runs today only through ReXGlue's implementation; <b>stub</b> and <b>missing</b> do not run anywhere yet. The percentages count native and ported.</p>
</section>

<section class="panel" id="panel-calls">
<h2>System calls</h2>
<p>Every export the reference runtime's tables know, plus the imports of known titles it does not. Sorted by the number of known titles importing it, so what matters first comes first.</p>
<div class="tools"><input type="search" id="calls-q" placeholder="name, group, library"><span id="calls-chips"></span><label class="t"><input type="checkbox" id="calls-used"> used by a known title only</label><span class="muted" id="calls-info"></span></div>
<div id="calls"></div>
</section>

<section class="panel" id="panel-instructions">
<h2>CPU instructions</h2>
<p>Every instruction id of the reference recompiler's table (PowerPC, VMX and VMX128). <b>reference</b>: the recompiler emits C++ for it; <b>native</b>: Any360's own translation rules cover it; <b>missing</b>: neither. "used" is the number of occurrences in the known titles' code.</p>
<div class="tools"><input type="search" id="ins-q" placeholder="mnemonic, id, family"><span id="ins-chips"></span><label class="t"><input type="checkbox" id="ins-used"> used by a known title only</label><span class="muted" id="ins-info"></span></div>
<div id="instructions"></div>
</section>

<section class="panel" id="panel-xdk">
<h2>XDK library functions</h2>
<p>Statically linked library functions known by name, with the address each title has them at (<code>db/xdk_seed.json</code>). The unnamed library ranges of each title (below) are the pool signature matching will name.</p>
<div id="xdk"></div>
<h3>Unnamed library functions per title</h3><div id="xdk-unnamed"></div>
</section>

<section class="panel" id="panel-sources">
<h2>Sources: the known titles</h2>
<p>Titles whose recompilation feeds the database (their imports and instruction mix). They are examples, not the unit of progress: each title's own repository tracks how native that game is.</p>
<div id="sources"></div>
</section>

<section class="panel" id="panel-toolkit">
<h2>Toolkit components by stage</h2>
<p>Where each component exists today and its state; kept by hand in <code>layer/status.json</code>. Stages: 0 scaffold (ReXGlue), 1 native system and graphics, 2 decompilation, 3 portable codebase.</p>
<div id="toolkit"></div>
<h2>SDK patch sets by area</h2><div id="patches"></div>
</section>

<section class="panel" id="panel-history">
<h2>History</h2>
<p>One point per day the page was generated (<a href="history.json">history.json</a>): native or ported system calls (green) over the ones that run on the reference (purple). Hover a column for the day's numbers.</p>
__HISTORY__
</section>

<footer>Database: <a href="https://github.com/GaetanVigner/Any360/tree/main/db">db/</a> · title reports: <a href="coverage/">docs/coverage/</a> · design and counting rules: <a href="DESIGN.md">DESIGN.md</a> · <a href="https://github.com/GaetanVigner/Any360">GitHub</a></footer>
</main>
<script type="application/json" id="data">__DATA__</script>
<script>
const D = JSON.parse(document.getElementById('data').textContent);
const CS = {native:'#2ea043',ported:'#2f81f7',reference:'#8957e5',stub:'#d29922',missing:'#f85149',todo:'#f85149'};
const SC = {done:'#2ea043','in progress':'#d29922',planned:'#6e7681','game-specific':'#8957e5'};
const $ = (s, r=document) => r.querySelector(s);
const h = s => String(s).replace(/[&<>"]/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));
const pct = (a, b) => b ? (a / b * 100).toFixed(1) + '%' : '-';
const bar = (a, b) => `<span class="bar"><i style="width:${b ? a / b * 100 : 0}%"></i></span>${pct(a, b)}`;
const num = n => Number(n).toLocaleString('en-US');
const dot = (c, t) => `<span class="dot" style="background:${c}"></span>${h(t)}`;

function table(sel, cols, rows, opts = {}) {
  // cols: [{k, t, n (numeric), f (formatter)}]; rows: objects. Click a header to sort; opts.page rows per page.
  const el = $(sel); let sort = opts.sort || null, asc = opts.asc ?? false, page = 0;
  const per = opts.page || Infinity;
  function render() {
    let rs = rows.slice();
    if (sort) { const c = cols.find(c => c.k === sort); rs.sort((a, b) => { const x = a[sort], y = b[sort]; const r = c.n ? (x - y) : String(x).localeCompare(String(y)); return asc ? r : -r; }); }
    const total = rs.length; rs = rs.slice(page * per, (page + 1) * per);
    el.innerHTML = `<table><thead><tr>${cols.map(c => `<th class="${c.n ? 'n' : ''} ${sort === c.k ? 'sorted' + (asc ? ' asc' : '') : ''}" data-k="${c.k}">${h(c.t)}</th>`).join('')}</tr></thead><tbody>` +
      rs.map(r => `<tr>${cols.map(c => `<td class="${c.n ? 'n' : ''}">${c.f ? c.f(r[c.k], r) : h(r[c.k] ?? '')}</td>`).join('')}</tr>`).join('') + '</tbody></table>' +
      (total > per ? `<div class="tools"><button class="small" data-p="-1">previous</button><span class="muted">${page * per + 1}-${Math.min((page + 1) * per, total)} of ${num(total)}</span><button class="small" data-p="1">next</button></div>` : '');
    el.querySelectorAll('th').forEach(th => th.onclick = () => { const k = th.dataset.k; if (sort === k) asc = !asc; else { sort = k; asc = false; } page = 0; render(); });
    el.querySelectorAll('button[data-p]').forEach(b => b.onclick = () => { page = Math.max(0, Math.min(Math.ceil(total / per) - 1, page + Number(b.dataset.p))); render(); });
  }
  render(); return { update(newRows) { rows = newRows; page = 0; render(); } };
}
function chips(sel, items, onChange) {
  const on = new Set(items);
  $(sel).innerHTML = items.map(i => `<span class="chip on" data-s="${i}" style="border-color:${CS[i]}">${dot(CS[i], i)}</span>`).join('');
  $(sel).querySelectorAll('.chip').forEach(c => c.onclick = () => { c.classList.toggle('on'); on.has(c.dataset.s) ? on.delete(c.dataset.s) : on.add(c.dataset.s); onChange(); });
  return on;
}

// Overview cards.
const S = D.summary, sc = S.system_calls, si = S.instructions, sx = S.xdk;
$('#overview-cards').innerHTML = `
  <div class="card"><b>${num(sc.native + sc.ported)} / ${num(sc.total)}</b><span>system calls native or ported (${pct(sc.native + sc.ported, sc.total)}); ${num(sc.runnable)} run on the reference</span></div>
  <div class="card"><b>${num(sc.used_by_titles)}</b><span>system calls the ${Object.keys(S.titles).length} known titles import</span></div>
  <div class="card"><b>${num(si.native)} / ${num(si.total)}</b><span>instruction ids with native rules; ${num(si.reference)} handled by the reference recompiler</span></div>
  <div class="card"><b>${num(sx.native + sx.ported)} / ${num(sx.total)}</b><span>named XDK library functions native</span></div>
  <div class="card"><b>${S.toolkit.done} / ${S.toolkit.components}</b><span>toolkit components done (${S.toolkit.in_progress} in progress)</span></div>`;

// System calls.
{
  const cols = [{k:'library',t:'Library'},{k:'name',t:'Export',f:v=>`<code>${h(v)}</code>`},{k:'ordinal',t:'Ordinal',n:1,f:v=>v==null?'':'0x'+v.toString(16).toUpperCase()},{k:'group',t:'Area'},
                {k:'state',t:'State',f:v=>dot(CS[v], v)},{k:'reference',t:'Reference'},{k:'n',t:'Titles',n:1,f:(v,r)=>v ? `${v} <span class="muted">(${r.titles.join(', ')})</span>` : '<span class="muted">0</span>'}];
  const rows = D.calls.map(r => ({...r, n: r.titles.length}));
  let t = null;
  const on = chips('#calls-chips', ['native','ported','reference','stub','missing'], apply);
  function apply() {
    const q = $('#calls-q').value.trim().toLowerCase(), used = $('#calls-used').checked;
    const rs = rows.filter(r => on.has(r.state) && (!used || r.n) && (!q || (r.name + ' ' + r.group + ' ' + r.library).toLowerCase().includes(q)));
    t ? t.update(rs) : t = table('#calls', cols, rs, {sort:'n', page:200}); $('#calls-info').textContent = `${num(rs.length)} of ${num(rows.length)}`;
  }
  $('#calls-q').oninput = apply; $('#calls-used').onchange = apply; apply();
}
// Instructions.
{
  const cols = [{k:'family',t:'Family'},{k:'mnemonic',t:'Mnemonic',f:v=>`<code>${h(v)}</code>`},{k:'id',t:'Id',f:v=>`<span class="muted">${h(v)}</span>`},{k:'state',t:'State',f:v=>dot(CS[v], v)},
                {k:'used',t:'Used (occurrences)',n:1,f:(v,r)=>v ? `${num(v)} <span class="muted">(${Object.entries(r.titles).map(([t,c])=>t+' '+num(c)).join(', ')})</span>` : '<span class="muted">0</span>'}];
  let t = null;
  const on = chips('#ins-chips', ['native','reference','missing'], apply);
  function apply() {
    const q = $('#ins-q').value.trim().toLowerCase(), used = $('#ins-used').checked;
    const rs = D.instructions.filter(r => on.has(r.state) && (!used || r.used) && (!q || (r.mnemonic + ' ' + r.id + ' ' + r.family).toLowerCase().includes(q)));
    t ? t.update(rs) : t = table('#instructions', cols, rs, {sort:'used', page:200}); $('#ins-info').textContent = `${num(rs.length)} of ${num(D.instructions.length)}`;
  }
  $('#ins-q').oninput = apply; $('#ins-used').onchange = apply; apply();
}
// XDK.
table('#xdk', [{k:'library',t:'Library'},{k:'name',t:'Function',f:v=>`<code>${h(v)}</code>`},{k:'any360',t:'State',f:v=>dot(CS[v], v)},
               {k:'titles',t:'Known at',f:v=>Object.entries(v).map(([t,a])=>`${h(t)} <code>${h(a)}</code>`).join(', ')},{k:'note',t:'Note',f:v=>`<span class="muted">${h(v||'')}</span>`}], D.xdk, {sort:'library', asc:true});
table('#xdk-unnamed', [{k:'title',t:'Title'},{k:'range',t:'Library range'},{k:'functions',t:'Functions',n:1,f:num}],
      Object.entries(sx.unnamed_by_title).flatMap(([title, ranges]) => Object.entries(ranges).map(([range, functions]) => ({title, range, functions}))), {sort:'functions'});
// Sources.
table('#sources', [{k:'title',t:'Title',f:(v)=>D.repos[v] ? `<a href="${D.repos[v]}">${h(v)}</a>` : h(v)},{k:'sdk',t:'Reference SDK'},{k:'imports',t:'System calls imported',n:1,f:num},
                   {k:'mnemonics',t:'Mnemonics used',n:1,f:num},{k:'instructions',t:'Instructions in code',n:1,f:num},{k:'unnamed_library_functions',t:'Unnamed library functions',n:1,f:num}],
      Object.entries(S.titles).map(([title, t]) => ({title, ...t})), {sort:'title', asc:true});
// Toolkit.
table('#toolkit', [{k:'area',t:'Stage / area'},{k:'name',t:'Component'},{k:'state',t:'State',f:v=>dot(SC[v]||'#6e7681', v)},{k:'source',t:'Comes from',f:v=>`<span class="muted">${h(v)}</span>`},{k:'note',t:'Note',f:v=>`<span class="muted">${h(v)}</span>`}], D.components, {sort:'area', asc:true});
table('#patches', [{k:'set',t:'Patch set'},{k:'area',t:'SDK area'},{k:'count',t:'Patches',n:1}], Object.entries(D.patches).flatMap(([set, areas]) => Object.entries(areas).map(([area, count]) => ({set, area, count}))), {sort:'count'});
// Tabs.
document.querySelectorAll('.tab').forEach(b => b.onclick = () => { document.querySelectorAll('.tab').forEach(x => x.classList.toggle('active', x === b)); document.querySelectorAll('.panel').forEach(p => p.classList.toggle('active', p.id === 'panel-' + b.dataset.tab)); if (location.hash !== '#' + b.dataset.tab) history.replaceState(null, '', '#' + b.dataset.tab); });
const goHash = () => { const b = document.querySelector(`.tab[data-tab="${location.hash.slice(1)}"]`); if (b && !b.classList.contains('active')) { b.click(); window.scrollTo(0, 0); } };
window.addEventListener('hashchange', goHash); goHash();
document.querySelectorAll('.map a').forEach(a => a.onclick = e => { e.preventDefault(); location.hash = a.getAttribute('href').split('#')[1] || 'overview'; });
</script>
</body></html>
'''

if __name__ == '__main__':
    main()
