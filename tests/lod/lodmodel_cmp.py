#!/usr/bin/env python3
"""lodmodel_cmp.py WORKDIR: F3, the C lod_model_build (build/host/lodmodeltest) against tools/f3_vtx_lod.py --quadric on every one-LOD model
under third_party/workshop: both write .mdl / .vvd / .dx90.vtx for levels 0.5 0.25 0.1, compared byte for byte. Prints the counts and the
first mismatches."""
import collections, os, subprocess, sys, filecmp
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
from f3_vtx_lod import parse
work = sys.argv[1]
st = collections.Counter(); bad = []
n = 0
for dp, _, fs in os.walk('third_party/workshop'):
    for f in sorted(fs):
        if not f.endswith('.dx90.vtx'):
            continue
        p = os.path.join(dp, f); b = p[:-9]
        if not (os.path.exists(b + '.vvd') and os.path.exists(b + '.mdl')):
            continue
        try:
            if parse(open(p, 'rb').read())['nlods'] != 1:
                continue
        except Exception:
            continue
        n += 1
        po, co = os.path.join(work, 'py%d' % n), os.path.join(work, 'c%d' % n)
        os.makedirs(co, exist_ok=True)
        rp = subprocess.run([sys.executable, '-I', 'tools/f3_vtx_lod.py', '--quadric', p, b + '.vvd', po, '0.5', '0.25', '0.1'], capture_output=True)
        rc = subprocess.run(['build/host/lodmodeltest', b, co], capture_output=True, text=True)
        if rp.returncode or rc.returncode:
            st['both skipped' if rp.returncode and rc.returncode else 'one skipped'] += 1
            if not (rp.returncode and rc.returncode): bad.append((p, 'py %d c %d %s' % (rp.returncode, rc.returncode, rc.stdout.strip())))
            continue
        name = os.path.basename(b)
        diff = [e for e in ('.mdl', '.vvd', '.dx90.vtx') if not filecmp.cmp(os.path.join(po, name + e), os.path.join(co, name + e), shallow=False)]
        st['identical' if not diff else 'differ'] += 1
        if diff: bad.append((p, ' '.join(diff)))
print(dict(st))
for p, why in bad[:8]:
    print(' ', p, why)
