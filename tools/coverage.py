#!/usr/bin/env python3
"""Any360 coverage: what an Xbox 360 recompilation needs from the ReXGlue SDK, and what the SDK provides.

Given a recomp's generated code (the ReXGlue codegen output) and an SDK source tree, writes a report of:
- the kernel / XAM / XBDM imports of the XEX (from the generated *_init.cpp import table), each as the SDK's runtime
  provides it: implemented, stub (declared but does nothing or returns a constant), variable, or missing;
- the PowerPC / VMX128 instruction mnemonics the recompiled code uses (the codegen writes one `// mnemonic` comment per
  instruction), each handled by the SDK's recompiler (its dispatch table) or not, plus the REX_UNIMPLEMENTED markers
  the codegen left in the output;
- the recompiled functions, and those the port replaces natively (REX_HOOK / REX_FUNC / *_BIND in the port's sources);
- the GPU-side imports (Vd*) as a static hint of the video features used. PM4 opcodes, shader features and EDRAM usage
  are runtime facts: they come from the SDK's `gpu_frame_stats` / draw trace, not from this tool.

Only counts and public API names (kernel export names, instruction mnemonics, function addresses) are written: no
game code or data. Standard library only.

  python tools/coverage.py --name crackdown --generated <recomp>/generated/default --sdk <rexglue-sdk-src> \
      [--port-src <recomp>/src] [--compare-sdk <other-sdk-src>] [--out docs/coverage]
  python tools/coverage.py --compare docs/coverage/crackdown.json docs/coverage/simpsons.json [--out docs/coverage]
"""
import argparse
import json
import re
from collections import Counter
from datetime import date
from pathlib import Path

IMPORT = re.compile(r'\{\s*0x[0-9A-Fa-f]+,\s*__imp__([A-Za-z0-9_]+)\s*\}')
EXPORT = re.compile(r'REX_(?:EXPORT(_STUB(?:_ONCE|_RETURN)?)?|HOOK_RAW())\(\s*__imp__([A-Za-z0-9_]+)')
TABLE_EXPORT = re.compile(r'XE_EXPORT\(\s*(\w+),\s*0x([0-9A-Fa-f]+),\s*([A-Za-z0-9_]+),\s*k(Function|Variable)')
VARIABLE_MAPPING = re.compile(r'SetVariableMapping\("(\w+)\.exe",\s*0x([0-9A-Fa-f]+)')
# One `// mnemonic operands` comment per instruction, followed by the instruction's code: a comment followed by
# another comment is prose (some codegens annotate functions), not an instruction.
MNEMONIC = re.compile(r'^[ \t]*// ([a-z][a-z0-9_.]*[+-]?)(?: [^\n]*)?\n(?![ \t]*//)', re.M)
DEFINE_FUNC = re.compile(r'DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)')
UNIMPLEMENTED = re.compile(r'REX_UNIMPLEMENTED\(0x([0-9A-Fa-f]+),\s*"([^"]+)"\)')
OPCODE = re.compile(r'^\{\s*"([a-z0-9_.+-]+)",.*?(PPC_INST_[A-Z0-9_]+)\s*\},?\s*$', re.M)
MACRO = re.compile(r'\{\s*"([a-z0-9_.]+)",\s*\d+,\s*\w+,\s*"([a-z0-9_.]+)[ %]')
DISPATCH = re.compile(r'\{\s*(PPC_INST_[A-Z0-9_]+)\s*,\s*build_\w+\s*\}')
HOOK = re.compile(r'(?:REX_HOOK(?:_RAW)?|REX_FUNC|[A-Z][A-Z0-9_]*_BIND(?:_REPLACEMENT)?)\(\s*(?:sub_|0x)?([0-9A-Fa-f]{8})\b')
GPU_PREFIXES = ('Vd', 'XGetVideoMode', 'XGetAVPack')


def read(path):
    return path.read_text(encoding='utf-8', errors='replace')


# --- SDK side ----------------------------------------------------------------------------------------------------

def sdk_exports(sdk):
    """name -> {library, ordinal, kind, state} for every export the SDK's kernel knows about."""
    kernel = sdk / 'src' / 'kernel'
    exports = {}
    for table in kernel.rglob('export_table.inc'):
        for library, ordinal, name, kind in TABLE_EXPORT.findall(read(table)):
            exports[name] = {'library': library, 'ordinal': int(ordinal, 16), 'kind': kind.lower(), 'state': 'missing'}
    mapped = set()
    for path in kernel.rglob('*.cpp'):
        text = read(path)
        library = path.relative_to(kernel).parts[0]
        for macro, _raw, name in EXPORT.findall(text):
            state = 'implemented' if not macro else 'stub'  # REX_HOOK_RAW(__imp__x): the variadic exports
            entry = exports.setdefault(name, {'library': library, 'ordinal': None, 'kind': 'function', 'state': 'missing'})
            if entry['state'] != 'implemented':
                entry['state'] = state
        mapped |= {(lib, int(ordinal, 16)) for lib, ordinal in VARIABLE_MAPPING.findall(text)}
    for entry in exports.values():
        if entry['kind'] == 'variable' and (entry['library'], entry['ordinal']) in mapped:
            entry['state'] = 'variable'
    return exports


