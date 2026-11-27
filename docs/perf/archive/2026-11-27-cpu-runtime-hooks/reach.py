"""python reach.py <build folder> <line>...: for each line given, inserts a throw before it in the translated e2e header, rebuilds, runs: reached or not."""
import shutil, subprocess, sys
from pathlib import Path
build = Path(sys.argv[1])
header = build / 'cpu' / 'generated' / 'cpu_e2e_translated.h'
saved = header.with_suffix('.h.orig'); shutil.copy(header, saved)
lines = saved.read_text(encoding='utf-8').split('\n')
vs = r'F:\VisualStudio\Common7\Tools\Launch-VsDevShell.ps1'
def build_it():
    return subprocess.run(['powershell','-NoProfile','-Command',f'& {{ . {vs} -Arch amd64 -SkipAutomaticLocation | Out-Null; ninja -j 2 -C {build} cpu/cpu_e2e_tests.exe }}'],capture_output=True,text=True).returncode
try:
    for n in map(int, sys.argv[2:]):
        mutated = lines[:n-1] + ['  std::abort();'] + lines[n-1:]
        header.write_text('\n'.join(mutated), encoding='utf-8')
        if build_it(): print(n, 'build failed'); continue
        t = subprocess.run([str(build/'cpu'/'cpu_e2e_tests.exe')], capture_output=True, text=True, timeout=600)
        func = next(l for l in reversed(lines[:n]) if l.startswith(('void sub_', 'inline void sub_', 'static void sub_')) or ' sub_' in l and l.rstrip().endswith('{') and '(' in l)
        print(n, 'reached' if t.returncode else 'NOT reached', '|', func.strip()[:60], flush=True)
finally:
    shutil.copy(saved, header); saved.unlink(); header.touch(); build_it()
