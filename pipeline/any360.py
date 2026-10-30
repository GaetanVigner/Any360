#!/usr/bin/env python3
"""Any360 pipeline: decompile an Xbox 360 title's functions and recompile the game with native replacements.

Works on any ReXGlue recomp (a title directory with a manifest, generated code and a decrypted image) and is driven
by one JSON file per title, titles/<title>/pipeline.json:

  {"image": "<decrypted XEX image (.bin)>", "image_base": "0x82000000", "generated": "<recomp>/generated/default",
   "port_src": "<recomp>/src", "golden": "<recomp>/tests/golden", "bind_macro": "CRACKDOWN_BIND",
   "ghidra": "C:/Tools/ghidra_12.1.4_PUBLIC", "language": "PowerPC:BE:64:A2ALT-32addr",
   "project_dir": "<where the Ghidra project lives>", "decomp_dir": "<recomp>/src/decomp"}

Steps (each one a subcommand, each idempotent):
  inventory   every function with its state (tools/inventory.py) and the system calls (tools/coverage.py)
  functions   the function list Ghidra seeds from (address + name per line), from the generated code
  import      create the Ghidra project: import the image at its base, seed the recompiler's functions, auto-analyse
  decompile   write the decompiled C of the given functions (addresses, sub_ names, or a module name from the
              inventory) to <decomp_dir>/raw/<address>.c, ready for clean-up with the recovered types
  bind        write the binding skeleton for the given functions: ANY360_BIND(address, handler) stubs that call the
              original until the decompiled body replaces it, one file per module, plus the golden list entry
  verify      run the title's sandbox golden tests (the recomp's test target) so a bound function is proven equal
  status      print where the title stands (functions native / verified / library / recompiled)

  python pipeline/any360.py <title> inventory
  python pipeline/any360.py <title> import            (long: minutes to an hour for a 20 MB image)
  python pipeline/any360.py <title> decompile sub_826B9C08 826C3750 "XDK D3D runtime"
  python pipeline/any360.py <title> bind 826B9C08 --module renderer
  python pipeline/any360.py <title> status

Nothing of the game is written into this repository: the image, the Ghidra project and the decompiled C live where
titles/<title>/pipeline.json points, in the title's own checkout.
"""
import argparse
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / 'tools'
GHIDRA_SCRIPTS = ROOT / 'pipeline' / 'ghidra'
PYTHON = sys.executable


def load(title):
    path = ROOT / 'titles' / title / 'pipeline.json'
    if not path.exists():
        sys.exit(f'no {path}: describe the title first (see the module docstring)')
    cfg = json.loads(path.read_text(encoding='utf-8'))
    cfg['title'] = title
    for key in ('image', 'generated', 'port_src', 'golden', 'project_dir', 'decomp_dir', 'ghidra'):
        if key in cfg:
            cfg[key] = Path(os.path.expandvars(cfg[key]))
    return cfg


def run(cmd, **kw):
    print('+', ' '.join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, **kw)


def headless(cfg):
    exe = cfg['ghidra'] / 'support' / ('analyzeHeadless.bat' if os.name == 'nt' else 'analyzeHeadless')
    if not exe.exists():
        sys.exit(f'Ghidra not found at {exe}')
    cfg['project_dir'].mkdir(parents=True, exist_ok=True)
    return [exe, cfg['project_dir'], cfg.get('project_name', cfg['title'])]


# --- steps ---------------------------------------------------------------------------------------------------------

def step_inventory(cfg, args):
    run([PYTHON, '-I', TOOLS / 'inventory.py', '--name', cfg['title'], '--generated', cfg['generated'],
         '--port-src', cfg['port_src']] + (['--golden', cfg['golden']] if cfg.get('golden') else []))
    if cfg.get('sdk'):
        run([PYTHON, '-I', TOOLS / 'coverage.py', '--name', cfg['title'], '--generated', cfg['generated'],
             '--sdk', cfg['sdk'], '--port-src', cfg['port_src']])


