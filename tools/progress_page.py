#!/usr/bin/env python3
"""Any360 progress page, in the style of AnyPS5's progress.html: a treemap of unit squares per area (one square per
function or system call, coloured by state, a tooltip per block), tables of counts and percentages, one tab per title,
and an interactive function table (sort, search, filter by state) loaded from the inventory.

Reads docs/coverage/<title>.json (tools/coverage.py), docs/coverage/inventory-<title>.json and
docs/data/functions-<title>.json (tools/inventory.py), docs/coverage/patches-<title>.json (tools/patch_inventory.py),
titles/<title>/render_passes.json and layer/status.json. Writes docs/progress.html, docs/progress.svg,
docs/progress.json and appends today's point to docs/history.json. Standard library only; run it before every commit.

  python tools/progress_page.py
"""
import json
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / 'docs'
COVERAGE = DOCS / 'coverage'
TITLES = ROOT / 'titles'
STATUS = ROOT / 'layer' / 'status.json'

BG, PANEL, BORDER, TEXT, MUTED = '#0d1117', '#161b22', '#30363d', '#c9d1d9', '#8b949e'
FUNCTION_STATES = [('N', 'native', '#2ea043'), ('V', 'verified', '#2f81f7'), ('L', 'library', '#8957e5'), ('R', 'recompiled', '#6e7681')]
CALL_STATES = [('implemented', '#2ea043'), ('variable', '#2ea043'), ('stub', '#d29922'), ('missing', '#f85149')]
PASS_STAGES = [('emulated', '#6e7681'), ('records', '#2f81f7'), ('dry run', '#8957e5'), ('native draw', '#d29922'), ('validated', '#2ea043'), ('native', '#1a7f37')]


def esc(text):
    return str(text).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')


def pct(part, total):
    return f'{part / total * 100:.1f}%' if total else '-'


def load_json(path, default=None):
    return json.loads(path.read_text(encoding='utf-8')) if path.exists() else default


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


