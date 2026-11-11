#!/usr/bin/env python3
"""Which registers a title's code reads after calling a function, before writing them itself.

    python tools/callee_registers.py <generated dir> <address>...

A native replacement gives the function's result (r3, memory). The function's own code also leaves values in the
registers it happens to use (a pointer past the end of what it copied, the last byte it read, a compare's flags),
and code built with whole-program optimization, or written in assembly next to it, may use them: the calling
convention says it cannot, the compiler knew it could. This lists, for each call site of each address, the
volatile registers read after the call before anything writes them, scanning the straight-line code up to the
next call, branch or label. A register listed here is one the replacement has to leave as the function does.
"""
import re
import sys
from pathlib import Path

VOLATILE = {'r0', 'r4', 'r5', 'r6', 'r7', 'r8', 'r9', 'r10', 'r11', 'r12', 'cr0', 'cr1', 'cr5', 'cr6', 'cr7', 'ctr'}
REG = re.compile(r'ctx\.(r\d+|cr\d|ctr)\b')
ASSIGN = re.compile(r'^\s*ctx\.(r\d+|ctr)\.\w+ = (.*)$')
COMPARE = re.compile(r'^\s*ctx\.(cr\d)\.compare')
# Where the straight-line scan ends: an unconditional jump or return, another call, the function's end. A label or
# a conditional branch does not end it (the code falls through, and a loop head after a call is common).
STOP = re.compile(r'^\s*(?:goto |return\b|sub_[0-9A-F]{8}\(ctx|REX_CALL|switch|\}\s*$)')
DEFINE = re.compile(r'^DEFINE_REX_FUNC\((sub_[0-9A-F]{8})\)|^(?:PPC_FUNC_IMPL|REX_FUNC)\((?:__imp__)?(sub_[0-9A-F]{8})\)')


def main():
    root = Path(sys.argv[1])
    wanted = {'sub_' + a.upper().replace('0X', '') for a in sys.argv[2:]}
    found = {name: {} for name in wanted}
    sites = {name: 0 for name in wanted}
    for path in sorted(root.glob('*recomp*.cpp')):
        lines = path.read_text(encoding='utf-8', errors='replace').splitlines()
        function = ''
        for i, line in enumerate(lines):
            m = DEFINE.match(line)
            if m:
                function = m.group(1) or m.group(2)
                continue
            stripped = line.strip()
            for name in wanted:
                if stripped != name + '(ctx, base);':
                    continue
                sites[name] += 1
                written, read = set(), set()
                for after in lines[i + 1:i + 200]:
                    text = after.strip()
                    if not text or text.startswith('//'):
                        continue
                    target = None
                    m = ASSIGN.match(after)
                    c = COMPARE.match(after)
                    if m:
                        target, used = m.group(1), REG.findall(m.group(2))
                    elif c:
                        target, used = c.group(1), REG.findall(after[c.end():])
                    else:
                        used = REG.findall(after)
                    for reg in used:
                        if reg in VOLATILE and reg not in written:
                            read.add(reg)
                    if target:
                        written.add(target)
                    if STOP.match(after) and not (m or c):
                        break
                for reg in read:
                    found[name].setdefault(reg, []).append(f'{function} ({path.name}:{i + 1})')
    for name in sorted(wanted):
        print(f'{name}: {sites[name]} call sites')
        for reg, where in sorted(found[name].items()):
            print(f'  {reg} read after the call at {len(where)} site(s): {", ".join(where[:4])}' + (' ...' if len(where) > 4 else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