def functions_file(cfg):
    """<project_dir>/functions.txt: '<hex address> sub_<address>' per recompiled function, from the generated code."""
    cfg['project_dir'].mkdir(parents=True, exist_ok=True)
    out = cfg['project_dir'] / 'functions.txt'
    addresses = set()
    for path in cfg['generated'].glob('*_recomp.*.cpp'):
        addresses |= {int(a, 16) for a in re.findall(r'DEFINE_REX_FUNC\(sub_([0-9A-Fa-f]{8})\)', path.read_text(encoding='utf-8', errors='replace'))}
    # Call targets that are not functions of the generated code: the XDK's register save/restore helpers
    # (__savegprlr_N, __restfpr_N...), which the codegen emits as helper calls. Seeded as returning functions.
    helpers = set()
    for path in cfg['generated'].glob('*_recomp.*.cpp'):
        helpers |= {int(a, 16) for a in re.findall(r'// bl 0x([0-9a-fA-F]{8})', path.read_text(encoding='utf-8', errors='replace'))}
    helpers -= addresses
    out.write_text(''.join(f'{a:08X} sub_{a:08X}\n' for a in sorted(addresses))
                   + ''.join(f'{a:08X} helper_{a:08X}\n' for a in sorted(helpers)), encoding='utf-8')
    print(f'{len(addresses)} functions + {len(helpers)} helpers -> {out}')
    return out


def step_functions(cfg, args):
    functions_file(cfg)


def step_import(cfg, args):
    """Import the decrypted image into a Ghidra project, seed the recompiler's functions and auto-analyse it."""
    functions = functions_file(cfg)
    cmd = headless(cfg) + ['-import', cfg['image'], '-loader', 'BinaryLoader', '-loader-baseAddr', cfg.get('image_base', '0x82000000'),
                           '-processor', cfg.get('language', 'PowerPC:BE:64:A2ALT-32addr'),
                           '-scriptPath', GHIDRA_SCRIPTS, '-preScript', 'SeedFunctions.java', functions,
                           '-max-cpu', str(cfg.get('max_cpu', os.cpu_count() or 4))]
    if args.no_analysis:
        cmd.append('-noanalysis')
    run(cmd)


def resolve_targets(cfg, names):
    """Addresses from sub_ names, hex, or module names of the inventory."""
    targets = []
    inventory = json.loads((ROOT / 'docs' / 'data' / f'functions-{cfg["title"]}.json').read_text(encoding='utf-8'))['rows'] \
        if (ROOT / 'docs' / 'data' / f'functions-{cfg["title"]}.json').exists() else []
    modules = json.loads((ROOT / 'titles' / cfg['title'] / 'modules.json').read_text(encoding='utf-8'))['modules'] \
        if (ROOT / 'titles' / cfg['title'] / 'modules.json').exists() else []
    for name in names:
        m = re.fullmatch(r'(?:sub_|0x)?([0-9A-Fa-f]{8})', name)
        if m:
            targets.append(int(m.group(1), 16))
            continue
        module = next((x for x in modules if x['name'] == name), None)
        if module is None:
            sys.exit(f'{name}: neither an address nor a module of titles/{cfg["title"]}/modules.json')
        start, end = int(module['start'], 16), int(module['end'], 16)
        targets += [int(r[0], 16) for r in inventory if start <= int(r[0], 16) < end]
    return sorted(set(targets))


def step_decompile(cfg, args):
    targets = resolve_targets(cfg, args.functions)
    raw = cfg['decomp_dir'] / 'raw'
    raw.mkdir(parents=True, exist_ok=True)
    out = raw / (f'{targets[0]:08X}.c' if len(targets) == 1 else f'batch-{targets[0]:08X}-{targets[-1]:08X}.c')
    run(headless(cfg) + ['-process', cfg['image'].name, '-noanalysis', '-scriptPath', GHIDRA_SCRIPTS,
                         '-postScript', 'Decompile.java', out] + [f'{t:08X}' for t in targets])
    print(f'{len(targets)} functions -> {out}')


