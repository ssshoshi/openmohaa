#!/usr/bin/env python3
"""Reproduce an in-game report on the real Windows build, unattended.

Takes a report (from its GitHub issue, or a local bundle) and a set of Windows
binaries, and:

  1. builds an isolated install: the binaries in D:\\Medal of Honor\\bugrepro,
     with its own home path, so the user's installs, saves and config are
     never touched;
  2. gives it the reporter's captured cvars (windowed, 1280x720) and the
     report's savegame;
  3. loads the save (or, without one, devmap + setviewpos with cheats on),
     runs any extra commands, takes a screenshot, and quits;
  4. prints a JSON summary: the screenshot, the console log, what happened.

It refuses to run while the user's own game is open: it would take the focus,
and the harness must never touch the user's process.

  tools/bugreport/repro.py --issue 5 --build "/mnt/d/Medal of Honor/openmohaa-ragdoll" --tag before
  tools/bugreport/repro.py --issue 5 --build .cmake-win/RelWithDebInfo --tag after
  tools/bugreport/repro.py --report "<home>/bugreports/<id>" --build ... --extra "r_showtris 1"

Runs from WSL; the game is launched through cmd.exe.
"""

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import upload  # noqa: E402  (asset_checkout, run, default_homes)

GAME_ROOT = "/mnt/d/Medal of Honor"        # fs_basepath: main/Pak*.pk3
GAME_ROOT_WIN = r"D:\Medal of Honor"
REPRO_DIR = os.path.join(GAME_ROOT, "bugrepro")
REPRO_DIR_WIN = GAME_ROOT_WIN + r"\bugrepro"
BINARIES = ["openmohaa.exe", "cgame.dll", "game.dll", "renderer_opengl1.dll", "renderer_opengl2.dll"]
RUNTIME = ["SDL2.dll", "OpenAL64.dll", "libcurl.dll"]  # from any install when the build lacks them
FOCUS = os.path.join(os.path.dirname(HERE), "win-bench", "focus.ps1")
LOAD_WAIT_MS = 30000


def game_running(exclude=REPRO_DIR_WIN):
    out = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command",
         "Get-Process openmohaa -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Path"],
        capture_output=True, text=True).stdout
    return [p.strip() for p in out.splitlines() if p.strip() and exclude.lower() not in p.lower()]


def report_from_issue(repo, number):
    body = upload.run(["gh", "issue", "view", str(number), "-R", repo, "--json", "body", "--jq", ".body"]).stdout
    m = re.search(r"<!-- bugreport-json\s*(\{.*\})\s*-->", body, re.S)
    if not m:
        sys.exit(f"issue #{number} has no bugreport-json block; not an in-game report")
    report = json.loads(m.group(1).replace("--\\>", "-->"))
    assets = os.path.join(upload.asset_checkout(repo), "reports", report["id"])
    if not os.path.isdir(assets):
        sys.exit(f"no assets for {report['id']} on the asset branch")
    return report, assets


def report_from_bundle(bundle):
    """A local bundle, laid out like the asset folder the uploader makes."""
    with open(os.path.join(bundle, "report.json"), encoding="utf-8") as f:
        report = json.load(f)
    home = os.path.dirname(os.path.dirname(os.path.abspath(bundle)))
    stage = os.path.join(REPRO_DIR, "_bundle")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    for n in os.listdir(bundle):
        shutil.copyfile(os.path.join(bundle, n), os.path.join(stage, n))
    cfg = os.path.join(home, report["files"].get("config", ""))
    if os.path.isfile(cfg):
        shutil.copyfile(cfg, os.path.join(stage, "config.cfg"))
    save = report["game"].get("savegame")
    if save:
        for p in glob.glob(os.path.join(home, "save", "*", save + ".*")):
            shutil.copyfile(p, os.path.join(stage, os.path.basename(p)))
    return report, stage


def windowed(cfg_text):
    """The reporter's cvars, in a window the harness can drive."""
    over = {"r_fullscreen": "0", "r_mode": "-1", "r_customwidth": "1280", "r_customheight": "720"}
    lines = []
    for line in cfg_text.splitlines():
        m = re.match(r'\s*seta?\s+(\S+)', line)
        if m and m.group(1).lower() in over:
            continue
        lines.append(line)
    lines += [f'seta {k} "{v}"' for k, v in over.items()]
    return "\n".join(lines) + "\n"


def prepare(build, report, assets):
    os.makedirs(REPRO_DIR, exist_ok=True)
    for n in BINARIES:
        src = os.path.join(build, n)
        if not os.path.isfile(src):
            sys.exit(f"{n} missing from {build}")
        shutil.copyfile(src, os.path.join(REPRO_DIR, n))
    for n in RUNTIME:
        src = os.path.join(build, n)
        if not os.path.isfile(src):
            found = glob.glob(os.path.join(GAME_ROOT, "openmohaa-*", n))
            src = found[0] if found else None
        if src:
            shutil.copyfile(src, os.path.join(REPRO_DIR, n))

    home = os.path.join(REPRO_DIR, "home", "main")
    shutil.rmtree(os.path.join(REPRO_DIR, "home"), ignore_errors=True)
    os.makedirs(os.path.join(home, "configs"))
    os.makedirs(os.path.join(home, "save", "omconfig"))

    cfg = os.path.join(assets, "config.cfg")
    if not os.path.isfile(cfg):
        # The user's own settings, when the report did not carry any.
        mine = [os.path.join(h, "configs", "omconfig.cfg") for h in upload.default_homes()]
        cfg = next((c for c in mine if os.path.isfile(c)), None)
    text = open(cfg, encoding="utf-8", errors="replace").read() if cfg else ""
    with open(os.path.join(home, "configs", "omconfig.cfg"), "w", encoding="utf-8") as f:
        f.write(windowed(text))

    save = report["game"].get("savegame")
    have_save = False
    if save:
        for p in glob.glob(os.path.join(assets, save + ".*")):
            shutil.copyfile(p, os.path.join(home, "save", "omconfig", os.path.basename(p)))
            have_save = have_save or p.endswith(".ssv")
    return home, have_save


