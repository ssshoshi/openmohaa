#!/usr/bin/env python3
"""Every cvar and cheat command the source declares, for the cvar browser.

Scans code/ for Cvar_Get-style calls and Event declarations flagged EV_CHEAT,
and writes a JSON work file with what the source itself says about each: the
default, flags, a declared range, where it is and the comment next to it (for
events, their own help text and arguments). The browser's descriptions
(data/opm-cvarhelp/help/cvars.txt) are written from this; --missing lists the
cvars the descriptions file does not cover yet.

  tools/cvarhelp/extract.py > /tmp/cvars.json
  tools/cvarhelp/extract.py --missing
  tools/cvarhelp/extract.py --cheats      # help/cheats.txt, generated
  tools/cvarhelp/extract.py --write       # help/cvars.txt: new cvars added,
                                          # locations refreshed, descriptions kept
  tools/cvarhelp/extract.py --context --prefix r_   # what the code does with them
  tools/cvarhelp/extract.py --merge new.tsv         # name<TAB>min<TAB>max<TAB>description
"""

import argparse
import json
import os
import re
import sys

ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", ".."))
CODE = os.path.join(ROOT, "code")
HELP = os.path.join(ROOT, "data", "opm-cvarhelp", "help")
SKIP_DIRS = {"thirdparty", "gamespy", "tools", "Launcher", "libmad", "ffmpeg"}

# Cvar_Get("name", "default", FLAGS) and the module import variants.
CVAR_GET = re.compile(
    r'\b(?:\w+\s*(?:\.|->)\s*)?Cvar_Get\s*\(\s*"([A-Za-z0-9_]+)"\s*,\s*("(?:[^"\\]|\\.)*"|[^,]+?)\s*,\s*([^;]*?)\)\s*;'
)
CHECK_RANGE = re.compile(r'Cvar_CheckRange\s*\(\s*(\w+)\s*,\s*([-\w.]+)\s*,\s*([-\w.]+)\s*,\s*(\w+)\s*\)')
ASSIGN = re.compile(r'(\w+)\s*=\s*(?:\w+\s*(?:\.|->)\s*)?Cvar_Get\s*\(\s*"([A-Za-z0-9_]+)"')
EVENT = re.compile(
    r'\bEvent\s+(\w+)\s*\(\s*"([^"]+)"\s*,\s*([^,]+?)\s*,\s*(NULL|"[^"]*")\s*,\s*(NULL|"[^"]*")\s*,\s*((?:"(?:[^"\\]|\\.)*"\s*)+)',
    re.S,
)

FLAG_NAMES = ["ARCHIVE", "USERINFO", "SERVERINFO", "SYSTEMINFO", "INIT", "LATCH", "ROM", "CHEAT",
              "TEMP", "NORESTART", "SERVER_CREATED", "USER_CREATED", "PROTECTED", "VM_CREATED",
              "SAVEGAME", "RESETSTRING"]


def sources():
    for base, dirs, files in os.walk(CODE):
        dirs[:] = [d for d in dirs if d not in SKIP_DIRS]
        for name in files:
            if name.endswith((".c", ".cpp")):
                yield os.path.join(base, name)


# "Added in OPM", "Fixed in 2.0": where a line came from, not what it does.
PROVENANCE = re.compile(r"^(added|changed|fixed|removed|modified)\s+in\s+(opm|\d[\d.]*)\b[:.]?\s*$", re.I)


def comment_near(lines, idx):
    """A comment on the line, or the block of // lines just above it."""
    line = lines[idx]
    m = re.search(r"//\s*(.+)$", line)
    if m and "Cvar_Get" in line:
        text = m.group(1).strip()
        return "" if PROVENANCE.match(text) else text
    out = []
    j = idx - 1
    while j >= 0 and lines[j].strip().startswith("//"):
        text = lines[j].strip()[2:].strip()
        if text and not PROVENANCE.match(text) and not set(text) <= set("=-*/ "):
            out.insert(0, text)
        j -= 1
    return " ".join(out)


def unquote(s):
    s = s.strip()
    if s.startswith('"') and s.endswith('"'):
        return bytes(s[1:-1], "utf-8").decode("unicode_escape")
    return s


