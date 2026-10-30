#!/usr/bin/env python3
"""Any360 progress page, in the style of AnyPS5's progress.html: tables of counts and percentages per area, one
column per title, with the raw numbers in progress.json and a summary chart in progress.svg.

Reads docs/coverage/<title>.json (tools/coverage.py), docs/coverage/patches-<title>.json (tools/patch_inventory.py)
and layer/status.json (the layer's own components and their state, kept by hand). Writes docs/progress.html,
docs/progress.svg and docs/progress.json. Standard library only; run it before every commit.

  python tools/progress_page.py
"""
import json
import sys
from datetime import date
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))  # tools/coverage.py: the family rule, also under python -I
from coverage import family  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / 'docs'
COVERAGE = DOCS / 'coverage'
STATUS = ROOT / 'layer' / 'status.json'

STATE_COLORS = {'done': '#2da44e', 'in progress': '#bf8700', 'planned': '#30363d', 'game-specific': '#6e7681'}


def esc(text):
    return str(text).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')


def pct(part, total):
    return f'{part / total * 100:.1f}%' if total else '-'


def load():
    titles = {}
    for path in sorted(COVERAGE.glob('*.json')):
        if path.name.startswith(('patches-', 'progress')):
            continue
        titles[path.stem] = json.loads(path.read_text(encoding='utf-8'))
    patches = {p.stem[len('patches-'):]: json.loads(p.read_text(encoding='utf-8')) for p in COVERAGE.glob('patches-*.json')}
    status = json.loads(STATUS.read_text(encoding='utf-8')) if STATUS.exists() else {'components': []}
    return titles, patches, status


def table(headers, rows):
    out = ['<table>', '<thead><tr>' + ''.join(f'<th>{esc(h)}</th>' for h in headers) + '</tr></thead>', '<tbody>']
    for row in rows:
        cells = []
        for i, cell in enumerate(row):
            cls = ' class="n"' if i and isinstance(cell, (int, float)) or (isinstance(cell, str) and cell.endswith('%')) else ''
            cells.append(f'<td{cls}>{cell if isinstance(cell, str) and cell.startswith("<") else esc(cell)}</td>')
        out.append('<tr>' + ''.join(cells) + '</tr>')
    out += ['</tbody>', '</table>']
    return '\n'.join(out)


def bar(part, total, color='#2da44e'):
    share = part / total * 100 if total else 0
    return (f'<span class="bar"><span style="width:{share:.1f}%;background:{color}"></span></span> '
            f'<span class="n">{pct(part, total)}</span>')


