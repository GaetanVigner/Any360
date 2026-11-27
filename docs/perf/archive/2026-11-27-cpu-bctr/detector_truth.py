import json, re, sys, glob, pickle
from pathlib import Path
sys.path.insert(0, 'tools')
import ppc_model
title = sys.argv[1]
p = json.load(open(f'titles/{title}/pipeline.json'))
found = ppc_model.functions(p['generated'])
truth = {}
for a, body in found.items():
    lines = body.splitlines()
    items = []
    for i, l in enumerate(lines):
        if l.strip() == '// bctr':
            nxt = lines[i+1].strip()
            m = re.match(r'switch \(ctx\.r(\d+)\.u32\) \{', nxt)
            if m:
                targets = []
                j = i + 2
                while not lines[j].strip().startswith('default:'):
                    s = lines[j].strip()
                    if s.startswith('case'):
                        t = lines[j+1].strip()
                        mm = re.match(r'goto loc_([0-9A-F]+);', t)
                        if mm: targets.append(('l', int(mm.group(1), 16)))
                        elif t.startswith('__builtin_trap'): targets.append(('0', 0))
                        else:
                            mm = re.match(r'(sub_[0-9A-F]+|\w+)\(ctx, base\);', t)
                            targets.append(('f', mm.group(1) if mm else t))
                    j += 1
                items.append(('switch', int(m.group(1)), targets))
            else:
                items.append(('tail', nxt))
    if items:
        truth[int(a, 16)] = items
pickle.dump(truth, open(f'{sys.argv[2]}/truth-{title}.pkl', 'wb'))
import collections
c = collections.Counter(k[0] for v in truth.values() for k in v)
print(title, len(truth), c, collections.Counter(k[1] for v in truth.values() for k in v if k[0]=='tail').most_common(3))