def sdk_instructions(sdk):
    """(mnemonic -> PPC_INST id, set of ids the recompiler handles). Macros (slwi, clrrwi...) map to their expansion."""
    disasm = sdk / 'thirdparty' / 'disasm' / 'ppc-dis.c'
    text = read(disasm)
    ids = {}
    for name, ident in OPCODE.findall(text):
        ids.setdefault(name, ident)
    for name, base in MACRO.findall(text):
        if name not in ids and base in ids:
            ids[name] = ids[base]
    dispatch = set(DISPATCH.findall(read(sdk / 'src' / 'codegen' / 'instruction_dispatch.cpp')))
    return ids, dispatch


def sdk_version(sdk):
    match = re.search(r'VERSION\s+(\d+\.\d+\.\d+)', read(sdk / 'CMakeLists.txt'))
    return match.group(1) if match else 'unknown'


# --- game side ---------------------------------------------------------------------------------------------------

def game_imports(generated):
    names = set()
    for init in generated.glob('*_init.cpp'):
        names |= set(IMPORT.findall(read(init)))
    return sorted(names)


def game_code(generated):
    mnemonics = Counter()
    functions = set()
    unimplemented = Counter()
    sources = sorted(generated.glob('*_recomp.*.cpp'))
    for path in sources:
        text = read(path)
        mnemonics.update(MNEMONIC.findall(text))
        functions |= {int(a, 16) for a in DEFINE_FUNC.findall(text)}
        unimplemented.update(name for _, name in UNIMPLEMENTED.findall(text))
    return mnemonics, functions, unimplemented, len(sources)


def port_hooks(port_src):
    hooked = set()
    if not port_src:
        return hooked
    for path in port_src.rglob('*'):
        if path.suffix in ('.cpp', '.h', '.inc', '.hpp'):
            hooked |= {int(a, 16) for a in HOOK.findall(read(path))}
    return hooked


# --- the report --------------------------------------------------------------------------------------------------

def normalise(mnemonic, ids):
    """The opcode table name a generated comment's mnemonic resolves to (branch hints stripped when needed)."""
    if mnemonic in ids:
        return mnemonic
    stripped = mnemonic.rstrip('+-')
    if stripped in ids:
        return stripped
    return None


def family(mnemonic):
    m = mnemonic.rstrip('.+-')
    if m.endswith('128') or m.startswith(('v', 'lv', 'stv')) or m in ('mfvscr', 'mtvscr'):
        return 'VMX / VMX128'
    if m.startswith(('f', 'lf', 'stf', 'mffs', 'mtfs', 'mcrfs')):
        return 'floating point'
    if m.startswith(('b', 'sc', 'tw', 'td', 'rfi')):
        return 'branch / trap'
    if m.startswith(('l', 'st', 'dcb', 'icb', 'sync', 'eieio', 'isync')):
        return 'load / store / cache'
    if m[:2] in ('mf', 'mt', 'mc'):
        return 'system'
    if m.startswith(('cmp', 'cr')):
        return 'compare / CR'
    return 'integer'


