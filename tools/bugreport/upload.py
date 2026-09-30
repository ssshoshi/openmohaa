#!/usr/bin/env python3
"""File the game's in-game reports as GitHub issues.

The game writes each report to <homepath>/main/bugreports/<id>/ with a
`pending` marker (code/cgame/cg_bugreport.cpp). For each pending one this:

  1. stamps the overlay (what was under the crosshair, where the player stood)
     onto the marked screenshot, since the game's screenshots are taken before
     its 2D pass is drawn;
  2. commits the screenshots, the savegame, the cvars, the game's and the sound
     system's dumps to reports/<id>/ on the repo's orphan `bug-assets` branch,
     so the issue can show the images and link the rest (GitHub's API cannot
     attach files to an issue);
  3. opens the issue, labelled by type, area and source;
  4. replaces `pending` with `sent`, holding the issue's URL.

Usage:
  tools/bugreport/upload.py                # every pending report
  tools/bugreport/upload.py --dry-run      # show what would be filed
  tools/bugreport/upload.py --home "/mnt/d/Medal of Honor/br-test/home/main"

Needs `gh` logged in with repo scope, git, and Pillow (for the overlay stamp;
without it the marked screenshot goes up as it is).
"""

import argparse
import datetime
import glob
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

DEFAULT_REPO = "ssshoshi/openmohaa"
ASSET_BRANCH = "bug-assets"
CACHE_DIR = os.path.expanduser("~/.cache/openmohaa-bugreport")
MIN_AGE = 10  # seconds: the savegame is written a frame or two after the marker
CONSOLE_TAIL = 300  # lines of qconsole.log kept with a report

AREAS = {
    "physics": "Physics",
    "texture": "Texture",
    "lighting": "Lighting",
    "sound": "Sound",
    "model": "Model",
    "script": "Script/AI",
    "gameplay": "Gameplay",
    "performance": "Performance",
    "other": "Other",
}


def run(cmd, **kw):
    kw.setdefault("text", True)
    kw.setdefault("capture_output", True)
    result = subprocess.run(cmd, **kw)
    if result.returncode:
        sys.exit(f"failed: {' '.join(cmd)}\n{result.stderr.strip()}")
    return result


def default_homes():
    """The game's home paths on this machine, WSL or not."""
    homes = glob.glob("/mnt/c/Users/*/AppData/Roaming/openmohaa/main")
    homes += glob.glob(os.path.expanduser("~/.openmohaa/main"))
    if os.environ.get("APPDATA"):
        homes.append(os.path.join(os.environ["APPDATA"], "openmohaa", "main"))
    return [h for h in homes if os.path.isdir(h)]


def pending_reports(home):
    for marker in sorted(glob.glob(os.path.join(home, "bugreports", "*", "pending"))):
        bundle = os.path.dirname(marker)
        if os.path.exists(os.path.join(bundle, "sent")):
            continue
        if time.time() - os.path.getmtime(marker) < MIN_AGE:
            continue
        if not os.path.exists(os.path.join(bundle, "report.json")):
            continue
        yield bundle


