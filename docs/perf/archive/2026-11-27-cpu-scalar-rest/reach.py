"""For each line given, inserts an abort before it in the translated e2e header, rebuilds, runs: reached or not.

  python -I reach.py <build folder> <line> ...
"""
import shutil
import subprocess
import sys
from pathlib import Path

build = Path(sys.argv[1])
header = build / 'cpu' / 'generated' / 'cpu_e2e_translated.h'
saved = header.with_suffix('.h.orig')
shutil.copy(header, saved)
lines = saved.read_text(encoding='utf-8').split('\n')


def build_it():
    return subprocess.run(['ninja', '-C', str(build), '-j', '2', 'cpu/cpu_e2e_tests.exe'], capture_output=True,
                          text=True).returncode


try:
    for n in map(int, sys.argv[2:]):
        header.write_text('\n'.join(lines[:n - 1] + ['  std::abort();'] + lines[n - 1:]), encoding='utf-8')
        if build_it():
            print(n, 'build failed')
            continue
        t = subprocess.run([str(build / 'cpu' / 'cpu_e2e_tests.exe')], capture_output=True, text=True, timeout=900)
        function = next(l for l in reversed(lines[:n]) if l.startswith('inline void sub_'))
        print(n, 'reached' if t.returncode else 'NOT reached', '|', function.strip()[:40], '|', lines[n - 1].strip()[:80],
              flush=True)
finally:
    shutil.copy(saved, header)
    saved.unlink()
    header.touch()
    build_it()
