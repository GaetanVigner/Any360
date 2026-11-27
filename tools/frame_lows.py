#!/usr/bin/env python3
"""A/B of a title's launches from their Any360 logs. In each launch, the periods without diagnostics in play (after a
period of at least --draws draws a frame; the first such period skipped as warm-up), each with its frame rate, median
frame time, 10% / 1% lows (the frame rates of its 90th and 99th percentile frame times: the stutter averages hide),
slowest frame, frames over 16.7 ms and the process's private memory. Each launch's period with the lowest 1% low is its dip (a one-off stall:
streaming, a load); the others are its steady periods. Per variant: the steady periods' medians, means and minimums,
and each dip with the texture loading time of the launch's worst diagnosed period; then each variant against the
first by a rank test (z: past 2 or so, a difference beyond the launches' spread).

  python tools/frame_lows.py <runs> <logs dir> [--draws 1500] [--after SECONDS]

The runtime's own GPU (a title launched without any360_gpu, its VdSwap calls counted) gives the same figures in its
"the runtime's GPU: N FPS (VdSwap calls); frame times: ..." lines, every period: those count as periods without
diagnostics. They carry no draw count, so such an A/B selects the periods by time instead: --after SECONDS keeps the
periods logged that long after the log's first line (the same input script brings both variants to the route), and
replaces the draw count's test for every variant.

Both lines end with the process's usage over the period (any360/host/usage.h): "CPU 245% of a core, 31% of the 8
allowed (mask 0xFF); GPU 62% (system), 60% (batches)". Per variant, the steady periods' CPU% (of a core and of the processors
allowed), GPU% (the system's count of the busiest engine, the batches' timestamps where the system gives none) and
the efficiency, frames per second per CPU% and per GPU% (each period's rate over its usage), with rank tests too.

<runs>: lines "=== <variant arguments> (<log file name>)", one a launch (a launch script's output).
"""
import argparse
import re
import statistics
from datetime import datetime
from pathlib import Path

RUN = re.compile(r'^﻿?=== (?P<variant>.+?) \((?P<log>[^()]+\.log)\)')
ON = re.compile(r'Any360: [\d.]+ FPS; per frame: (?P<draws>\d+) draws')
OFF = re.compile(r'Any360: (?P<fps>[\d.]+) FPS with the diagnostics off .*?frame times: median (?P<median>[\d.]+) ms '
                 r'\([\d.]+ FPS\), 10% low (?P<p10>[\d.]+) FPS, 1% low (?P<p1>[\d.]+) FPS.*?slowest frame '
                 r'(?P<slowest>[\d.]+) ms, (?P<over>\d+) over 16.7 ms.*?(?P<private>\d+) MB private')
RUNTIME = re.compile(r"Any360: the runtime's GPU: (?P<fps>[\d.]+) FPS \(VdSwap calls\); frame times: median "
                     r'(?P<median>[\d.]+) ms \([\d.]+ FPS\), 10% low (?P<p10>[\d.]+) FPS, 1% low (?P<p1>[\d.]+) FPS; '
                     r'slowest frame (?P<slowest>[\d.]+) ms, (?P<over>\d+) over 16.7 ms; (?P<private>\d+) MB private')
USAGE = re.compile(r'CPU (?P<cpu>[\d.]+)% of a core(?:, (?P<cpu_allowed>[\d.]+)% of the (?P<cores>\d+) allowed)?'
                   r'(?: \(mask 0x(?P<mask>[0-9A-Fa-f]+)(?:, set again (?P<resets>\d+) times?)?\))?'
                   r'(?:; GPU (?:(?P<gpu>[\d.]+)% \(system\))?(?:, )?(?:(?P<gpu_own>[\d.]+)% \(batches\))?)?')
STAMP = re.compile(r'^\[(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d(?:\.\d+)?)\]')
LOADING = re.compile(r'textures ms: hashing [\d.]+, loading for uploads (?P<loading>[\d.]+)')


def stamp(line):
    m = STAMP.match(line)
    return datetime.strptime(m[1], '%Y-%m-%d %H:%M:%S.%f' if '.' in m[1] else '%Y-%m-%d %H:%M:%S') if m else None


def period(m, line):
    """A period's figures from its frame line: the frame times, and the usage where the line gives it (else None)."""
    q = {k: float(m[k]) for k in ('fps', 'median', 'p10', 'p1', 'slowest', 'over', 'private')}
    u = USAGE.search(line, m.end())
    for k in ('cpu', 'cpu_allowed', 'cores', 'gpu', 'gpu_own', 'resets'):
        q[k] = float(u[k]) if u and u[k] is not None else None
    q['mask'] = int(u['mask'], 16) if u and u['mask'] else None
    if q['gpu'] is None:
        q['gpu'] = q['gpu_own']
    q['fps_per_cpu'] = q['fps'] / q['cpu'] if q['cpu'] else None
    q['fps_per_gpu'] = q['fps'] / q['gpu'] if q['gpu'] else None
    return q


def launch(log, draws, after=None):
    periods, loading, city, seen, start = [], [], False, 0, None
    for line in log.read_text(encoding='utf-8', errors='replace').splitlines():
        if after is not None:
            t = stamp(line)
            if t is not None:
                start = start or t
                city = (t - start).total_seconds() >= after
            m = OFF.search(line) or RUNTIME.search(line)
            if m and city:
                periods.append(period(m, line))
            continue
        m = ON.search(line)
        if m:
            city = int(m['draws']) >= draws
            continue
        m = LOADING.search(line)
        if m and city:
            loading.append(float(m['loading']))
            continue
        m = OFF.search(line)
        if m and city:
            seen += 1
            if seen > 1:
                periods.append(period(m, line))
    return periods, loading


