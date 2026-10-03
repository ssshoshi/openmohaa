#!/usr/bin/env python3
"""Triage in-game reports and draft fixes, with Claude Code on this machine.

Runs `claude -p` (headless Claude Code, logged in with a Claude subscription:
no API key) once per issue:

  triage   every open `source:ingame` issue not yet labelled `triaged`: read
           the report, look for duplicates, find the likely code, reproduce it
           from the report's savegame on the real GPU (repro.py), and comment.
           Labels it `triaged`, and `agent:fixable` when the cause is clear.
  fix      every open issue the owner labelled `agent:fix`: in a fresh
           worktree on fix/<n>-<slug>, make the fix, build it for Windows,
           show before/after screenshots, and open a draft pull request that
           says `Fixes #<n>`. Nothing is ever merged: the owner reviews it.

Only issues opened by the owner are handled, and a fix only runs when the
owner added `agent:fix` (checked in the issue's events), so an issue from
anyone else cannot set the agent to work. Issue text is also treated as data
in the prompts, never as instructions.

  tools/bugreport/triage.py                  # one pass: triage new, fix requested
  tools/bugreport/triage.py --dry-run        # what a pass would do
  tools/bugreport/triage.py --issue 5 --mode triage
  tools/bugreport/triage.py --loop 900       # a pass every 15 minutes

Each run's transcript is kept in ~/.cache/openmohaa-bugreport/logs/.
"""

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import upload  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
OWNER = "ssshoshi"
DEFAULT_BUILD = "/mnt/d/Medal of Honor/openmohaa-play"  # the deployed build, for "before"
AGENT_DIR = os.path.expanduser("~/projects/openmohaa-agent")
LOG_DIR = os.path.join(upload.CACHE_DIR, "logs")
TIMEOUT = {"triage": 45 * 60, "fix": 120 * 60}

LABELS = {"working": "agent:working", "triaged": "triaged", "fix": "agent:fix",
          "pr": "agent:pr-open", "tried": "agent:attempted"}

# What the agent may do. Anything else is refused (--permission-mode dontAsk).
COMMON_TOOLS = [
    "Read", "Grep", "Glob", "Write",
    "Bash(gh issue view *)", "Bash(gh issue list *)", "Bash(gh pr list *)", "Bash(gh pr view *)",
    "Bash(git status*)", "Bash(git diff*)", "Bash(git log*)", "Bash(git show *)", "Bash(git grep *)",
    "Bash(python3 tools/bugreport/repro.py *)", "Bash(python3 tools/bugreport/post.py *)",
    "Bash(ls *)", "Bash(grep *)", "Bash(wc *)", "Bash(head *)", "Bash(tail *)",
]
TOOLS = {
    "triage": COMMON_TOOLS + ["Bash(gh issue edit * --add-label *)"],
    "fix": COMMON_TOOLS + [
        "Edit", "Bash(git add *)", "Bash(git commit *)", "Bash(git push -u fork fix/*)",
        "Bash(gh pr create *)", "Bash(cmake *)", "Bash(ninja *)",
    ],
}


def gh_json(args):
    return json.loads(upload.run(["gh"] + args).stdout or "null")


def label(repo, number, add=(), remove=()):
    cmd = ["gh", "issue", "edit", str(number), "-R", repo]
    for name in add:
        cmd += ["--add-label", name]
    for name in remove:
        cmd += ["--remove-label", name]
    if add or remove:
        subprocess.run(cmd, capture_output=True, text=True)


def labelled_by_owner(repo, number, name):
    """Whether the owner is who last added this label."""
    events = gh_json(["api", f"repos/{repo}/issues/{number}/events", "--paginate"]) or []
    adders = [e.get("actor", {}).get("login") for e in events
              if e.get("event") == "labeled" and e.get("label", {}).get("name") == name]
    return bool(adders) and adders[-1] == OWNER


def candidates(repo):
    issues = gh_json(["issue", "list", "-R", repo, "--label", "source:ingame", "--state", "open",
                      "--limit", "100", "--json", "number,title,labels,author"]) or []
    work = []
    for i in issues:
        names = {l["name"] for l in i["labels"]}
        if i["author"]["login"] != OWNER or LABELS["working"] in names:
            continue
        if LABELS["fix"] in names and LABELS["pr"] not in names:
            if labelled_by_owner(repo, i["number"], LABELS["fix"]):
                work.append((i["number"], "fix", i["title"]))
        elif LABELS["triaged"] not in names:
            work.append((i["number"], "triage", i["title"]))
    return work


def git(*args, cwd=ROOT):
    return upload.run(["git", "-C", cwd] + list(args)).stdout.strip()


def slug(title):
    s = re.sub(r"^\[[^\]]*\]\s*", "", title.lower())
    return re.sub(r"[^a-z0-9]+", "-", s).strip("-")[:40] or "report"


