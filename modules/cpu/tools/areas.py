#!/usr/bin/env python3
"""areas.py <console.log>:<i> ...: VProf report i grouped into the C2 areas (ms per frame), one column per report.
Self times, except where an area is a scope's inclusive time (marked incl), so nothing is counted twice."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from vprof import pick

AREAS = [  # (area, owner, [(scope, 'self'|'incl')])
    ("server tick, without physics", "CPU", [("_Host_RunFrame_Server", "incl"), ("PhysFrame", "-incl")]),
    ("physics (Jolt, server)", "CPU", [("PhysFrame", "incl")]),
    ("Lua (all calls into Lua)", "CPU", [("CLuaInterface::CallFunctionProtected", "self")]),
    ("bone setup (client + server)", "CPU", [("C_BaseAnimating::SetupBones", "incl"), ("CBaseAnimating::SetupBones", "incl")]),
    ("world and renderable lists (5 views)", "CPU", [("BuildWorldRenderLists", "incl"), ("CClientLeafSystem::ComputeTranslucentRenderLeaf", "incl"),
                                                      ("BuildRenderablesList", "incl"), ("CClientLeafSystem::PreRender", "self")]),
    ("client simulation and physics", "CPU", [("Client SimulateEntities", "incl"), ("CPhysicsSystem::PhysicsSimulate", "incl"),
                                               ("UpdateClientSideAnimations", "incl")]),
    ("snapshots and interpolation", "CPU", [("C_BaseEntity::InterpolateServerEntities", "incl"), ("NET_ProcessSocket", "incl")]),
    ("input and VGUI", "CPU", [("_Host_RunFrame_Input", "incl"), ("CEngineVGui::Simulate", "incl")]),
    ("model draws (DrawModel incl, minus bones)", "Render", [("C_BaseAnimating::DrawModel", "incl"), ("C_BaseAnimating::SetupBones", "-incl")]),
    ("world, static props, shadows, lightmaps", "Render", [("DrawWorld", "incl"), ("CStaticPropMgr::DrawStaticProps", "incl"),
                                                            ("CShadowMgr::RenderShadows", "incl"), ("CVisibleShadowList::FindShadows", "incl"),
                                                            ("EndUpdateLightmaps", "incl")]),
    ("waiting on the render thread", "-", [("Mat_ThreadedEndframe", "incl")]),
]


def main(args):
    reps = [pick(a) for a in args]
    print(f"{'area':45s} {'owner':6s} " + " ".join(f"{os.path.basename(os.path.dirname(a.rpartition(':')[0]))[7:][:12]:>12s}" for a in args))
    tot = [0.0] * len(reps)
    for name, owner, parts in AREAS:
        vals = []
        for r in reps:
            v = 0.0
            for sc, kind in parts:
                s = r["scopes"].get(sc)
                if not s:
                    continue
                if kind == "-incl":
                    v -= s["incl"]
                else:
                    v += s[kind]
            vals.append(max(v, 0.0))
        tot = [t + v for t, v in zip(tot, vals)]
        print(f"{name:45s} {owner:6s} " + " ".join(f"{v:12.2f}" for v in vals))
    print(f"{'frame (VProf average)':45s} {'':6s} " + " ".join(f"{r['ms']:12.2f}" for r in reps))
    print(f"{'rest (not in these areas)':45s} {'':6s} " + " ".join(f"{r['ms'] - t:12.2f}" for r, t in zip(reps, tot)))


if __name__ == "__main__":
    main(sys.argv[1:])
