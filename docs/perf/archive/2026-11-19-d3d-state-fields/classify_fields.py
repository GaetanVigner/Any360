# Classifies the counted D3D functions into the field shapes (strict: every instruction matched, else rejected) and
# prints each with its operands. Usage: python -I classify_fields.py <crackdown checkout>
import glob
import json
import re
import sys

S = r'C:/Users/Gaetan/AppData/Local/Temp/claude/4b3779b33b6aeead/c1219ef0-d3b6-41af-9cc5-9e73d9171756/scratchpad/'
root = sys.argv[1]
body = {}
for f in glob.glob(root + '/generated/default/crackdown_recomp.*.cpp'):
    t = open(f, encoding='utf-8', errors='replace').read()
    for m in re.finditer(r'DEFINE_REX_FUNC\(sub_(826[0-9A-F]{5})\) \{(.*?)\n\}', t, re.S):
        body[m.group(1)] = [l.strip()[3:].strip() for l in m.group(2).split('\n') if l.strip().startswith('// ')]
counts = {l.split()[0].upper(): int(l.split()[1]) for l in open(S + 'call_counts_d3d.txt')}

N = r'(-?\d+)'


def dirty_ops(ins, reg_off):
    """The dirty tail: ld r11,O(r3) then ori/oris/(li r12,1 + rldicr r12,r12,N,63 before) or + std(s), blr.
    Returns (offset, bits) or None."""
    return None


def match_dirty(lines):
    # Accepts: [li r12,1 ...] ld r11,O(r3) | (ori r11,r11,X | oris r11,r11,X | or r11,r11,r12) | std r11,O(r3) ... blr
    bits = 0
    off = None
    r12 = None
    i = 0
    out = []
    while i < len(lines):
        l = lines[i]
        m = re.fullmatch(r'ld r11,(\d+)\(r3\)', l)
        if m:
            off = int(m.group(1)); i += 1; continue
        m = re.fullmatch(r'ori r11,r11,(\d+)', l)
        if m and off is not None:
            bits |= int(m.group(1)); i += 1; continue
        m = re.fullmatch(r'oris r11,r11,(\d+)', l)
        if m and off is not None:
            bits |= int(m.group(1)) << 16; i += 1; continue
        if l == 'or r11,r11,r12' and off is not None and r12 is not None:
            bits |= r12; i += 1; continue
        m = re.fullmatch(r'std r11,(\d+)\(r3\)', l)
        if m and off is not None and int(m.group(1)) == off:
            i += 1; continue
        if l == 'blr' and i == len(lines) - 1 and off is not None:
            return off, bits
        return None
    return None


def strip_r12(lines):
    # li r12,1 and rldicr r12,r12,N,63 anywhere before the or: the bit 1 << N.
    r12 = None
    rest = []
    for l in lines:
        if l == 'li r12,1':
            r12 = 1; continue
        m = re.fullmatch(r'rldicr r12,r12,(\d+),63', l)
        if m and r12 == 1:
            r12 = 1 << int(m.group(1)); continue
        rest.append(l)
    return r12, rest


