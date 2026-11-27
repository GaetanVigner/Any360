import json, struct, sys
sys.path.insert(0, sys.argv.pop(1))
import ppc_translate as t
for title in sys.argv[1:]:
    p = json.load(open(f'titles/{title}/pipeline.json'))
    image = open(p['image'], 'rb').read()
    table = json.load(open(f'docs/data/functions-{title}.json', encoding='utf-8'))
    sizes = {int(r[0], 16): int(r[1]) for r in table['rows']}
    done, order = set(), []
    ok = 0; insns = 0
    for a in sorted(sizes):
        try:
            t.translate_tree(image, sizes, a, done, order, False)
        except (t.Unsupported, struct.error):
            continue
    good = {a for a, _ in order}
    print(title, len(good), len(sizes), f'{100*len(good)/len(sizes):.1f}%', sum(sizes[a] for a in good), sum(sizes.values()),
          f'{100*sum(sizes[a] for a in good)/sum(sizes.values()):.1f}%')
