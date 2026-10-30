#!/usr/bin/env python3
"""Builds the Any360 database: every function of the Xbox 360 platform surface a native port has to provide, whatever
the title, with its state and the known titles that use it. Three tables, written to db/:

  db/system_calls.json  every export of xboxkrnl.exe, xam.xex and xbdm.dll the reference runtime knows (name, library,
                        ordinal, kind, group), the reference's state (implemented / stub / missing in ReXGlue), the
                        Any360 state (native in this repository, ported in a title's repository, todo) and the titles
                        importing it
  db/instructions.json  every PowerPC / VMX128 instruction id of the recompiler's table, its family, whether the
                        reference recompiler handles it and the titles that execute it
  db/xdk.json           the statically linked XDK library functions known by name (D3D, CRT, XAudio...), with the
                        address each title has them at; grows with signature matching
  db/summary.json       the counts the progress page and README quote

Inputs: the SDK of the first title whose pipeline.json names one (tools/coverage.py reads its tables), the per-title
coverage reports docs/coverage/<title>.json, each title's port sources (for the imports it already provides natively),
db/xdk_seed.json (hand-kept names) and layer/sys/native.json (what Any360 provides itself). Standard library only.

  python tools/build_db.py
"""
import json
import re
import sys
from collections import Counter
from datetime import date
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import coverage  # noqa: E402  (tools/coverage.py: the SDK readers and the instruction families)

DB = ROOT / 'db'
TITLES = ROOT / 'titles'
COVERAGE = ROOT / 'docs' / 'coverage'
NATIVE = ROOT / 'layer' / 'sys' / 'native.json'

# Group of an export the reference does not implement, from its name (the implemented ones take the group of the
# source file that implements them, Xenia's grouping).
KERNEL_GROUPS = [
    ('XMA', 'audio xma'), ('XAudio', 'audio'), ('XeCrypt', 'crypt'), ('XeKeys', 'crypt'), ('Dbg', 'debug'),
    ('Rtl', 'rtl'), ('Mm', 'memory'), ('Ob', 'ob'), ('Io', 'io'), ('Nt', 'io'), ('Fsc', 'io'), ('Vd', 'video'),
    ('Hal', 'hal'), ('Ke', 'threading'), ('Kf', 'threading'), ('Ex', 'misc'), ('Xex', 'module'), ('Hid', 'hid'),
    ('Str', 'strings'), ('Usbcam', 'usbcam'), ('XUsb', 'usbcam'), ('Xe', 'misc'), ('Kd', 'debug'), ('Ps', 'threading'),
    ('Int', 'misc'), ('Ipc', 'misc'), ('Sata', 'io'), ('Smc', 'hal'), ('Stf', 'io'), ('Xpa', 'misc'), ('Vsnprintf', 'strings'),
    ('Nls', 'strings'), ('Kis', 'misc'), ('Mcr', 'misc'), ('Oda', 'misc'), ('Pfp', 'misc'), ('Usbd', 'usb'), ('Mtpd', 'usb'),
    ('XInputd', 'hid'), ('XVoiced', 'hid'), ('Drv', 'hid'), ('Etx', 'net'), ('Nic', 'net'), ('Wifi', 'net'), ('Iptv', 'net'),
    ('Dump', 'debug'), ('Ki', 'debug'), ('Ani', 'video'), ('LDI', 'misc'), ('_', 'strings'), ('Xbox', 'info'),
    ('Mic', 'devices'), ('Svod', 'devices'), ('Rmc', 'devices'), ('Nomnil', 'devices'), ('Mca', 'devices'), ('Detroit', 'devices'),
    ('DevAuth', 'devices'), ('NullCable', 'devices'), ('Ema', 'devices'), ('Tid', 'devices'), ('Dm', 'debug'), ('Title', 'devices'),
    ('Vgc', 'devices'), ('Vvc', 'devices'), ('Ve', 'devices'), ('Rgc', 'misc'),
]
WIN32 = ('Get', 'Set', 'Create', 'Wait', 'Sleep', 'Release', 'Reset', 'Open', 'Duplicate', 'Exit', 'Resume', 'Raise', 'Cancel',
         'System', 'Is', 'Query', 'Local', 'File', 'Debug', 'Close', 'Enter', 'Leave', 'Initialize', 'Delete', 'Try', 'Terminate',
         'Suspend', 'Interlocked', 'Virtual', 'Heap', 'Output', 'Tls', 'Load', 'Free', 'Find', 'Read', 'Write', 'Flush', 'Move',
         'Remove', 'Copy', 'Compare', 'Char', 'lstr', 'wsprintf', 'MultiByte', 'WideChar', 'Format')
