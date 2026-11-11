#!/usr/bin/env python3
# Replays ambvol.c's engine_bind on the shipped DLLs' bytes: every pattern and RVA in the source, against engine.dll, materialsystem.dll and
# shaderapidx9.dll, without running anything. The in-game check is the log line "ambvol: engine checks passed". Usage: check_engine_patterns.py <bin dir>
import os
import re
import struct
import sys


class PE:
    def __init__(self, path):
        b = self.b = open(path, 'rb').read()
        e = struct.unpack_from('<I', b, 0x3c)[0]
        nsec = struct.unpack_from('<H', b, e + 6)[0]
        opt = struct.unpack_from('<H', b, e + 20)[0]
        self.base = struct.unpack_from('<Q', b, e + 24 + 24)[0]
        self.size = struct.unpack_from('<I', b, e + 24 + 56)[0]
        self.secs = []
        for i in range(nsec):
            o = e + 24 + opt + 40 * i
            vs, va, rs, rp = struct.unpack_from('<IIII', b, o + 8)
            self.secs.append((va, vs, rp, rs))

    def rd(self, va, n):
        rva = va - self.base
        for sva, vs, rp, rs in self.secs:
            if sva <= rva < sva + rs:
                return self.b[rp + rva - sva:rp + rva - sva + n]
        raise ValueError(hex(va))

    def q(self, va):
        return struct.unpack('<Q', self.rd(va, 8))[0]

    def rip(self, code, off_rel, off_next):
        return code + off_next + struct.unpack('<i', self.rd(code + off_rel, 4))[0]


src = open(os.path.join(os.path.dirname(__file__), '../../src/proxy_shaderapi/ambvol.c')).read()


def string_const(name):
    m = re.search(r'static const char ' + name + r'\[\]\s*=\s*((?:"[^"]*"\s*)+);', src)
    assert m, name
    return ''.join(re.findall(r'"([^"]*)"', m.group(1)))


def enum_value(name):
    m = re.search(r'\b' + name + r'\s*=\s*(0x[0-9a-f]+|\d+)', src)
    assert m, name
    return int(m.group(1), 0)


def match(pe, code, pat):
    toks = pat.split()
    data = pe.rd(code, len(toks))
    for i, t in enumerate(toks):
        assert t == '??' or int(t, 16) == data[i], 'byte %d at %#x: %s, code has %02x' % (i, code, t, data[i])
    return True


d = sys.argv[1]
en = PE(os.path.join(d, 'engine.dll'))
ms = PE(os.path.join(d, 'materialsystem.dll'))
sa = PE(os.path.join(d, 'shaderapidx9.dll'))
# The registered factories 0x18007abc0 and 0x180148d30 each return a static object: its vptr is in the file, or set by a static constructor (0: not checked here).
# The two vtables are CEngineClient and CVRenderView, ghidra/out/engine.dll/vtables.txt.
VT_CLIENT, VT_RENDERVIEW = 0x18039d048, 0x1803c7bf0
for factory, off, vt_want, name in ((0x18007abc0, 0, VT_CLIENT, 'VEngineClient015'), (0x180148d30, 0, VT_RENDERVIEW, 'VEngineRenderView014')):
    obj = en.rip(factory, 3, 7)
    vp = en.q(obj)
    print('%s: object %#x, vptr in the file %#x' % (name, obj, vp))
    assert vp in (0, vt_want)