def treemap(x0, y0, w, h, title, groups, states, done_states):
    """groups: [(name, {state: count})]. One unit square per item (merged when a block holds more than ~400)."""
    total = sum(sum(c.values()) for _, c in groups)
    done = sum(c.get(s, 0) for _, c in groups for s in done_states)
    out = [f'<text x="{x0}" y="{y0 - 9}" font-size="16" fill="{TEXT}" font-weight="600">{esc(title)}: '
           f'{pct(done, total)} ({done:,}/{total:,})</text>',
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
        tip = f'{name}: {count:,} ({share:.0f}% ' + '/'.join(done_states) + ')' + (f' [{per} per square]' if per > 1 else '')
        out.append(f'<g class="blk"><title>{esc(tip)}</title>')
        for i, state in enumerate(units):
            cx, cy = bx + (i % cols) * cw, by + (i // cols) * ch
            out.append(f'<rect x="{cx:.2f}" y="{cy:.2f}" width="{max(cw - .5, .3):.2f}" height="{max(ch - .5, .3):.2f}" fill="{color[state]}"/>')
        out.append(f'<rect x="{bx:.1f}" y="{by:.1f}" width="{bw:.1f}" height="{bh:.1f}" fill="none" stroke="{BG}" stroke-width="1.5"/>')
        if bw > 40 and bh > 14:
            label = name if bw > 7 * len(name) else name[:max(2, int(bw / 7))]
            out.append(f'<text x="{bx + 4:.1f}" y="{by + 13:.1f}" font-size="12" fill="#ffffff" stroke="{BG}" '
                       f'stroke-width="3" paint-order="stroke">{esc(label)}</text>')
        out.append('</g>')
    return out


def legend(x, y, entries):
    out = []
    for i, (label, color) in enumerate(entries):
        lx = x + i * 118
        out.append(f'<rect x="{lx}" y="{y}" width="10" height="10" rx="2" fill="{color}"/>')
        out.append(f'<text x="{lx + 14}" y="{y + 9}" font-size="11" fill="{MUTED}">{esc(label)}</text>')
    return out


def title_svg(name, inv, cov, passes):
    """Two treemaps for a title (functions by module, system calls by library) plus the render pass strip."""
    width, map_h, header = 1000, 260, 34
    out = []
    groups = []
    for m, v in inv['modules'].items():
        rest = v['functions'] - v['native'] - v['verified']
        groups.append((m, {'N': v['native'], 'V': v['verified'], 'L': rest if v['kind'] == 'library' else 0,
                           'R': rest if v['kind'] != 'library' else 0}))
    out += treemap(0, header, 495, map_h, f'{name}: functions native', groups, [(s, c) for s, _, c in FUNCTION_STATES], ['N', 'V', 'L'])
    by_lib = {}
    for c in cov.get('imports', []):
        by_lib.setdefault(c['library'], {}).setdefault(c['state'], 0)
        by_lib[c['library']][c['state']] += 1
    out += treemap(505, header, 495, map_h, 'system calls provided', list(by_lib.items()), CALL_STATES, ['implemented', 'variable'])
    y = header + map_h + 8
    out += legend(0, y, [(lbl, c) for _, lbl, c in FUNCTION_STATES])
    out += legend(505, y, [(s, c) for s, c in CALL_STATES if s != 'variable'])
    height = y + 20
    names = passes.get('pass_names', []) if passes else []
    if names:
        stages = passes.get('passes', {})
        native = sum(1 for n in names if stages.get(n, 'emulated') in ('validated', 'native'))
        y = height + 24
        out.append(f'<text x="0" y="{y - 8}" font-size="14" fill="{TEXT}" font-weight="600">render passes native: {native}/{len(names)}</text>')
        color = dict(PASS_STAGES)
        cw = (width - 2 * (len(names) - 1)) / len(names)
        for i, n in enumerate(names):
            st = stages.get(n, 'emulated')
            out.append(f'<rect x="{i * (cw + 2):.1f}" y="{y}" width="{cw:.1f}" height="16" rx="2" fill="{color.get(st, MUTED)}">'
                       f'<title>{esc(n)}: {esc(st)}</title></rect>')
        height = y + 20
        out += legend(0, height + 4, PASS_STAGES)
        height += 24
    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" width="{width}" height="{height}" font-family="sans-serif">',
           f'<rect width="{width}" height="{height}" fill="{BG}"/>', '<style>.blk:hover rect{opacity:.8}</style>'] + out + ['</svg>']
    return '\n'.join(svg), height


# --- history ---------------------------------------------------------------------------------------------------

def history_svg(history, names):
    if not history:
        return ''
    w, h, left, top, plot_h = 1000, 70 + 150 * len(names), 60, 10, 120
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w}" height="{h}" font-family="sans-serif" font-size="12">',
           f'<rect width="{w}" height="{h}" rx="6" fill="{BG}"/>', '<style>.hit{fill:#fff;fill-opacity:0}.hit:hover{fill-opacity:.08}</style>']
    days = [p['date'] for p in history]
    xs = [left + (i * (w - left - 220) / max(len(days) - 1, 1)) for i in range(len(days))]
    for k, name in enumerate(names):
        y0 = top + k * 150 + 20
        series = [(p['titles'].get(name, {}).get('functions_native', 0), p['titles'].get(name, {}).get('imports_provided', 0),
                   p['titles'].get(name, {}).get('imports', 0)) for p in history]
        maximum = max(max(s[0] for s in series), 1)
        out.append(f'<text x="{left}" y="{y0 - 6}" fill="{TEXT}" font-weight="bold" font-size="13">{esc(name)}: functions native over time</text>')
        for g in range(3):
            gy = y0 + plot_h - g * plot_h / 2
            out.append(f'<line x1="{left}" x2="{w - 220}" y1="{gy:.1f}" y2="{gy:.1f}" stroke="{BORDER}"/>')
            out.append(f'<text x="{left - 6}" y="{gy + 4:.1f}" text-anchor="end" fill="{MUTED}">{int(maximum * g / 2):,}</text>')
        pts = ' '.join(f'{x:.1f},{y0 + plot_h - s[0] / maximum * plot_h:.1f}' for x, s in zip(xs, series))
        out.append(f'<polyline points="{pts}" fill="none" stroke="#2f81f7" stroke-width="2"/>')
        for x, d, s in zip(xs, days, series):
            out.append(f'<rect class="hit" x="{x - 6:.1f}" y="{y0}" width="12" height="{plot_h}"><title>{d}: {s[0]:,} functions native, '
                       f'{s[1]}/{s[2]} system calls provided</title></rect>')
        out.append(f'<text x="{w - 210}" y="{y0 + plot_h / 2:.0f}" fill="{TEXT}"><tspan fill="#2f81f7">●</tspan> native {series[-1][0]:,}</text>')
        out.append(f'<text x="{left}" y="{y0 + plot_h + 16}" fill="{MUTED}">{days[0]}</text>')
        out.append(f'<text x="{w - 220}" y="{y0 + plot_h + 16}" fill="{MUTED}" text-anchor="end">{days[-1]}</text>')
    out.append('</svg>')
    return '\n'.join(out)