def workdir(mode, number, title, base):
    """A checkout of main to read (triage), or a fresh branch to fix on."""
    git("fetch", "-q", "fork")
    os.makedirs(AGENT_DIR, exist_ok=True)
    if mode == "triage":
        path = os.path.join(AGENT_DIR, "main")
        if not os.path.isdir(path):
            git("worktree", "add", "--detach", path, base)
        git("checkout", "-q", "--detach", base, cwd=path)
        git("reset", "-q", "--hard", cwd=path)
        return path, None
    branch = f"fix/{number}-{slug(title)}"
    path = os.path.join(AGENT_DIR, f"fix-{number}")
    if not os.path.isdir(path):
        existing = subprocess.run(["git", "-C", ROOT, "rev-parse", "--verify", "-q", branch],
                                  capture_output=True).returncode == 0
        if existing:
            git("worktree", "add", path, branch)
        else:
            git("worktree", "add", "-b", branch, path, base)
    return path, branch


def prompt(mode, repo, number, branch, build):
    with open(os.path.join(HERE, "prompts", f"{mode}.md"), encoding="utf-8") as f:
        text = f.read()
    assets = os.path.join(upload.CACHE_DIR, repo.replace("/", "_") + "-assets")
    return (text.replace("{issue}", str(number)).replace("{repo}", repo).replace("{build}", build)
            .replace("{assets}", assets).replace("{branch}", branch or ""))


def run_claude(mode, cwd, text, number):
    os.makedirs(LOG_DIR, exist_ok=True)
    log = os.path.join(LOG_DIR, f"issue-{number}-{mode}-{datetime.datetime.now():%Y%m%d-%H%M%S}.log")
    cmd = ["claude", "-p", text,
           "--permission-mode", "dontAsk",
           # No MCP servers or plugin connectors: the agent needs none.
           "--strict-mcp-config",
           "--allowedTools", " ".join(TOOLS[mode]),
           "--add-dir", upload.CACHE_DIR,
           "--add-dir", "/mnt/d/Medal of Honor/bugrepro",
           "--output-format", "text"]
    with open(log, "w", encoding="utf-8") as out:
        try:
            result = subprocess.run(cmd, cwd=cwd, stdout=out, stderr=subprocess.STDOUT, text=True,
                                    timeout=TIMEOUT[mode])
            code = result.returncode
        except subprocess.TimeoutExpired:
            out.write(f"\n[triage.py] timed out after {TIMEOUT[mode] // 60} minutes\n")
            code = -1
    tail = open(log, encoding="utf-8", errors="replace").read().strip().splitlines()[-1:] or [""]
    return code, log, tail[0]


def handle(repo, number, mode, title, build, dry, base="fork/main"):
    print(f"#{number} {mode}: {title}")
    if dry:
        return
    # The assets the agent reads (screenshots, savegame) must be current.
    upload.asset_checkout(repo)
    path, branch = workdir(mode, number, title, base)
    label(repo, number, add=[LABELS["working"]])
    try:
        code, log, last = run_claude(mode, path, prompt(mode, repo, number, branch, build), number)
    finally:
        label(repo, number, remove=[LABELS["working"]])
    print(f"  exit {code}: {last}\n  log {log}")
    limited = code != 0 and re.search(r"(session|usage|rate) limit", last, re.I) is not None

    if mode == "triage":
        # It was told to leave the checkout alone; make sure.
        git("reset", "-q", "--hard", cwd=path)
        git("clean", "-q", "-fd", cwd=path)
        if code == 0:
            label(repo, number, add=[LABELS["triaged"]])
        return "limit" if limited else None

    prs = gh_json(["pr", "list", "-R", repo, "--head", branch, "--state", "open", "--json", "url"]) or []
    if prs:
        label(repo, number, add=[LABELS["pr"]])
        print(f"  draft PR {prs[0]['url']}")
        if code != 0 and limited:
            return "limit"
    elif code != 0:
        # The run itself failed (a usage limit, a crash, the timeout), which is
        # not an attempt at the fix: agent:fix stays, and the next pass retries.
        print("  the run failed; left queued for the next pass")
        return "limit" if limited else None
    else:
        # Not asked again until the owner labels it agent:fix afresh.
        label(repo, number, add=[LABELS["tried"]], remove=[LABELS["fix"]])


def one_pass(repo, build, dry, base):
    work = candidates(repo)
    if not work:
        print("nothing to do")
    for number, mode, title in work:
        if handle(repo, number, mode, title, build, dry, base) == "limit":
            # Every run after this one would fail the same way.
            print("usage limit reached; stopping this pass")
            break


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--repo", default=upload.DEFAULT_REPO)
    ap.add_argument("--build", default=DEFAULT_BUILD, help="Windows binaries used for 'before' reproductions")
    ap.add_argument("--issue", type=int, help="handle one issue")
    ap.add_argument("--mode", choices=["triage", "fix"], help="with --issue: which step")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--base", default="fork/main", help="what the agent's checkouts start from")
    ap.add_argument("--loop", type=int, metavar="SECONDS", help="repeat a pass every SECONDS")
    args = ap.parse_args()

    if args.issue:
        info = gh_json(["issue", "view", str(args.issue), "-R", args.repo, "--json", "title,author"])
        if info["author"]["login"] != OWNER:
            sys.exit(f"#{args.issue} was not opened by {OWNER}; not handling it")
        handle(args.repo, args.issue, args.mode or "triage", info["title"], args.build, args.dry_run, args.base)
        return

    while True:
        one_pass(args.repo, args.build, args.dry_run, args.base)
        if not args.loop:
            break
        time.sleep(args.loop)


if __name__ == "__main__":
    main()
