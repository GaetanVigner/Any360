#!/usr/bin/env python3
"""A/B of a title's launches from their Any360 logs: the periods without diagnostics in play (after a period of at
least --draws draws a frame; the first such period skipped as warm-up), each with its frame rate, median frame time
and 10% / 1% lows (the frame rates of its 90th and 99th percentile frame times: the stutter averages hide). Per
variant: the periods' medians, means and minimums.

  python tools/frame_lows.py <runs> <logs dir> [--draws 1500]

<runs>: lines "=== <variant arguments> (<log file name>)", one a launch (a launch script's output).
"""
import re
import statistics
import sys
from pathlib import Path

RUN = re.compile(r'^﻿?=== (?P<variant>.+?) \((?P<log>[^()]+\.log)\)')
ON = re.compile(r'Any360: [\d.]+ FPS; per frame: (?P<draws>\d+) draws')
OFF = re.compile(r'Any360: (?P<fps>[\d.]+) FPS with the diagnostics off .*?frame times: median (?P<median>[\d.]+) ms '
                 r'\([\d.]+ FPS\), 10% low (?P<p10>[\d.]+) FPS, 1% low (?P<p1>[\d.]+) FPS')


def periods(log, draws):
    out, city, seen = [], False, 0
    for line in log.read_text(encoding='utf-8', errors='replace').splitlines():
        m = ON.search(line)
        if m:
            city = int(m['draws']) >= draws
            continue
        m = OFF.search(line)
        if m and city:
            seen += 1
            if seen > 1:
                out.append({k: float(m[k]) for k in ('fps', 'median', 'p10', 'p1')})
    return out


def main():
    import argparse
    p = argparse.ArgumentParser()
    p.add_argument('runs')
    p.add_argument('logs')
    p.add_argument('--draws', type=int, default=1500)
    a = p.parse_args()
    ab, logs = Path(a.runs), Path(a.logs)
    by = {}
    for line in ab.read_text(encoding='utf-8-sig', errors='replace').splitlines():
        m = RUN.match(line)
        if m:
            variant = re.sub(r' --any360_input_script=\S+', '', m['variant'])
            ps = periods(logs / m['log'], a.draws)
            by.setdefault(variant, []).append((m['log'], ps))
    for variant, runs in by.items():
        allp = [p for _, ps in runs for p in ps]
        print(f'{variant}: {len(runs)} launches, {len(allp)} periods in play')
        for log, ps in runs:
            if ps:
                print(f'  {log}: FPS {min(p["fps"] for p in ps):.0f}-{max(p["fps"] for p in ps):.0f}, '
                      f'1% low {min(p["p1"] for p in ps):.0f}-{max(p["p1"] for p in ps):.0f}')
            else:
                print(f'  {log}: no period in play')
        if not allp:
            continue
        for key, name in (('fps', 'average FPS'), ('p10', '10% low'), ('p1', '1% low')):
            vals = [p[key] for p in allp]
            print(f'  {name}: median {statistics.median(vals):.1f}, mean {statistics.mean(vals):.1f}, '
                  f'min {min(vals):.1f}')
        meds = [1e3 / p['median'] for p in allp]
        print(f'  median frame rate: median {statistics.median(meds):.1f}, mean {statistics.mean(meds):.1f}')


if __name__ == '__main__':
    main()
