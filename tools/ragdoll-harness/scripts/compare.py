#!/usr/bin/env python3
"""Runs the Jolt harness (./rdsim_jolt, from ./build.sh --jolt) with each solver
and sets the two side by side: which checks each fails in how many scenarios,
and the measures that tell the solvers apart, as the median over every
scenario and per scenario.

    scripts/compare.py              every scenario
    scripts/compare.py -v           and each scenario's measures
    RD_CVAR=... scripts/compare.py  with tuning overrides, as rdsim takes them

Both runs are the same binary: RD_SOLVER=0 is the particle solver, 1 hands the
body to the Jolt ragdoll once the blend ends. The checks are the ones rdsim's
PASS/FAIL line applies, read back from what it prints.
"""
import os, re, statistics, subprocess, sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(HERE, "rdsim_jolt")

# name: (regex over a scenario's text, which group, better is 'lo' or 'hi')
MEASURES = {
    "stretch late %":   (r"^\S.*?\|\s*[\d.]+%\s+([\d.]+)%", 1, "lo"),
    "self overlap %":   (r"^\S.*?\|[^|]*\|\s*(-?[\d.]+)\s*\|", 1, "lo"),
    "late move":        (r"^\S.*?\|(?:[^|]*\|){4}\s*(-?[\d.]+)\s*\|", 1, "lo"),
    "deepest":          (r"^\S.*?\|(?:[^|]*\|){5}\s*(-?[\d.]+)\s*\|", 1, "hi"),
    "head-to-foot":     (r"head-to-foot ([\d.]+)", 1, "hi"),
    "slid":             (r"slid after landing: ([\d.]+)", 1, "lo"),
    "settled frame":    (r"settled at frame (-?\d+)", 1, "lo"),
    "whip worst":       (r"whips: \d+ joint steps .*worst ([\d.]+)", 1, "lo"),
    "spring level":     (r"spring: level speed gained ([\d.]+)", 1, "lo"),
    "spring up":        (r"upward ([\d.]+) u/s in the half", 1, "lo"),
    "stretch worst x":  (r"stretch: worst bone ([\d.]+)x", 1, "lo"),
    "trunk sunk max":   (r"trunk mean [\d.]+ max ([\d.]+)", 1, "lo"),
    "limbs sunk max":   (r"limbs mean [\d.]+ max ([\d.]+)", 1, "lo"),
    "limb in trunk %":  (r"limb inside trunk: (-?[\d.]+)%", 1, "lo"),
    "limb in limb %":   (r"limb inside another limb: (-?[\d.]+)%", 1, "lo"),
    "knees folded":     (r"knees folded at rest: (-?[\d.]+)", 1, "lo"),
    "knee out of plane": (r"knee out of plane: ([\d.]+)", 1, "lo"),
    "spread":           (r"spread vs living body: ([\d.]+)x", 1, "lo"),
    "torso twist":      (r"torso twist (-?[\d.]+) deg overall", 1, "lo"),
    "joint twist":      (r"deg overall, (-?[\d.]+) deg at the worst", 1, "lo"),
    "spine bend max":   (r"spine bend per joint: (.*?)\(", 1, "lo"),
    "cloud distortion %": (r"particle cloud distortion: ([\d.]+)%", 1, "lo"),
    "foot turned":      (r"foot turned from rest: (-?[\d.]+)", 1, "lo"),
    "jitter":           (r"jitter once settled: ([\d.]+)", 1, "lo"),
}

# The PASS line's checks, from the printed values.
def failures(m):
    f = []
    def get(k):
        return m.get(k)
    if get("stretch late %") is not None and get("stretch late %") >= 2.0: f.append("late stretch")
    if get("late move") is not None and get("late move") >= 0.5: f.append("still moving")
    if get("deepest") is not None and get("deepest") <= -1.5: f.append("deep")
    if get("self overlap %") is not None and get("self overlap %") >= 34.0: f.append("self overlap")
    if m.get("knee back", 0) > 0: f.append("knee back")
    if m.get("hyper", 0) >= 0.5: f.append("hyperextended")
    if get("knee out of plane") is not None and get("knee out of plane") >= 5.0: f.append("knee out of plane")
    if get("cloud distortion %") is not None and get("cloud distortion %") >= 10.0: f.append("cloud distortion")
    if get("torso twist") is not None and get("torso twist") >= 50.0: f.append("torso twist")
    if get("joint twist") is not None and get("joint twist") >= 30.0: f.append("joint twist")
    if get("spine bend max") is not None and get("spine bend max") >= 35.0: f.append("spine bend")
    if get("foot turned") is not None and get("foot turned") >= 55.0: f.append("foot turned")
    for k, lim in (("wrist", 70), ("neck bend", 60), ("ankle", 60)):
        if m.get(k, 0) >= lim: f.append(k)
    return f

