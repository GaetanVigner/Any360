#!/usr/bin/env python3
"""Any360 function inventory: every function of a recompiled title, what it is and whether it is translated yet.

From a recomp's generated code (one DEFINE_REX_FUNC per function, one `// mnemonic` comment per instruction) and the
port's sources (the functions it binds to native C++: REX_HOOK / REX_HOOK_RAW / REX_FUNC / <GAME>_BIND*), writes:
- docs/data/functions-<title>.json: one compact row per function [address, instructions, status, handler, file],
  status R recompiled / N native (bound to C++) / V verified (golden test) / L library (an SDK or middleware range
  the title links, replaced as a unit rather than decompiled);
- docs/coverage/inventory-<title>.json: totals per module (titles/<title>/modules.json: named address ranges, the
  rest grouped by 1 MB area), the translated functions with their handler names and purposes, the imports with
  their state (from docs/coverage/<title>.json when present).
Addresses, counts and host-side names only: no game code.

  python tools/inventory.py --name crackdown --generated <recomp>/generated/default --port-src <recomp>/src \
      [--golden <recomp>/tests/golden] [--bind-macro CRACKDOWN_BIND] [--out docs]
"""
import argparse
import json
import re
from collections import Counter, defaultdict
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FUNC = re.compile(r'DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)')
MNEMONIC = re.compile(r'^[ \t]*// [a-z][a-z0-9_.]*[+-]?(?: [^\n]*)?\n(?![ \t]*//)', re.M)
BIND = re.compile(r'(?P<macro>REX_HOOK(?:_RAW)?|REX_FUNC|[A-Z][A-Z0-9_]*_BIND(?:_REPLACEMENT)?)\(\s*(?:sub_|0x)?'
                  r'(?P<address>[0-9A-Fa-f]{8})\b\s*(?:,\s*(?P<handler>[A-Za-z_][A-Za-z0-9_:]*))?')
COMMENT = re.compile(r'(?:^[ \t]*//[^\n]*\n)+', re.M)


def read(path):
    return path.read_text(encoding='utf-8', errors='replace')


def functions(generated):
    """address -> instruction count, from the generated translation units."""
    sizes = {}
    for path in sorted(generated.glob('*_recomp.*.cpp')):
        text = read(path)
        starts = [(m.start(), int(m.group(1), 16)) for m in FUNC.finditer(text)]
        for i, (start, address) in enumerate(starts):
            end = starts[i + 1][0] if i + 1 < len(starts) else len(text)
            sizes[address] = len(MNEMONIC.findall(text[start:end] + '\n'))
    return sizes


def bindings(port_src):
    """address -> {handler, file, purpose} for every function the port binds to native code."""
    bound = {}
    if not port_src:
        return bound
    for path in sorted(port_src.rglob('*')):
        if path.suffix not in ('.cpp', '.h', '.inc', '.hpp'):
            continue
        text = read(path)
        # The file's leading comment block: the purpose of its bindings when none has a comment of its own.
        header = COMMENT.match(text.lstrip('﻿'))
        header = ' '.join(l.strip().lstrip('/').strip() for l in header.group().strip().splitlines())[:240] if header else ''
        for m in BIND.finditer(text):
            address = int(m.group('address'), 16)
            # The comment block right before the binding, as its purpose (host-side prose, not game code).
            before = text[:m.start()].rstrip()
            purpose = ''
            cm = COMMENT.search(before[-1200:])
            if cm and before.endswith(cm.group().rstrip()):
                purpose = ' '.join(l.strip().lstrip('/').strip() for l in cm.group().strip().splitlines())[:240]
            entry = bound.setdefault(address, {'handler': '', 'file': str(path.relative_to(port_src)).replace('\\', '/'),
                                               'purpose': '', 'impl': '', 'header': header})
            if m.group('handler') and not entry['handler']:
                entry['handler'] = m.group('handler')
            if purpose and not entry['purpose']:
                entry['purpose'] = purpose
    # The handler's definition usually lives in another file, under the comment that says what the replacement does:
    # that comment is the purpose, that file the implementation.
    by_handler = {}
    for entry in bound.values():
        if entry['handler']:
            by_handler.setdefault(entry['handler'].split('::')[-1], []).append(entry)
    if by_handler:
        pattern = re.compile(r'^[ \t]*(?:static\s+)?[A-Za-z_][\w:<>*& ]*?\b(' + '|'.join(map(re.escape, by_handler))
                             + r')\s*\([^;{]*\)\s*\{', re.M)
        for path in sorted(port_src.rglob('*.cpp')):
            text = read(path)
            for m in pattern.finditer(text):
                before = text[:m.start()].rstrip()
                cm = COMMENT.search(before[-1500:])
                purpose = ''
                if cm and before.endswith(cm.group().rstrip()):
                    purpose = ' '.join(l.strip().lstrip('/').strip() for l in cm.group().strip().splitlines())[:240]
                for entry in by_handler[m.group(1)]:
                    if not entry['impl']:
                        entry['impl'] = str(path.relative_to(port_src)).replace('\\', '/')
                        if purpose:
                            entry['purpose'] = purpose
    for entry in bound.values():
        entry['purpose'] = entry['purpose'] or entry.pop('header', '')
        entry.pop('header', None)
    return bound