def main():
    titles, patches, status = load()
    names = list(titles)
    summary = {'date': date.today().isoformat(), 'titles': {}, 'layer': {}}

    # --- system calls per library -------------------------------------------------------------------------------
    libraries = sorted({c['library'] for t in titles.values() for c in t['imports']})
    rows = []
    for lib in libraries:
        row = [lib]
        for name in names:
            calls = [c for c in titles[name]['imports'] if c['library'] == lib]
            provided = sum(1 for c in calls if c['state'] in ('implemented', 'variable'))
            row += [f'{provided} / {len(calls)}', bar(provided, len(calls))]
        rows.append(row)
    total_row = ['Total']
    for name in names:
        calls = titles[name]['imports']
        provided = sum(1 for c in calls if c['state'] in ('implemented', 'variable'))
        total_row += [f'{provided} / {len(calls)}', bar(provided, len(calls))]
        summary['titles'][name] = {'imports': len(calls), 'imports_provided': provided,
                                   'functions': titles[name]['functions']['total'],
                                   'functions_native': titles[name]['functions']['hooked']}
    rows.append(total_row)
    syscalls = table(['Library'] + [h for n in names for h in (f'{n}: provided / imported', '%')], rows)

    # --- instructions per family --------------------------------------------------------------------------------
    families = sorted({f for t in titles.values() for f in t['families']},
                      key=lambda f: -sum(t['families'].get(f, 0) for t in titles.values()))
    rows = []
    for fam in families:
        row = [fam]
        for name in names:
            items = [i for i in titles[name]['instructions'] if family_of(i, titles[name]) == fam]
            total = sum(i['count'] for i in items)
            handled = sum(i['count'] for i in items if i['state'] == 'handled')
            row += [f'{handled:,} / {total:,}', bar(handled, total)]
        rows.append(row)
    total_row = ['Total']
    for name in names:
        total = sum(i['count'] for i in titles[name]['instructions'])
        handled = sum(i['count'] for i in titles[name]['instructions'] if i['state'] == 'handled')
        total_row += [f'{handled:,} / {total:,}', bar(handled, total)]
        summary['titles'][name].update({'instructions': total, 'instructions_handled': handled,
                                        'mnemonics': len(titles[name]['instructions'])})
    rows.append(total_row)
    instructions = table(['Family'] + [h for n in names for h in (f'{n}: handled / used', '%')], rows)

    # --- SDK-wide -----------------------------------------------------------------------------------------------
    sdk_rows = []
    for name in names:
        s = titles[name]['sdk']
        sdk_rows.append([f'ReXGlue {s["version"]} ({name})', f'{s["exports_implemented"]} / {s["exports_known"]}',
                         bar(s['exports_implemented'], s['exports_known']),
                         f'{s["instruction_ids_handled"]} / {s["instruction_ids"]}',
                         bar(s['instruction_ids_handled'], s['instruction_ids'])])
    sdk = table(['SDK', 'kernel exports implemented / known', '%', 'opcodes with a builder / known', '%'], sdk_rows)

    # --- game code ----------------------------------------------------------------------------------------------
    code = table(['Title', 'recompiled functions', 'replaced natively', '%'],
                 [[n, f'{titles[n]["functions"]["total"]:,}', titles[n]['functions']['hooked'],
                   bar(titles[n]['functions']['hooked'], titles[n]['functions']['total'])] for n in names])

    # --- the layer ----------------------------------------------------------------------------------------------
    comps = status.get('components', [])
    layer_rows = []
    for c in comps:
        color = STATE_COLORS.get(c['state'], '#6e7681')
        layer_rows.append([c['area'], c['name'], f'<span class="dot" style="background:{color}"></span>{esc(c["state"])}',
                           c.get('source', ''), c.get('note', '')])
    layer = table(['Area', 'Component', 'State', 'Comes from', 'Note'], layer_rows)
    done = sum(1 for c in comps if c['state'] == 'done')
    summary['layer'] = {'components': len(comps), 'done': done,
                        'in_progress': sum(1 for c in comps if c['state'] == 'in progress')}

    patch_rows = []
    for name, inv in patches.items():
        for area, count in inv['areas'].items():
            patch_rows.append([name, area, count])
        summary['layer'][f'patches_{name}'] = {'count': inv['count'], 'game_named': inv['game_named']}
    patch_table = table(['Patch set', 'SDK area', 'Patches'], patch_rows)

    # --- SVG summary --------------------------------------------------------------------------------------------
    svg = summary_svg(summary, names)
    (DOCS / 'progress.svg').write_text(svg, encoding='utf-8')
    (DOCS / 'progress.json').write_text(json.dumps(summary, indent=1) + '\n', encoding='utf-8')

    html = f'''<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Any360 progress</title>
<style>
:root {{ --bg:#0d1117; --panel:#161b22; --line:#30363d; --fg:#c9d1d9; --muted:#8b949e; --ok:#2da44e; }}
body {{ margin:0; padding:24px 16px 48px; background:var(--bg); color:var(--fg); font:14px/1.5 "Segoe UI",Helvetica,Arial,sans-serif; }}
main {{ max-width:1100px; margin:0 auto; }}
h1 {{ font-size:24px; margin:0 0 4px; }} h2 {{ font-size:18px; margin:36px 0 8px; border-bottom:1px solid var(--line); padding-bottom:4px; }}
p {{ color:var(--muted); max-width:80ch; }} a {{ color:#58a6ff; text-decoration:none; }} a:hover {{ text-decoration:underline; }}
table {{ border-collapse:collapse; width:100%; margin:8px 0 16px; background:var(--panel); font-size:13px; }}
th, td {{ text-align:left; padding:6px 10px; border-bottom:1px solid var(--line); vertical-align:top; }}
th {{ color:var(--muted); font-weight:600; }} tr:last-child td {{ border-bottom:none; }}
td.n {{ font-variant-numeric:tabular-nums; white-space:nowrap; }}
tbody tr:last-child td:first-child {{ font-weight:600; }}
.bar {{ display:inline-block; width:90px; height:8px; background:var(--line); border-radius:4px; vertical-align:middle; overflow:hidden; }}
.bar span {{ display:block; height:100%; }}
.dot {{ display:inline-block; width:9px; height:9px; border-radius:50%; margin-right:6px; }}
img {{ max-width:100%; background:var(--panel); border-radius:6px; }}
footer {{ margin-top:40px; color:var(--muted); font-size:12px; }}
code {{ background:var(--panel); padding:1px 4px; border-radius:3px; }}
</style>
</head>
<body><main>
<h1>Any360 progress</h1>
<p>What the <a href="https://github.com/rexglue/rexglue-sdk">ReXGlue</a> runtime and recompiler provide of what each Xbox 360
title needs, and how much of the native-port layer exists. Generated by <code>tools/progress_page.py</code> on
{summary["date"]} from <a href="coverage/">docs/coverage/</a>; raw numbers in <a href="progress.json">progress.json</a>.
Counts and public API names only: no game code or data.</p>
<img src="progress.svg" alt="Any360 progress summary" width="1100">

<h2>System calls</h2>
<p>Kernel and XAM functions each title's XEX imports, as the SDK runtime provides them (implemented or mapped variable,
against the total imported). Stubs return without doing the work; the per-title reports list them.</p>
{syscalls}

<h2>PowerPC instructions</h2>
<p>Static instruction count of each title's recompiled code by family, and the share the SDK recompiler translates
(a mnemonic without a builder becomes a <code>REX_UNIMPLEMENTED</code> trap).</p>
{instructions}

<h2>Game code replaced natively</h2>
<p>Recompiled functions of each title, and those its port replaces with a PC implementation (hooks). A native port
grows this number; everything else runs as recompiled guest code.</p>
{code}

<h2>The SDK itself</h2>
<p>Independent of any title: the kernel exports the runtime implements of those its export tables know, and the
opcodes the recompiler has a builder for of those its disassembler decodes (the rest are POWER, 64-bit server or
privileged forms no title uses).</p>
{sdk}

<h2>The layer</h2>
<p>Components of the Xbox 360 native-port layer, where each comes from and its state ({done} of {len(comps)} done).
The list is kept by hand in <code>layer/status.json</code>; the design is in <a href="DESIGN.md">docs/DESIGN.md</a>.</p>
{layer}
<p>SDK patch sets the layer is built from, by the SDK area they change (<code>tools/patch_inventory.py</code>).</p>
{patch_table}

<footer>Reports: {" · ".join(f'<a href="coverage/{n}.md">{n}</a>' for n in names)} ·
<a href="coverage/COMPARISON-{"-vs-".join(names)}.md">comparison</a> · counting rules in
<a href="DESIGN.md#counting-rules">DESIGN.md</a>.</footer>
</main></body></html>
'''
    (DOCS / 'progress.html').write_text(html, encoding='utf-8')
    parts = [f"{n}: {summary['titles'][n]['imports_provided']}/{summary['titles'][n]['imports']} imports, "
             f"{summary['titles'][n]['functions_native']}/{summary['titles'][n]['functions']} functions native" for n in names]
    print(f'progress page: {"; ".join(parts)}; layer {done}/{len(comps)}; wrote {DOCS / "progress.html"}')