XAM_GROUPS = [
    ('NetDll_', 'net'), ('XNet', 'net'), ('XHttp', 'net'), ('XRnm', 'net'), ('XStorage', 'net'), ('XamUser', 'user'),
    ('XUser', 'user'), ('XamContent', 'content'), ('XContent', 'content'), ('XamShow', 'ui'), ('Xui', 'ui'), ('XamInput', 'input'),
    ('XInput', 'input'), ('XamVoice', 'voice'), ('XamNotify', 'notify'), ('XNotify', 'notify'), ('XamEnum', 'enum'),
    ('XamLoader', 'module'), ('XamAvatar', 'avatar'), ('XamParty', 'party'), ('XamTask', 'task'), ('XamVideo', 'video'),
    ('XamNui', 'nui'), ('XamSession', 'live'), ('XSession', 'live'), ('XLive', 'live'), ('XOnline', 'live'), ('XLocator', 'live'),
    ('XMsg', 'msg'), ('XGetLanguage', 'locale'), ('XGetLocale', 'locale'), ('XamGetLocale', 'locale'), ('XGetAVPack', 'video'),
    ('XGetVideo', 'video'), ('XMP', 'media'), ('XMedia', 'media'), ('XAudio', 'audio'), ('XMA', 'audio'), ('XString', 'strings'),
    ('XamTitle', 'info'), ('XamGet', 'info'), ('XamCache', 'misc'), ('XamAlloc', 'misc'), ('XamFree', 'misc'),
    ('XamProfile', 'user'), ('XamUpdate', 'module'), ('XamPackage', 'content'), ('XamIpty', 'ui'), ('XamShutdown', 'misc'),
    ('XamFirst', 'misc'), ('XamInput', 'input'), ('XFriends', 'live'), ('XPresence', 'live'), ('XMarketplace', 'live'),
    ('XamMarketplace', 'live'), ('XamPii', 'user'), ('XamAccount', 'user'), ('XamDevice', 'content device'), ('XamVoiceChat', 'voice'),
    ('XamXnet', 'net'), ('XamPlayTimer', 'misc'), ('XamUsb', 'misc'), ('XamSwap', 'misc'), ('XamBuild', 'misc'), ('XamNavigate', 'ui'),
    ('XamShowPartyUI', 'party'), ('XamReadTile', 'content'), ('XamWriteTile', 'content'), ('XamSetDashContext', 'ui'),
    ('XamGetDashContext', 'ui'), ('XamSetDvdLaunch', 'module'), ('XamIs', 'info'), ('XamFeature', 'info'),
    ('XamLive', 'live'), ('XamVerify', 'crypt'), ('XamApp', 'module'), ('XamSetNuiLaunch', 'nui'), ('XamMute', 'voice'),
    ('XamHud', 'ui'), ('XamSys', 'misc'), ('Rtl', 'rtl'), ('D3D', 'graphics'), ('Controlpack', 'input'), ('g_', 'ui'), ('Curl', 'net'), ('PIX', 'debug'), ('XamBackground', 'background download'), ('XamNetwork', 'net'),
    ('XamFitness', 'fitness'), ('XamPlay', 'media'), ('XamLrc', 'media'), ('XamOffline', 'live'), ('Xamp', 'media'),
    ('XCustom', 'ui'), ('XamWeb', 'net'), ('RtlDebug', 'debug'), ('XamReminder', 'ui'), ('XMem', 'memory'),
    ('XamXlfs', 'file'), ('XGet', 'info'), ('XAutomation', 'debug'), ('EnumerateMedia', 'media'), ('ControlPack', 'input'),
    ('XamMedia', 'media'), ('Upnp', 'net'), ('GamerCard', 'user'), ('XamCert', 'crypt'), ('XOnline', 'live'), ('XG', 'graphics'),
    ('XamInstrumentation', 'debug'), ('XDebug', 'debug'), ('XamFamily', 'user'), ('XamDebug', 'debug'), ('XamDownload', 'content'),
    ('XamPng', 'ui'), ('Xnp', 'net'), ('XamRead', 'content'), ('XamWrite', 'content'), ('XamGet', 'info'), ('XamSet', 'info'),
    ('XamIs', 'info'), ('XamDbg', 'debug'), ('XamDev', 'content device'), ('XamEnable', 'misc'), ('XamCode', 'misc'), ('XamUserGet', 'user'), ('XamUserWrite', 'user'), ('XamUserRead', 'user'),
    ('XamUserCreate', 'user'), ('XamUserIs', 'user'), ('Xam', 'misc'), ('X', 'misc'),
]


