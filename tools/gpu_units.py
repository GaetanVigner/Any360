"""The GPU half of the platform surface, unit by unit: what a native port of any Xbox 360 game needs from the Xenos
GPU, enumerated from the hardware's own lists (PM4 opcodes, shader microcode opcodes, primitive types, vertex /
texture / render target formats) plus the render features a renderer implements. Each unit gets a state:

  native     any360::gfx handles it and a test checks it (PM4 opcodes: handled by layer/gfx's command processor,
             which gfx_cp_tests checks against the reference semantics; everything else: listed in
             layer/gfx/native.json, each entry exercised by gfx_render_tests or gfx_shader_tests)
  reference  the reference GPU (ReXGlue's, Xenia's) handles it; any360::gfx inherits it (the vendored translator)
             or will take it, but no Any360 test exercises it yet
  missing    neither does

The enums come from the vendored headers in layer/gfx/third_party/xenos_shader (public hardware definitions, no
game data). Used by tools/build_db.py (db/gpu.json) and tools/progress_page.py.
"""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
XS = ROOT / 'layer' / 'gfx' / 'third_party' / 'xenos_shader' / 'include' / 'rex' / 'graphics'
GFX = ROOT / 'layer' / 'gfx'

# Render features a Xenos renderer provides, beyond the enumerable lists (registers and paths the reference reads).
FEATURES = [
    'viewport transform', 'window offset', 'window scissor', 'screen scissor', 'depth test', 'depth write',
    'stencil front', 'stencil back', 'blending', 'blend constants', 'colour write mask', 'alpha test',
    'alpha to coverage', 'colour exponent bias', 'culling', 'front face', 'wireframe fill', 'polygon offset',
    'user clip planes', 'point sprites', 'primitive restart', 'multiple render targets', 'MSAA 2x', 'MSAA 4x',
    'render targets from RB_* registers', 'gamma render targets', 'float24 depth', 'textures (2D)', 'textures (3D)',
    'cube textures', 'texture arrays', 'texture filtering', 'anisotropic filtering', 'texture tiling (untile)',
    'mip maps', 'vertex textures', 'resolve colour', 'resolve depth', 'resolve with clear', 'resolve to guest memory',
    'occlusion queries', 'memexport', 'tessellation', 'interpolators', 'centroid interpolation', 'swap / present',
    'gamma ramp', 'vertex fetch (shared memory)', 'index buffers (DMA)', 'auto indices', 'immediate indices',
    'shader constants (float)', 'shader constants (bool / loop)', 'dynamic constant addressing', 'predication (bins)',
]


def parse_enum(path, name):
    """[(member, value)] of `enum [class] name [: type] { ... }` in a header (values as written, or counted)."""
    text = path.read_text(encoding='utf-8', errors='replace')
    m = re.search(r'enum\s+(?:class\s+)?' + re.escape(name) + r'\b[^{;]*\{(.*?)\};', text, re.S)
    if not m:
        return []
    body = re.sub(r'//[^\n]*|/\*.*?\*/', '', m.group(1), flags=re.S)
    out, next_value = [], 0
    for item in body.split(','):
        item = item.strip()
        if not item or item.startswith('#'):
            continue
        im = re.match(r'(\w+)\s*(?:=\s*([^,]+))?$', item)
        if not im:
            continue
        member, value = im.group(1), im.group(2)
        if value is not None:
            try:
                next_value = int(value.strip().replace("'", ''), 0)
            except ValueError:
                aliased = dict(out).get(value.strip())
                if aliased is None:
                    continue  # an expression we do not evaluate
                next_value = aliased
        out.append((member, next_value))
        next_value += 1
    return out


def unique_by_value(members):
    """Aliases (two names, one value) count once: the first name."""
    seen, out = set(), []
    for name, value in members:
        if value in seen:
            continue
        seen.add(value)
        out.append((name, value))
    return out


def reference_pm4(reference_sdk):
    """Type-3 opcodes the reference command processor handles: its ExecutePacketType3_X methods and the opcodes its
    switch handles inline."""
    handled = {'SET_BIN_MASK_LO', 'SET_BIN_MASK_HI', 'SET_BIN_SELECT_LO', 'SET_BIN_SELECT_HI', 'SET_BIN_MASK',
               'SET_BIN_SELECT', 'CONTEXT_UPDATE', 'WAIT_FOR_IDLE'}
    header = Path(reference_sdk) / 'include' / 'rex' / 'graphics' / 'command_processor.h' if reference_sdk else None
    if header and header.exists():
        for m in re.finditer(r'ExecutePacketType3_(\w+)\(', header.read_text(encoding='utf-8', errors='replace')):
            handled.add(m.group(1))
        handled.add('INDIRECT_BUFFER_PFD')  # same handler as INDIRECT_BUFFER
    return handled


def native_pm4():
    """Type-3 opcodes layer/gfx's command processor handles (each `case pm4::X:` of its packet switch)."""
    src = GFX / 'src' / 'command_processor.cpp'
    return set(re.findall(r'case pm4::(\w+):', src.read_text(encoding='utf-8'))) if src.exists() else set()


def build(reference_sdk=None):
    native_list = set()
    native_path = GFX / 'native.json'
    if native_path.exists():
        native_list = set(json.loads(native_path.read_text(encoding='utf-8')).get('gpu', []))
    rows = []

    def add(family, name, value, reference, native):
        unit = f'{family}:{name}'
        state = 'native' if native or unit in native_list else 'reference' if reference else 'missing'
        rows.append({'id': unit, 'name': name, 'family': family, 'value': value, 'state': state})

    ref_pm4, our_pm4 = reference_pm4(reference_sdk), native_pm4()
    for name in ('type 0 (register writes)', 'type 1 (two registers)', 'type 2 (no-op)'):
        add('command processor', name, None, True, bool(our_pm4))
    for member, value in unique_by_value(parse_enum(XS / 'xenos.h', 'Type3Opcode')):
        op = member[len('PM4_'):] if member.startswith('PM4_') else member
        add('command processor', op, value, op in ref_pm4, op in our_pm4)
    # Shader microcode: the vendored translator (Xenia's) handles every opcode; native once a test runs it.
    for family, enum in (('shader control flow', 'ControlFlowOpcode'), ('shader ALU vector', 'AluVectorOpcode'),
                         ('shader ALU scalar', 'AluScalarOpcode'), ('shader fetch', 'FetchOpcode')):
        for member, value in unique_by_value(parse_enum(XS / 'format' / 'ucode.h', enum)):
            add(family, member, value, True, False)
    for family, enum in (('primitive types', 'PrimitiveType'), ('vertex formats', 'VertexFormat'),
                         ('texture formats', 'TextureFormat'), ('render target formats', 'ColorRenderTargetFormat'),
                         ('render target formats', 'DepthRenderTargetFormat')):
        for member, value in unique_by_value(parse_enum(XS / 'xenos.h', enum)):
            if member in ('kNone', 'kUnknown') or member.startswith('kCount'):
                continue
            add(family, member, value, True, False)
    for feature in FEATURES:
        add('render features', feature, None, True, False)
    unknown = native_list - {r['id'] for r in rows}
    if unknown:
        raise SystemExit(f'layer/gfx/native.json names units that do not exist: {sorted(unknown)}')
    return rows
