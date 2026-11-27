import json, struct, sys, pickle, collections
sys.path.insert(0, 'tools')
import ppc_translate as t
title = sys.argv[1]
p = json.load(open(f'titles/{title}/pipeline.json'))
image = open(p['image'], 'rb').read()
table = json.load(open(f'docs/data/functions-{title}.json', encoding='utf-8'))
sizes = {int(r[0], 16): int(r[1]) for r in table['rows']}
truth = pickle.load(open(f'{sys.argv[2]}/truth-{title}.pkl', 'rb'))
res = collections.Counter(); bad = []
for f, items in truth.items():
    if f not in sizes: continue
    s = f - t.IMAGE_BASE
    ws = struct.unpack(f'>{sizes[f]}I', image[s:s+4*sizes[f]])
    bctrs = [f + 4*i for i, w in enumerate(ws) if w == t.BCTR]
    if len(bctrs) != len(items): res['count differs'] += 1; continue
    for b, it in zip(bctrs, items):
        try:
            got = t.jump_table(image, t.IMAGE_BASE, b, f)
        except t.Unsupported as e:
            res[(it[0], 'unsupported')] += 1; continue
        ok = got is None if it[0] == 'tail' else (got is not None and got[0] == it[1] and got[1] == [x for _, x in it[2]])
        res[(it[0], ok)] += 1
        if not ok: bad.append((hex(b), it[0], got and len(got[1]), len(it[2]) if it[0]=='switch' else ''))
print(title, sorted(res.items(), key=str)); print(bad[:10])
