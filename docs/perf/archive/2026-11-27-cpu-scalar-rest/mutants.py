"""Mutation check of cpu_e2e_tests on this batch's instructions: changes one integer constant (decimal or hexadecimal) at random in a statement
of the translated e2e header that only they write (Instruction.scalar_rest; not a statement another instruction
writes the same, such as a record form's cr0 compare), rebuilds, runs the test, counts how many
mutants it catches. Restores the header at the end.

  python -I mutants.py <build folder> <count> <seed>
"""
import json
import random
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(ROOT / 'tools'))
import ppc_translate  # noqa: E402

build = Path(sys.argv[1])
n = int(sys.argv[2])
rng = random.Random(int(sys.argv[3]))
header = build / 'cpu' / 'generated' / 'cpu_e2e_translated.h'
table = (build / 'cpu' / 'generated' / 'cpu_e2e_table.h').read_text(encoding='utf-8')
title = re.search(r'kTitle\[\] = "([^"]+)"', table).group(1)
pipeline = json.loads((ROOT / 'titles' / title / 'pipeline.json').read_text(encoding='utf-8'))
image = Path(pipeline['image']).read_bytes()
functions = json.loads((ROOT / 'docs' / 'data' / f'functions-{title}.json').read_text(encoding='utf-8'))
sizes = {int(r[0], 16): int(r[1]) for r in functions['rows']}

saved = header.with_suffix('.h.orig')
shutil.copy(header, saved)
text = saved.read_text(encoding='utf-8')
names = set(re.findall(r'inline void sub_([0-9A-F]{8})\(Ctx& ctx, const Memory& base\) \{', text))
statements, others = set(), set()
for name in names:
    for i in ppc_translate.decode_function(image, int(name, 16), sizes[int(name, 16)]):
        (statements if i.scalar_rest() is not None else others).update(i.code)
statements -= others
lines = text.split('\n')
offsets, position = [], 0
for line in lines:
    offsets.append(position)
    position += len(line) + 1
sites = []
for k, line in enumerate(lines):
    if line.strip() not in statements:
        continue
    for m in re.finditer(r'(?<=[ (,+&|~-])(0x[0-9A-F]+|\d+)(?=(?:u|U|ull|ULL|ll)?[;), ])', line):
        if not line[max(0, m.start() - 6):m.start()].endswith(('ctx.r', 'ctx.f', 'ctx.cr', 'loc_', '.u', '.s', 'int')):
            sites.append((k, m))
print(f'mutants: {len(statements)} statements of this batch in the header, {len(sites)} constants', flush=True)
caught = 0
try:
    for _ in range(n):
        k, m = rng.choice(sites)
        literal = m.group(1)
        value = int(literal, 0)
        changed = f'0x{value + 1:X}' if literal.startswith('0x') else str(value + 1)
        start = offsets[k] + m.start()
        mutated = text[:start] + changed + text[offsets[k] + m.end():]
        header.write_text(mutated, encoding='utf-8')
        r = subprocess.run(['ninja', '-C', str(build), '-j', '2', 'cpu/cpu_e2e_tests.exe'], capture_output=True, text=True)
        if r.returncode:
            print(f'line {k + 1}: {literal} -> {changed}: build failed', flush=True)
            continue
        t = subprocess.run([str(build / 'cpu' / 'cpu_e2e_tests.exe')], capture_output=True, text=True, timeout=900)
        hit = t.returncode != 0
        caught += hit
        print(f'line {k + 1}: {literal} -> {changed}: {"caught" if hit else "MISSED"} | {lines[k].strip()[:110]}', flush=True)
finally:
    shutil.copy(saved, header)
    saved.unlink()
    header.touch()
    subprocess.run(['ninja', '-C', str(build), '-j', '2', 'cpu/cpu_e2e_tests.exe'], capture_output=True)
print(f'mutants: {caught} of {n} caught')