def stamp_overlay(src, dst, report):
    """The overlay the player saw, drawn onto the marked screenshot."""
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError:
        shutil.copyfile(src, dst)
        return

    img = Image.open(src).convert("RGB")
    draw = ImageDraw.Draw(img, "RGBA")
    size = max(12, img.height // 48)
    try:
        font = ImageFont.load_default(size=size)
    except TypeError:  # Pillow < 10.1
        font = ImageFont.load_default()

    game = report["game"]
    lines = [f"REPORT: {report['type']} / {report['category']}   {report['title']}"]
    lines += report["target"].get("overlay", [])
    o, a = game["view_origin"], game["view_angles"]
    lines.append(f"at {o[0]:.0f} {o[1]:.0f} {o[2]:.0f}  facing {a[0]:.0f} {a[1]:.0f}  on {game['map']}")

    x, y = img.width // 80, int(img.height * 0.3)
    pad, step = size // 3, int(size * 1.35)
    width = max(draw.textlength(line, font=font) for line in lines)
    draw.rectangle([x - pad, y - pad, x + width + pad, y + step * len(lines) + pad], fill=(0, 0, 0, 150))
    for i, line in enumerate(lines):
        colour = (255, 255, 80) if i == 0 else (235, 235, 235)
        draw.text((x, y + i * step), line, font=font, fill=colour)
    img.save(dst, quality=90)


def collect(home, bundle, report, stage):
    """Copies what goes up into stage/, returning {name: description}."""
    rid = report["id"]
    files = {}

    def take(src, name, what):
        if src and os.path.isfile(src):
            shutil.copyfile(src, os.path.join(stage, name))
            files[name] = what

    shot = os.path.join(home, report["files"]["screenshot"])
    marked = os.path.join(home, report["files"]["screenshot_marked"])
    take(shot, "shot.jpg", "screenshot")
    if os.path.isfile(marked):
        stamp_overlay(marked, os.path.join(stage, "shot_marked.jpg"), report)
        files["shot_marked.jpg"] = "screenshot with the target marked"

    for name in ("server.txt", "sounds.txt"):
        take(os.path.join(bundle, name), name, name)
    take(os.path.join(home, report["files"].get("config", "")), "config.cfg", "archived cvars")

    save = report["game"].get("savegame")
    if save:
        for path in glob.glob(os.path.join(home, "save", "*", save + ".*")):
            ext = os.path.splitext(path)[1]
            take(path, f"{save}{ext}", "savegame")

    # The console around the report, when the log is from the same session.
    log = os.path.join(home, "qconsole.log")
    made = os.path.getmtime(os.path.join(bundle, "report.json"))
    if os.path.isfile(log) and os.path.getmtime(log) >= made:
        with open(log, encoding="utf-8", errors="replace") as f:
            tail = f.readlines()[-CONSOLE_TAIL:]
        with open(os.path.join(stage, "console.txt"), "w", encoding="utf-8") as f:
            f.writelines(line.replace("\r", "") for line in tail)
        files["console.txt"] = f"last {CONSOLE_TAIL} console lines"

    shutil.copyfile(os.path.join(bundle, "report.json"), os.path.join(stage, "report.json"))
    files["report.json"] = "everything the client captured"
    return files


def asset_checkout(repo):
    """A local checkout of the asset branch, made or brought up to date."""
    path = os.path.join(CACHE_DIR, repo.replace("/", "_") + "-assets")
    url = f"https://github.com/{repo}.git"
    if not os.path.isdir(os.path.join(path, ".git")):
        os.makedirs(path, exist_ok=True)
        run(["git", "init", "-q", path])
        run(["git", "-C", path, "remote", "add", "origin", url])
    remote = run(["git", "-C", path, "ls-remote", "--heads", "origin", ASSET_BRANCH]).stdout.strip()
    if remote:
        run(["git", "-C", path, "fetch", "-q", "--depth", "1", "origin", ASSET_BRANCH])
        run(["git", "-C", path, "checkout", "-q", "-B", ASSET_BRANCH, "FETCH_HEAD"])
    else:
        run(["git", "-C", path, "checkout", "-q", "--orphan", ASSET_BRANCH])
        with open(os.path.join(path, "README.md"), "w") as f:
            f.write("Screenshots, saves and dumps for in-game reports (tools/bugreport/upload.py).\n"
                    "One folder per report under reports/.\n")
        run(["git", "-C", path, "add", "README.md"])
        run(["git", "-C", path, "commit", "-q", "-m", "chore: start the in-game report assets"])
    return path


def push_assets(repo, rid, stage, folder="reports", message=None):
    """Commits stage/'s files to <folder>/<rid>/ on the asset branch; returns
    (raw URL base, tree URL) pinned to that commit."""
    path = asset_checkout(repo)
    sub = f"{folder}/{rid}"
    dest = os.path.join(path, sub)
    os.makedirs(dest, exist_ok=True)
    for name in os.listdir(stage):
        shutil.copyfile(os.path.join(stage, name), os.path.join(dest, name))
    run(["git", "-C", path, "add", sub])
    # Nothing new when a report is sent again after the issue failed.
    if subprocess.run(["git", "-C", path, "diff", "--cached", "--quiet"]).returncode:
        run(["git", "-C", path, "commit", "-q", "-m", message or f"chore: assets for in-game report {rid}"])
    # gh's credential helper, so no separate git login is needed.
    run(["git", "-C", path, "-c", "credential.helper=", "-c", "credential.helper=!gh auth git-credential",
         "push", "-q", "origin", f"HEAD:refs/heads/{ASSET_BRANCH}"])
    sha = run(["git", "-C", path, "rev-parse", "HEAD"]).stdout.strip()
    return f"https://raw.githubusercontent.com/{repo}/{sha}/{sub}", \
        f"https://github.com/{repo}/tree/{sha}/{sub}"


def fence(text, lang=""):
    return f"```{lang}\n{text.rstrip()}\n```"


def details(summary, body):
    return f"<details><summary>{summary}</summary>\n\n{body}\n\n</details>\n"


def issue_body(report, files, raw, tree, stage):
    g, b, aim, t = report["game"], report["build"], report["aim"], report["target"]
    o, a = g["view_origin"], g["view_angles"]
    lines = []

    if report["description"].strip():
        lines += [report["description"].strip().replace("\\n", "\n"), ""]

    shots = [n for n in ("shot_marked.jpg", "shot.jpg") if n in files]
    if shots:
        lines += [" ".join(f"![{n}]({raw}/{n})" for n in shots), ""]

    lines += [
        "| | |",
        "|---|---|",
        f"| Type / area | {report['type']} / {AREAS.get(report['category'], report['category'])} |",
        f"| Map | `{g['map']}` |",
        f"| View | `{o[0]:.0f} {o[1]:.0f} {o[2]:.0f}`, pitch {a[0]:.0f}, yaw {a[1]:.0f} |",
        f"| Repro | `{g['repro']}` |",
    ]
    if g.get("savegame") and any(n.startswith(g["savegame"]) for n in files):
        lines.append(f"| Savegame | `loadgame {g['savegame']}` (files in [assets]({tree})) |")
    lines += [
        f"| Build | {b['version']} |",
        f"| Renderer | {b['renderer']} on {b['gl_renderer']}, {b['resolution'][0]}x{b['resolution'][1]}, {b['fps']} fps |",
        "",
        "**Target**",
        "",
    ]

    kind = t["kind"]
    if kind in ("entity", "static_model"):
        num = f"entity #{t['entnum']}" if kind == "entity" else f"static model #{t['index']}"
        lines.append(f"- {num}: `{t['model']}` at `{' '.join(f'{v:.0f}' for v in t['origin'])}`")
        shaders = ", ".join(f"`{s['surface']}`=`{s['shader']}`" for s in t.get("model_shaders", []) if s["shader"])
        if shaders:
            lines.append(f"- surfaces: {shaders}")
        if kind == "static_model":
            lines.append(f"- physics: {'solid' if t['solid'] else 'not solid'}"
                         f"{', moves' if t['dynamic'] else ''}; {t['physics_why']}")
    else:
        lines.append(f"- {kind}")
    if aim.get("hit"):
        through = f" (behind invisible `{aim['through_invisible']}`)" if aim.get("through_invisible") else ""
        lines.append(f"- crosshair on `{aim['shader']}`{through} at `{' '.join(f'{v:.0f}' for v in aim['position'])}`, "
                     f"{aim['distance']} units; light grid there `{' '.join(f'{v:.0f}' for v in aim['light'])}`")
    lines.append("")

    for name, summary in (("server.txt", "Game state (entity, AI, scripts)"),
                          ("sounds.txt", "Sounds playing"),
                          ("console.txt", "Console")):
        path = os.path.join(stage, name)
        if name in files and os.path.getsize(path):
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
            if len(text) > 20000:
                text = text[-20000:]
            lines.append(details(summary, fence(text)))

    lines.append(f"All files: [reports/{report['id']}]({tree})")
    lines.append("")
    lines.append("<!-- bugreport-json")
    lines.append(json.dumps(report, indent=1).replace("-->", "--\\>"))
    lines.append("-->")
    return "\n".join(lines)


def file_issue(repo, report, body, dry):
    title = f"[{AREAS.get(report['category'], report['category'])}] {report['title']}"
    labels = [report["type"], f"area:{report['category']}", "source:ingame"]
    if dry:
        print(f"  would open: {title}  labels {labels}")
        return None
    with tempfile.NamedTemporaryFile("w", suffix=".md", delete=False, encoding="utf-8") as f:
        f.write(body)
        body_file = f.name
    try:
        cmd = ["gh", "issue", "create", "-R", repo, "--title", title, "--body-file", body_file]
        for label in labels:
            cmd += ["--label", label]
        return run(cmd).stdout.strip().splitlines()[-1]
    finally:
        os.unlink(body_file)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--home", action="append", help="the game's <homepath>/main (repeatable)")
    ap.add_argument("--repo", default=DEFAULT_REPO)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    homes = args.home or default_homes()
    if not homes:
        sys.exit("no game home path found; pass --home <homepath>/main")

    filed = 0
    for home in homes:
        for bundle in pending_reports(home):
            with open(os.path.join(bundle, "report.json"), encoding="utf-8") as f:
                report = json.load(f)
            rid = report["id"]
            print(f"{rid}: {report['type']} / {report['category']}: {report['title']}")

            with tempfile.TemporaryDirectory() as stage:
                files = collect(home, bundle, report, stage)
                if args.dry_run:
                    print(f"  files: {', '.join(sorted(files))}")
                    raw = tree = f"https://example.invalid/{rid}"
                    body = issue_body(report, files, raw, tree, stage)
                    file_issue(args.repo, report, body, True)
                    continue
                raw, tree = push_assets(args.repo, rid, stage)
                body = issue_body(report, files, raw, tree, stage)
                url = file_issue(args.repo, report, body, False)

            with open(os.path.join(bundle, "sent"), "w") as f:
                f.write(f"{url}\n{datetime.datetime.now().isoformat(timespec='seconds')}\n")
            os.remove(os.path.join(bundle, "pending"))
            print(f"  filed {url}")
            filed += 1

    if not args.dry_run:
        print(f"{filed} report(s) filed")


if __name__ == "__main__":
    main()