def coverage(name, generated, sdk, port_src, compare_sdk):
    exports = sdk_exports(sdk)
    ids, dispatch = sdk_instructions(sdk)
    imports = game_imports(generated)
    mnemonics, functions, unimplemented, source_count = game_code(generated)
    hooked = port_hooks(port_src) & functions

    calls = []
    for imp in imports:
        entry = exports.get(imp)
        if entry:
            calls.append({'name': imp, 'library': entry['library'], 'state': entry['state'], 'kind': entry['kind']})
        else:
            library = 'xam' if imp.startswith('X') else 'xboxkrnl'
            calls.append({'name': imp, 'library': library, 'state': 'missing', 'kind': 'function'})
    if compare_sdk:
        other = sdk_exports(compare_sdk)
        for call in calls:
            call['other_state'] = other.get(call['name'], {}).get('state', 'missing')

    instructions = []
    unknown = Counter()
    for mnemonic, count in mnemonics.most_common():
        key = normalise(mnemonic, ids)
        if key is None:
            unknown[mnemonic] += count
            continue
        ident = ids[key]
        instructions.append({'mnemonic': mnemonic, 'id': ident, 'count': count,
                             'state': 'handled' if ident in dispatch else 'unhandled'})
    families = Counter()
    for item in instructions:
        families[family(item['mnemonic'])] += item['count']

    return {
        'name': name,
        'date': date.today().isoformat(),
        'sdk': {'path': str(sdk), 'version': sdk_version(sdk), 'exports_known': len(exports),
                'exports_implemented': sum(1 for e in exports.values() if e['state'] in ('implemented', 'variable')),
                'exports_stub': sum(1 for e in exports.values() if e['state'] == 'stub'),
                'instruction_ids': len(set(ids.values())), 'instruction_ids_handled': len(dispatch & set(ids.values()))},
        'compare_sdk': {'path': str(compare_sdk), 'version': sdk_version(compare_sdk)} if compare_sdk else None,
        'imports': calls,
        'instructions': instructions,
        'unknown_tokens': dict(unknown.most_common()),
        'unimplemented_markers': dict(unimplemented.most_common()),
        'functions': {'total': len(functions), 'hooked': len(hooked), 'sources': source_count,
                      'hooked_addresses': sorted(f'{a:08X}' for a in hooked)},
        'families': dict(families.most_common()),
    }


def markdown(report):
    r = report
    by_lib = {}
    for call in r['imports']:
        by_lib.setdefault(call['library'], []).append(call)
    md = [f'# Coverage: {r["name"]}', '',
          f'Generated by `tools/coverage.py` on {r["date"]} against ReXGlue SDK {r["sdk"]["version"]} '
          f'(`{Path(r["sdk"]["path"]).name}`). Raw numbers: [{r["name"]}.json]({r["name"]}.json).', '',
          '## Summary', '', '| | Value |', '|---|---|',
          f'| Recompiled functions | {r["functions"]["total"]:,} in {r["functions"]["sources"]} translation units |',
          f'| Functions the port replaces natively | {r["functions"]["hooked"]} |',
          f'| Kernel / XAM imports | {len(r["imports"])} |']
    counts = Counter(c['state'] for c in r['imports'])
    md.append(f'| &nbsp;&nbsp;implemented / variable / stub / missing | {counts["implemented"]} / {counts["variable"]} / '
              f'{counts["stub"]} / {counts["missing"]} |')
    handled = sum(i['count'] for i in r['instructions'] if i['state'] == 'handled')
    total = sum(i['count'] for i in r['instructions'])
    distinct_unhandled = [i for i in r['instructions'] if i['state'] == 'unhandled']
    md += [f'| Instructions (static count) | {total:,}, {len(r["instructions"])} distinct mnemonics |',
           f'| &nbsp;&nbsp;handled by the recompiler | {handled:,} ({handled / max(total, 1) * 100:.3f}%), '
           f'{len(distinct_unhandled)} mnemonics not handled |',
           f'| `REX_UNIMPLEMENTED` markers in the output | {sum(r["unimplemented_markers"].values())} |',
           f'| SDK recompiler: opcode ids with a builder | {r["sdk"]["instruction_ids_handled"]} of '
           f'{r["sdk"]["instruction_ids"]} known to its disassembler |',
           f'| SDK kernel: exports implemented / stub / known | {r["sdk"]["exports_implemented"]} / '
           f'{r["sdk"]["exports_stub"]} / {r["sdk"]["exports_known"]} |', '']
    md += ['## Kernel and XAM imports', '',
           'Every function and variable the XEX imports, as the SDK runtime provides it. *Implemented*: a real '
           'implementation; *variable*: a kernel variable the runtime maps; *stub*: declared, returns without doing '
           'the work (a title that depends on it misbehaves); *missing*: not declared at all (an unresolved import at '
           'load, or a call that faults).', '']
    other = r.get('compare_sdk')
    md += ['| Library | Implemented | Variable | Stub | Missing | Total | % provided |', '|' + '---|' * 7]
    for lib, calls in sorted(by_lib.items()):
        n = Counter(c['state'] for c in calls)
        provided = n['implemented'] + n['variable']
        md.append(f'| {lib} | {n["implemented"]} | {n["variable"]} | {n["stub"]} | {n["missing"]} | {len(calls)} | '
                  f'{provided / len(calls) * 100:.0f}% |')
    md += ['', '### Stubs and missing imports', '',
           'What a native port of this title must write first (the runtime does nothing or does not exist here).', '']
    cols = '| Library | Import | State |' + (f' {other["version"]} |' if other else '')
    sep = '|' + '---|' * (4 if other else 3)
    md += [cols, sep]
    for lib, calls in sorted(by_lib.items()):
        for c in calls:
            if c['state'] in ('stub', 'missing'):
                md.append(f'| {lib} | `{c["name"]}` | {c["state"]} |' + (f' {c["other_state"]} |' if other else ''))
    md += ['', '<details><summary>Every import</summary>', '', cols, sep]
    for lib, calls in sorted(by_lib.items()):
        for c in calls:
            md.append(f'| {lib} | `{c["name"]}` | {c["state"]} |' + (f' {c["other_state"]} |' if other else ''))
    md += ['', '</details>', '']
    gpu = [c for c in r['imports'] if c['name'].startswith(GPU_PREFIXES)]
    md += ['### Video (Vd*) imports', '',
           'The kernel side of the GPU: ring buffer setup, interrupts, swaps, EDRAM. PM4 opcodes, shader features and '
           'render target formats are runtime facts (`gpu_frame_stats`, draw traces), not visible here.', '',
           '| Import | State |', '|---|---|']
    md += [f'| `{c["name"]}` | {c["state"]} |' for c in gpu]
    md += ['', '## PowerPC instructions', '',
           'Mnemonics in the generated code (one comment per instruction), by whether the SDK recompiler has a builder '
           'for the opcode. A mnemonic without a builder is emitted as `REX_UNIMPLEMENTED` and traps when reached.', '',
           '| Family | Instructions | Share |', '|---|---|---|']
    md += [f'| {fam} | {n:,} | {n / max(total, 1) * 100:.1f}% |' for fam, n in r['families'].items()]
    md += ['', '### Not handled', '']
    if distinct_unhandled:
        md += ['| Mnemonic | Opcode id | Count |', '|---|---|---|']
        md += [f'| `{i["mnemonic"]}` | {i["id"]} | {i["count"]} |' for i in distinct_unhandled]
    else:
        md.append('Every mnemonic the code uses has a builder.')
    if r['unimplemented_markers']:
        md += ['', '`REX_UNIMPLEMENTED` markers left by the codegen:', '', '| Opcode | Count |', '|---|---|']
        md += [f'| {k} | {v} |' for k, v in r['unimplemented_markers'].items()]
    if r['unknown_tokens']:
        md += ['', 'Comment tokens that match no opcode table entry (not instructions, or extended forms the table '
                   'lacks):', '', '| Token | Count |', '|---|---|']
        md += [f'| `{k}` | {v} |' for k, v in list(r['unknown_tokens'].items())[:40]]
    md += ['', '<details><summary>Every mnemonic</summary>', '', '| Mnemonic | Opcode id | Count | State |',
           '|---|---|---|---|']
    md += [f'| `{i["mnemonic"]}` | {i["id"]} | {i["count"]:,} | {i["state"]} |' for i in r['instructions']]
    md += ['', '</details>', '']
    return '\n'.join(md)


