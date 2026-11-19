"""Where a title's processor time goes, function by function: the table of the functions worth making native first.

Joins the function profiler's reports (any360/host/profiler.h: profile.csv, one or more runs of the same route,
averaged), the call counts of the same stretch (two snapshots `address count` a known number of seconds apart) and
what the title's progress reports know of each function (its module, kind, size, state: native, verified, library,
recompiled; tools/title_progress.py's functions.csv) and the names Any360 has for it (titles/<title>/d3d_names.json,
db/xdk_seed.json). Writes titles/<title>/hot_functions.csv and HOT_FUNCTIONS.md:
  - by total time (a function's own time a second of play: the order to work in),
  - by time a call (the individually expensive ones among those called at least 100 times a second),
  - the host code title functions call (bindings' handlers, natives, kernel exports) and the host functions the
    time ends in (memcpy, the driver).
The `note` column of an existing hot_functions.csv is kept, by address: what a function is, what was done about it
and where its numbers are, so the table is completed step by step and regenerated with each new profile.

  python tools/hot_functions.py crackdown --profile run1.csv run2.csv --calls a.txt b.txt --seconds 80
      --functions <title repo>/docs/progress/functions.csv [--route "the route"] [--date YYYY-MM-DD]
"""
import argparse
import csv
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def number(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def read_profiles(paths):
    own, total, self_, host, called = {}, {}, {}, {}, {}
    for p in paths:
        for row in csv.DictReader(open(p, encoding='utf-8', errors='replace')):
            ms = number(row.get('own_ms'))
            if ms is None:
                continue  # a line cut short (profiler reports before 2026-11-19's fix of long host symbols)
            kind = row['kind']
            if kind == 'title':
                a = row['address'].upper()
                own[a] = own.get(a, 0.0) + ms
                total[a] = total.get(a, 0.0) + (number(row.get('total_ms')) or 0.0)
                self_[a] = self_.get(a, 0.0) + (number(row.get('self_ms')) or 0.0)
            elif kind == 'called':
                key = (row['address'].upper(), row['symbol'])
                called[key] = called.get(key, 0.0) + ms
            else:
                host[row['symbol']] = host.get(row['symbol'], 0.0) + ms
    n = float(len(paths))
    return tuple({k: v / n for k, v in d.items()} for d in (own, total, self_, host, called))


def read_counts(path):
    counts = {}
    if path and Path(path).exists():
        for line in open(path, encoding='utf-8'):
            parts = line.split()
            if len(parts) == 2:
                counts[parts[0].upper().rjust(8, '0')] = int(parts[1])
    return counts


def short(symbol, width=60):
    s = symbol.replace('|', '\\|')
    return s if len(s) <= width else s[:width - 3] + '...'


def main():
    p = argparse.ArgumentParser()
    p.add_argument('title')
    p.add_argument('--profile', nargs='+', required=True)
    p.add_argument('--profile-seconds', type=float, default=80.0, help='the stretch each profile sampled')
    p.add_argument('--calls', nargs=2, metavar=('BEFORE', 'AFTER'))
    p.add_argument('--seconds', type=float, default=80.0, help='the seconds between the two call snapshots')
    p.add_argument('--functions', help="the title's functions.csv (tools/title_progress.py)")
    p.add_argument('--route', default='the city route (any360-ab.ps1 input script), 80 s of steady play')
    p.add_argument('--date', default='')
    p.add_argument('--top', type=int, default=100)
    args = p.parse_args()
    out_dir = ROOT / 'titles' / args.title
    own, total, self_, host, called = read_profiles(args.profile)
    before, after = (read_counts(args.calls[0]), read_counts(args.calls[1])) if args.calls else ({}, {})
    calls = {a: (after[a] - before.get(a, 0)) / args.seconds for a in after}
    info = {}
    if args.functions:
        for row in csv.DictReader(open(args.functions, encoding='utf-8')):
            info[row['address'].upper()] = row
    names = {}
    names_path = out_dir / 'd3d_names.json'
    if names_path.exists():
        for k, v in json.load(open(names_path, encoding='utf-8')).items():
            if k.startswith('0x'):
                names[k[2:].upper()] = v[0]
    for entry in json.load(open(ROOT / 'db' / 'xdk_seed.json', encoding='utf-8')).get('functions', []):
        a = (entry.get('titles') or {}).get(args.title)
        if a:
            names.setdefault(a[2:].upper(), entry['name'])
    notes = {}
    csv_path = out_dir / 'hot_functions.csv'
    if csv_path.exists():
        for row in csv.DictReader(open(csv_path, encoding='utf-8')):
            if row.get('note'):
                notes[row['address']] = row['note']
    per_s = args.profile_seconds
    # The host code each title function calls the most.
    top_called = {}
    for (caller, symbol), ms in called.items():
        if ms > top_called.get(caller, ('', 0.0))[1]:
            top_called[caller] = (symbol, ms)
    title_ms = sum(own.values())
    rows = []
    for a, ms in own.items():
        f = info.get(a, {})
        c = calls.get(a)
        own_s = ms / per_s
        tc = top_called.get(a)
        rows.append({
            'address': a, 'name': names.get(a, ''), 'module': f.get('module', ''), 'kind': f.get('kind', ''),
            'instructions': f.get('instructions', ''), 'state': f.get('state', ''),
            'own_ms_per_s': own_s, 'self_ms_per_s': self_.get(a, 0.0) / per_s,
            'share': 100.0 * ms / title_ms if title_ms else 0.0, 'total_ms_per_s': total.get(a, 0.0) / per_s,
            'calls_per_s': c, 'ns_per_call': (own_s * 1e6 / c) if c else None,
            'calls_host': tc[0] if tc else '', 'calls_host_ms_per_s': tc[1] / per_s if tc else 0.0,
            'note': notes.get(a, ''),
        })
    rows.sort(key=lambda r: -r['own_ms_per_s'])
    for i, r in enumerate(rows):
        r['rank'] = i + 1
    with open(csv_path, 'w', encoding='utf-8', newline='') as f:
        w = csv.writer(f)
        w.writerow(['rank', 'address', 'name', 'module', 'kind', 'instructions', 'state', 'own_ms_per_s', 'self_ms_per_s',
                    'share_pct', 'total_ms_per_s', 'calls_per_s', 'ns_per_call', 'calls_host', 'calls_host_ms_per_s', 'note'])
        for r in rows:
            w.writerow([r['rank'], r['address'], r['name'], r['module'], r['kind'], r['instructions'], r['state'],
                        '%.3f' % r['own_ms_per_s'], '%.3f' % r['self_ms_per_s'], '%.3f' % r['share'],
                        '%.3f' % r['total_ms_per_s'], '' if r['calls_per_s'] is None else '%.1f' % r['calls_per_s'],
                        '' if r['ns_per_call'] is None else '%.1f' % r['ns_per_call'], r['calls_host'],
                        '%.3f' % r['calls_host_ms_per_s'], r['note']])

    def fmt_calls(v):
        if v is None:
            return '-'
        return '%.0f' % v if v < 10000 else '%.0f K' % (v / 1000) if v < 1e6 else '%.2f M' % (v / 1e6)

    def fmt_ns(v):
        if v is None:
            return '-'
        return '%.1f' % v if v < 100 else '%.0f' % v if v < 1e6 else '%.1f ms' % (v / 1e6)

    def what(r):
        return '%s (`%s`)' % (r['name'], r['address']) if r['name'] else '`sub_%s`' % r['address']

    def where(r):
        return r['module'].replace('code ', '') if r['module'] else ''

    def host_part(r):
        if not r['calls_host'] or r['calls_host_ms_per_s'] < 0.05:
            return ''
        return '`%s` %.1f' % (short(r['calls_host'], 40), r['calls_host_ms_per_s'])

    host_rows = sorted(host.items(), key=lambda kv: -kv[1])
    host_ms = sum(v for _, v in host_rows) / per_s
    called_rows = {}
    for (caller, symbol), ms in called.items():
        e = called_rows.setdefault(symbol, [0.0, {}])
        e[0] += ms
        e[1][caller] = e[1].get(caller, 0.0) + ms
    lines = [
        '# %s: where the processor time goes' % args.title.capitalize(),
        '',
        'Generated by `tools/hot_functions.py` from the function profiler (`--any360_profile_seconds`, '
        '`any360/host/profiler.h`: every 2 ms each running thread\'s stack) and a call count of the same stretch. %s%s; '
        '%d profile run(s) averaged.' % (args.route, (', ' + args.date) if args.date else '', len(args.profile)),
        '',
        'Times are milliseconds of one core a second of play (1000 = a whole core). A function\'s **own** time is the '
        'time it is the innermost title function: its own code (**self**) plus the host code it calls (bindings\' '
        'handlers, Any360\'s natives, kernel exports: the biggest one is named). A binding that only forwards leaves no '
        'frame of its own, so a bound function\'s time shows under its caller, in that column. **ns/call** is the own '
        'time over the calls counted. Making a function native works on its self time; the first table is the order '
        'to work in, the second shows the individually expensive ones. The `note` column is kept from one '
        'regeneration to the next: what a function is, what was done, where its numbers are.',
        '',
        'Processor time caught running: %.0f ms a second (%.2f cores), %.0f of it under title functions.'
        % (host_ms, host_ms / 1000, title_ms / per_s),
        '',
        '## By total time',
        '',
        '| # | function | where | state | instr. | own ms/s | self ms/s | host code it calls, ms/s | calls/s | ns/call | note |',
        '|---:|---|---|---|---:|---:|---:|---|---:|---:|---|',
    ]
    for r in rows[:args.top]:
        lines.append('| %d | %s | %s | %s | %s | %.2f | %.2f | %s | %s | %s | %s |' % (
            r['rank'], what(r), where(r), r['state'], r['instructions'], r['own_ms_per_s'], r['self_ms_per_s'],
            host_part(r), fmt_calls(r['calls_per_s']), fmt_ns(r['ns_per_call']), r['note']))
    expensive = [r for r in rows if r['calls_per_s'] and r['calls_per_s'] >= 100 and r['ns_per_call']]
    expensive.sort(key=lambda r: -r['ns_per_call'])
    lines += ['', '## By time a call (called 100 times a second or more)', '',
              '| function | where | state | instr. | ns/call | calls/s | own ms/s | self ms/s | note |',
              '|---|---|---|---:|---:|---:|---:|---:|---|']
    for r in expensive[:40]:
        lines.append('| %s | %s | %s | %s | %s | %s | %.2f | %.2f | %s |' % (
            what(r), where(r), r['state'], r['instructions'], fmt_ns(r['ns_per_call']), fmt_calls(r['calls_per_s']),
            r['own_ms_per_s'], r['self_ms_per_s'], r['note']))
    lines += ['', '## Host code title functions call (bindings\' handlers, natives, kernel exports)', '',
              '| host function | ms/s | mostly under |', '|---|---:|---|']
    for symbol, (ms, callers) in sorted(called_rows.items(), key=lambda kv: -kv[1][0])[:30]:
        caller = max(callers.items(), key=lambda kv: kv[1])[0]
        lines.append('| `%s` | %.1f | %s |' % (short(symbol, 90), ms / per_s, names.get(caller, '`sub_%s`' % caller)))
    lines += ['', '## Where the time ends (the innermost host function; waits in the kernel left out)', '',
              '| host function | ms/s | share |', '|---|---:|---:|']
    for name, ms in host_rows[:25]:
        lines.append('| `%s` | %.1f | %.1f%% |' % (short(name, 90), ms / per_s, 100.0 * ms / per_s / host_ms if host_ms else 0))
    lines.append('')
    (out_dir / 'HOT_FUNCTIONS.md').write_text('\n'.join(lines), encoding='utf-8')
    print('%s: %d title functions, %.0f ms/s of %.0f ms/s under title functions; wrote %s and %s'
          % (args.title, len(rows), title_ms / per_s, host_ms, csv_path, out_dir / 'HOT_FUNCTIONS.md'))


if __name__ == '__main__':
    main()
