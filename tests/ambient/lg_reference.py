#!/usr/bin/env python3
"""P5 acceptance (d): an independent Python reading of what src/ambient/lg_map.c reads, for tests/ambient/lgtest.c to equal byte for byte.
  dump      every worldlight of the engine's lump choice (54 if hdr and non-empty, else 15), one line each, floats as %.9g;
  pvsdist   at N points drawn from model 0's bounds with a 64-bit LCG (only points in a leaf with a cluster count), the number of positional
            worldlights (point, spot, surface) whose cluster is in the point's PVS and whose 1/255 radius (on the brightest channel; the
            light's own radius when it has one) reaches within 64 units of it: the PVS-and-radius distribution of 12b / 12 section 7 row (1).
Usage: python3 -I tests/ambient/lg_reference.py dump <map.bsp> <hdr 0|1>
       python3 -I tests/ambient/lg_reference.py pvsdist <map.bsp> <seed> <n> [<box: 6 numbers, else model 0's bounds>]"""
import math, struct, sys

WL0 = struct.Struct('<3f3f3fiiifffffffiii')
TYPES = ['surface', 'point', 'spot', 'skylight', 'quakelight', 'skyambient']


def lumps(b):
    assert b[:4] == b'VBSP'
    out = []
    for i in range(64):
        ofs, ln, ver, _ = struct.unpack_from('<iii4s', b, 8 + 16 * i)
        out.append((ofs, ln, ver))
    return out


def lights(b, hdr):
    L = lumps(b)
    pick = None
    for idx in ([54, 15] if hdr else [15]):
        ofs, ln, ver = L[idx]
        if ln and b[ofs:ofs + 4] != b'LZMA':
            pick = (ofs, ln, ver)
            break
        if ln:
            return []
    if not pick:
        return []
    ofs, ln, ver = pick
    rs = 100 if ver == 1 else 88
    out = []
    for i in range(ln // rs):
        rec = b[ofs + rs * i:ofs + rs * (i + 1)]
        if rs == 100:
            rec = rec[:36] + rec[48:]
        out.append(WL0.unpack(rec))
    return out


def g(x):
    return '%.9g' % x


def dump(b, hdr):
    ls = lights(b, hdr)
    print('lights %d' % len(ls))
    for i, w in enumerate(ls):
        # index, type, style, cluster, flags, texinfo, owner, origin, intensity, normal, stopdot, stopdot2, exponent, radius, c, l, q
        print('wl %d %d %d %d %d %d %d %s %s %s %s' % (
            i, w[10], w[11], w[9], w[19], w[20], w[21], ' '.join(g(x) for x in w[0:3]), ' '.join(g(x) for x in w[3:6]),
            ' '.join(g(x) for x in w[6:9]), ' '.join(g(x) for x in w[12:19])))


class Map:
    def __init__(self, b):
        L = lumps(b)
        o, n, _ = L[1]
        self.planes = [struct.unpack_from('<4f', b, o + 20 * i) for i in range(n // 20)]
        o, n, _ = L[5]
        self.nodes = [struct.unpack_from('<3i', b, o + 32 * i) for i in range(n // 32)]
        o, n, v = L[10]
        sz = 56 if v == 0 else 32
        self.leaves = [struct.unpack_from('<ih', b, o + sz * i) for i in range(n // sz)]
        o, n, _ = L[14]
        m = struct.unpack_from('<9f3i', b, o)
        self.mins, self.maxs, self.head = m[0:3], m[3:6], m[9]
        o, n, _ = L[4]
        nc = struct.unpack_from('<i', b, o)[0]
        rb = (nc + 7) // 8
        self.pvs = []
        for c in range(nc):
            p = o + struct.unpack_from('<i', b, o + 4 + 8 * c)[0]
            row = bytearray()
            while len(row) < rb:
                x = b[p]
                p += 1
                if x:
                    row.append(x)
                else:
                    row.extend(b'\0' * min(b[p], rb - len(row)))
                    p += 1
            self.pvs.append(bytes(row))

    def leaf(self, p):
        node = self.head
        while node >= 0:
            pl, c0, c1 = self.nodes[node]
            nx, ny, nz, d = self.planes[pl]
            node = c0 if nx * p[0] + ny * p[1] + nz * p[2] - d >= 0 else c1
        return -1 - node

    def cluster(self, p):
        contents, cl = self.leaves[self.leaf(p)]
        return -1 if contents & 1 else cl

    def sees(self, a, c):
        if a < 0 or c < 0 or a >= len(self.pvs) or c >= len(self.pvs):
            return False
        return (self.pvs[a][c >> 3] >> (c & 7)) & 1 == 1


def radius255(w):
    if w[15] > 0:
        return w[15]
    c, l, q = w[16], w[17], w[18]
    if w[10] == 0:
        c, l, q = 0.0, 0.0, 1.0
    elif c == 0 and l == 0 and q == 0:
        c = 1.0
    t = 255.0 * max(w[3], w[4], w[5])
    if q > 0:
        disc = l * l - 4 * q * (c - t)
        return (math.sqrt(disc) - l) / (2 * q) if disc >= 0 else 0.0
    if l > 0:
        return (t - c) / l
    return 1e30 if t >= c else 0.0


def pvsdist(b, seed, n, box=None):
    m = Map(b)
    if box:
        m.mins, m.maxs = box[0:3], box[3:6]
    ls = [w for w in lights(b, 1) if w[10] in (0, 1, 2)]
    rad = [radius255(w) for w in ls]
    x = seed & (2 ** 64 - 1)
    counts, tries = [], 0
    while len(counts) < n and tries < 100 * n:
        tries += 1
        p = []
        for k in range(3):
            x = (x * 6364136223846793005 + 1442695040888963407) & (2 ** 64 - 1)
            u = (x >> 11) / 9007199254740992.0
            p.append(m.mins[k] + u * (m.maxs[k] - m.mins[k]))
        cl = m.cluster(p)
        if cl < 0:
            continue
        c = 0
        for w, r in zip(ls, rad):
            dx, dy, dz = p[0] - w[0], p[1] - w[1], p[2] - w[2]
            d = math.sqrt(dx * dx + dy * dy + dz * dz)
            if m.sees(cl, w[9]) and d - 64.0 <= r:
                c += 1
        counts.append(c)
    s = sorted(counts)
    k = len(s)
    chk = sum((i + 1) * v for i, v in enumerate(counts))
    print('pvsdist points %d tries %d p50 %d p90 %d p99 %d max %d checksum %d' % (k, tries, s[k * 50 // 100], s[k * 90 // 100], s[k * 99 // 100], s[-1], chk))


def main():
    b = open(sys.argv[2], 'rb').read()
    if sys.argv[1] == 'dump':
        dump(b, int(sys.argv[3]))
    else:
        pvsdist(b, int(sys.argv[3]), int(sys.argv[4]), [float(x) for x in sys.argv[5:11]] if len(sys.argv) >= 11 else None)


if __name__ == '__main__':
    main()
