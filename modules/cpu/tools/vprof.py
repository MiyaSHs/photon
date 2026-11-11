#!/usr/bin/env python3
"""vprof.py: read the engine's VProf reports (vprof_generate_report, printed into console.log) and compare them.

  vprof.py <console.log> [--list]                     every report: frames, ms per frame, peak
  vprof.py <console.log> <i> [top]                    report i (0-based): scopes by self time per frame (ms), with inclusive time and calls
  vprof.py <a.log>:<i> <b.log>:<j> [top] [--incl]     B - A per scope, ms per frame (self time; --incl: including children)

Each report lists only the ~120 most expensive scopes (the engine cuts the rest), so a scope missing from one side reads as 0 there.
Never prints anything but VProf lines (console.log also holds the account name in its last lines)."""
import re
import sys

ROW = re.compile(r"^\s*(.+?)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s*$")


def reports(path):
    out, cur, sec = [], None, None
    with open(path, errors="replace") as f:
        for line in f:
            if "BEGIN VPROF REPORT" in line:
                cur, sec = {"scopes": {}}, None
                continue
            if cur is None:
                continue
            if "END VPROF REPORT" in line:
                out.append(cur)
                cur = None
                continue
            m = re.match(r"(\d+) frames sampled for ([\d.]+) seconds", line)
            if m:
                cur["frames"], cur["secs"] = int(m.group(1)), float(m.group(2))
            m = re.match(r"Average ([\d.]+) fps, ([\d.]+) ms per frame", line)
            if m:
                cur["ms"] = float(m.group(2))
            m = re.match(r"Peak ([\d.]+) ms frame", line)
            if m:
                cur["peak"] = float(m.group(1))
            if line.startswith("-- "):
                sec = line.strip()
                continue
            if sec and sec.startswith("-- Profile scopes sorted by time (including"):
                m = ROW.match(line)
                if m and not m.group(1).startswith("Scope") and not m.group(1).startswith("---"):
                    fr = max(1, cur.get("frames", 1))
                    cur["scopes"][m.group(1).strip()] = {
                        "calls": int(m.group(2)) / fr, "incl": float(m.group(4)) / fr, "self": float(m.group(6)) / fr,
                        "peak": float(m.group(11))}
    return out


def pick(arg):
    path, _, i = arg.rpartition(":")
    return reports(path)[int(i)]


def main(a):
    if len(a) == 1 or "--list" in a:
        for i, r in enumerate(reports(a[0])):
            print(f"{i}: {r.get('frames')} frames in {r.get('secs')} s, {r.get('ms')} ms per frame, peak {r.get('peak')} ms, {len(r['scopes'])} scopes")
        return
    incl = "--incl" in a
    a = [x for x in a if not x.startswith("--")]
    if ":" not in a[1]:
        r = reports(a[0])[int(a[1])]
        top = int(a[2]) if len(a) > 2 else 40
        print(f"{r['frames']} frames, {r['ms']} ms per frame, peak {r['peak']} ms")
        print(f"{'scope':60s} {'self ms':>8s} {'incl ms':>8s} {'calls':>8s} {'peak':>8s}")
        for k, v in sorted(r["scopes"].items(), key=lambda kv: -kv[1]["incl" if incl else "self"])[:top]:
            print(f"{k[:60]:60s} {v['self']:8.3f} {v['incl']:8.3f} {v['calls']:8.1f} {v['peak']:8.2f}")
        return
    A, B = pick(a[0]), pick(a[1])
    top = int(a[2]) if len(a) > 2 else 40
    key = "incl" if incl else "self"
    print(f"A: {A['frames']} frames, {A['ms']} ms per frame; B: {B['frames']} frames, {B['ms']} ms per frame; B - A = {B['ms'] - A['ms']:+.2f} ms")
    names = set(A["scopes"]) | set(B["scopes"])
    rows = []
    for k in names:
        va, vb = A["scopes"].get(k, {}), B["scopes"].get(k, {})
        rows.append((vb.get(key, 0) - va.get(key, 0), k, va.get(key, 0), vb.get(key, 0), va.get("calls", 0), vb.get("calls", 0)))
    rows.sort(key=lambda r: -abs(r[0]))
    print(f"{'scope':60s} {'A ms':>7s} {'B ms':>7s} {'B-A':>7s} {'A calls':>8s} {'B calls':>8s}  ({key} time per frame)")
    for d, k, x, y, ca, cb in rows[:top]:
        print(f"{k[:60]:60s} {x:7.3f} {y:7.3f} {d:+7.3f} {ca:8.1f} {cb:8.1f}")


if __name__ == "__main__":
    main(sys.argv[1:])
