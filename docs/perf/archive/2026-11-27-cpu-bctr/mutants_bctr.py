"""Mutation check of cpu_e2e_tests on bctr: in the translated e2e header, one change a mutant among
- two cases of a switch (different labels) swapped,
- the switch's index register changed to the next one,
- a tail call through ctr given ctr + 4,
then rebuilt and run (cores 2-3), counting the mutants the test catches. Restores the header at the end.

  python -I mutants_bctr.py <build folder> <count> <seed> <build script> <reach output>

Switches and swapped cases are drawn among those the e2e states reach (the reach output)."""
import random
import re
import shutil
import subprocess
import sys
from pathlib import Path

build = Path(sys.argv[1])
n = int(sys.argv[2])
rng = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
header = build / 'cpu' / 'generated' / 'cpu_e2e_translated.h'
saved = header.with_suffix('.h.orig')
shutil.copy(header, saved)
lines = saved.read_text(encoding='utf-8').split('\n')
vs = r'F:\VisualStudio\Common7\Tools\Launch-VsDevShell.ps1'


def run(command):
    return subprocess.run(['cmd', '/c', 'start', '/affinity', 'C', '/wait', '/b'] + command, capture_output=True,
                          text=True, timeout=3600)


def build_it():
    return run(['powershell', '-NoProfile', '-File', sys.argv[4], 'cpu_e2e_tests']).returncode


reached = {}
for line in open(sys.argv[5]):
    if line.startswith('reach switch') and ' mask ' in line:
        mask = int(line.split(' mask ')[1].split()[0], 16)
        if mask:
            reached[int(line.split(' line ')[1]) - 1] = mask


switches = sorted(reached)
tails = [i for i, l in enumerate(lines) if l.strip() == 'ANY360_CPU_CALL_INDIRECT(ctx, base, ctx.ctr.u32);'
         and lines[i + 1].strip() == 'return;' and not lines[i - 1].strip().startswith('ctx.lr =')]
caught = 0
if build_it() or 'all match the reference' not in run([str(build / 'cpu' / 'cpu_e2e_tests.exe')]).stdout:
    sys.exit('mutants: the unmutated test does not report all match')
try:
    for k in range(n):
        mutated = list(lines)
        kind = ('swap', 'register', 'tail')[k % 3]
        if kind == 'tail':
            i = rng.choice(tails)
            mutated[i] = mutated[i].replace('ctx.ctr.u32', 'ctx.ctr.u32 + 4')
        else:
            i = rng.choice(switches)
            if kind == 'register':
                r = int(re.search(r'ctx\.r(\d+)', lines[i]).group(1))
                mutated[i] = mutated[i].replace(f'ctx.r{r}.', f'ctx.r{(r + 1) % 32}.')
            else:
                cases = []
                j = i + 1
                while not lines[j].strip().startswith('default'):
                    cases.append(j)
                    j += 1
                target = lambda c: lines[c].split('goto ')[1]
                hit = [c for n, c in enumerate(cases) if n < 64 and reached[i] >> n & 1]
                pairs = [(a, b) for a in hit for b in cases if a != b and target(a) != target(b)]
                if not pairs:
                    continue
                a, b = rng.choice(pairs)
                mutated[a] = lines[a].replace(target(a), target(b))
                mutated[b] = lines[b].replace(target(b), target(a))
        func = next(l for l in reversed(lines[:i]) if l.startswith('inline void sub_') and l.endswith('{'))
        header.write_text('\n'.join(mutated), encoding='utf-8')
        if build_it():
            print(f'mutant {k} {kind} line {i + 1}: build failed', flush=True)
            continue
        t = run([str(build / 'cpu' / 'cpu_e2e_tests.exe')])
        hit = 'all match the reference' not in t.stdout
        caught += hit
        print(f'mutant {k} {kind} line {i + 1}: {"caught" if hit else "MISSED"} | {func[12:24]} | {mutated[i].strip()[:70]}',
              flush=True)
finally:
    shutil.copy(saved, header)
    saved.unlink()
    header.touch()
    build_it()
print(f'mutants: {caught} of {n} caught')
