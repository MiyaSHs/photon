#!/usr/bin/env python3
"""luaprobe.py <console.log> [top]: the PHOTON_CPU_LUA windows (modules/cpu/lua/photon_cpu_probe.lua), per realm and window in order,
the hooks ranked by ms per frame (client) or per tick (server), with the window's total and its garbage rate.
Only PHOTON_CPU_LUA lines are read (console.log also holds the account name)."""
import re
import sys


def windows(path):
    out, cur = [], {}
    for line in open(path, errors="replace"):
        if not line.startswith("PHOTON_CPU_LUA "):
            continue
        p = line.split()
        realm = p[1]
        if p[2] == "window":
            w = {"realm": realm, "unit": p[3], "n": int(p[4]), "secs": float(p[6]), "gc_kb": float(p[8]), "gc_cycles": int(p[10]),
                 "hooks": []}
            out.append(w)
            cur[realm] = w
            continue
        m = re.match(r"PHOTON_CPU_LUA (\S+) (\S+) (.+) calls ([\d.]+) ms ([\d.]+)\s*$", line)
        if m and realm in cur:
            cur[realm]["hooks"].append((float(m.group(5)), float(m.group(4)), m.group(2), m.group(3)))
    return out


def main(a):
    top = int(a[1]) if len(a) > 1 else 15
    for i, w in enumerate(windows(a[0])):
        tot = sum(h[0] for h in w["hooks"] if h[3] != "GM") + sum(h[0] for h in w["hooks"] if h[3] == "GM")
        print(f"[{i}] {w['realm']}: {w['n']} {w['unit']} in {w['secs']} s; wrapped total {tot:.3f} ms per {w['unit'][:-1]} "
              f"(inclusive, nested calls count twice); garbage {w['gc_kb']:.1f} KB per {w['unit'][:-1]}, {w['gc_cycles']} cycles")
        for ms, calls, ev, ident in sorted(w["hooks"], reverse=True)[:top]:
            print(f"    {ms:7.4f} ms {calls:7.2f} calls  {ev} / {ident}")


if __name__ == "__main__":
    main(sys.argv[1:])
