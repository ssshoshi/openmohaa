#!/usr/bin/env python3
"""Worktrees and branches: what is where, and clearing out what is merged.

Work gets spread over worktrees (sessions, agents, scratch builds under /tmp)
and some of it is left uncommitted. `status` says, per worktree, what is
uncommitted and whether it is in main already; `gc` removes clean worktrees
whose work is merged and the merged local branches, and with --salvage saves
the uncommitted changes of stale /tmp scratch worktrees as patches first.
Nothing is removed without --apply, and every removal is logged with what
it takes to undo it.

    tools/wt.py status            # one line a worktree
    tools/wt.py gc                # what would go
    tools/wt.py gc --salvage --apply
"""

import argparse
import os
import subprocess
import sys
import tempfile
import time

REMOTE = os.environ.get("WT_REMOTE", "fork")
MAIN = f"{REMOTE}/main"
SALVAGE = os.path.expanduser("~/projects/openmohaa-salvage")
REPO = "ssshoshi/openmohaa"
# Worktrees that stay even when clean and merged: the report pipeline's main,
# the fix agents' base, the orchestrator's (its skills live there). More with
# WT_KEEP, colon separated.
PERMANENT = ["~/projects/openmohaa-main", "~/projects/openmohaa-agent/main", "~/projects/openmohaa-orch"]
PERMANENT = {os.path.realpath(os.path.expanduser(p))
             for p in PERMANENT + [p for p in os.environ.get("WT_KEEP", "").split(":") if p]}


def git(*args, cwd=None, check=False):
    r = subprocess.run(["git", *args], cwd=cwd, capture_output=True, text=True)
    if check and r.returncode:
        sys.exit(r.stderr.strip())
    return r.stdout.strip() if r.returncode == 0 else None


def worktrees():
    out, cur = [], {}
    for line in (git("worktree", "list", "--porcelain") or "").splitlines():
        if line.startswith("worktree "):
            cur = {"path": line[9:], "branch": None}
            out.append(cur)
        elif line.startswith("branch "):
            cur["branch"] = line[7:].replace("refs/heads/", "")
        elif line == "detached":
            cur["branch"] = None
    return out


def merged_prs():
    r = subprocess.run(["gh", "pr", "list", "-R", REPO, "--state", "merged", "--limit", "300",
                        "--json", "headRefName", "-q", ".[].headRefName"], capture_output=True, text=True)
    return set(r.stdout.split()) if r.returncode == 0 else set()


def in_main(path, files, index):
    """How many of the changed files' changes main has already: the change
    applies in reverse on top of main."""
    n = 0
    for f in files:
        diff = git("diff", "HEAD", "--binary", "--", f, cwd=path)
        if not diff:
            continue
        with tempfile.NamedTemporaryFile("w", delete=False) as t:
            t.write(diff + "\n")
        env = dict(os.environ, GIT_INDEX_FILE=index)
        if subprocess.run(["git", "apply", "--cached", "--check", "-R", t.name], env=env,
                          capture_output=True).returncode == 0:
            n += 1
        os.unlink(t.name)
    return n


def describe(wt, prs, index):
    p = wt["path"]
    status = (git("status", "--porcelain", cwd=p) or "").splitlines()
    tracked = [l[3:] for l in status if not l.startswith("??")]
    untracked = [l[3:] for l in status if l.startswith("??") and "__pycache__" not in l]
    head = git("rev-parse", "HEAD", cwd=p)
    merged = bool(head) and subprocess.run(["git", "merge-base", "--is-ancestor", head, MAIN],
                                           cwd=p).returncode == 0
    if wt["branch"] in prs:
        merged = True
    ahead = git("rev-list", "--count", f"{MAIN}..HEAD", cwd=p) or "?"
    behind = git("rev-list", "--count", f"HEAD..{MAIN}", cwd=p) or "?"
    age = int((time.time() - int(git("log", "-1", "--format=%ct", cwd=p) or time.time())) / 86400)
    return {
        **wt,
        "tracked": tracked,
        "untracked": untracked,
        "in_main": in_main(p, tracked, index) if tracked else 0,
        "merged": merged,
        "ahead": ahead,
        "behind": behind,
        "age": age,
        "scratch": p.startswith("/tmp/"),
    }


def survey():
    prs = merged_prs()
    fd, index = tempfile.mkstemp()
    os.close(fd)
    subprocess.run(["git", "read-tree", MAIN], env=dict(os.environ, GIT_INDEX_FILE=index), check=True)
    try:
        return [describe(wt, prs, index) for wt in worktrees()], prs
    finally:
        os.unlink(index)