# --- the page ----------------------------------------------------------------------------------------------------

def main():
    names = sorted(p.stem[len('inventory-'):] for p in COVERAGE.glob('inventory-*.json'))
    inv = {n: load_json(COVERAGE / f'inventory-{n}.json') for n in names}
    cov = {n: load_json(COVERAGE / f'{n}.json', {}) for n in names}
    passes = {n: load_json(TITLES / n / 'render_passes.json', {}) for n in names}
    patches = {p.stem[len('patches-'):]: load_json(p) for p in COVERAGE.glob('patches-*.json')}
    status = load_json(STATUS, {'components': []})
    today = date.today().isoformat()

    summary = {'date': today, 'titles': {}, 'toolkit': {}}
    svgs = []
    for n in names:
        i, c = inv[n], cov[n]
        provided = sum(1 for x in c.get('imports', []) if x['state'] in ('implemented', 'variable'))
        handled = sum(x['count'] for x in c.get('instructions', []) if x['state'] == 'handled')
        total_i = sum(x['count'] for x in c.get('instructions', []))
        pn = passes[n].get('pass_names', [])
        summary['titles'][n] = {
            'functions': i['functions'], 'functions_native': i['native'], 'functions_verified': i['verified'],
            'functions_library': i['library'], 'instructions': i['instructions'],
            'imports': len(c.get('imports', [])), 'imports_provided': provided,
            'imports_stub': sum(1 for x in c.get('imports', []) if x['state'] == 'stub'),
            'instructions_handled': handled, 'instructions_total': total_i, 'mnemonics': len(c.get('instructions', [])),
            'passes': len(pn), 'passes_native': sum(1 for p in pn if passes[n].get('passes', {}).get(p) in ('validated', 'native')),
            'sdk': c.get('sdk', {}).get('version', '')}
        svgs.append(title_svg(n, i, c, passes[n]))
    comps = status['components']
    summary['toolkit'] = {'components': len(comps), 'done': sum(1 for x in comps if x['state'] == 'done'),
                          'in_progress': sum(1 for x in comps if x['state'] == 'in progress'),
                          'patches': {k: v['count'] for k, v in patches.items()}}

    history_path = DOCS / 'history.json'
    history = [p for p in load_json(history_path, []) if p['date'] != today]
    history.append({'date': today, 'titles': {n: {k: summary['titles'][n][k] for k in
                                                   ('functions_native', 'functions_verified', 'imports_provided', 'imports', 'passes_native')}
                                               for n in names}, 'toolkit_done': summary['toolkit']['done']})
    history_path.write_text(json.dumps(history, indent=1) + '\n', encoding='utf-8')
    (DOCS / 'progress.json').write_text(json.dumps(summary, indent=1) + '\n', encoding='utf-8')

    y, parts = 0, []
    for svg, height in svgs:
        body = svg.split('\n', 3)[3].rsplit('</svg>', 1)[0]
        parts.append(f'<g transform="translate(0,{y})">{body}</g>')
        y += height + 16
    (DOCS / 'progress.svg').write_text(
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1000 {y}" width="1000" height="{y}" font-family="sans-serif">'
        f'<rect width="1000" height="{y}" fill="{BG}"/>' + ''.join(parts) + '</svg>\n', encoding='utf-8')

    page_data = {'date': today, 'titles': {}, 'components': comps, 'patches': {k: v['areas'] for k, v in patches.items()}}
    for n in names:
        page_data['titles'][n] = {
            'summary': summary['titles'][n], 'modules': inv[n]['modules'], 'imports': cov[n].get('imports', []),
            'translated': inv[n]['translated'], 'passes': passes[n].get('passes', {}),
            'pass_names': passes[n].get('pass_names', []), 'functions_file': f'data/functions-{n}.json'}

    html = TEMPLATE.replace('__DATA__', json.dumps(page_data, separators=(',', ':')).replace('</', '<\\/'))
    html = html.replace('__DATE__', today)
    html = html.replace('__TABS__', ''.join(f'<button class="tab" data-tab="{esc(n)}">{esc(n)}</button>' for n in names))
    html = html.replace('__PANELS__', ''.join(f'<div class="map">{svg}</div>' for svg, _ in svgs))
    html = html.replace('__HISTORY__', history_svg(history, names))
    (DOCS / 'progress.html').write_text(html, encoding='utf-8')
    print('progress page: ' + '; '.join(
        f'{n}: {summary["titles"][n]["functions_native"]}/{summary["titles"][n]["functions"]} functions native, '
        f'{summary["titles"][n]["imports_provided"]}/{summary["titles"][n]["imports"]} calls, '
        f'{summary["titles"][n]["passes_native"]}/{summary["titles"][n]["passes"]} passes' for n in names)
        + f'; toolkit {summary["toolkit"]["done"]}/{summary["toolkit"]["components"]}; wrote {DOCS / "progress.html"}')


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
.muted{color:var(--muted)}footer{margin-top:36px;color:var(--muted);font-size:12px}
</style></head><body><main>
<h1>Any360 progress</h1>
<p>How native each Xbox 360 title is on its way from a <a href="https://github.com/rexglue/rexglue-sdk">ReXGlue</a> static recompilation (stage 0, the oracle) to a portable native codebase (stage 3): the game's functions replaced by native C++, the system calls the runtime still provides, the render passes drawn natively, and the toolkit's components. Generated by <a href="https://github.com/GaetanVigner/Any360/blob/main/tools/progress_page.py">tools/progress_page.py</a> on __DATE__; raw numbers in <a href="progress.json">progress.json</a>, per-function inventories in <a href="data/">data/</a>. Counts, addresses and public API names only: no game code or data.</p>
<nav><button class="tab active" data-tab="overview">Overview</button>__TABS__<button class="tab" data-tab="toolkit">Toolkit</button><button class="tab" data-tab="history">History</button></nav>