match(en, en.q(VT_CLIENT + 8 * enum_value('ENG_SLOT_INGAME')), string_const('k_pat_ingame'))
match(en, en.q(VT_CLIENT + 8 * enum_value('ENG_SLOT_LEVELNAME')), string_const('k_pat_levelname'))
smv = en.q(VT_RENDERVIEW + 8 * enum_value('RV_SLOT_SETMAINVIEW'))
match(en, smv, string_const('k_pat_setmainview'))
ren_p = en.rip(smv, 3, 7)
ren = en.q(ren_p)
assert en.base <= ren < en.base + en.size, hex(ren)
vt = en.q(ren)
fn = en.q(vt + 8 * enum_value('RENDERER_SLOT_SETMAINVIEW'))
match(en, fn, string_const('k_pat_renderer'))
x = en.rip(fn, enum_value('REN_OFF_X'), enum_value('REN_NEXT_X'))
y = en.rip(fn, enum_value('REN_OFF_Y'), enum_value('REN_NEXT_Y'))
z = en.rip(fn, enum_value('REN_OFF_Z'), enum_value('REN_NEXT_Z'))
assert y == x + 4 and z == x + 8, (hex(x), hex(y), hex(z))
assert en.base <= x and z + 4 <= en.base + en.size
print('main view origin at %#x (renderer object %#x, setter %#x)' % (x, ren, fn))
# The engine's light averaging, read only (amb_track.h): FUN_180105dc0's readers of r_lightaverage and r_lightinterp and its call of the client time FUN_1800aaea0;
# the ConVars' static constructors give their names and flags (r_lightaverage 0, r_lightinterp FCVAR_CHEAT 0x4000).
la = en.base + enum_value('RVA_LA_READ'); li = en.base + enum_value('RVA_LI_READ'); tc = en.base + enum_value('RVA_TIME_CALL')
match(en, la, string_const('k_pat_la_read'))
match(en, li, string_const('k_pat_li_read'))
match(en, tc, string_const('k_pat_time_call'))
la_cv, li_cv = en.rip(la, 16, 20), en.rip(li, 10, 14)
assert en.rip(la, 3, 7) == la_cv + enum_value('CV_OFF_PARENT') and en.rip(la, 32, 36) == la_cv + enum_value('CV_OFF_NVALUE')
assert en.rip(li, 3, 7) == li_cv + enum_value('CV_OFF_PARENT') and en.rip(li, 23, 27) == li_cv + enum_value('CV_OFF_FVALUE')
fn = en.rip(tc, 40, 44)
match(en, fn, string_const('k_pat_time_fn'))
assert fn == 0x1800aaea0 and en.rip(tc, 3, 7) == 0x180653fa0
for ctor, obj, name, flags in ((0x18000eac0, la_cv, b'r_lightaverage\0', '45 33 c9'), (0x18000eb40, li_cv, b'r_lightinterp\0', '41 b9 00 40 00 00')):
    code = en.rd(ctor, 64)
    assert bytes.fromhex(flags.replace(' ', '')) in code, (hex(ctor), flags)
    i = code.index(bytes.fromhex('488d15'))
    assert en.rd(en.rip(ctor + i, 3, 7), len(name)) == name, name
    j = code.index(bytes.fromhex('488d0d'))
    assert en.rip(ctor + j, 3, 7) == obj, (hex(ctor), hex(obj))
# the blend: k = expf(-dt * rate) (FUN_180364bd0, the argument xored with the sign bit), the cube clamped to [0, 8] (FUN_180105bc0)
assert en.rd(0x180395ae0, 4) == bytes.fromhex('00000080') and struct.unpack('<f', en.rd(0x18039cf7c, 4))[0] == 8.0
assert struct.unpack('<f', en.rd(0x1803c3a58, 4))[0] == -999999.0
print('light averaging: r_lightaverage at %#x, r_lightinterp at %#x, client time FUN_1800aaea0(%#x)' % (la_cv, li_cv, en.rip(tc, 3, 7)))

ml = en.base + enum_value('RVA_MAPLOAD_OPEN')
match(en, ml, string_const('k_pat_mapload'))
print('filesystem global at %#x, "rb" = %r' % (en.rip(ml, 3, 7), en.rd(en.rip(ml, 10, 14), 3)))
assert en.rd(en.rip(ml, 10, 14), 3) == b'rb\0'

vtb = ms.base + enum_value('RVA_MS_CV_VTBL')
assert ms.q(vtb + 8 * enum_value('CV_SLOT_GETINT')) == ms.base + enum_value('RVA_MS_CV_GETINT')
assert ms.rd(ms.base + enum_value('RVA_MS_STR_FULLBRIGHT'), 15) == b'mat_fullbright\0'
# stock's own GetInt, the body vlg.c's pixelfog_value relies on, compared up to its tail jump
assert ms.rd(ms.base + enum_value('RVA_MS_CV_GETINT'), 25) == sa.rd(sa.base + 0x47c0, 25)
# FUN_1800092d0's static constructor call: lea rdx, name; lea rcx, object
ctor_name, ctor_obj = 0x1800092ed, 0x1800092f4
assert ms.rip(ctor_name, 3, 7) == ms.base + enum_value('RVA_MS_STR_FULLBRIGHT')
assert ms.rip(ctor_obj, 3, 7) == ms.base + enum_value('RVA_MS_CV_FULLBRIGHT')
print('mat_fullbright ConVar checks hold')

