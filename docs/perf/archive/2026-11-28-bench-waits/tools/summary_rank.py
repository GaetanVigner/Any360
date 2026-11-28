"""Rank test (Mann-Whitney z) on the bench's end-of-run lines (summary.txt from ab.ps1): FPS, median ms, 10%/1% lows.
usage: python summary_rank.py <summary.txt> [first variant]"""
import re, sys, statistics, math, collections
LINE = re.compile(r'^(\S+) (\d+) .*?any360_bench: ([\d.]+) FPS, median ([\d.]+) ms, 10% low ([\d.]+) FPS, 1% low ([\d.]+) FPS, '
                  r'slowest ([\d.]+) ms, (\d+) over')
runs = collections.defaultdict(list)
for line in open(sys.argv[1], encoding='utf-8-sig'):
    m = LINE.match(line)
    if m:
        runs[m[1]].append(dict(fps=float(m[3]), median=float(m[4]), p10=float(m[5]), p1=float(m[6]), slowest=float(m[7]),
                               over=int(m[8])))
names = list(runs)
first = sys.argv[2] if len(sys.argv) > 2 else names[0]
keys = ['fps', 'median', 'p10', 'p1', 'slowest', 'over']
def z(a, b):
    u = sum((x > y) + 0.5 * (x == y) for x in a for y in b)
    n, m = len(a), len(b)
    return (u - n * m / 2) / math.sqrt(n * m * (n + m + 1) / 12)
for name in names:
    r = runs[name]
    print(f"{name}: {len(r)} runs; " + ", ".join(f"{k} median {statistics.median(x[k] for x in r):.1f} (min {min(x[k] for x in r):.1f}, max {max(x[k] for x in r):.1f})" for k in keys))
for name in names:
    if name == first: continue
    print(f"{first} against {name}: " + ", ".join(f"{k} z {z([x[k] for x in runs[first]], [x[k] for x in runs[name]]):+.2f}" for k in keys))