<section class="panel active" id="panel-overview">
<div class="cards" id="overview-cards"></div>
__PANELS__
<p>One square per function (left) and per system call (right), grouped by module and by library; hover a block for its numbers. Library ranges (XDK D3D runtime, CRT, import thunks) are replaced as a unit by Any360 rather than decompiled; the rest of the game's functions are the stage 2 work. Modules named <code>code 82Xxxxxx</code> are 1 MB areas not yet named by symbol recovery.</p>
</section>

<div id="title-panels"></div>

<section class="panel" id="panel-toolkit">
<h2>Toolkit components by stage</h2>
<p>Where each component exists today and its state; kept by hand in <code>layer/status.json</code>. Stages: 0 scaffold (ReXGlue), 1 native system and graphics, 2 decompilation, 3 portable codebase.</p>
<div id="toolkit"></div>
<h2>SDK patch sets by area</h2><div id="patches"></div>
</section>

<section class="panel" id="panel-history">
<h2>History</h2>
<p>One point per day the page was generated (<a href="history.json">history.json</a>). Hover a column for the day's numbers.</p>
__HISTORY__
</section>

<footer>Reports: <a href="coverage/">docs/coverage/</a> · design and counting rules: <a href="DESIGN.md">DESIGN.md</a> · <a href="https://github.com/GaetanVigner/Any360">GitHub</a></footer>
</main>
<script type="application/json" id="data">__DATA__</script>
<script>
const D = JSON.parse(document.getElementById('data').textContent);
const FS = {N:['native','#2ea043'],V:['verified','#2f81f7'],L:['library','#8957e5'],R:['recompiled','#6e7681']};
const CS = {implemented:'#2ea043',variable:'#2ea043',stub:'#d29922',missing:'#f85149'};
const PS = {emulated:'#6e7681',records:'#2f81f7','dry run':'#8957e5','native draw':'#d29922',validated:'#2ea043',native:'#1a7f37'};
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
function chips(sel, items, colors, onChange) {
  const on = new Set(items.map(i => i.k));
  $(sel).innerHTML = items.map(i => `<span class="chip on" data-s="${i.k}" style="border-color:${colors[i.k]}">${dot(colors[i.k], i.t)}</span>`).join('');
  $(sel).querySelectorAll('.chip').forEach(c => c.onclick = () => { c.classList.toggle('on'); on.has(c.dataset.s) ? on.delete(c.dataset.s) : on.add(c.dataset.s); onChange(on); });
  return on;
}

