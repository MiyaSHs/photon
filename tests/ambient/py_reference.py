#!/usr/bin/env python3
"""Reference dumps for tests/ambient (P3 a): runs tools/lpv2_ambient_volume.py's own functions (imported, unchanged)
and writes raw little-endian files that ambtest.c reads.

  python3 -I tests/ambient/py_reference.py bake <map.bsp> <out.bin> --bounds x0 y0 z0 x1 y1 z1 --voxel V --zvoxel Z
         --soft S --blur B
      bake_leaflocal() of the tool (the --mode leaflocal default). File: b"AMBV", i32 org[3], i32 dim[3],
      f64 voxel_xy, voxel_z, soft, i32 blur, f32 vol[z][y][x][6][3] (C layout, cast from the tool's float64),
      u8 open[z][y][x].
  python3 -I tests/ambient/py_reference.py points <map.bsp> <out.bin> --bounds ... [--n 2000] [--seed 1]
      BSP.engine_cube(pts, soft=1) (the engine rule) at n random points in the world bounds, n in --bounds, and n on
      the 8-unit lattice inside --bounds (points exactly on axial planes). File: b"AMBP", i32 N, f64 pts[N][3],
      i32 leaf[N], i32 count[N] (samples used, 0 = zero cube), f64 val[N][6][3].
"""
import argparse, importlib.util, os, struct, sys
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location("lpv2", os.path.join(ROOT, "tools", "lpv2_ambient_volume.py"))
lpv2 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lpv2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=("bake", "points"))
    ap.add_argument("bsp")
    ap.add_argument("out")
    ap.add_argument("--bounds", type=float, nargs=6, required=True)
    ap.add_argument("--voxel", type=float, default=32)
    ap.add_argument("--zvoxel", type=float, default=32)
    ap.add_argument("--soft", type=float, default=32)
    ap.add_argument("--blur", type=int, default=4)
    ap.add_argument("--n", type=int, default=2000)
    ap.add_argument("--seed", type=int, default=1)
    a = ap.parse_args()
    b = lpv2.BSP(a.bsp)
    if a.what == "bake":
        lo, dims, st, V3, valid, _ = lpv2.bake_leaflocal(b, a.voxel, a.zvoxel, a.soft, a.bounds, a.blur)
        org = np.rint(lo / st).astype(np.int32)
        assert np.all(org * st == lo), (lo, st)
        with open(a.out, "wb") as f:
            f.write(b"AMBV" + struct.pack("<6i3di", *org, *dims, a.voxel, a.zvoxel, a.soft, a.blur))
            f.write(np.ascontiguousarray(V3.transpose(2, 1, 0, 3, 4), dtype="<f4").tobytes())
            f.write(np.ascontiguousarray(valid.transpose(2, 1, 0), dtype=np.uint8).tobytes())
        print(f"py_reference bake: org {org.tolist()} dims {dims.tolist()} open {int(valid.sum())}")
    else:
        rng = np.random.default_rng(a.seed)
        bmin, bmax = np.array(a.bounds[:3]), np.array(a.bounds[3:])
        pts = np.concatenate([
            rng.uniform(b.wmin, b.wmax, (a.n, 3)),
            rng.uniform(bmin, bmax, (a.n, 3)),
            np.floor(rng.uniform(bmin, bmax, (a.n, 3)) / 8) * 8,
        ])
        leaf = b.point_leaf(pts)
        ref = np.where(b.acount[leaf] == 0, b.afirst[leaf], leaf)
        cnt = b.acount[ref]
        val, _ = b.engine_cube(pts, 1.0)
        with open(a.out, "wb") as f:
            f.write(b"AMBP" + struct.pack("<i", len(pts)))
            f.write(pts.astype("<f8").tobytes())
            f.write(leaf.astype("<i4").tobytes())
            f.write(cnt.astype("<i4").tobytes())
            f.write(val.astype("<f8").tobytes())
        print(f"py_reference points: {len(pts)} points, {int((cnt == 0).sum())} with no samples")


if __name__ == "__main__":
    sys.exit(main())