BIND_TEMPLATE = '''// {title}: native replacements of module "{module}" (Any360 pipeline, stage 2). Each binding takes over the
// recompiled function at its address; until the decompiled body replaces the call to `original`, behaviour is unchanged.
#include "any360/bind.h"

namespace {title}::decomp::{module_id} {{

{handlers}
}}  // namespace {title}::decomp::{module_id}

{bindings}'''

HANDLER_TEMPLATE = '''// sub_{address}: {purpose}
void {handler}(PPCContext& ctx, uint8_t* base, PPCFunc* original) {{
  // TODO(stage 2): replace with the decompiled body ({raw}); keep calling `original` until the golden test passes.
  original(ctx, base);
}}
'''


def step_bind(cfg, args):
    targets = resolve_targets(cfg, args.functions)
    module = args.module or 'misc'
    module_id = re.sub(r'\W+', '_', module).strip('_').lower()
    macro = cfg.get('bind_macro', 'ANY360_BIND')
    handlers, bindings = [], []
    for t in targets:
        handler = f'Sub{t:08X}'
        handlers.append(HANDLER_TEMPLATE.format(address=f'{t:08X}', handler=handler, purpose=args.purpose or 'to be named',
                                                raw=f'raw/{t:08X}.c'))
        bindings.append(f'{macro}({t:08X}, {module_id}::{handler})')
    out_dir = cfg['decomp_dir'] / module_id
    out_dir.mkdir(parents=True, exist_ok=True)
    out = out_dir / f'{module_id}_bindings.cpp'
    out.write_text(BIND_TEMPLATE.format(title=cfg['title'], module=module, module_id=module_id, handlers='\n'.join(handlers),
                                        bindings='\n'.join(bindings) + '\n'), encoding='utf-8')
    goldens = cfg['decomp_dir'] / 'goldens.txt'
    with goldens.open('a', encoding='utf-8') as f:
        for t in targets:
            f.write(f'{t:08X} {module_id}\n')
    print(f'{len(targets)} bindings -> {out}; golden list {goldens}')


def step_verify(cfg, args):
    if not cfg.get('verify_command'):
        sys.exit('no verify_command in pipeline.json (the title\'s sandbox golden test command)')
    run(cfg['verify_command'], shell=True)


def step_status(cfg, args):
    path = ROOT / 'docs' / 'coverage' / f'inventory-{cfg["title"]}.json'
    if not path.exists():
        sys.exit('run inventory first')
    inv = json.loads(path.read_text(encoding='utf-8'))
    print(f'{cfg["title"]}: {inv["functions"]:,} functions, {inv["native"]} native, {inv["verified"]} verified, '
          f'{inv["library"]} in library ranges, {inv["functions"] - inv["native"] - inv["verified"] - inv["library"]:,} recompiled')
    for name, m in list(inv['modules'].items())[:12]:
        done = m['native'] + m['verified'] + (m['functions'] - m['native'] - m['verified'] if m['kind'] == 'library' else 0)
        print(f'  {name:40s} {m["functions"]:6d} functions {done / m["functions"] * 100:5.1f}% done')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('title')
    sub = parser.add_subparsers(dest='step', required=True)
    sub.add_parser('inventory')
    sub.add_parser('functions')
    p = sub.add_parser('import')
    p.add_argument('--no-analysis', action='store_true')
    p = sub.add_parser('decompile')
    p.add_argument('functions', nargs='+')
    p = sub.add_parser('bind')
    p.add_argument('functions', nargs='+')
    p.add_argument('--module')
    p.add_argument('--purpose')
    sub.add_parser('verify')
    sub.add_parser('status')
    args = parser.parse_args()
    cfg = load(args.title)
    globals()[f'step_{args.step}'](cfg, args)


if __name__ == '__main__':
    main()