# filesystem_stdio: the pinned stamp, Size(handle) at slot 7 of the IBaseFileSystem part and the +0x50 test in its Read and Size. The vtable address is from its
# RTTI (the review's dump); ambvol.c itself only compares the stamp.
fs = PE(os.path.join(d, 'filesystem_stdio.dll'))
stamp = struct.unpack_from('<I', fs.b, struct.unpack_from('<I', fs.b, 0x3c)[0] + 8)[0]
want_stamp = int(re.search(r'k_fs_stamp = (0x[0-9a-f]+)', src).group(1), 0)
assert stamp == want_stamp, hex(stamp)
assert fs.rd(0x180020600, 4) == bytes.fromhex('488b4950'), 'BFS Read: mov rcx,[rcx+0x50]'
assert fs.rd(0x180023a24, 4) == bytes.fromhex('488b4150'), 'Size: mov rax,[rcx+0x50]'
assert fs.rd(0x180023a43, 3) == bytes.fromhex('8b4120'), 'Size, VPK branch: the exact size at +0x20'
print('filesystem_stdio checks hold: stamp %#x' % stamp)

hw_vt = 0x1800788c0   # CHardwareConfig, analysis/10
assert sa.q(hw_vt + 8 * enum_value('HW_SLOT_HDR')) == sa.base + enum_value('RVA_HW_HDR')
assert sa.q(hw_vt + 8 * enum_value('HW_SLOT_SRGB')) == sa.base + enum_value('RVA_HW_SRGB')

# P5 (src/proxy_shaderapi/lights.c): the dlight / elight / lightstyle reads and the two conversions lg_light.c follows
lsrc = open(os.path.join(os.path.dirname(__file__), '../../src/proxy_shaderapi/lights.c')).read()


def lconst(name):
    m = re.search(r'static const char ' + name + r'\[\]\s*=\s*((?:"[^"]*"\s*)+);', lsrc)
    assert m, name
    return ''.join(re.findall(r'"([^"]*)"', m.group(1)))


def lenum(name):
    m = re.search(r'\b' + name + r'\s*=\s*(0x[0-9a-f]+|\d+)', lsrc)
    assert m, name
    return int(m.group(1), 0)


def fnv(b):
    h = 0x811c9dc5
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xffffffff
    return h


VT_EFX = 0x1803c7028   # CVEfx, "VEngineEffects001" (05 section 3)
fd = en.q(VT_EFX + 8 * lenum('EFX_SLOT_ACTIVEDLIGHTS'))
fe = en.q(VT_EFX + 8 * lenum('EFX_SLOT_ELIGHTBYKEY'))
assert fd == 0x180138820 and fe == 0x180138be0, (hex(fd), hex(fe))
match(en, fd, lconst('k_pat_dlights'))
match(en, fe, lconst('k_pat_elights'))
dflag, dmask = en.rip(fd, lenum('DL_OFF_FLAG'), lenum('DL_NEXT_FLAG')), en.rip(fd, lenum('DL_OFF_MASK'), lenum('DL_NEXT_MASK'))
darr = en.rip(fd, lenum('DL_OFF_ARRAY'), lenum('DL_NEXT_ARRAY'))
eflag, escan = en.rip(fe, lenum('EL_OFF_FLAG'), lenum('EL_NEXT_FLAG')), en.rip(fe, lenum('EL_OFF_SCAN'), lenum('EL_NEXT_SCAN'))
earr = en.rip(fe, lenum('EL_OFF_ARRAY'), lenum('EL_NEXT_ARRAY'))
assert (dflag, dmask, darr, eflag, earr) == (0x180650c00, 0x1807de104, 0x1806d9250, 0x180650c01, 0x180651e60), [hex(x) for x in (dflag, dmask, darr, eflag, earr)]
assert en.rip(fd, lenum('DL_OFF_ARRAY1'), lenum('DL_NEXT_ARRAY1')) == darr + 0x48 and escan == earr + 0x28
# ComputeDynamicLighting (FUN_1801d8550), what models are lit from, reads the same flag, mask and arrays
cdl = en.rd(0x1801d8550, 0x2c0)
refs = set()
for i in range(len(cdl) - 7):
    for op_len, rel_at in ((7, 3), (6, 2), (7, 2), (8, 4)):   # lea/mov r64 [rip], mov r32 [rip], cmp byte [rip] imm8, cmp [rip] r8 with REX
        if i + op_len <= len(cdl):
            t = 0x1801d8550 + i + op_len + struct.unpack_from('<i', cdl, i + rel_at)[0]
            refs.add(t)