def goldens(golden_dir):
    verified = set()
    if golden_dir and golden_dir.exists():
        for path in golden_dir.glob('*.txt'):
            for line in read(path).splitlines():
                if line and not line.startswith('#'):
                    try:
                        verified.add(int(line.split()[0], 16))
                    except ValueError:
                        pass
    return verified


def load_modules(name):
    path = ROOT / 'titles' / name / 'modules.json'
    if not path.exists():
        return []
    data = json.loads(read(path))
    return [(int(m['start'], 16), int(m['end'], 16), m['name'], m.get('kind', 'game')) for m in data['modules']]


def module_of(address, modules):
    for start, end, name, kind in modules:
        if start <= address < end:
            return name, kind
    return f'code {address >> 20:03X}xxxxx', 'game'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--name', required=True)
    parser.add_argument('--generated', type=Path, required=True)
    parser.add_argument('--port-src', type=Path)
    parser.add_argument('--golden', type=Path)
    parser.add_argument('--out', type=Path, default=ROOT / 'docs')
    args = parser.parse_args()

    sizes = functions(args.generated)
    bound = bindings(args.port_src)
    verified = goldens(args.golden)
    modules = load_modules(args.name)
    coverage_path = args.out / 'coverage' / f'{args.name}.json'
    imports = json.loads(read(coverage_path))['imports'] if coverage_path.exists() else []

    rows = []
    per_module = defaultdict(lambda: {'kind': 'game', 'functions': 0, 'instructions': 0, 'native': 0, 'verified': 0,
                                      'native_instructions': 0})
    for address in sorted(sizes):
        module, kind = module_of(address, modules)
        if address in bound:
            status = 'N'
        elif address in verified:
            status = 'V'
        elif kind == 'library':
            status = 'L'
        else:
            status = 'R'
        b = bound.get(address, {})
        rows.append([f'{address:08X}', sizes[address], status, b.get('handler', ''), b.get('impl') or b.get('file', '')])
        m = per_module[module]
        m['kind'] = kind
        m['functions'] += 1
        m['instructions'] += sizes[address]
        if status == 'N':
            m['native'] += 1
            m['native_instructions'] += sizes[address]
        if status == 'V':
            m['verified'] += 1

    translated = [{'address': f'{a:08X}', 'instructions': sizes.get(a, 0), **b} for a, b in sorted(bound.items())
                  if a in sizes]
    categories = Counter(t['file'].split('/')[-1].rsplit('.', 1)[0] for t in translated)
    summary = {
        'name': args.name, 'date': date.today().isoformat(),
        'functions': len(rows), 'instructions': sum(sizes.values()),
        'native': sum(1 for r in rows if r[2] == 'N'), 'verified': sum(1 for r in rows if r[2] == 'V'),
        'library': sum(1 for r in rows if r[2] == 'L'),
        'modules': dict(sorted(per_module.items(), key=lambda kv: -kv[1]['instructions'])),
        'translated': translated, 'translated_by_file': dict(categories.most_common()),
        'imports': {'total': len(imports),
                    'by_state': dict(Counter(c['state'] for c in imports)),
                    'by_library': {lib: dict(Counter(c['state'] for c in imports if c['library'] == lib))
                                   for lib in sorted({c['library'] for c in imports})}},
    }
    (args.out / 'data').mkdir(parents=True, exist_ok=True)
    (args.out / 'data' / f'functions-{args.name}.json').write_text(
        json.dumps({'name': args.name, 'date': summary['date'], 'columns': ['address', 'instructions', 'status',
                                                                             'handler', 'file'], 'rows': rows},
                   separators=(',', ':')) + '\n', encoding='utf-8')
    (args.out / 'coverage' / f'inventory-{args.name}.json').write_text(json.dumps(summary, indent=1) + '\n',
                                                                         encoding='utf-8')
    print(f'{args.name}: {len(rows)} functions, {summary["instructions"]:,} instructions, {summary["native"]} native, '
          f'{summary["verified"]} verified, {summary["library"]} in library ranges; modules: '
          f'{[(m, v["functions"]) for m, v in list(summary["modules"].items())[:6]]}')


if __name__ == '__main__':
    main()
