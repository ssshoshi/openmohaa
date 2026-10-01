#!/usr/bin/env python3
"""Drive the live orchestrator install from WSL.

The game runs from its own install, D:\\Medal of Honor\\openmohaa-live, with its
own home path (home\\ under it), so nothing done live ever reaches the user's
other installs, their saves or the shared main folder. The game reads console
commands from home\\main\\orch\\cmd (com_cmddir) and the voice sidecar speaks
what is written to home\\main\\orch\\say.

  orch.py setup --build <dir>        copy the five binaries (and runtime DLLs) in
  orch.py seed                       copy the user's config, physics.txt and saves in
  orch.py launch [--save X|--map Y]  start the game (detached)
  orch.py cmd "viewpos; orch_state"  run console commands, print their output
  orch.py say "text"                 speak it and show it in the game's panel
  orch.py heard "text"               show what the player said, as understood
  orch.py state                      where the player is and what they look at
  orch.py status                     is the game / the sidecar running
  orch.py stop                       close the live game (never any other)
"""

import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time

GAME_ROOT = "/mnt/d/Medal of Honor"
GAME_ROOT_WIN = r"D:\Medal of Honor"
LIVE = os.environ.get("ORCH_LIVE", os.path.join(GAME_ROOT, "openmohaa-live"))
HOME = os.path.join(LIVE, "home")
MAIN = os.path.join(HOME, "main")
ORCH = os.path.join(MAIN, "orch")
CMD_DIR = os.path.join(ORCH, "cmd")
SAY_DIR = os.path.join(ORCH, "say")
SIDECAR_ALIVE = os.path.join(ORCH, "sidecar.alive")

BINARIES = ["openmohaa.exe", "cgame.dll", "game.dll", "renderer_opengl1.dll", "renderer_opengl2.dll"]
RUNTIME = ["SDL2.dll", "OpenAL64.dll", "libcurl.dll"]
HERE = os.path.dirname(os.path.abspath(__file__))
FOCUS = os.path.join(os.path.dirname(HERE), "win-bench", "focus.ps1")


def winpath(p):
    return subprocess.run(["wslpath", "-w", p], capture_output=True, text=True).stdout.strip()


def user_home():
    """The user's real home path (read only: it is never written to)."""
    found = glob.glob("/mnt/c/Users/*/AppData/Roaming/openmohaa/main")
    return found[0] if found else None


def live_processes():
    live_win = winpath(LIVE).lower()
    out = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command",
         "Get-Process openmohaa -ErrorAction SilentlyContinue | ForEach-Object { \"$($_.Id) $($_.Path)\" }"],
        capture_output=True, text=True).stdout
    procs = []
    for line in out.splitlines():
        pid, _, path = line.strip().partition(" ")
        if pid:
            procs.append((int(pid), path, path.lower().startswith(live_win)))
    return procs


# ---------------------------------------------------------------- install

def cmd_setup(args):
    if any(live for _, _, live in live_processes()):
        sys.exit("the live game is running; stop it first (orch.py stop)")
    os.makedirs(LIVE, exist_ok=True)
    for n in BINARIES:
        src = os.path.join(args.build, n)
        if not os.path.isfile(src):
            sys.exit(f"{n} missing from {args.build}")
        shutil.copyfile(src, os.path.join(LIVE, n))
    for n in RUNTIME:
        src = os.path.join(args.build, n)
        if not os.path.isfile(src):
            found = sorted(glob.glob(os.path.join(GAME_ROOT, "openmohaa-ragdoll", n))
                           + glob.glob(os.path.join(GAME_ROOT, "openmohaa-*", n)))
            src = found[0] if found else None
        if src:
            shutil.copyfile(src, os.path.join(LIVE, n))
    # DLLs swapped out while the game ran (hotswap) are free to go now.
    for old in glob.glob(os.path.join(LIVE, "*.old-*.dll")):
        os.remove(old)
    print(f"installed {len(BINARIES)} binaries into {LIVE}")