def run(solver):
    env = dict(os.environ, RD_SOLVER=str(solver))
    out = subprocess.run([BIN], cwd=HERE, env=env, capture_output=True, text=True).stdout
    scen, cur = {}, None
    for line in out.splitlines():
        if re.match(r"^\S.*\|\s*(PASS|FAIL)\s*$", line):
            cur = line.split("|")[0].strip()
            scen[cur] = {"_text": [line], "_verdict": line.split("|")[-1].strip()}
            cols = [c.strip() for c in line.split("|")]
            try:
                scen[cur]["hyper"] = float(cols[3])
                scen[cur]["knee back"] = float(cols[4])
            except ValueError:
                pass
        elif cur and line.startswith("   "):
            scen[cur]["_text"].append(line)
    for name, s in scen.items():
        text = "\n".join(s["_text"])
        for k, (rx, g, _) in MEASURES.items():
            mm = re.search(rx, text, re.M)
            if not mm:
                continue
            if k == "spine bend max":
                nums = [float(x) for x in re.findall(r"(-?\d+)", mm.group(1))]
                if nums:
                    s[k] = max(nums)
                continue
            try:
                s[k] = float(mm.group(g))
            except ValueError:
                pass
        mb = re.search(r"max bend from rest: wrist (\d+) deg\s+neck (\d+) deg\s+ankle (\d+)", text)
        if mb:
            s["wrist"], s["neck bend"], s["ankle"] = (float(x) for x in mb.groups())
    return scen

def main():
    verbose = "-v" in sys.argv
    if not os.path.exists(BIN):
        sys.exit("no %s: run ./build.sh --jolt first" % BIN)
    runs = {"particles": run(0), "jolt": run(1)}
    names = [n for n in runs["particles"] if n in runs["jolt"]]

    print("%d scenarios\n" % len(names))
    print("%-20s %10s %10s" % ("", "particles", "jolt"))
    for label in runs:
        pass
    print("%-20s %10d %10d" % ("pass", *(sum(1 for n in names if runs[l][n]["_verdict"] == "PASS") for l in runs)))

    print("\nfailed check (scenarios)")
    counts = {l: {} for l in runs}
    for l in runs:
        for n in names:
            for f in failures(runs[l][n]):
                counts[l][f] = counts[l].get(f, 0) + 1
    for f in sorted(set(counts["particles"]) | set(counts["jolt"])):
        print("  %-18s %10d %10d" % (f, counts["particles"].get(f, 0), counts["jolt"].get(f, 0)))

    print("\nmedian over scenarios (better)")
    for k, (_, _, better) in MEASURES.items():
        vals = {l: [runs[l][n][k] for n in names if k in runs[l][n]] for l in runs}
        if not vals["particles"] or not vals["jolt"]:
            continue
        mp, mj = statistics.median(vals["particles"]), statistics.median(vals["jolt"])
        print("  %-18s %10.2f %10.2f   %s" % (k, mp, mj, "lower" if better == "lo" else "higher"))

    if verbose:
        for n in names:
            p, j = runs["particles"][n], runs["jolt"][n]
            print("\n%s: %s / %s" % (n, p["_verdict"], j["_verdict"]))
            print("   particles fail: %s" % (", ".join(failures(p)) or "-"))
            print("   jolt fails:     %s" % (", ".join(failures(j)) or "-"))
            for k in MEASURES:
                if k in p or k in j:
                    print("     %-18s %8s %8s" % (k, "%.2f" % p[k] if k in p else "-", "%.2f" % j[k] if k in j else "-"))

if __name__ == "__main__":
    main()
