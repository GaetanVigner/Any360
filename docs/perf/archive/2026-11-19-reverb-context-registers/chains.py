"""Dependency chains of the promoted reverb loop (generated header): longest path in one iteration and the longest
loop-carried recurrence, under two latency models (as compiled: double ops with float rounding conversions; and
single-precision ops with no conversions)."""
import re
import sys

text = open(sys.argv[1], encoding='utf-8').read()
start = text.index('\nloc_82A4E774:\n')
end = text.index('if (ctx.cr6.lt) goto loc_82A4E774;', start)
lines = [l.strip() for l in text[start:end].split('\n')]
lines = [l for l in lines if l and not l.startswith('//') and not re.fullmatch(r'loc_[0-9A-F]+:', l)]

VAR = r'(ctx\.[fr]\d+|temp|m_\d+|s_\d+|q_\w+|b_\w+)'


def model(name):
    def lat(rhs):
        conv = name == 'double'
        if re.search(r'std::fma', rhs):
            return 4 + (8 if conv else 4)  # fma in double, then cvtsd2ss (+ cvtss2sd); float form keeps one cvt pair
        if re.search(r'double\(float\(.*[-+*].*\)\)', rhs):
            return 3 + (8 if conv else 0)
        if 'Load32' in rhs:
            return 6
        if re.match(r'double\(temp\.f32\)', rhs):
            return 4 if conv else 0
        if re.match(r'float\(', rhs):
            return 4 if conv else 0
        return 1
    return lat


for name in ('double', 'float'):
    lat = model(name)
    ready = {}       # variable -> time its value is ready, this iteration
    origin = {}      # variable -> set of carried variables (top-of-iteration values) it depends on, with path length
    top = {}
    longest = 0
    carried_paths = {}
    for l in lines:
        m = re.match(r'^(?:if \(.*\) )?' + VAR + r'(?:\.\w+)? = (.*);$', l)
        if not m:
            m2 = re.match(r'^Store32\(base, (.*), (.*)\);$', l)
            if m2:
                continue
            continue
        dst, rhs = m.group(1), m.group(2)
        srcs = set(re.findall(VAR, rhs))
        t0 = 0
        deps = {}
        for s in srcs:
            if s in ready:
                t0 = max(t0, ready[s])
                for c, d in origin[s].items():
                    deps[c] = max(deps.get(c, 0), d)
            else:
                deps[s] = max(deps.get(s, 0), 0)  # its value at the top of the iteration
        L = lat(rhs)
        ready[dst] = t0 + L
        origin[dst] = {c: d + L for c, d in deps.items()}
        longest = max(longest, ready[dst])
    # A recurrence: a variable's end value depending on its own top value.
    rec = sorted(((origin[v].get(v, -1), v) for v in origin if origin[v].get(v, -1) >= 0), reverse=True)
    print('%s model: longest path in an iteration %d cycles; longest recurrences %s' % (name, longest, rec[:6]))
print('statements', len(lines))
