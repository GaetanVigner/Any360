"""python mutate_lines.py <build folder> <line>:<old>:<new>...: for each, replaces old by new on that line of the
translated e2e header, rebuilds, runs cpu_e2e_tests: caught or missed. Restores the header at the end."""
import shutil
import subprocess
import sys
from pathlib import Path

build = Path(sys.argv[1])
header = build / 'cpu' / 'generated' / 'cpu_e2e_translated.h'
saved = header.with_suffix('.h.orig')
shutil.copy(header, saved)
lines = saved.read_text(encoding='utf-8').split('\n')
vs = r'F:\VisualStudio\Common7\Tools\Launch-VsDevShell.ps1'


def build_it():
    return subprocess.run(['powershell', '-NoProfile', '-Command',
                           f'& {{ . {vs} -Arch amd64 -SkipAutomaticLocation | Out-Null; ninja -j 2 -C {build} cpu/cpu_e2e_tests.exe }}'],
                          capture_output=True, text=True).returncode


try:
    for spec in sys.argv[2:]:
        n, old, new = spec.split(':', 2)
        n = int(n)
        if old not in lines[n - 1]:
            print(f'line {n}: {old!r} not found')
            continue
        mutated = lines[:n - 1] + [lines[n - 1].replace(old, new, 1)] + lines[n:]
        header.write_text('\n'.join(mutated), encoding='utf-8')
        if build_it():
            print(f'line {n}: build failed')
            continue
        t = subprocess.run([str(build / 'cpu' / 'cpu_e2e_tests.exe')], capture_output=True, text=True, timeout=900)
        print(f'line {n}: {old} -> {new}: {"caught" if t.returncode else "MISSED"} | {lines[n - 1].strip()}', flush=True)
finally:
    shutil.copy(saved, header)
    saved.unlink()
    header.touch()
    build_it()
