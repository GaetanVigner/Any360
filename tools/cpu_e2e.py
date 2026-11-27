#!/usr/bin/env python3
"""The end-to-end check of tools/ppc_translate.py: a title's functions translated by Any360 from the XEX's PowerPC
words, beside the same functions as ReXGlue recompiled them (tools/ppc_model.py, on the same context), for
layer/cpu/tests to run both on the same inputs and compare every register and every byte of memory.

The functions are those both tools take (Any360's covered instructions; nothing the model leaves out, such as indirect
calls), spread over the title's address space: `count` of them, every k-th in address order. Writes
<out>/cpu_e2e_{translated,reference,table}.h, with ANY360_CPU_STEP() at each entry and label (a test bounds a run on made-up
data with it), and the same without it, cpu_e2e_plain_*.h (the bench times those).
Nothing of the title is kept in the repository: the headers are made in the build folder from the title's own files.

  python -I tools/cpu_e2e.py --title <title> --count <n> --out <folder>
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import ppc_model  # noqa: E402
import ppc_translate  # noqa: E402


def supported_by_model(found, address, seen=None):
    seen = set() if seen is None else seen
    if address in seen:
        return True
    seen.add(address)
    body = found.get(address)
    if body is None:
        return False
    for line in body.splitlines():
        if not line.strip().startswith('//') and ppc_model.UNSUPPORTED.search(line):
            return False
    return all(supported_by_model(found, c, seen) for c in re.findall(r'\bsub_([0-9A-F]{8})\(ctx, base\)', body)
               if c != address)


STEP = 'ANY360_CPU_STEP();'
STEP_MACRO = ['#ifndef ANY360_CPU_STEP', '#define ANY360_CPU_STEP()', '#endif']
STEP_AT = re.compile(r'loc_[0-9A-F]{8}:|inline void sub_[0-9A-F]{8}\(Ctx& ctx, const Memory& base\) \{')


def with_steps(text):
    """The header with ANY360_CPU_STEP() at each function's entry and each label: a test bounds the run of a function on
    made-up data by defining it (both versions pass the same steps); empty otherwise."""
    out = []
    for line in text.splitlines():
        out.append(line)
        if STEP_AT.fullmatch(line):
            out.append('  ' + STEP)
        elif line == '#pragma once':
            out += STEP_MACRO
    return '\n'.join(out) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    parser.add_argument('--title', required=True)
    parser.add_argument('--count', type=int, default=300)
    parser.add_argument('--min', type=int, default=6)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()

    pipeline = json.loads((ROOT / 'titles' / args.title / 'pipeline.json').read_text(encoding='utf-8'))
    image = Path(pipeline['image']).read_bytes()
    table = json.loads((ROOT / 'docs' / 'data' / f'functions-{args.title}.json').read_text(encoding='utf-8'))
    sizes = {int(r[0], 16): int(r[1]) for r in table['rows']}
    found = ppc_model.functions(pipeline['generated'])

    candidates = []
    for address in sorted(sizes):
        if sizes[address] < args.min:
            continue
        try:
            ppc_translate.translate_tree(image, sizes, address, set(), [], False)
        except ppc_translate.Unsupported:
            continue
        if supported_by_model(found, f'{address:08X}'):
            candidates.append(address)
    step = max(1, len(candidates) // args.count)
    roots = candidates[::step][:args.count]

    done, order = set(), []
    for root in roots:
        ppc_translate.translate_tree(image, sizes, root, done, order, False)
    bodies = [(f'sub_{a:08X}', body) for a, body in order]
    args.out.mkdir(parents=True, exist_ok=True)
    what = f'{len(roots)} roots, {len(bodies)} functions'
    for suffix, stepped in (('', True), ('_plain', False)):
        translated = args.out / f'cpu_e2e{suffix}_translated.h'
        reference = args.out / f'cpu_e2e{suffix}_reference.h'
        text = ppc_translate.header(f'{args.title}\'s image', what, f'any360::cpu::translated::e2e{suffix}', bodies)
        translated.write_text(with_steps(text) if stepped else text, encoding='utf-8')
        subprocess.run([sys.executable, '-I', str(ROOT / 'tools' / 'ppc_model.py'), pipeline['generated'],
                        f'cpu_e2e{suffix}', str(reference)] + [f'{r:08X}' for r in roots], check=True,
                       stdout=subprocess.DEVNULL)
        if stepped:
            reference.write_text(with_steps(reference.read_text(encoding='utf-8')), encoding='utf-8')
            steps = [path.read_text(encoding='utf-8').count(STEP) for path in (translated, reference)]
            if steps[0] != steps[1]:
                sys.exit(f'cpu_e2e: the two versions have {steps[0]} and {steps[1]} steps')
        rows = ',\n'.join(f'    {{0x{r:08X}u, {sizes[r]}u, &translated::e2e{suffix}::sub_{r:08X}, '
                          f'&reference::cpu_e2e{suffix}::sub_{r:08X}}}' for r in roots)
        (args.out / f'cpu_e2e{suffix}_table.h').write_text(f'''// Generated by tools/cpu_e2e.py from {args.title}: the functions both versions take{"" if stepped else ", without steps"}.
#pragma once

#include "cpu_e2e{suffix}_reference.h"
#include "cpu_e2e{suffix}_translated.h"

namespace any360::cpu::e2e{suffix} {{

using Function = void (*)(any360::xdk::ppc_model::Ctx&, const any360::xdk::Memory&);
struct Pair {{
  uint32_t address, instructions;
  Function translated, reference;
}};
namespace translated = any360::cpu::translated;
namespace reference = any360::xdk::reference;
inline constexpr char kTitle[] = "{args.title}";
inline constexpr unsigned kCandidates = {len(candidates)};
inline constexpr Pair kPairs[] = {{
{rows}
}};

}}  // namespace any360::cpu::e2e{suffix}
''', encoding='utf-8')
    print(f'cpu_e2e: {args.title}: {len(candidates)} functions both tools take, {len(roots)} picked, '
          f'{len(bodies)} translated with their callees')


if __name__ == '__main__':
    main()