def script(report, have_save, tag, extra):
    g = report["game"]
    out = ["echo REPRO start"]
    if have_save:
        out += [f"loadgame {g['savegame']}"]
    else:
        out += [g["repro"].split(";")[0].strip()]  # devmap <map>
    out += [f"wait {LOAD_WAIT_MS}", "finishloadingscreen", "wait 3000"]
    if not have_save:
        out += ["thereisnomonkey 1", g["repro"].split(";")[1].strip(), "wait 1000"]
    for cmd in extra:
        out += [cmd, "wait 500"]
    out += ["wait 1500", "echo REPRO shot", f"screenshotJPEG repro_{tag}", "wait 1500", "echo REPRO done", "quit"]
    return "\n".join(out) + "\n"


def launch(home, report, timeout, sets=()):
    target = 0
    version = report["build"].get("version", "")
    if "Spearhead" in version:
        target = 1
    elif "Breakthrough" in version:
        target = 2
    bat = os.path.join(REPRO_DIR, "run.bat")
    with open(bat, "w", newline="\r\n") as f:
        f.write("@echo off\n")
        f.write(f'cd /d "{REPRO_DIR_WIN}"\n')
        f.write(f'openmohaa.exe +set fs_basepath "{GAME_ROOT_WIN}" +set fs_homepath "{REPRO_DIR_WIN}\\home" '
                f"+set logfile 2 +set developer 1 +set cheats 1 +set cl_playintro 0 +set ui_skip_eamovie 1 "
                f"+set ui_skip_titlescreen 1 +set ui_skip_legalscreen 1 +set com_target_game {target} "
                + "".join(f'+set {k} "{v}" ' for k, v in sets)
                + "+exec repro.cfg\n")
    proc = subprocess.Popen(["cmd.exe", "/c", REPRO_DIR_WIN + r"\run.bat"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(8)
    if os.path.isfile(FOCUS):
        subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        subprocess.run(["wslpath", "-w", FOCUS], capture_output=True, text=True).stdout.strip()],
                       capture_output=True)
    try:
        proc.wait(timeout=timeout)
        return True
    except subprocess.TimeoutExpired:
        # Only the harness's own process, never the user's.
        subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                        f"Get-Process openmohaa -ErrorAction SilentlyContinue | "
                        f"Where-Object {{ $_.Path -like '{REPRO_DIR_WIN}*' }} | Stop-Process -Force"],
                       capture_output=True)
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--issue", type=int, help="GitHub issue of an in-game report")
    src.add_argument("--report", help="a local report bundle folder")
    ap.add_argument("--repo", default=upload.DEFAULT_REPO)
    ap.add_argument("--build", required=True, help="folder with openmohaa.exe, cgame.dll, game.dll and the renderers")
    ap.add_argument("--tag", default="repro", help="name for this run's screenshot (before, after...)")
    ap.add_argument("--extra", action="append", default=[], help="console command to run before the screenshot")
    ap.add_argument("--set", action="append", default=[], metavar="CVAR=VALUE",
                    help="a cvar set at launch, before the renderer starts (repeatable)")
    ap.add_argument("--out", help="folder to copy the screenshot and log to")
    ap.add_argument("--timeout", type=int, default=180)
    args = ap.parse_args()

    running = game_running()
    if running:
        sys.exit(f"the game is running ({running[0]}); not reproducing while it is")

    report, assets = report_from_issue(args.repo, args.issue) if args.issue else report_from_bundle(args.report)
    home, have_save = prepare(args.build, report, assets)
    with open(os.path.join(home, "repro.cfg"), "w", newline="\n") as f:
        f.write(script(report, have_save, args.tag, args.extra))

    sets = [tuple(s.split("=", 1)) for s in args.set if "=" in s]
    finished = launch(home, report, args.timeout, sets)

    log = os.path.join(home, "qconsole.log")
    text = open(log, encoding="utf-8", errors="replace").read().replace("\r", "") if os.path.isfile(log) else ""
    shot = os.path.join(home, "screenshots", f"repro_{args.tag}.jpg")
    out_dir = args.out or os.path.join(REPRO_DIR, "results", f"{report['id']}_{args.tag}")
    os.makedirs(out_dir, exist_ok=True)
    result = {
        "report": report["id"],
        "tag": args.tag,
        "used_save": have_save,
        "finished": finished,
        "reached_shot": "REPRO shot" in text,
        "screenshot": None,
        "log": None,
        "errors": [l for l in text.splitlines() if re.search(r"\b(ERROR|Error:|FATAL|Couldn't load)", l)][-20:],
    }
    if os.path.isfile(shot):
        result["screenshot"] = os.path.join(out_dir, os.path.basename(shot))
        shutil.copyfile(shot, result["screenshot"])
    if text:
        result["log"] = os.path.join(out_dir, "console.txt")
        with open(result["log"], "w", encoding="utf-8") as f:
            f.write(text)
    print(json.dumps(result, indent=1))
    sys.exit(0 if result["screenshot"] else 1)


if __name__ == "__main__":
    main()