def short(path):
    home = os.path.expanduser("~/")
    return path.replace(home, "~/").replace("/tmp/claude-1000/", "/tmp/…/")


def cmd_status(args):
    git("fetch", REMOTE, "-q")
    rows, _ = survey()
    for r in rows:
        work = ""
        if r["tracked"]:
            work = f"{len(r['tracked'])} uncommitted ({r['in_main']} already in main)"
        if r["untracked"]:
            work += (", " if work else "") + f"{len(r['untracked'])} untracked"
        state = "merged" if r["merged"] and not r["tracked"] else ""
        print(f"{short(r['path']):48} {r['branch'] or '(detached)':28} +{r['ahead']}/-{r['behind']} "
              f"{r['age']}d  {work or 'clean'}  {state}")
    branches = (git("for-each-ref", "--format=%(refname:short)", "refs/heads/") or "").split()
    print(f"\n{len(rows)} worktrees, {len(branches)} local branches")


def cmd_gc(args):
    git("fetch", REMOTE, "-q")
    rows, prs = survey()
    here = os.path.realpath(git("rev-parse", "--show-toplevel") or ".")
    first = rows[0]["path"] if rows else None  # the main checkout: never removed
    go, salvage = [], []
    for r in rows:
        if r["path"] in (first, here) or os.path.realpath(r["path"]) in PERMANENT:
            continue
        clean = not r["tracked"] and not r["untracked"]
        if clean and (r["merged"] or r["scratch"]):
            go.append(r)
        elif args.salvage and r["scratch"] and r["age"] >= args.days:
            salvage.append(r)

    checked_out = {r["branch"] for r in rows if r["branch"]} - {r["branch"] for r in go + salvage}
    keep = {"main", "upstream-main"}
    branches = []
    for b in (git("for-each-ref", "--format=%(refname:short)", "refs/heads/") or "").split():
        if b in keep or b in checked_out:
            continue
        if b in prs or subprocess.run(["git", "merge-base", "--is-ancestor", b, MAIN]).returncode == 0:
            branches.append(b)

    for r in go:
        print(f"remove worktree  {short(r['path'])}  ({'merged' if r['merged'] else 'clean scratch'})")
    for r in salvage:
        print(f"salvage+remove   {short(r['path'])}  ({len(r['tracked'])} uncommitted, "
              f"{r['in_main']} in main, {r['age']}d old)")
    for b in branches:
        print(f"delete branch    {b}")
    if not (go or salvage or branches):
        print("nothing to clear")
        return
    if not args.apply:
        print("\n(dry run: --apply does it)")
        return

    out = os.path.join(SALVAGE, time.strftime("%Y-%m-%d"))
    os.makedirs(out, exist_ok=True)
    log = open(os.path.join(out, "NOTES.txt"), "a")
    log.write(f"\n# tools/wt.py gc {time.strftime('%H:%M')}\n"
              "# restore a worktree: git worktree add <dir> <sha>; git apply NAME.patch; tar xzf NAME.untracked.tgz\n"
              "# restore a branch: git branch <name> <sha>\n")
    for r in salvage:
        name = short(r["path"]).replace("/tmp/…/", "").strip("/").replace("/", "-")
        with open(os.path.join(out, name + ".patch"), "w") as f:
            f.write((git("diff", "HEAD", "--binary", cwd=r["path"]) or "") + "\n")
        if r["untracked"]:
            subprocess.run(["tar", "czf", os.path.join(out, name + ".untracked.tgz"), *r["untracked"]],
                           cwd=r["path"])
        log.write(f"worktree {name} {git('rev-parse', 'HEAD', cwd=r['path'])} {r['path']}\n")
    for r in go + salvage:
        if r in go:
            log.write(f"worktree {short(r['path'])} {git('rev-parse', 'HEAD', cwd=r['path'])} (clean)\n")
        subprocess.run(["git", "worktree", "remove", "--force", r["path"]])
    git("worktree", "prune")
    for b in branches:
        log.write(f"branch {b} {git('rev-parse', b)}\n")
        git("branch", "-D", b)
    print(f"done; undo notes in {out}/NOTES.txt")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="what", required=True)
    sub.add_parser("status").set_defaults(fn=cmd_status)
    p = sub.add_parser("gc")
    p.add_argument("--apply", action="store_true")
    p.add_argument("--salvage", action="store_true",
                   help="also /tmp scratch worktrees with uncommitted work: saved as patches, then removed")
    p.add_argument("--days", type=int, default=2, help="--salvage: only those this many days untouched")
    p.set_defaults(fn=cmd_gc)
    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