def scan():
    cvars, ranges, cheats = {}, {}, {}
    for path in sources():
        rel = os.path.relpath(path, ROOT)
        with open(path, encoding="utf-8", errors="replace") as f:
            text = f.read()
        lines = text.splitlines()
        offsets = [0]
        for line in lines:
            offsets.append(offsets[-1] + len(line) + 1)

        def line_of(pos):
            lo, hi = 0, len(offsets) - 1
            while lo < hi:
                mid = (lo + hi + 1) // 2
                if offsets[mid] <= pos:
                    lo = mid
                else:
                    hi = mid - 1
            return lo

        var_to_cvar = {v: n for v, n in ASSIGN.findall(text)}
        for m in CHECK_RANGE.finditer(text):
            name = var_to_cvar.get(m.group(1))
            if name:
                ranges[name] = [m.group(2), m.group(3), m.group(4) == "qtrue"]

        for var, cname in ASSIGN.findall(text):
            e = cvars.get(cname.lower())
            if e is not None and var not in e.setdefault("vars", []):
                e["vars"].append(var)

        for m in CVAR_GET.finditer(text):
            name, default, flags = m.group(1), m.group(2), m.group(3)
            ln = line_of(m.start())
            flags = sorted({f for f in FLAG_NAMES if re.search(r"\bCVAR_" + f + r"\b", flags)})
            entry = cvars.setdefault(name.lower(), {
                "name": name, "default": unquote(default), "flags": [], "where": [], "comment": "",
            })
            entry["flags"] = sorted(set(entry["flags"]) | set(flags))
            entry.setdefault("vars", [])
            before = text[max(0, m.start() - 80):m.start()]
            am = re.search(r"(\w+)\s*=\s*(?:\w+\s*(?:\.|->)\s*)?$", before)
            if am and am.group(1) not in entry["vars"]:
                entry["vars"].append(am.group(1))
            entry["where"].append(f"{rel}:{ln + 1}")
            if not entry["comment"]:
                entry["comment"] = comment_near(lines, ln)

        if "/fgame/" in path.replace("\\", "/") or "/cgame/" in path.replace("\\", "/"):
            for m in EVENT.finditer(text):
                if "EV_CHEAT" not in m.group(3):
                    continue
                doc = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(6)))
                args = unquote(m.group(5)) if m.group(5) != "NULL" else ""
                cheats.setdefault(m.group(2).lower(), {
                    "name": m.group(2), "args": args, "doc": doc.strip(),
                    "where": f"{rel}:{line_of(m.start()) + 1}",
                })

    for name, r in ranges.items():
        if name.lower() in cvars:
            cvars[name.lower()]["range"] = r
    return cvars, cheats


def described():
    path = os.path.join(HELP, "cvars.txt")
    names = set()
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                if line.strip() and not line.startswith("#"):
                    names.add(line.split("\t", 1)[0].strip().lower())
    return names


def read_help():
    """help/cvars.txt as {lower name: [name, min, max, where, description]}."""
    path = os.path.join(HELP, "cvars.txt")
    rows = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.rstrip("\n")
                if not line.strip() or line.startswith("#"):
                    continue
                cols = (line.split("\t") + [""] * 5)[:5]
                rows[cols[0].lower()] = cols
    return rows