def comparison(reports):
    names = [r['name'] for r in reports]
    n = len(names)
    md = [f'# Coverage comparison: {" vs ".join(names)}', '',
          f'Generated by `tools/coverage.py --compare` on {date.today().isoformat()}. What each title needs from the '
          'layer, and what only one of them needs: the "only" tables are the work the layer must add for the next '
          'title.', '', '## Summary', '', '| | ' + ' | '.join(names) + ' |', '|---|' + '---|' * n]

    def row(label, values):
        md.append(f'| {label} | ' + ' | '.join(str(v) for v in values) + ' |')

    row('SDK the report was made against', [r['sdk']['version'] for r in reports])
    row('Recompiled functions', [f'{r["functions"]["total"]:,}' for r in reports])
    row('Functions replaced natively by the port', [r['functions']['hooked'] for r in reports])
    row('Imports', [len(r['imports']) for r in reports])
    for state in ('implemented', 'variable', 'stub', 'missing'):
        row(f'&nbsp;&nbsp;{state}', [sum(1 for c in r['imports'] if c['state'] == state) for r in reports])
    row('Distinct mnemonics', [len(r['instructions']) for r in reports])
    row('Mnemonics not handled', [sum(1 for i in r['instructions'] if i['state'] == 'unhandled') for r in reports])
    row('Instructions (static)', [f'{sum(i["count"] for i in r["instructions"]):,}' for r in reports])
    row('VMX / VMX128 share', [f'{r["families"].get("VMX / VMX128", 0) / max(sum(r["families"].values()), 1) * 100:.1f}%'
                               for r in reports])

    imports = [{c['name']: c for c in r['imports']} for r in reports]
    all_names = sorted(set().union(*imports))
    common = [x for x in all_names if all(x in m for m in imports)]

    def only(i, maps):
        return [x for x in sorted(set().union(*maps)) if x in maps[i] and not any(x in m for j, m in enumerate(maps) if j != i)]

    md += ['', '## Imports', '', f'{len(common)} imports in common, ' +
           ', '.join(f'{len(only(i, imports))} only in {names[i]}' for i in range(n)) + '.', '']
    for i in range(n):
        items = only(i, imports)
        md += [f'### Only {names[i]} ({len(items)})', '', '| Library | Import | State |', '|---|---|---|']
        md += [f'| {imports[i][x]["library"]} | `{x}` | {imports[i][x]["state"]} |' for x in items]
        md.append('')
    md += ['### Stubs and missing imports either title needs', '', '| Import | ' + ' | '.join(names) + ' |',
           '|---|' + '---|' * n]
    for x in all_names:
        states = [m[x]['state'] if x in m else '-' for m in imports]
        if any(s in ('stub', 'missing') for s in states):
            md.append(f'| `{x}` | ' + ' | '.join(states) + ' |')

    mnems = [{i['mnemonic']: i for i in r['instructions']} for r in reports]
    md += ['', '## Instructions', '']
    for i in range(n):
        items = only(i, mnems)
        md += [f'### Mnemonics only {names[i]} uses ({len(items)})', '', '| Mnemonic | Count | State |', '|---|---|---|']
        md += [f'| `{m}` | {mnems[i][m]["count"]} | {mnems[i][m]["state"]} |' for m in items]
        md.append('')
    unhandled = sorted({m for d in mnems for m, i in d.items() if i['state'] == 'unhandled'})
    md += ['### Mnemonics the recompiler does not handle, either title', '']
    if unhandled:
        md += ['| Mnemonic | ' + ' | '.join(names) + ' |', '|---|' + '---|' * n]
        md += [f'| `{m}` | ' + ' | '.join(str(d[m]['count']) if m in d else '-' for d in mnems) + ' |' for m in unhandled]
    else:
        md.append('None.')
    md += ['', '## Families', '', '| Family | ' + ' | '.join(names) + ' |', '|---|' + '---|' * n]
    fams = sorted(set().union(*(r['families'] for r in reports)),
                  key=lambda f: -sum(r['families'].get(f, 0) for r in reports))
    for f in fams:
        md.append(f'| {f} | ' + ' | '.join(
            f'{r["families"].get(f, 0):,} ({r["families"].get(f, 0) / max(sum(r["families"].values()), 1) * 100:.1f}%)'
            for r in reports) + ' |')
    return '\n'.join(md) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--name', default='game')
    parser.add_argument('--generated', type=Path)
    parser.add_argument('--sdk', type=Path)
    parser.add_argument('--port-src', type=Path)
    parser.add_argument('--compare-sdk', type=Path)
    parser.add_argument('--compare', nargs='+', type=Path, help='coverage JSON files to compare')
    parser.add_argument('--out', type=Path, default=Path(__file__).resolve().parent.parent / 'docs' / 'coverage')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    if args.compare:
        reports = [json.loads(p.read_text(encoding='utf-8')) for p in args.compare]
        path = args.out / ('COMPARISON-' + '-vs-'.join(r['name'] for r in reports) + '.md')
        path.write_text(comparison(reports), encoding='utf-8')
        print(f'wrote {path}')
        return
    if not args.generated or not args.sdk:
        parser.error('--generated and --sdk are required (or --compare)')
    report = coverage(args.name, args.generated, args.sdk, args.port_src, args.compare_sdk)
    (args.out / f'{args.name}.json').write_text(json.dumps(report, indent=1) + '\n', encoding='utf-8')
    (args.out / f'{args.name}.md').write_text(markdown(report), encoding='utf-8')
    counts = Counter(c['state'] for c in report['imports'])
    unhandled = [i['mnemonic'] for i in report['instructions'] if i['state'] == 'unhandled']
    print(f'{report["name"]}: {report["functions"]["total"]} functions ({report["functions"]["hooked"]} hooked), '
          f'{len(report["imports"])} imports ({counts["implemented"]} implemented, {counts["variable"]} variables, '
          f'{counts["stub"]} stubs, {counts["missing"]} missing), {len(report["instructions"])} mnemonics '
          f'({len(unhandled)} unhandled: {", ".join(unhandled[:10])}), unknown tokens: '
          f'{list(report["unknown_tokens"])[:8]}; wrote {args.out / (report["name"] + ".md")}')


if __name__ == '__main__':
    main()
