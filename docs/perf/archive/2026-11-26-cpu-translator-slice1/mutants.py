"""Mutation check of cpu_e2e_tests: changes one integer constant of the translated header at random, rebuilds, runs
the test, counts how many mutants it catches. Restores the header at the end."""
import random
import re
import shutil
import subprocess
import sys
from pathlib import Path

build = Path('F:/Crackdown/any360-sys/out/sys-step12')
header = build / 'cpu' / 'generated' / 'cpu_e2e_translated.h'
saved = header.with_suffix('.h.orig')
shutil.copy(header, saved)
text = saved.read_text(encoding='utf-8')
sites = [m for m in re.finditer(r'(?<=[ (,+-])(\d+)(?=u?[;),])', text)
         if not text[max(0, m.start() - 6):m.start()].endswith(('ctx.r', 'ctx.f', 'ctx.cr', 'loc_'))]
rng = random.Random(int(sys.argv[2]) if len(sys.argv) > 2 else 1)
n = int(sys.argv[1]) if len(sys.argv) > 1 else 20
caught = 0
vs = r'F:\VisualStudio\Common7\Tools\Launch-VsDevShell.ps1'
try:
    for k in range(n):
        m = rng.choice(sites)
        value = int(m.group(1))
        mutated = text[:m.start()] + str(value + 1) + text[m.end():]
        line = text.count('\n', 0, m.start()) + 1
        header.write_text(mutated, encoding='utf-8')
        r = subprocess.run(['powershell', '-NoProfile', '-Command',
                            f'& {{ . {vs} -Arch amd64 -SkipAutomaticLocation | Out-Null; ninja -C {build} cpu/cpu_e2e_tests.exe }}'],
                           capture_output=True, text=True)
        if r.returncode:
            print(f'mutant {k} line {line}: build failed')
            continue
        t = subprocess.run([str(build / 'cpu' / 'cpu_e2e_tests.exe')], capture_output=True, text=True, timeout=600)
        hit = t.returncode != 0
        caught += hit
        print(f'mutant {k} line {line}: {value} -> {value + 1}: {"caught" if hit else "MISSED"} | {text.splitlines()[line - 1].strip()}',
              flush=True)
finally:
    shutil.copy(saved, header)
    saved.unlink()
    header.touch()
    subprocess.run(['powershell', '-NoProfile', '-Command',
                    f'& {{ . {vs} -Arch amd64 -SkipAutomaticLocation | Out-Null; ninja -C {build} cpu/cpu_e2e_tests.exe }}'],
                   capture_output=True)
print(f'mutants: {caught} of {n} caught')