def family_of(item, report):
    # The coverage JSON keeps families as totals; recompute per mnemonic with the same rule.
    return family(item['mnemonic'])


def summary_svg(summary, names):
    width, row_h = 1100, 26
    rows = []
    for n in names:
        t = summary['titles'][n]
        rows += [(f'{n}: system calls provided', t['imports_provided'], t['imports']),
                 (f'{n}: instructions translated', t['instructions_handled'], t['instructions']),
                 (f'{n}: functions replaced natively', t['functions_native'], t['functions'])]
    rows.append(('layer components done', summary['layer']['done'], summary['layer']['components']))
    height = 60 + row_h * len(rows)
    out = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
           'font-family="Segoe UI, Helvetica, Arial, sans-serif" font-size="12">',
           '<rect width="100%" height="100%" fill="#161b22"/>',
           f'<text x="20" y="28" fill="#c9d1d9" font-size="16" font-weight="600">Any360: what runs natively, '
           f'{summary["date"]}</text>']
    for i, (label, part, total) in enumerate(rows):
        y = 50 + i * row_h
        share = part / total if total else 0
        out.append(f'<text x="20" y="{y + 14}" fill="#8b949e">{esc(label)}</text>')
        out.append(f'<rect x="330" y="{y + 4}" width="600" height="14" rx="4" fill="#30363d"/>')
        out.append(f'<rect x="330" y="{y + 4}" width="{max(share * 600, 1 if part else 0):.1f}" height="14" rx="4" fill="#2da44e"/>')
        out.append(f'<text x="945" y="{y + 15}" fill="#c9d1d9">{part:,} / {total:,} ({share * 100:.1f}%)</text>')
    out.append('</svg>')
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    main()