def classify(a, b):
    r12, rest = strip_r12(b)
    j = ' | '.join(rest)

    def tail(k):
        global_r12[0] = r12
        lines = rest[k:]
        # emulate 'or r11,r11,r12'
        bits = 0; off = None; i = 0
        while i < len(lines):
            l = lines[i]
            m = re.fullmatch(r'ld r11,(\d+)\(r3\)', l)
            if m: off = int(m.group(1)); i += 1; continue
            m = re.fullmatch(r'ori r11,r11,(\d+)', l)
            if m and off is not None: bits |= int(m.group(1)); i += 1; continue
            m = re.fullmatch(r'oris r11,r11,(\d+)', l)
            if m and off is not None: bits |= int(m.group(1)) << 16; i += 1; continue
            if l == 'or r11,r11,r12' and off is not None and r12: bits |= r12; i += 1; continue
            if l == 'or r11,r10,r11' and off is not None and False: pass
            m = re.fullmatch(r'std r11,(\d+)\(r3\)', l)
            if m and off is not None and int(m.group(1)) == off: i += 1; continue
            if l == 'blr' and i == len(lines) - 1 and off is not None: return off, bits
            return None
        return None
    # Device word, rlwimi insert.
    m = re.fullmatch(r'lwz r11,(\d+)\(r3\)', rest[0]) if rest else None
    if m and len(rest) >= 3:
        w = int(m.group(1))
        m2 = re.fullmatch(r'rlwimi r11,r4,(\d+),(\d+),(\d+)', rest[1])
        if m2 and rest[2] == 'stw r11,%d(r3)' % w:
            t = tail(3)
            if t: return dict(shape='insert', word=w, sh=int(m2.group(1)), mb=int(m2.group(2)), me=int(m2.group(3)), dirty_at=t[0], dirty=t[1])
        m2 = re.fullmatch(r'rlwinm r11,r11,0,(\d+),(\d+)', rest[1])
        if m2 and rest[2] == 'or r11,r11,r4' and rest[3] == 'stw r11,%d(r3)' % w:
            t = tail(4)
            if t: return dict(shape='or_all', word=w, mb=int(m2.group(1)), me=int(m2.group(2)), dirty_at=t[0], dirty=t[1])
        m2 = re.fullmatch(r'rlwinm r3,r11,(\d+),(\d+),(\d+)', rest[1])
        if m2 and rest[2:] == ['blr']:
            return dict(shape='get', word=w, sh=int(m2.group(1)), mb=int(m2.group(2)), me=int(m2.group(3)))
    m = re.fullmatch(r'lwz r10,(\d+)\(r3\)', rest[0]) if rest else None
    if m and len(rest) > 5:
        w = int(m.group(1))
        m2 = re.fullmatch(r'rlwinm r11,r4,(\d+),(\d+),(\d+)', rest[1])
        m3 = re.fullmatch(r'rlwinm r10,r10,0,(\d+),(\d+)', rest[2])
        if m2 and m3 and rest[3] == 'or r11,r10,r11' and rest[4] == 'stw r11,%d(r3)' % w:
            t = tail(5)
            if t: return dict(shape='or_rotated', word=w, sh=int(m2.group(1)), mb=int(m2.group(2)), me=int(m2.group(3)),
                              kmb=int(m3.group(1)), kme=int(m3.group(2)), dirty_at=t[0], dirty=t[1])
    m = re.fullmatch(r'stw r4,(\d+)\(r3\)', rest[0]) if rest else None
    if m:
        t = tail(1)
        if t: return dict(shape='store', word=int(m.group(1)), dirty_at=t[0], dirty=t[1])
    m = re.fullmatch(r'stb r4,(\d+)\(r3\)', rest[0]) if rest else None
    if m:
        t = tail(1)
        if t: return dict(shape='store_byte', at=int(m.group(1)), dirty_at=t[0], dirty=t[1])
    m = re.fullmatch(r'lwz r3,(\d+)\(r3\)', rest[0]) if rest else None
    if m and rest[1:] == ['blr']:
        return dict(shape='get_word', word=int(m.group(1)))
    m = re.fullmatch(r'add r11,r3,r4', rest[0]) if rest else None
    if m and len(rest) == 3:
        m2 = re.fullmatch(r'lbz r3,(\d+)\(r11\)', rest[1])
        if m2 and rest[2] == 'blr': return dict(shape='get_stage_byte', at=int(m2.group(1)))
    # Sampler: stage word get.
    if rest[:2] == ['mulli r11,r4,24', 'add r11,r11,r3'] and len(rest) == 5:
        m2 = re.fullmatch(r'lwz r11,(\d+)\(r11\)', rest[2]); m3 = re.fullmatch(r'rlwinm r3,r11,(\d+),(\d+),(\d+)', rest[3])
        if m2 and m3 and rest[4] == 'blr':
            return dict(shape='stage_get', at=int(m2.group(1)), sh=int(m3.group(1)), mb=int(m3.group(2)), me=int(m3.group(3)))
        m2 = re.fullmatch(r'lhz r11,(\d+)\(r11\)', rest[2]); m3 = re.fullmatch(r'clrlwi r3,r11,(\d+)', rest[3])
        if m2 and m3 and rest[4] == 'blr':
            return dict(shape='stage_get_half', at=int(m2.group(1)), clr=int(m3.group(1)))
    if rest[:2] == ['mulli r11,r4,24', 'add r11,r11,r3'] and len(rest) == 7:
        m2 = re.fullmatch(r'lwz r11,(\d+)\(r11\)', rest[2]); m3 = re.fullmatch(r'clrlwi r11,r11,(\d+)', rest[3])
        if m2 and m3 and rest[4:] == ['subfic r11,r11,0', 'subfe r3,r11,r11', 'blr']:
            return dict(shape='stage_get_mask', at=int(m2.group(1)), clr=int(m3.group(1)))
    # Sampler setters: the prologue then lwz r9,O(r11) | rlwimi r9,r5,SH,MB,ME | stw r9,O(r11) | ld r11,24(r3) | or r11,r10,r11 | std r11,24(r3) | blr
    pro = ['addi r10,r4,32', 'li r9,1', 'clrldi r10,r10,32', 'rldicr r9,r9,63,63', 'mulli r11,r4,24', 'add r11,r11,r3']
    if rest[:6] == pro:
        k = 6; base = 0
        if rest[k] == 'addi r11,r11,1024': base = 1024; k += 1
        if rest[k] == 'srd r10,r9,r10':
            k += 1
            m2 = re.fullmatch(r'lwz r9,(\d+)\(r11\)', rest[k]); m3 = re.fullmatch(r'rlwimi r9,r5,(\d+),(\d+),(\d+)', rest[k + 1])
            if m2 and m3 and rest[k + 2] == 'stw r9,%s(r11)' % m2.group(1) and rest[k + 3:] == ['ld r11,24(r3)', 'or r11,r10,r11', 'std r11,24(r3)', 'blr']:
                return dict(shape='stage_insert', at=base + int(m2.group(1)), sh=int(m3.group(1)), mb=int(m3.group(2)), me=int(m3.group(3)))
    if rest[:6] == pro and rest[6:] == ['cntlzw r8,r5', 'addi r11,r11,1024', 'rlwinm r8,r8,27,31,31', 'xori r8,r8,1', 'srd r10,r9,r10',
                                        'lwz r9,20(r11)', 'rlwinm r9,r9,0,0,29', 'or r9,r8,r9', 'stw r9,20(r11)', 'ld r11,24(r3)',
                                        'or r11,r10,r11', 'std r11,24(r3)', 'blr']:
        return dict(shape='stage_bool_low2', at=1044)
    return None


global_r12 = [None]
names = json.load(open(r'F:/Crackdown/any360-amd/titles/crackdown/d3d_names.json', encoding='utf-8'))
found = []
for a, n in sorted(counts.items(), key=lambda x: -x[1]):
    c = classify(a, body.get(a, []))
    if c:
        c['address'] = a; c['calls'] = n
        found.append(c)
json.dump(found, open(S + 'fields.json', 'w'), indent=0)
shapes = {}
for c in found:
    shapes[c['shape']] = shapes.get(c['shape'], 0) + 1
    print(c['address'], c['calls'], {k: v for k, v in c.items() if k not in ('address', 'calls')})
print(len(found), shapes, sum(c['calls'] for c in found))