def load_json(path, default=None):
    return json.loads(path.read_text(encoding='utf-8')) if path.exists() else default


def titles():
    out = {}
    for cfg_path in sorted(TITLES.glob('*/pipeline.json')):
        cfg = load_json(cfg_path)
        out[cfg_path.parent.name] = {'config': cfg, 'coverage': load_json(COVERAGE / f'{cfg_path.parent.name}.json', {}),
                                     'inventory': load_json(COVERAGE / f'inventory-{cfg_path.parent.name}.json', {})}
    return out


def group_of(name, library, implemented_file):
    if library == 'xbdm':
        return 'debug monitor'
    if implemented_file:
        stem = implemented_file.stem
        for prefix in (library + '_', 'xboxkrnl_', 'xam_', 'xbdm_'):
            if stem.startswith(prefix):
                stem = stem[len(prefix):]
        return stem.replace('_', ' ')
    if library == 'xbdm':
        return 'debug monitor'
    for prefix, group in (KERNEL_GROUPS if library == 'xboxkrnl' else XAM_GROUPS):
        if name.startswith(prefix):
            return group
    if library == 'xam' and name.startswith(WIN32):
        return 'win32'
    return 'other'


def sdk_export_files(sdk):
    """name -> the kernel source file that implements (or stubs) it."""
    kernel = sdk / 'src' / 'kernel'
    files = {}
    for path in kernel.rglob('*.cpp'):
        for _macro, _raw, name in coverage.EXPORT.findall(coverage.read(path)):
            files.setdefault(name, path)
    return files


def port_imports(port_src):
    """The imports a title's port provides itself (REX_HOOK_RAW(__imp__X) or any other use of the import symbol)."""
    names = set()
    if not port_src or not port_src.exists():
        return names
    for path in port_src.rglob('*'):
        if path.suffix in ('.cpp', '.h', '.inc', '.hpp'):
            names |= set(re.findall(r'__imp__([A-Za-z0-9_]+)', coverage.read(path)))
    return names


def build_system_calls(sdk, known, native):
    exports = coverage.sdk_exports(sdk)
    files = sdk_export_files(sdk)
    used = {t: {i['name'] for i in k['coverage'].get('imports', [])} for t, k in known.items()}
    ported = {t: port_imports(Path(k['config']['port_src'])) & used[t] for t, k in known.items()}
    rows = []
    for name, e in exports.items():
        if e['ordinal'] is None and name.startswith(XDK_PREFIXES):
            continue  # a statically linked library function the reference hooks by name: db/xdk.json
        titles_using = sorted(t for t in known if name in used[t])
        state = 'native' if name in native.get('system_calls', []) else \
            'ported' if any(name in ported[t] for t in titles_using) else 'todo'
        rows.append({'name': name, 'library': e['library'], 'ordinal': e['ordinal'], 'kind': e['kind'],
                     'group': group_of(name, e['library'], files.get(name) if e['state'] in ('implemented', 'variable') else None),
                     'reference': 'implemented' if e['state'] in ('implemented', 'variable') else e['state'],
                     'any360': state, 'titles': titles_using})
    # Imports of a known title the reference does not know at all.
    for t in known:
        for name in used[t] - set(exports):
            row = next((r for r in rows if r['name'] == name), None)
            if row:
                row['titles'].append(t)
            else:
                library = 'xam' if name.startswith('X') else 'xboxkrnl'
                rows.append({'name': name, 'library': library, 'ordinal': None, 'kind': 'function',
                             'group': group_of(name, library, None), 'reference': 'missing',
                             'any360': 'ported' if name in ported[t] else 'todo', 'titles': [t]})
    rows.sort(key=lambda r: (r['library'], r['ordinal'] if r['ordinal'] is not None else 1 << 30, r['name']))
    return rows


def build_instructions(sdk, known, native):
    ids, dispatch = coverage.sdk_instructions(sdk)
    mnemonics = {}
    for mnemonic, ident in ids.items():
        mnemonics.setdefault(ident, mnemonic)
    counts = {t: Counter() for t in known}
    for t, k in known.items():
        for item in k['coverage'].get('instructions', []):
            counts[t][item['id']] += item['count']
    rows = []
    for ident, mnemonic in sorted(mnemonics.items()):
        rows.append({'id': ident, 'mnemonic': mnemonic, 'family': coverage.family(mnemonic),
                     'reference': 'handled' if ident in dispatch else 'missing',
                     'any360': 'native' if ident in native.get('instructions', []) else 'todo',
                     'titles': {t: counts[t][ident] for t in known if counts[t][ident]}})
    return rows


