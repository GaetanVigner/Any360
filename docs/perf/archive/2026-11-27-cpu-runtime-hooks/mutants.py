"""Mutation check of cpu_e2e_tests: changes one integer constant of the translated header at random, rebuilds, runs
the test, counts how many mutants it catches. Restores the header at the end. With --hooks, only constants on the
lines that call the runtime's hooks (import ordinals, trap conditions and types, the time base's shift).

  python mutants.py <build folder> <count> <seed> [--hooks]   (run under start /affinity: ninja -j 2)"""
import random
import re
import shutil
import subprocess
import sys
from pathlib import Path

build = Path(sys.argv[1])
n = int(sys.argv[2])
rng = random.Random(int(sys.argv[3]))
hooks = '--hooks' in sys.argv
header = build / 'cpu' / 'generated' / 'cpu_e2e_translated.h'
saved = header.with_suffix('.h.orig')
shutil.copy(header, saved)
text = saved.read_text(encoding='utf-8')
if hooks:
    lines = text.split('\n')
    starts = [0]
    for l in lines:
        starts.append(starts[-1] + len(l) + 1)
    sites = [(starts[i] + m.start(), starts[i] + m.end(), m.group(1)) for i, l in enumerate(lines)
             if re.search(r'ANY360_CPU_(TRAP|CALL_IMPORT|QUERY_TIMEBASE|\w+_GLOBAL_LOCK)', l)
             for m in re.finditer(r'(?<=[ (,+-])(\d+)(?=(?:u|ll|ull)?[;), ])', l)]
else:
    sites = [(m.start(), m.end(), m.group(1)) for m in re.finditer(r'(?<=[ (,+-])(\d+)(?=u?[;),])', text)
             if not text[max(0, m.start() - 6):m.start()].endswith(('ctx.r', 'ctx.f', 'ctx.cr', 'loc_'))]
print(f'{len(sites)} sites', flush=True)
vs = r'F:\VisualStudio\Common7\Tools\Launch-VsDevShell.ps1'


def build_it():
    return subprocess.run(['powershell', '-NoProfile', '-Command',
                           f'& {{ . {vs} -Arch amd64 -SkipAutomaticLocation | Out-Null; ninja -j 2 -C {build} cpu/cpu_e2e_tests.exe }}'],
                          capture_output=True, text=True).returncode


caught = 0
picked = rng.sample(sites, min(n, len(sites)))
try:
    for k, (start, end, digits) in enumerate(picked):
        value = int(digits)
        mutated = text[:start] + str(value + 1) + text[end:]
        line = text.count('\n', 0, start) + 1
        header.write_text(mutated, encoding='utf-8')
        if build_it():
            print(f'mutant {k} line {line}: build failed')
            continue
        t = subprocess.run([str(build / 'cpu' / 'cpu_e2e_tests.exe')], capture_output=True, text=True, timeout=900)
        hit = t.returncode != 0
        caught += hit
        print(f'mutant {k} line {line}: {value} -> {value + 1}: {"caught" if hit else "MISSED"} | {text.splitlines()[line - 1].strip()}',
              flush=True)
finally:
    shutil.copy(saved, header)
    saved.unlink()
    header.touch()
    build_it()
print(f'mutants: {caught} of {len(picked)} caught')