def cmd_seed(args):
    src = args.home or user_home()
    if not src:
        sys.exit("can't find the user's home path; pass --home")
    os.makedirs(os.path.join(MAIN, "configs"), exist_ok=True)
    copied = []
    cfg = os.path.join(src, "configs", "omconfig.cfg")
    if os.path.isfile(cfg):
        text = open(cfg, encoding="utf-8", errors="replace").read()
        if args.windowed:
            text = windowed(text)
        with open(os.path.join(MAIN, "configs", "omconfig.cfg"), "w", encoding="utf-8") as f:
            f.write(text)
        copied.append("omconfig.cfg")
    for name in ["physics.txt"]:
        if os.path.isfile(os.path.join(src, name)):
            shutil.copyfile(os.path.join(src, name), os.path.join(MAIN, name))
            copied.append(name)
    saves = os.path.join(src, "save")
    if os.path.isdir(saves) and not args.no_saves:
        shutil.copytree(saves, os.path.join(MAIN, "save"), dirs_exist_ok=True)
        copied.append("save/")
    print("seeded " + ", ".join(copied) + f" from {src}")


def windowed(cfg_text):
    import re
    over = {"r_fullscreen": "0", "r_mode": "-1", "r_customwidth": "1280", "r_customheight": "720"}
    lines = [l for l in cfg_text.splitlines()
             if not (re.match(r'\s*seta?\s+(\S+)', l) and re.match(r'\s*seta?\s+(\S+)', l).group(1).lower() in over)]
    lines += [f'seta {k} "{v}"' for k, v in over.items()]
    return "\n".join(lines) + "\n"


def cmd_launch(args):
    if any(live for _, _, live in live_processes()):
        sys.exit("the live game is already running")
    for d in (CMD_DIR, SAY_DIR):
        os.makedirs(d, exist_ok=True)
    # Commands left over from the last run would fire at start.
    for stale in glob.glob(os.path.join(CMD_DIR, "*")):
        os.remove(stale)

    start = []
    if args.save:
        start = [f"loadgame {args.save}"]
    elif args.map:
        start = [f"devmap {args.map}"]
    with open(os.path.join(MAIN, "orch_start.cfg"), "w", newline="\n") as f:
        f.write("\n".join(start + ["waitload", "orch on"] + (args.extra or [])) + "\n")

    sets = {
        "fs_basepath": GAME_ROOT_WIN,
        "fs_homepath": winpath(HOME),
        "com_cmddir": "orch/cmd",
        "logfile": "2",
        "developer": "1",
        "cheats": "1",
        "thereisnomonkey": "1",
        "cl_playintro": "0",
        "ui_autoContinue": "1",
        "ui_skip_eamovie": "1",
        "ui_skip_titlescreen": "1",
        "ui_skip_legalscreen": "1",
    }
    line = "openmohaa.exe " + " ".join(f'+set {k} "{v}"' for k, v in sets.items()) + " +exec orch_start.cfg"
    bat = os.path.join(LIVE, "run.bat")
    with open(bat, "w", newline="\r\n") as f:
        f.write("@echo off\n")
        f.write(f'cd /d "{winpath(LIVE)}"\n')
        f.write("start \"\" " + line + "\n")
    # No pipes: the game inherits them and the call would wait for it to exit.
    subprocess.run(["cmd.exe", "/c", winpath(bat)], cwd="/mnt/c",
                   stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if args.focus and os.path.isfile(FOCUS):
        time.sleep(6)
        subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", winpath(FOCUS)],
                       capture_output=True)
    print("launched" + (f" ({start[0]})" if start else ""))


def cmd_stop(args):
    live = [pid for pid, _, is_live in live_processes() if is_live]
    for pid in live:
        subprocess.run(["powershell.exe", "-NoProfile", "-Command", f"Stop-Process -Id {pid} -Force"],
                       capture_output=True)
    print(f"stopped {len(live)} live process(es)")


# ---------------------------------------------------------------- talking to the game