XDK_PREFIXES = ('D3D', 'XG', 'XMA', 'XAudio', 'XACT', 'Xui')


def xdk_library(name):
    return 'XAudio' if name.startswith(('XMA', 'XAudio')) else 'XACT' if name.startswith('XACT') else         'XGraphics' if name.startswith('XG') else 'XUI' if name.startswith('Xui') else 'D3D'


def build_xdk(sdk, known, native):
    seed = load_json(DB / 'xdk_seed.json', {'functions': []})
    rows = []
    seeded = {f['name'] for f in seed['functions']}
    for name, e in coverage.sdk_exports(sdk).items():
        if e['ordinal'] is None and name.startswith(XDK_PREFIXES) and name not in seeded:
            rows.append({'name': name, 'library': xdk_library(name), 'any360': 'native' if name in native.get('xdk', []) else
                         'reference' if e['state'] == 'implemented' else 'todo', 'titles': {},
                         'note': 'hooked by name in the reference runtime' if e['state'] == 'implemented' else 'stubbed by name in the reference runtime'})
    for f in seed['functions']:
        rows.append({'name': f['name'], 'library': f['library'], 'any360': 'native' if f['name'] in native.get('xdk', []) else
                     f.get('any360', 'todo'), 'titles': f.get('titles', {}), 'note': f.get('note', '')})
    rows.sort(key=lambda r: (r['library'], r['name']))
    unnamed = {}
    for t, k in known.items():
        unnamed[t] = {n: m['functions'] for n, m in k['inventory'].get('modules', {}).items() if m.get('kind') == 'library'}
    return rows, unnamed


def main():
    known = titles()
    sdk = next((Path(k['config']['sdk']) for k in known.values() if k['config'].get('sdk') and Path(k['config']['sdk']).exists()), None)
    if not sdk:
        sys.exit('no title names an existing SDK (titles/<title>/pipeline.json "sdk")')
    native = load_json(NATIVE, {'system_calls': [], 'instructions': [], 'xdk': []})
    calls = build_system_calls(sdk, known, native)
    instructions = build_instructions(sdk, known, native)
    xdk, unnamed = build_xdk(sdk, known, native)
    DB.mkdir(exist_ok=True)

    def count(rows, key):
        return dict(Counter(r[key] for r in rows))

    summary = {
        'date': date.today().isoformat(), 'reference': {'sdk': 'ReXGlue', 'version': coverage.sdk_version(sdk)},
        'titles': {t: {'imports': len(k['coverage'].get('imports', [])), 'mnemonics': len(k['coverage'].get('instructions', [])),
                       'instructions': sum(i['count'] for i in k['coverage'].get('instructions', [])),
                       'sdk': k['coverage'].get('sdk', {}).get('version', ''), 'unnamed_library_functions': sum(unnamed[t].values())}
                   for t, k in known.items()},
        'system_calls': {'total': len(calls), 'used': sum(1 for r in calls if r['titles']), 'any360': count(calls, 'any360'),
                         'reference': count(calls, 'reference'), 'libraries': dict(Counter(r['library'] for r in calls))},
        'instructions': {'total': len(instructions), 'used': sum(1 for r in instructions if r['titles']),
                         'any360': count(instructions, 'any360'), 'reference': count(instructions, 'reference')},
        'xdk': {'total': len(xdk), 'any360': count(xdk, 'any360'), 'unnamed_by_title': unnamed},
    }
    (DB / 'system_calls.json').write_text(json.dumps(calls, indent=0, separators=(',', ':')) + '\n', encoding='utf-8')
    (DB / 'instructions.json').write_text(json.dumps(instructions, indent=0, separators=(',', ':')) + '\n', encoding='utf-8')
    (DB / 'xdk.json').write_text(json.dumps(xdk, indent=1) + '\n', encoding='utf-8')
    (DB / 'summary.json').write_text(json.dumps(summary, indent=1) + '\n', encoding='utf-8')
    s = summary['system_calls']
    print(f"db: {s['total']} system calls ({s['used']} used by {len(known)} titles; any360 {s['any360']}; reference {s['reference']}); "
          f"{summary['instructions']['total']} instruction ids ({summary['instructions']['reference']}); {len(xdk)} named XDK functions")


if __name__ == '__main__':
    main()