def z_score(a, b):
    """Mann-Whitney U as a z score (normal approximation): positive when a's values rank above b's."""
    values = sorted([(v, 0) for v in a] + [(v, 1) for v in b])
    ranks, i = [0.0] * len(values), 0
    while i < len(values):
        j = i
        while j + 1 < len(values) and values[j + 1][0] == values[i][0]:
            j += 1
        for k in range(i, j + 1):
            ranks[k] = (i + j) / 2 + 1
        i = j + 1
    n1, n2 = len(a), len(b)
    if not n1 or not n2:
        return 0.0
    u = sum(r for r, (_, g) in zip(ranks, values) if g == 0) - n1 * (n1 + 1) / 2
    return (u - n1 * n2 / 2) / (n1 * n2 * (n1 + n2 + 1) / 12) ** 0.5


USAGE_FIGURES = (('cpu', 'CPU% of a core', 0), ('cpu_allowed', 'CPU% of the allowed', 0), ('gpu', 'GPU%', 0),
                 ('fps_per_cpu', 'FPS per CPU%', 2), ('fps_per_gpu', 'FPS per GPU%', 2))


def usage_text(periods):
    """The periods' usage and efficiency: each figure's mean and median over the periods that give it."""
    parts = []
    for k, name, digits in USAGE_FIGURES:
        values = [q[k] for q in periods if q.get(k) is not None]
        if values:
            parts.append(f'{name} mean {statistics.mean(values):.{digits}f} (median {statistics.median(values):.{digits}f})')
    cores = {int(q['cores']) for q in periods if q.get('cores')}
    masks = {q['mask'] for q in periods if q.get('mask')}
    if parts and cores:
        parts[0] += (f' [{"/".join(str(c) for c in sorted(cores))} processors allowed' +
                     (f', mask {"/".join(f"0x{m:X}" for m in sorted(masks))}' if masks else '') + ']')
    resets = sum(int(q['resets']) for q in periods if q.get('resets'))
    if resets:
        parts.append(f'mask set again {resets} time' + ('s' if resets != 1 else ''))
    return '; '.join(parts)


def main():
    p = argparse.ArgumentParser()
    p.add_argument('runs')
    p.add_argument('logs')
    p.add_argument('--draws', type=int, default=1500)
    p.add_argument('--after', type=float, help='periods from this many seconds after the log starts (no draw test)')
    a = p.parse_args()
    by = {}
    for line in Path(a.runs).read_text(encoding='utf-8-sig', errors='replace').splitlines():
        m = RUN.match(line)
        if m:
            variant = re.sub(r' --any360_input_script=\S+', '', m['variant']).strip()
            by.setdefault(variant, []).append((m['log'],) + launch(Path(a.logs) / m['log'], a.draws, a.after))
    summary = {}
    for variant, runs in by.items():
        steady, dips = [], []
        print(f'{variant}: {len(runs)} launches')
        for log, periods, loading in runs:
            if not periods:
                print(f'  {log}: no period in play')
                continue
            dip = min(periods, key=lambda q: q['p1'])
            dips.append(dict(dip, loading=max(loading) if loading else 0.0))
            steady += [q for q in periods if q is not dip]
            print(f'  {log}: dip 1% low {dip["p1"]:.0f} FPS, slowest {dip["slowest"]:.1f} ms, {dip["over"]:.0f} over '
                  f'16.7 ms, texture loading up to {dips[-1]["loading"]:.2f} ms a frame; steady FPS '
                  f'{min((q["fps"] for q in periods if q is not dip), default=0):.0f}-'
                  f'{max((q["fps"] for q in periods if q is not dip), default=0):.0f}')
        summary[variant] = (steady, dips)
        if steady:
            print(f'  steady ({len(steady)} periods): ' + ', '.join(
                f'{name} mean {statistics.mean(q[k] for q in steady):.1f} (median {statistics.median(q[k] for q in steady):.1f}, '
                f'min {min(q[k] for q in steady):.1f})' for k, name in (('fps', 'FPS'), ('p10', '10% low'), ('p1', '1% low'))) +
                  f'; process private memory mean {statistics.mean(q["private"] for q in steady):.0f} MB, max '
                  f'{max(q["private"] for q in steady):.0f} MB')
            usage = usage_text(steady)
            if usage:
                print(f'  usage: {usage}')
        if dips:
            print(f'  dips ({len(dips)}): 1% low mean {statistics.mean(d["p1"] for d in dips):.1f}, slowest frame mean '
                  f'{statistics.mean(d["slowest"] for d in dips):.1f} ms, over 16.7 ms mean '
                  f'{statistics.mean(d["over"] for d in dips):.1f}, texture loading mean '
                  f'{statistics.mean(d["loading"] for d in dips):.2f} ms a frame')
    names = list(summary)
    for other in names[1:]:
        (s0, d0), (s1, d1) = summary[names[0]], summary[other]
        print(f'{names[0]} against {other}: steady ' +
              ', '.join(f'{k} z {z_score([q[k] for q in s0], [q[k] for q in s1]):+.2f}' for k in ('fps', 'p10', 'p1')) +
              f'; dips 1% low z {z_score([d["p1"] for d in d0], [d["p1"] for d in d1]):+.2f}, slowest z '
              f'{z_score([d["slowest"] for d in d0], [d["slowest"] for d in d1]):+.2f}')
        effs = [(k, [q[k] for q in s0 if q.get(k) is not None], [q[k] for q in s1 if q.get(k) is not None])
                for k in ('cpu', 'gpu', 'fps_per_cpu', 'fps_per_gpu')]
        effs = [f'{k} z {z_score(a, b):+.2f}' for k, a, b in effs if a and b]
        if effs:
            print(f'{names[0]} against {other}: usage ' + ', '.join(effs))


if __name__ == '__main__':
    main()