// Overview cards.
$('#overview-cards').innerHTML = Object.entries(D.titles).map(([n, t]) => { const s = t.summary; return `
  <div class="card"><b>${pct(s.functions_native + s.functions_verified + s.functions_library, s.functions)}</b><span>${h(n)}: functions native or replaced (${num(s.functions_native)} native, ${num(s.functions_verified)} verified, ${num(s.functions_library)} library of ${num(s.functions)})</span></div>
  <div class="card"><b>${pct(s.imports_provided, s.imports)}</b><span>${h(n)}: system calls the runtime provides (${s.imports_stub} stubs of ${s.imports})</span></div>
  <div class="card"><b>${s.passes_native}/${s.passes || '?'}</b><span>${h(n)}: render passes native</span></div>`; }).join('') +
  `<div class="card"><b>${D.components.filter(c => c.state === 'done').length}/${D.components.length}</b><span>toolkit components done</span></div>`;

// One panel per title.
for (const [n, t] of Object.entries(D.titles)) {
  const s = t.summary, sec = document.createElement('section'); sec.className = 'panel'; sec.id = 'panel-' + n;
  sec.innerHTML = `<h2>${h(n)} <span class="muted" style="font-size:13px">ReXGlue ${h(s.sdk)}</span></h2>
    <div class="cards"><div class="card"><b>${num(s.functions)}</b><span>recompiled functions, ${num(s.instructions)} instructions</span></div>
    <div class="card"><b>${num(s.functions_native)}</b><span>bound to native C++</span></div><div class="card"><b>${num(s.functions_verified)}</b><span>verified in the sandbox</span></div>
    <div class="card"><b>${num(s.functions_library)}</b><span>in library ranges (replaced as a unit)</span></div><div class="card"><b>${s.imports_provided}/${s.imports}</b><span>system calls provided</span></div>
    <div class="card"><b>${pct(s.instructions_handled, s.instructions_total)}</b><span>instructions the recompiler handles (${s.mnemonics} mnemonics)</span></div></div>
    <h3>Functions by module</h3><div id="mod-${n}"></div>
    <h3>Translated so far: what the port already replaces with native code</h3><div id="tr-${n}"></div>
    <h3>System calls</h3><div class="tools" id="imp-tools-${n}"></div><div id="imp-${n}"></div>
    <h3>Render passes</h3><div id="pass-${n}"></div>
    <h3>Every function</h3><div class="tools"><input type="search" placeholder="address or handler" id="fn-q-${n}"><span id="fn-chips-${n}"></span><span class="muted" id="fn-info-${n}"></span></div><div id="fn-${n}"></div>`;
  $('#title-panels').appendChild(sec);
  table('#mod-' + n, [{k:'name',t:'Module'},{k:'kind',t:'Kind'},{k:'functions',t:'Functions',n:1,f:num},{k:'instructions',t:'Instructions',n:1,f:num},{k:'native',t:'Native',n:1},{k:'verified',t:'Verified',n:1},
    {k:'done',t:'% native, verified or library',n:1,f:(v,r)=>bar(v, r.functions)}],
    Object.entries(t.modules).map(([name, m]) => ({name, ...m, done: m.native + m.verified + (m.kind === 'library' ? m.functions - m.native - m.verified : 0)})), {sort:'instructions'});
  table('#tr-' + n, [{k:'address',t:'Address',f:v=>`<code>sub_${h(v)}</code>`},{k:'handler',t:'Handler'},{k:'file',t:'File'},{k:'instructions',t:'Guest instructions',n:1},{k:'purpose',t:'Purpose',f:v=>`<span class="muted">${h(v)}</span>`}], t.translated, {sort:'file', asc:true});
  const imp = table('#imp-' + n, [{k:'library',t:'Library'},{k:'name',t:'Import',f:v=>`<code>${h(v)}</code>`},{k:'state',t:'Runtime',f:v=>dot(CS[v], v)},{k:'other_state',t:'Other SDK',f:v=>h(v||'')}], t.imports, {sort:'state', asc:true, page:400});
  chips('#imp-tools-' + n, [...new Set(t.imports.map(c => c.state))].map(s => ({k:s, t:`${s} (${t.imports.filter(c => c.state === s).length})`})), CS, on => imp.update(t.imports.filter(x => on.has(x.state))));
  table('#pass-' + n, [{k:'name',t:'Pass'},{k:'stage',t:'Stage',f:v=>dot(PS[v], v)}], t.pass_names.map(p => ({name: p, stage: t.passes[p] || 'emulated'})));
  // Every function, fetched on demand (needs http: GitHub Pages or a local server).
  let rows = [], fn = null;
  const apply = () => { const q = $('#fn-q-' + n).value.trim().toUpperCase(); const rs = rows.filter(r => fnOn.has(r.status) && (!q || r.address.includes(q) || r.handler.toUpperCase().includes(q)));
    const cols = [{k:'address',t:'Function',f:v=>`<code>sub_${h(v)}</code>`},{k:'instructions',t:'Instructions',n:1},{k:'status',t:'State',f:v=>dot(FS[v][1], FS[v][0])},{k:'handler',t:'Handler'},{k:'file',t:'File'}];
    fn ? fn.update(rs) : fn = table('#fn-' + n, cols, rs, {sort:'instructions', page:200}); $('#fn-info-' + n).textContent = `${num(rs.length)} of ${num(rows.length)}`; };
  const fnOn = chips('#fn-chips-' + n, Object.entries(FS).map(([k, [t]]) => ({k, t})), Object.fromEntries(Object.entries(FS).map(([k, [, c]]) => [k, c])), apply);
  $('#fn-q-' + n).oninput = apply;
  fetch(t.functions_file).then(r => r.json()).then(j => { rows = j.rows.map(r => ({address:r[0], instructions:r[1], status:r[2], handler:r[3], file:r[4]})); apply(); })
    .catch(() => { $('#fn-' + n).innerHTML = `<p class="muted">The per-function table loads <code>${h(t.functions_file)}</code> over http (GitHub Pages or a local server); a file:// page cannot fetch it.</p>`; });
}

table('#toolkit', [{k:'area',t:'Stage / area'},{k:'name',t:'Component'},{k:'state',t:'State',f:v=>dot(SC[v]||'#6e7681', v)},{k:'source',t:'Comes from',f:v=>`<span class="muted">${h(v)}</span>`},{k:'note',t:'Note',f:v=>`<span class="muted">${h(v)}</span>`}], D.components, {sort:'area', asc:true});
table('#patches', [{k:'set',t:'Patch set'},{k:'area',t:'SDK area'},{k:'count',t:'Patches',n:1}], Object.entries(D.patches).flatMap(([set, areas]) => Object.entries(areas).map(([area, count]) => ({set, area, count}))), {sort:'count'});

document.querySelectorAll('.tab').forEach(b => b.onclick = () => { document.querySelectorAll('.tab').forEach(x => x.classList.toggle('active', x === b)); document.querySelectorAll('.panel').forEach(p => p.classList.toggle('active', p.id === 'panel-' + b.dataset.tab)); history.replaceState(null, '', '#' + b.dataset.tab); });
if (location.hash) { const b = document.querySelector(`.tab[data-tab="${location.hash.slice(1)}"]`); if (b) b.click(); }
</script></body></html>
'''


if __name__ == '__main__':
    main()