for a in (dflag, dmask, darr, eflag, earr):
    assert a in refs, 'FUN_1801d8550 does not reference %#x' % a
na = en.base + lenum('RVA_NONAMBIENT_LIGHTS')
match(en, na, lconst('k_pat_styles'))
styles = en.rip(na, lenum('NA_OFF_STYLES'), lenum('NA_NEXT_STYLES'))
assert styles == 0x1807e2320, hex(styles)
assert struct.unpack('<f', en.rd(en.rip(na, lenum('NA_OFF_SCALE'), lenum('NA_NEXT_SCALE')), 4))[0] == struct.unpack('<f', struct.pack('<f', 0.0037878789))[0]
assert en.rip(na, lenum('NA_OFF_CALL'), lenum('NA_NEXT_CALL')) == en.base + lenum('RVA_DESC_FROM_WORLDLIGHT')
hd = int(re.search(r'k_hash_desc = (0x[0-9a-f]+)', lsrc).group(1), 0)
hw = int(re.search(r'k_hash_wfd = (0x[0-9a-f]+)', lsrc).group(1), 0)
d0, dn = en.base + lenum('RVA_DESC_FROM_WORLDLIGHT'), lenum('LEN_DESC_FROM_WORLDLIGHT')
w0, wn = en.base + lenum('RVA_WORLDLIGHT_FROM_DLIGHT'), lenum('LEN_WORLDLIGHT_FROM_DLIGHT')
assert fnv(en.rd(d0, dn)) == hd and fnv(en.rd(w0, wn)) == hw
tab = en.rip(w0, lenum('WD_OFF_TABLE'), lenum('WD_NEXT_TABLE'))
flo = en.rip(w0, lenum('WD_OFF_FLOOR'), lenum('WD_NEXT_FLOOR'))
assert tab == 0x1804f3b80 and flo == 0x1804f13e0, (hex(tab), hex(flo))
# no base relocation inside any byte range lights.c compares or hashes (the loader would change them)
e = struct.unpack_from('<I', en.b, 0x3c)[0]
rel_rva, rel_size = struct.unpack_from('<II', en.b, e + 24 + 112 + 5 * 8)
relocs, off = [], 0
while off < rel_size:
    page, size = struct.unpack('<II', en.rd(en.base + rel_rva + off, 8))
    blk = en.rd(en.base + rel_rva + off + 8, size - 8)
    for k in range(0, len(blk), 2):
        v = struct.unpack_from('<H', blk, k)[0]
        if v >> 12:
            relocs.append(en.base + page + (v & 0xfff))
    off += size
ranges = [(d0, dn), (w0, wn), (fd, len(lconst('k_pat_dlights').split())), (fe, len(lconst('k_pat_elights').split())),
          (na, len(lconst('k_pat_styles').split()))]
for r0, rn in ranges:
    bad = [hex(x) for x in relocs if r0 - 8 < x < r0 + rn]
    assert not bad, 'relocations in %#x+%d: %s' % (r0, rn, bad)
print('lights: dlights %#x (mask %#x, flag %#x), elights %#x, lightstyles %#x; conversions hashed (%#x, %#x); %d relocations, none in the checked code'
      % (darr, dmask, dflag, earr, styles, hd, hw, len(relocs)))
print('ENGINE PATTERNS OK')