def run_commands(text, timeout=5.0):
    """Runs console commands through the command directory; returns their output,
    or None when the game didn't pick them up in time."""
    os.makedirs(CMD_DIR, exist_ok=True)
    name = f"{time.time_ns()}"
    tmp = os.path.join(CMD_DIR, name + ".tmp_w")
    with open(tmp, "w", newline="\n", encoding="utf-8") as f:
        f.write(text.rstrip("\n") + "\n")
    os.replace(tmp, os.path.join(CMD_DIR, name + ".txt"))

    out = os.path.join(CMD_DIR, name + ".out")
    deadline = time.time() + timeout
    while time.time() < deadline:
        if os.path.isfile(out):
            with open(out, encoding="utf-8", errors="replace") as f:
                result = f.read()
            os.remove(out)
            return result
        time.sleep(0.05)
    # Not picked up: take it back so it doesn't fire later by surprise.
    try:
        os.remove(os.path.join(CMD_DIR, name + ".txt"))
    except FileNotFoundError:
        pass
    return None


def quote(text):
    # The console has no escapes inside quotes.
    return '"' + text.replace('"', "'").replace("\n", " ") + '"'


def cmd_cmd(args):
    result = run_commands(" ; ".join(args.commands) if not args.lines else "\n".join(args.commands), args.timeout)
    if result is None:
        sys.exit("the game didn't take the commands (not running, or still loading?)")
    sys.stdout.write(result)


def cmd_say(args):
    text = " ".join(args.text)
    os.makedirs(SAY_DIR, exist_ok=True)
    name = f"{time.time_ns()}"
    tmp = os.path.join(SAY_DIR, name + ".tmp_w")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text)
    os.replace(tmp, os.path.join(SAY_DIR, name + ".txt"))
    if run_commands("orch_msg " + quote(text), timeout=2.0) is None:
        print("(the game didn't take the message; spoken only)", file=sys.stderr)


def cmd_heard(args):
    run_commands("orch_msg -heard " + quote(" ".join(args.text)), timeout=2.0)


def cmd_state(args):
    out = run_commands("orch_state", args.timeout)
    if out is None:
        sys.exit("the game didn't answer")
    for line in out.splitlines():
        if line.startswith("{"):
            print(json.dumps(json.loads(line), indent=1))
            return
    sys.stdout.write(out)


def cmd_status(args):
    procs = live_processes()
    live = [p for p in procs if p[2]]
    alive_age = time.time() - os.path.getmtime(SIDECAR_ALIVE) if os.path.isfile(SIDECAR_ALIVE) else None
    print(json.dumps({
        "live_game_running": bool(live),
        "other_games_running": [p[1] for p in procs if not p[2]],
        "sidecar_running": alive_age is not None and alive_age < 5,
        "install": LIVE,
    }, indent=1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="what", required=True)

    p = sub.add_parser("setup")
    p.add_argument("--build", required=True, help="folder with openmohaa.exe, cgame.dll, game.dll and the renderers")
    p.set_defaults(fn=cmd_setup)

    p = sub.add_parser("seed")
    p.add_argument("--home", help="the user's home path main/ folder (found by default)")
    p.add_argument("--windowed", action="store_true", help="1280x720 in a window, for unattended tests")
    p.add_argument("--no-saves", action="store_true")
    p.set_defaults(fn=cmd_seed)

    p = sub.add_parser("launch")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--save")
    g.add_argument("--map")
    p.add_argument("--extra", action="append", help="console command to run once loaded")
    p.add_argument("--focus", action="store_true", help="foreground the window after start")
    p.set_defaults(fn=cmd_launch)

    p = sub.add_parser("stop")
    p.set_defaults(fn=cmd_stop)

    p = sub.add_parser("cmd")
    p.add_argument("commands", nargs="+")
    p.add_argument("--lines", action="store_true", help="each argument is a line (default: joined with ;)")
    p.add_argument("--timeout", type=float, default=5.0)
    p.set_defaults(fn=cmd_cmd)

    p = sub.add_parser("say")
    p.add_argument("text", nargs="+")
    p.set_defaults(fn=cmd_say)

    p = sub.add_parser("heard")
    p.add_argument("text", nargs="+")
    p.set_defaults(fn=cmd_heard)

    p = sub.add_parser("state")
    p.add_argument("--timeout", type=float, default=5.0)
    p.set_defaults(fn=cmd_state)

    p = sub.add_parser("status")
    p.set_defaults(fn=cmd_status)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