def write_help(cvars):
    rows = read_help()
    for key, v in cvars.items():
        row = rows.get(key)
        where = v["where"][0]
        if row is None:
            lo, hi = (v["range"][0], v["range"][1]) if "range" in v else ("", "")
            # A code comment is a start; it is rewritten when described properly.
            rows[key] = [v["name"], lo, hi, where, v["comment"].replace("\t", " ")]
        else:
            row[3] = where
    os.makedirs(HELP, exist_ok=True)
    with open(os.path.join(HELP, "cvars.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("# The cvar browser's descriptions (client/cl_uicvarbrowser.cpp).\n")
        f.write("# name<TAB>min<TAB>max<TAB>declared at<TAB>description   (\\n in a description is a line break)\n")
        f.write("# Refresh with tools/cvarhelp/extract.py --write: descriptions and ranges are kept.\n")
        for key in sorted(rows):
            f.write("\t".join(rows[key]) + "\n")
    print(f"{len(rows)} cvars in help/cvars.txt", file=sys.stderr)


def usage_lines(cvars, names, per=6):
    """Lines that read each cvar: through its variable, or by name."""
    wanted = {}
    for key in names:
        v = cvars[key]
        pats = [re.escape(x) + r"\s*->\s*(?:integer|value|string|modified)" for x in v.get("vars", []) if len(x) > 3]
        pats.append(r'"' + re.escape(v["name"]) + r'"')
        wanted[key] = re.compile("|".join(pats))
    found = {k: [] for k in names}
    for path in sources():
        rel = os.path.relpath(path, ROOT)
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
        for i, line in enumerate(lines):
            if "Cvar_Get" in line or "Cvar_CheckRange" in line:
                continue
            for key, pat in wanted.items():
                if len(found[key]) < per and pat.search(line):
                    found[key].append(f"{rel}:{i + 1}: {line.strip()[:160]}")
    return found


def merge(tsv):
    rows = read_help()
    n = 0
    with open(tsv, encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip() or line.startswith("#"):
                continue
            name, lo, hi, desc = (line.split("\t") + ["", "", "", ""])[:4]
            row = rows.get(name.lower())
            if row is None:
                print(f"not a known cvar: {name}", file=sys.stderr)
                continue
            row[1], row[2], row[4] = lo, hi, desc
            n += 1
    path = os.path.join(HELP, "cvars.txt")
    with open(path, encoding="utf-8") as f:
        header = [l for l in f if l.startswith("#")]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.writelines(header)
        for key in sorted(rows):
            f.write("\t".join(rows[key]) + "\n")
    print(f"{n} descriptions merged", file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--missing", action="store_true", help="cvars without a description yet")
    ap.add_argument("--cheats", action="store_true", help="write help/cheats.txt")
    ap.add_argument("--prefix", default="", help="only cvars starting with this")
    ap.add_argument("--write", action="store_true", help="add new cvars to help/cvars.txt")
    ap.add_argument("--context", action="store_true", help="each cvar with the lines that use it")
    ap.add_argument("--merge", metavar="TSV", help="descriptions to fold into help/cvars.txt")
    args = ap.parse_args()

    cvars, cheats = scan()

    if args.write:
        write_help(cvars)
        return

    if args.merge:
        merge(args.merge)
        return

    if args.context:
        keys = [k for k in sorted(cvars) if k.startswith(args.prefix.lower())]
        found = usage_lines(cvars, keys)
        for k in keys:
            v = cvars[k]
            rng = f"  range {v['range'][0]}..{v['range'][1]}" if "range" in v else ""
            print(f"== {v['name']}  default \"{v['default']}\"  [{' '.join(v['flags'])}]{rng}  {v['where'][0]}")
            if v["comment"]:
                print(f"   // {v['comment'][:300]}")
            for u in found[k]:
                print(f"   {u}")
        return

    if args.cheats:
        os.makedirs(HELP, exist_ok=True)
        with open(os.path.join(HELP, "cheats.txt"), "w", encoding="utf-8", newline="\n") as f:
            f.write("# Generated by tools/cvarhelp/extract.py --cheats from the EV_CHEAT events.\n")
            f.write("# name<TAB>arguments<TAB>description\n")
            for key in sorted(cheats):
                c = cheats[key]
                f.write(f"{c['name']}\t{c['args']}\t{c['doc']}\n")
        print(f"{len(cheats)} cheat commands", file=sys.stderr)
        return

    items = [v for k, v in sorted(cvars.items()) if k.startswith(args.prefix.lower())]
    if args.missing:
        have = described()
        items = [v for v in items if v["name"].lower() not in have]
        for v in items:
            print(v["name"])
        print(f"{len(items)} without a description", file=sys.stderr)
        return

    json.dump(items, sys.stdout, indent=1)
    print(f"{len(items)} cvars, {len(cheats)} cheats", file=sys.stderr)


if __name__ == "__main__":
    main()
