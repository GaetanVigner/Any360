# Throwaway call count of the profile's hot functions (A/B worktree only, never committed).
#   apply: the binding macros count their calls (guest_binding.h includes call_counter_core.h), and the profile's
#          top unbound title functions get counting hooks (call_counter_hot.inc, included at the end of
#          guest_bindings_base.cpp).
#   --remove: both taken out again, by content.
# Usage: python -I count_run.py <crackdown checkout> <profile.csv>... [--top N] [--remove]
import csv
import glob
import io
import re
import sys

S = r'C:/Users/Gaetan/AppData/Local/Temp/claude/4b3779b33b6aeead/c1219ef0-d3b6-41af-9cc5-9e73d9171756/scratchpad/'
root = sys.argv[1]
remove = '--remove' in sys.argv
top = 600
if '--top' in sys.argv:
    top = int(sys.argv[sys.argv.index('--top') + 1])
profiles = [a for a in sys.argv[2:] if a.endswith('.csv')]
CORE = '#include "%scall_counter_core.h"  // THROWAWAY call count' % S
HOT = '#include "%scall_counter_hot.inc"  // THROWAWAY call count' % S
MACROS = ['crackdown::hooks::handler(ctx, base, CRACKDOWN_GUEST_ORIGINAL(address));',
          'crackdown::hooks::handler(ctx, base);',
          '::any360::host::rexglue::xdk::__VA_ARGS__(ctx, base, CRACKDOWN_GUEST_ORIGINAL(address));',
          '::any360::host::rexglue::d3d_draw::handler(ctx, base, CRACKDOWN_GUEST_ORIGINAL(address));']


def rw(path, fn):
    t = io.open(path, encoding='utf-8', newline='').read()
    nl = '\r\n' if '\r\n' in t else '\n'
    t = fn(t.replace('\r\n', '\n'))
    io.open(path, 'w', encoding='utf-8', newline='').write(t.replace('\n', nl))


def binding_h(t):
    if remove:
        t = t.replace(CORE + '\n', '')
        for m in MACROS:
            t = t.replace('CALL_COUNT(address); ' + m, m)
        return t
    assert CORE not in t
    t = t.replace('#include <rex/hook.h>\n', '#include <rex/hook.h>\n' + CORE + '\n', 1)
    for m in MACROS:
        assert m in t, m
        t = t.replace(m, 'CALL_COUNT(address); ' + m)
    return t


def bindings_base(t):
    if remove:
        return t.replace('\n' + HOT, '').replace(HOT + '\n', '').replace(HOT, '')
    assert HOT not in t
    return t.rstrip('\n') + '\n' + HOT + '\n'


if not remove:
    bound = set()
    for f in glob.glob(root + '/src/**/*.cpp', recursive=True) + glob.glob(root + '/src/**/*.h', recursive=True):
        txt = open(f, encoding='utf-8', errors='replace').read()
        for m in re.finditer(r'(?:BIND[A-Z0-9_]*|REX_HOOK_RAW|REX_HOOK)\(\s*(?:CRACKDOWN_GUEST_FUNC\()?(?:sub_)?(82[0-9A-F]{6})', txt):
            bound.add(m.group(1))
    own = {}
    for p in profiles:
        for row in csv.DictReader(open(p, encoding='utf-8', errors='replace')):
            if row['kind'] == 'title':
                own[row['address'].upper()] = own.get(row['address'].upper(), 0.0) + float(row['own_ms'])
    hot = [a for a, _ in sorted(own.items(), key=lambda kv: -kv[1])][:top]
    unbound = [a for a in hot if a not in bound]
    lines = ['// Throwaway: counting hooks of the profile\'s hot unbound title functions.']
    for a in unbound:
        lines.append('REX_EXTERN(CRACKDOWN_GUEST_ORIGINAL(%s)); REX_HOOK_RAW(CRACKDOWN_GUEST_FUNC(%s)) { CALL_COUNT(%s); '
                     'CRACKDOWN_GUEST_ORIGINAL(%s)(ctx, base); }' % (a, a, a, a))
    open(S + 'call_counter_hot.inc', 'w').write('\n'.join(lines) + '\n')
    print('hot %d, of them bound %d, hooked %d' % (len(hot), len(hot) - len(unbound), len(unbound)))
rw(root + '/src/guest_binding.h', binding_h)
rw(root + '/src/guest_bindings_base.cpp', bindings_base)
print('removed' if remove else 'applied')
