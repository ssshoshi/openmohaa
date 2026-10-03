#!/usr/bin/env python3
"""Drive the live orchestrator install from WSL.

The game runs from its own install, D:\\Medal of Honor\\openmohaa-live, with its
own home path (home\\ under it), so nothing done live ever reaches the user's
other installs, their saves or the shared main folder. The game reads console
commands from home\\main\\orch\\cmd (com_cmddir) and the voice sidecar speaks
what is written to home\\main\\orch\\say.

  orch.py setup --build <dir>        copy the five binaries (and runtime DLLs) in
  orch.py swap cgame|game|renderer [--build build/win64]
                                     build one module and swap it into the running
                                     game: rename the loaded DLL, copy, reload
                                     (vid_restart, or save/killserver/load for game)
  orch.py script "<code>" [--label L] | --file F
                                     run script code in the game now (orch_runscript)
  orch.py seed                       copy the user's config, physics.txt and saves in
  orch.py launch [--save X|--map Y]  start the game (detached)
  orch.py cmd "viewpos; orch_state"  run console commands, print their output
  orch.py say "text"                 speak it and show it in the game's panel
  orch.py heard "text"               show what the player said, as understood
  orch.py state                      where the player is and what they look at
  orch.py log [--last 30] [--type damage,think]
                                     what the game logged lately (orch/log): the
                                     player's trail and gaze, long frames, damage,
                                     triggers, AI think changes, move orders
  orch.py status                     is the game / the sidecar running
  orch.py stop                       close the live game (never any other)

Without an agent (a usage limit reached, or just to note things down):

  orch.py offline [--save X|--map Y] start the game if needed and the sidecar in
                                     queue mode: what you say is filed for later
  orch.py backlog [--all]            the items filed so far
  orch.py backlog show <id>          one item, in full
  orch.py backlog add "text"         file one by hand (or from a live session)
  orch.py backlog doing|done|drop|reopen <id> [--note "..."]
"""

import argparse
import glob
import hashlib
import json
import re
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
BACKLOG = os.path.join(ORCH, "backlog")
LOG_DIR = os.path.join(ORCH, "log")

BINARIES = ["openmohaa.exe", "cgame.dll", "game.dll", "renderer_opengl1.dll", "renderer_opengl2.dll"]
RUNTIME = ["SDL2.dll", "OpenAL64.dll", "libcurl.dll"]
HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.join(ORCH, "scripts")
# What the installed binaries were built from: their interfaces' hashes.
MANIFEST = os.path.join(LIVE, "orch-build.json")
# A module can be swapped alone while these are what the exe was built with.
INTERFACES = {
    "cgame": ["code/cgame/cg_public.h", "code/fgame/bg_public.h", "code/qcommon/q_shared.h"],
    "game": ["code/fgame/g_public.h", "code/fgame/bg_public.h", "code/qcommon/q_shared.h"],
    "renderer": ["code/renderercommon/tr_public.h", "code/renderercommon/tr_types.h", "code/qcommon/q_shared.h"],
}
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
            # The user's main install first; never the live one itself.
            found = [p for p in glob.glob(os.path.join(GAME_ROOT, "openmohaa-play", n))
                     + sorted(glob.glob(os.path.join(GAME_ROOT, "openmohaa-*", n)))
                     if os.path.dirname(os.path.abspath(p)) != os.path.abspath(LIVE)]
            src = found[0] if found else None
        if src:
            shutil.copyfile(src, os.path.join(LIVE, n))
    # DLLs swapped out while the game ran (hotswap) are free to go now.
    for old in glob.glob(os.path.join(LIVE, "*.old-*.dll")):
        os.remove(old)
    source = build_source(args.build)
    manifest = {"build": os.path.abspath(args.build), "source": source,
                "commit": git_head(source), "interfaces": interface_hashes(source),
                "time": time.strftime("%Y-%m-%dT%H:%M:%S")}
    with open(MANIFEST, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=1)
    print(f"installed {len(BINARIES)} binaries into {LIVE}")


def build_source(build):
    """The source tree a build folder (or its Release/ subfolder) was configured from."""
    for d in (build, os.path.dirname(os.path.abspath(build))):
        cache = os.path.join(d, "CMakeCache.txt")
        if os.path.isfile(cache):
            for line in open(cache, encoding="utf-8", errors="replace"):
                if line.startswith("CMAKE_HOME_DIRECTORY:"):
                    return line.split("=", 1)[1].strip()
    return None


def git_head(source):
    if not source:
        return None
    r = subprocess.run(["git", "-C", source, "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
    return r.stdout.strip() or None


def interface_hashes(source):
    if not source:
        return {}
    out = {}
    for module, files in INTERFACES.items():
        h = hashlib.sha1()
        for rel in files:
            path = os.path.join(source, rel)
            if os.path.isfile(path):
                h.update(open(path, "rb").read())
        out[module] = h.hexdigest()
    return out


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
    # Left by a game that was killed; it would stop the start at a dialog.
    pid_file = os.path.join(MAIN, "OpenMoHAA.pid")
    if os.path.isfile(pid_file):
        os.remove(pid_file)
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
        f.write(line + "\n")
    # Interop holds cmd.exe until the game exits, even through "start", so
    # leave it running on its own.
    subprocess.Popen(["cmd.exe", "/c", winpath(bat)], cwd="/mnt/c", start_new_session=True,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if args.focus and os.path.isfile(FOCUS):
        time.sleep(6)
        subprocess.run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", winpath(FOCUS)],
                       capture_output=True)
    print("launched" + (f" ({start[0]})" if start else ""))


def cmd_stop(args):
    # A clean quit first: a killed game leaves OpenMoHAA.pid behind, and the
    # next start then stops at an "Abnormal Exit" dialog.
    if any(is_live for _, _, is_live in live_processes()):
        run_commands("quit", timeout=2.0)
        deadline = time.time() + 10
        while time.time() < deadline and any(is_live for _, _, is_live in live_processes()):
            time.sleep(0.5)
    live = [pid for pid, _, is_live in live_processes() if is_live]
    for pid in live:
        subprocess.run(["powershell.exe", "-NoProfile", "-Command", f"Stop-Process -Id {pid} -Force"],
                       capture_output=True)
    print(f"stopped {len(live)} live process(es)")
    if getattr(args, "sidecar", False) and os.path.isfile(SIDECAR_ALIVE):
        # The sidecar writes its Windows process id there every second.
        pid = open(SIDECAR_ALIVE).read().strip()
        if pid.isdigit():
            subprocess.run(["powershell.exe", "-NoProfile", "-Command", f"Stop-Process -Id {pid} -Force"],
                           capture_output=True)
            os.remove(SIDECAR_ALIVE)
            print(f"stopped the sidecar ({pid})")


def sidecar_python():
    found = glob.glob("/mnt/c/Users/*/AppData/Local/openmohaa-orch/venv/Scripts/python.exe")
    return found[0] if found else None


def cmd_start(args):
    """A live session in one go: the install brought up to this worktree's
    build if it is older, the player's config and saves seeded, the sidecar
    started on its own with its output in a log (watch that: a monitor on the
    sidecar itself would end with the monitor), the game launched and waited
    for, and "Ready." said."""
    python = sidecar_python()
    if not python:
        sys.exit("no sidecar venv; run setup_windows.ps1 first (see README.md)")
    repo = subprocess.run(["git", "rev-parse", "--show-toplevel"], capture_output=True, text=True).stdout.strip()
    build = os.path.join(repo, "build", "win64")
    try:
        installed = json.load(open(MANIFEST, encoding="utf-8")).get("commit")
    except (OSError, ValueError):
        installed = None
    head = git_head(repo)
    if installed != head and not args.no_build:
        if any(live for _, _, live in live_processes()):
            sys.exit("the live install is older than this worktree and its game is running; close it, or --no-build")
        print(f"building {head} (the live install has {installed}) ...", flush=True)
        r = subprocess.run(["cmake", "--build", build, "-j4"], capture_output=True, text=True)
        if r.returncode:
            sys.exit((r.stdout + r.stderr)[-3000:])
        cmd_setup(argparse.Namespace(build=os.path.join(build, "Release")))
    cmd_seed(argparse.Namespace(home=None, windowed=args.windowed, no_saves=False))

    log = os.path.join(ORCH, "sidecar.log")
    alive = os.path.isfile(SIDECAR_ALIVE) and time.time() - os.path.getmtime(SIDECAR_ALIVE) < 5
    if not alive:
        sidecar = [python, "-u", winpath(os.path.join(HERE, "sidecar.py")), "--home", winpath(HOME)]
        if args.queue:
            sidecar.append("--queue")
        with open(log, "a", encoding="utf-8") as out:
            subprocess.Popen(sidecar, stdout=out, stderr=subprocess.STDOUT, start_new_session=True)

    # The game loads while the sidecar loads its voice models.
    if not any(live for _, _, live in live_processes()):
        args.extra, args.focus = None, False
        cmd_launch(args)
    deadline = time.time() + 120
    while not alive and time.time() < deadline and \
            "READY" not in open(log, encoding="utf-8", errors="replace").read()[-4000:]:
        time.sleep(1)
    if not wait_in_game(180):
        sys.exit("the game did not get into a level")
    open_items = [i for i in backlog_items() if i["status"] == "open"]
    cmd_say(argparse.Namespace(text=[f"Ready. {len(open_items)} open on the list." if open_items else "Ready."]))
    print(f"in game; watch the sidecar: tail -F \"{log}\" | grep --line-buffered -E '^(TURN|QUEUED|MODE|READY|ERROR)'")


def cmd_offline(args):
    """The game and the sidecar in queue mode, for when no agent can answer."""
    python = sidecar_python()
    if not python:
        sys.exit("no sidecar venv; run setup_windows.ps1 first (see README.md)")
    if not any(live for _, _, live in live_processes()):
        args.extra, args.focus = None, False
        cmd_launch(args)
    print("queue mode: hold the talk key and say what you want done; Ctrl+C to stop.\n"
          "Later, ask the agent to work the orchestrator backlog.", flush=True)
    sidecar = [python, winpath(os.path.join(HERE, "sidecar.py")), "--home", winpath(HOME), "--queue"]
    try:
        subprocess.run(sidecar + [a for a in args.sidecar_args if a != "--"])
    except KeyboardInterrupt:
        pass
    open_items = [i for i in backlog_items() if i["status"] == "open"]
    print(f"{len(open_items)} open item(s) in the backlog ({BACKLOG})")


# ---------------------------------------------------------------- the backlog

def backlog_path(n):
    return os.path.join(BACKLOG, f"{int(n):04d}.json")


def backlog_items():
    items = []
    for path in sorted(glob.glob(os.path.join(BACKLOG, "[0-9]*.json"))):
        try:
            with open(path, encoding="utf-8") as f:
                items.append(json.load(f))
        except (OSError, ValueError):
            pass
    return items


def write_item(item):
    os.makedirs(BACKLOG, exist_ok=True)
    path = backlog_path(item["id"])
    tmp = f"{path}.{os.getpid()}.tmp_w"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(item, f, indent=1)
    os.replace(tmp, path)


def reserve_id():
    """The next backlog number, claimed by creating its file exclusively so
    the sidecar (Windows, the same folder) can't take it too."""
    os.makedirs(BACKLOG, exist_ok=True)
    n = max((int(os.path.basename(p)[:-5]) for p in glob.glob(os.path.join(BACKLOG, "[0-9]*.json"))
             if os.path.basename(p)[:-5].isdigit()), default=0) + 1
    while True:
        try:
            os.close(os.open(backlog_path(n), os.O_CREAT | os.O_EXCL | os.O_WRONLY))
            return n
        except FileExistsError:
            n += 1


def summary(item):
    shots = sum(1 for e in item.get("events", []) if e.get("type") == "shot")
    where = (item.get("state") or {}).get("map") or ""
    text = item.get("speech") or "(shot only)"
    if len(text) > 100:
        text = text[:97] + "..."
    extra = "  ".join(x for x in (where, f"{shots} shot(s)" if shots else "") if x)
    return f"{item['id']:>4}  {item['status']:<7} {item.get('created', '')[:16]}  {text}" + (f"  [{extra}]" if extra else "")


SHOT_ID = r"\d{8}-\d{6}-\d{3}"


def backlog_prune(args):
    """The orchestrator's own leftovers (shots, their savegames and records,
    session folders) that no open backlog item points at and that are older
    than --keep-days. Files of the player's own are never matched: only the
    orch_<shot id> names and orch/ folders."""
    import re

    keep_ids, keep_sessions = set(), set()
    for item in backlog_items():
        if item.get("status") in ("done", "dropped"):
            continue
        text = json.dumps(item)
        keep_ids |= set(re.findall(SHOT_ID, text))
        # The session folder: sessions/<name>/turns/<n>.json for a turn.
        for key in ("turn", "session"):
            m = re.search(r"sessions[/\\]([^/\\]+)", item.get(key) or "")
            if m:
                keep_sessions.add(m.group(1))
        turn = item.get("turn")
        if turn and os.path.isfile(turn):
            keep_ids |= set(re.findall(SHOT_ID, open(turn, encoding="utf-8", errors="replace").read()))
    cutoff = time.time() - args.keep_days * 86400

    victims = []
    pat = re.compile(r"orch_(" + SHOT_ID + r")")
    places = [os.path.join(MAIN, "screenshots"), os.path.join(ORCH, "events")] + \
        glob.glob(os.path.join(MAIN, "save", "*"))
    for folder in places:
        for path in glob.glob(os.path.join(folder, "*")):
            name = os.path.basename(path)
            m = pat.search(name) or (re.match(SHOT_ID, name) if folder.endswith("events") else None)
            if not m:
                continue
            shot = m.group(1) if m.re is pat else m.group(0)
            if shot not in keep_ids and os.path.getmtime(path) < cutoff:
                victims.append(path)
    # An item's own save (orch_item<n>), once the item is closed.
    closed = {str(i["id"]) for i in backlog_items() if i.get("status") in ("done", "dropped")}
    for path in glob.glob(os.path.join(MAIN, "save", "*", "orch_item*")):
        m = re.match(r"orch_item(\d+)\.", os.path.basename(path))
        if m and m.group(1) in closed:
            victims.append(path)
    for path in glob.glob(os.path.join(ORCH, "sessions", "*")):
        if os.path.basename(path) not in keep_sessions and os.path.getmtime(path) < cutoff:
            victims.append(path)

    size = sum(os.path.getsize(p) if os.path.isfile(p) else
               sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(p) for f in fs) for p in victims)
    print(f"{len(victims)} files and folders, {size / 1e6:.0f} MB, not referenced by an open item "
          f"and older than {args.keep_days} days (kept: {len(keep_ids)} shots, {len(keep_sessions)} sessions)")
    if not args.apply:
        for p in victims[:10]:
            print("  " + os.path.relpath(p, MAIN))
        print("(dry run: --apply removes them)")
        return
    for p in victims:
        shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
    print("removed")


def backlog_repro(item, args):
    """Puts an item back on screen in a test install: its shot's savegame
    (or its own), the player where the shot was taken, facing the same way."""
    shot = next((e for e in item.get("events", []) if e.get("type") == "shot" and e.get("savegame")), None)
    save = shot["savegame"] if shot else item.get("savegame")
    if not save:
        sys.exit(f"item {item['id']} has no savegame (filed without the game running, or in multiplayer)")
    where = item.get("state") or {}
    if shot and shot.get("json") and os.path.isfile(shot["json"]):
        with open(shot["json"], encoding="utf-8") as f:
            where = json.load(f)
    origin, angles = where.get("player_origin"), where.get("view_angles")

    target = args.into or os.path.join(GAME_ROOT, "orch-test")
    copied = 0
    for src in glob.glob(os.path.join(MAIN, "save", "*", save + ".*")):
        dest = os.path.join(target, "home", "main", "save", os.path.basename(os.path.dirname(src)))
        os.makedirs(dest, exist_ok=True)
        shutil.copy2(src, dest)
        copied += 1
    if not copied:
        sys.exit(f"savegame {save} is not in {MAIN}/save (pruned?)")

    me = [sys.executable, os.path.abspath(__file__)]
    env = dict(os.environ, ORCH_LIVE=target)
    for step in (["stop"], ["launch", "--save", save], ["wait", "--timeout", "180"], ["cmd", "god 1"]):
        subprocess.run(me + step, env=env, check=step[0] == "wait")
    if origin:
        view = ["view", *[str(v) for v in origin]]
        if angles:
            view += ["--pitch", str(angles[0]), "--yaw", str(angles[1])]
        subprocess.run(me + view, env=env, stdout=subprocess.DEVNULL)
    print(f"item {item['id']}: {save} in {target}" + (f", at {origin}" if origin else "")
          + f"; drive it with ORCH_LIVE=\"{target}\" orch.py ...")


def cmd_backlog(args):
    if args.action == "prune":
        return backlog_prune(args)
    if args.action in (None, "list"):
        items = [i for i in backlog_items() if args.all or i["status"] in ("open", "doing")]
        for item in items:
            print(summary(item))
        if not items:
            print("the backlog is empty" if args.all else "nothing open in the backlog")
        return
    if args.action == "add":
        text = " ".join(args.rest)
        if not text:
            sys.exit("backlog add: what to file?")
        n = reserve_id()
        item = {"id": n, "status": "open", "created": time.strftime("%Y-%m-%dT%H:%M:%S"),
                "source": "agent", "speech": text, "state": None, "events": []}
        if args.turn:
            item["turn"] = args.turn
            try:
                with open(args.turn, encoding="utf-8") as f:
                    import noise
                    item["events"] = noise.collapse(json.load(f).get("events", []))
            except (OSError, ValueError):
                pass
        if args.state:
            out = run_commands("orch_state", timeout=2.0)
            for line in (out or "").splitlines():
                if line.startswith("{"):
                    item["state"] = json.loads(line)
            st = item["state"] or {}
            if st.get("in_game") and st.get("single_player") and \
                    not any(e.get("savegame") for e in item["events"] if e.get("type") == "shot"):
                item["savegame"] = f"orch_item{n}"
                run_commands(f"savegame orch_item{n}", timeout=5.0)
        write_item(item)
        print(f"filed {n}")
        return

    if len(args.rest) != 1 or not args.rest[0].isdigit():
        sys.exit(f"backlog {args.action} <id>")
    path = backlog_path(args.rest[0])
    if not os.path.isfile(path):
        sys.exit(f"no item {args.rest[0]}")
    with open(path, encoding="utf-8") as f:
        item = json.load(f)
    if args.action == "repro":
        return backlog_repro(item, args)
    if args.action == "show":
        # Items filed before the sidecar filtered the console: the same here.
        import noise
        shown = dict(item, events=noise.collapse(item.get("events", [])))
        if not args.full:
            shown.pop("context", None)  # orch.py log reads the game log around it
        print(json.dumps(shown, indent=1))
        return
    item["status"] = {"doing": "doing", "done": "done", "drop": "dropped", "reopen": "open"}[args.action]
    item.setdefault("history", []).append(
        {"time": time.strftime("%Y-%m-%dT%H:%M:%S"), "status": item["status"], "note": args.note})
    if args.note:
        item["note"] = args.note
    write_item(item)
    print(summary(item))


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


def read_logs(since):
    """The game's log entries (both sides) from the Unix time since on, oldest first."""
    entries = []
    game = os.path.join(LOG_DIR, "game.jsonl")
    if os.path.isfile(game):
        with open(game, "rb") as f:
            # The end is enough: the file only grows.
            f.seek(max(0, os.path.getsize(game) - 4 * 1024 * 1024))
            for line in f.read().split(b"\n"):
                try:
                    e = json.loads(line.decode("utf-8", "replace"))
                except ValueError:
                    continue
                e["source"] = "game"
                entries.append(e)
    seen = set()
    for path in glob.glob(os.path.join(LOG_DIR, "client.*.jsonl")):
        try:
            with open(path, encoding="utf-8", errors="replace") as f:
                lines = f.read().splitlines()
            head = json.loads(lines[0])
        except (OSError, ValueError, IndexError):
            continue
        key = (head.get("writer"), head.get("batch"))
        if key in seen:
            continue
        seen.add(key)
        for line in lines[1:]:
            try:
                e = json.loads(line)
            except ValueError:
                continue
            e["source"] = "client"
            entries.append(e)
    entries = [e for e in entries if isinstance(e.get("t"), (int, float)) and e["t"] >= since]
    return sorted(entries, key=lambda e: e["t"])


def cmd_log(args):
    t_now = time.time()
    types = set(args.type.split(",")) if args.type else None
    for e in read_logs(t_now - args.last):
        if types and e.get("type") not in types:
            continue
        ago = round(e.pop("t") - t_now, 1)
        print(json.dumps(dict({"ago": ago}, **e)))


def console_tail(since_size):
    log = os.path.join(MAIN, "qconsole.log")
    if not os.path.isfile(log):
        return ""
    with open(log, "rb") as f:
        f.seek(since_size if os.path.getsize(log) >= since_size else 0)
        return f.read().decode("utf-8", "replace")


def wait_in_game(timeout):
    """Until the game answers orch_state from inside a level."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        out = run_commands("orch_state", timeout=2.0)
        if out and '"in_game": true' in out:
            return True
        # A full screen menu (the main menu comes up when the window loses the
        # focus) pauses the world, and a new cgame gets no snapshot under it.
        if out and '"paused" is:"2"' in (run_commands("paused", timeout=2.0) or ""):
            run_commands("popmenu 0", timeout=2.0)
        time.sleep(1.0)
    return False


def cmd_swap(args):
    module = args.module
    build = os.path.abspath(args.build)
    out_dir = next((os.path.join(build, d) for d in ("Release", "RelWithDebInfo", "Debug")
                    if os.path.isdir(os.path.join(build, d))), build)
    running = any(live for _, _, live in live_processes())

    renderer = None
    if module == "renderer":
        renderer = "opengl1"
        if running:
            out = run_commands("cl_renderer", timeout=3.0) or ""
            m = __import__("re").search(r'is:"(\w+)"', out)
            renderer = m.group(1) if m else renderer
        if args.renderer:
            renderer = args.renderer
    target = {"cgame": "cgame", "game": "game", "renderer": f"renderer_{renderer}"}[module]
    dll = target + ".dll"

    # Only while the interfaces are the ones the exe was built with.
    if not args.force:
        try:
            manifest = json.load(open(MANIFEST, encoding="utf-8"))
        except (OSError, ValueError):
            manifest = None
        now_hash = interface_hashes(build_source(build)).get(module)
        then_hash = (manifest or {}).get("interfaces", {}).get(module)
        if not manifest:
            print("(no orch-build.json from setup: can't check the interfaces)", file=sys.stderr)
        elif now_hash != then_hash:
            sys.exit(f"{module}'s interface with the exe changed since setup ({', '.join(INTERFACES[module])}); "
                     "rebuild everything and relaunch: orch.py stop; orch.py setup --build ...; "
                     "orch.py launch --save <save>  (or --force)")

    if not args.no_build:
        r = subprocess.run(["cmake", "--build", build, "--target", target, "--parallel", "4"],
                           capture_output=True, text=True)
        if r.returncode:
            sys.stdout.write("\n".join(l for l in (r.stdout + r.stderr).splitlines()
                                       if "error" in l.lower())[-4000:] + "\n")
            sys.exit(f"build of {target} failed")

    new = os.path.join(out_dir, dll)
    if not os.path.isfile(new):
        sys.exit(f"{new} not built")
    live_dll = os.path.join(LIVE, dll)

    if not running:
        shutil.copyfile(new, live_dll)
        print(f"copied {dll} (the game isn't running)")
        return

    # Windows lets a loaded DLL be renamed, not overwritten.
    n = 1
    while os.path.exists(os.path.join(LIVE, f"{target}.old-{n}.dll")):
        n += 1
    old = os.path.join(LIVE, f"{target}.old-{n}.dll")
    os.rename(live_dll, old)
    shutil.copyfile(new, live_dll)

    log = os.path.join(MAIN, "qconsole.log")
    log_size = os.path.getsize(log) if os.path.isfile(log) else 0
    if module == "game":
        # Same-map loadgame would keep the old DLL loaded.
        started = time.time()
        run_commands("savegame orch_swap", timeout=15.0)
        time.sleep(1.0)
        saves = glob.glob(os.path.join(MAIN, "save", "*", "orch_swap.sav"))
        if not any(os.path.getmtime(p) >= started - 1 for p in saves):
            # Put the loaded one back under its name; nothing was reloaded.
            os.remove(live_dll)
            os.rename(old, live_dll)
            sys.exit("the game couldn't save here (dead, a cinematic?); nothing swapped")
        replies = [run_commands("killserver", timeout=15.0), run_commands("loadgame orch_swap", timeout=90.0)]
    else:
        replies = [run_commands("vid_restart", timeout=90.0)]

    ok = wait_in_game(90)
    # What the commands print goes to their replies, not to qconsole.log.
    tail = "\n".join(r or "" for r in replies) + "\n" + console_tail(log_size)
    if args.verbose:
        print(tail)
    reloaded = {"game": "InitGame", "cgame": "CL_InitCGame", "renderer": "Initializing Renderer"}[module]
    if reloaded.lower() not in tail.lower():
        print(f"(didn't see '{reloaded}' in the output; check the game)", file=sys.stderr)
    bad = [l for l in tail.splitlines() if __import__("re").search(
        r"(failed to load game DLL|Sys_LoadGameDll\(.*\) failed|Sys_GetCGameAPI\(.*\) failed|Couldn't load game"
        r"|game is version \d+, not|Failed to load renderer|Mismatched REF_API_VERSION)", l)]
    if not ok or bad:
        print("\n".join(bad[-5:]))
        sys.exit(f"the swapped {dll} didn't come up; the old one is {os.path.basename(old)}. "
                 "Relaunch: orch.py stop; orch.py launch" + (" --save orch_swap" if module == "game" else ""))
    print(f"swapped {dll} ({os.path.basename(old)} kept until the next setup)")


def cmd_script(args):
    if args.file:
        code = open(args.file, encoding="utf-8").read()
    else:
        code = " ".join(args.code)
    if not code.strip():
        sys.exit("no script")
    os.makedirs(SCRIPTS, exist_ok=True)
    name = f"s{time.strftime('%Y%m%d_%H%M%S')}_{time.time_ns() % 1000000}.scr"
    with open(os.path.join(SCRIPTS, name), "w", newline="\n", encoding="utf-8") as f:
        f.write(code.rstrip() + "\n")
    out = run_commands(f"orch_runscript orch/scripts/{name}" + (f" {args.label}" if args.label else ""), args.timeout)
    if out is None:
        sys.exit("the game didn't take the command (not running, or still loading?)")
    sys.stdout.write(out)


def small_shot(path, width=1280):
    """A copy of a screenshot at most width wide, beside it (<name>_small.jpg):
    what an agent reads, at under half the tokens of the full size."""
    from PIL import Image

    out = path[:-4] + "_small.jpg"
    if not os.path.isfile(out) and os.path.isfile(path):
        im = Image.open(path)
        im.thumbnail((width, width))
        im.convert("RGB").save(out, quality=82)
    return out


SERVER_FIELDS = ("Classname", "Targetname", "Modelname", "Animname", "Think", "Health")


def cmd_look(args):
    """A turn or a backlog item in brief: what was said, each shot's target
    from the game's side, the console without its spam, and a small copy of
    each marked screenshot to look at."""
    import noise

    src = args.what
    if src.isdigit():
        with open(backlog_path(src), encoding="utf-8") as f:
            data = json.load(f)
        print(f"item {data['id']} [{data['status']}] {data.get('speech') or '(shot only)'}")
        st = data.get("state") or {}
        if st:
            print(f"  map {st.get('map')}  player {st.get('player_origin')}  looking at {st.get('looking_at')}")
        if data.get("savegame"):
            print(f"  savegame {data['savegame']}")
        if data.get("note"):
            print(f"  note: {data['note'][:300]}")
    else:
        with open(src, encoding="utf-8") as f:
            data = json.load(f)
        print(f"turn {data.get('turn')}: {data.get('speech') or '(no speech)'}")

    for e in noise.collapse(data.get("events", [])):
        if e["type"] == "speech":
            print(f"  said: {e['text']}")
        elif e["type"] == "shot":
            print(f"  shot {e.get('id')}: {e.get('summary')}  (savegame {e.get('savegame')})")
            server = (e.get("files") or {}).get("server")
            if server and os.path.isfile(server):
                fields = {}
                section = None
                for line in open(server, encoding="utf-8", errors="replace"):
                    if line.startswith("== "):
                        section = line.strip("= \n")
                    k, _, v = line.partition(":")
                    if section in ("target", "actor") and k.strip() in SERVER_FIELDS and k.strip() not in fields:
                        fields[k.strip()] = v.strip()
                if fields:
                    print("    " + "  ".join(f"{k} {v}" for k, v in fields.items()))
            marked = (e.get("files") or {}).get("screenshot_marked")
            if marked:
                print(f"    look: {small_shot(marked, args.width)}")
        elif e["type"] == "console":
            n = f" x{e['count']}" if e.get("count", 1) > 1 else ""
            print(f"  console{n}: {e['line'][:200]}")


def cmd_wait(args):
    if not wait_in_game(args.timeout):
        sys.exit("not in game after %d s" % args.timeout)
    print("in game")


def cmd_view(args):
    """Puts the player at x y z looking at pitch/yaw: tele takes a position
    only, so the angles go through the player's viewangles setter."""
    x, y, z = args.pos
    out = run_commands(f"tele {x} {y} {z}", 5.0)
    if out is None:
        sys.exit("the game didn't take the command (not running, or still loading?)")
    if args.pitch is not None or args.yaw is not None:
        state = json.loads(run_commands("orch_state", 5.0) or "{}")
        pitch = args.pitch if args.pitch is not None else state.get("view_angles", [0, 0])[0]
        yaw = args.yaw if args.yaw is not None else state.get("view_angles", [0, 0])[1]
        args.code, args.file, args.label, args.timeout = [f"$player.viewangles = ( {pitch} {yaw} 0 )"], None, None, 5.0
        cmd_script(args)
    time.sleep(0.3)
    cmd_state(argparse.Namespace(timeout=5.0))


def cmd_frames(args):
    """N screenshots args.every seconds apart, after an optional script, on one
    contact sheet: one image to look at instead of N."""
    from PIL import Image

    shots = os.path.join(MAIN, "screenshots")
    tag = args.name or time.strftime("f%H%M%S")
    if args.script:
        args.code, args.file, args.label, args.timeout = [args.script], None, None, 5.0
        cmd_script(args)
    names = []
    for i in range(args.count):
        if i:
            time.sleep(args.every)
        name = f"{tag}_{i}"
        if run_commands(f"screenshotJPEG {name}", 5.0) is None:
            sys.exit("the game didn't take the command (not running, or still loading?)")
        names.append(os.path.join(shots, name + ".jpg"))

    tiles = []
    for path in names:
        for _ in range(50):
            if os.path.isfile(path) and os.path.getsize(path) > 0:
                break
            time.sleep(0.1)
        im = Image.open(path)
        if args.crop:
            x0, y0, x1, y1 = args.crop
            w, h = im.size
            im = im.crop((int(x0 * w), int(y0 * h), int(x1 * w), int(y1 * h)))
        im.thumbnail((args.width, args.width))
        tiles.append(im.convert("RGB"))

    cols = min(args.cols, len(tiles))
    rows = (len(tiles) + cols - 1) // cols
    tw, th = tiles[0].size
    sheet = Image.new("RGB", (cols * tw, rows * th))
    for i, im in enumerate(tiles):
        sheet.paste(im, ((i % cols) * tw, (i // cols) * th))
    out = os.path.join(shots, tag + "_sheet.jpg")
    sheet.save(out, quality=85)
    print(out)


# What the game writes into its home path itself; anything else there is a
# loose override from a test (a .tik, a .scr, a shader) that shadows the paks.
OWN = {"orch", "save", "screenshots", "configs", "demos", "physics.txt", "qconsole.log"}


def cmd_reset(args):
    found = loose_files()
    for path in found:
        print(os.path.relpath(path, MAIN))
    if not found:
        print("no loose overrides in", MAIN)
    elif args.apply:
        for entry in {os.path.relpath(p, MAIN).split(os.sep)[0] for p in found}:
            path = os.path.join(MAIN, entry)
            shutil.rmtree(path) if os.path.isdir(path) else os.remove(path)
        print("removed %d files" % len(found))
    else:
        print("(%d files; --apply removes them)" % len(found))


def loose_files():
    found = []
    for entry in sorted(os.listdir(MAIN)):
        if entry in OWN or entry.endswith((".cfg", ".log", ".pid")):
            continue
        path = os.path.join(MAIN, entry)
        if os.path.isdir(path):
            for root, _, files in os.walk(path):
                found += [os.path.join(root, f) for f in files]
        else:
            found.append(path)
    return found


def cmd_promote(args):
    """Loose overrides in the install's home made into repo files: a change to
    one of our paks goes back into its data/ file or content/ patch; a change
    to a retail or mod file becomes a diff in content/<pak>/ (they are not
    ours to publish); a new file goes into data/<pak>/."""
    sys.path.insert(0, os.path.dirname(HERE))
    import paks

    repo = args.repo or subprocess.run(["git", "rev-parse", "--show-toplevel"],
                                       capture_output=True, text=True).stdout.strip()
    idx = paks.index()
    plan = []
    for path in loose_files():
        rel = os.path.relpath(path, MAIN).replace(os.sep, "/")
        holders = paks.holders(idx, rel)
        winner = holders[-1] if holders else None
        ours = re.match(r"zzzzzzzzz-(opm-[\w-]+)\.pk3$", winner or "")
        if ours and os.path.isfile(os.path.join(repo, "data", ours.group(1), rel)):
            plan.append((path, "data", os.path.join(repo, "data", ours.group(1), rel)))
        elif ours and os.path.isfile(os.path.join(repo, "content", ours.group(1), rel + ".patch")):
            plan.append((path, "patch", ours.group(1)))
        elif winner and not ours:
            if not args.pak:
                plan.append((path, "needs --pak", "a content/<pak> for the diff"))
            else:
                plan.append((path, "patch", args.pak))
        else:
            if not args.pak:
                plan.append((path, "needs --pak", "a data/<pak> for the new file"))
            else:
                plan.append((path, "data", os.path.join(repo, "data", args.pak, rel)))

    for path, how, dest in plan:
        rel = os.path.relpath(path, MAIN)
        print(f"{rel}  ->  {how}: {os.path.relpath(dest, repo) if how == 'data' else dest}")
    if not plan:
        print("no loose overrides in", MAIN)
        return
    if not args.apply:
        print("(dry run: --apply writes them into the repo and removes the loose copies)")
        return
    done = 0
    for path, how, dest in plan:
        rel = os.path.relpath(path, MAIN).replace(os.sep, "/")
        if how == "data":
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.copy2(path, dest)
        elif how == "patch":
            r = subprocess.run([sys.executable, "tools/content/pack.py", "--diff", dest, rel, path], cwd=repo)
            if r.returncode:
                print(f"  could not diff {rel}; left in place")
                continue
        else:
            continue
        os.remove(path)
        done += 1
    print(f"promoted {done} of {len(plan)}; commit them in {repo}")


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

    p = sub.add_parser("start", help="a live session in one go: build if stale, seed, sidecar, game, Ready")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--save")
    g.add_argument("--map")
    p.add_argument("--queue", action="store_true", help="the sidecar in queue mode")
    p.add_argument("--no-build", action="store_true", help="even if the install is older than this worktree")
    p.add_argument("--windowed", action="store_true", help="seed for a 1280x720 window (unattended tests)")
    p.set_defaults(fn=cmd_start)

    p = sub.add_parser("offline", help="game + sidecar in queue mode, no agent needed")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--save")
    g.add_argument("--map")
    p.add_argument("sidecar_args", nargs=argparse.REMAINDER,
                   help="passed on to sidecar.py (after --), e.g. -- --ptt-key mouse5")
    p.set_defaults(fn=cmd_offline)

    p = sub.add_parser("backlog", help="what was filed in queue mode")
    p.add_argument("action", nargs="?",
                   choices=["list", "show", "add", "doing", "done", "drop", "reopen", "prune", "repro"])
    p.add_argument("rest", nargs="*", help="the item id, or for add the text")
    p.add_argument("--all", action="store_true", help="list done and dropped items too")
    p.add_argument("--note", help="what was done, or why not (doing/done/drop/reopen)")
    p.add_argument("--turn", help="add: the turn JSON it came from (its shots come along)")
    p.add_argument("--state", action="store_true", help="add: record where the player is now")
    p.add_argument("--keep-days", type=float, default=2, help="prune: keep what is newer than this")
    p.add_argument("--full", action="store_true", help="show: also the game log around it (context)")
    p.add_argument("--into", help="repro: the install to load it in (default the orch-test install)")
    p.add_argument("--apply", action="store_true", help="prune: remove (otherwise a dry run)")
    p.set_defaults(fn=cmd_backlog)

    p = sub.add_parser("stop")
    p.add_argument("--sidecar", action="store_true", help="the sidecar too")
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

    p = sub.add_parser("swap", help="build one module and swap it into the running game")
    p.add_argument("module", choices=["cgame", "game", "renderer"])
    p.add_argument("--build", default="build/win64", help="the cmake build folder (default build/win64)")
    p.add_argument("--renderer", help="opengl1 or opengl2 (default: the game's cl_renderer)")
    p.add_argument("--no-build", action="store_true", help="swap what is already built")
    p.add_argument("--force", action="store_true", help="even if the module's interface changed since setup")
    p.add_argument("--verbose", action="store_true", help="print what the reload printed")
    p.set_defaults(fn=cmd_swap)

    p = sub.add_parser("script", help="run script code in the game now")
    p.add_argument("code", nargs="*", help="script source, e.g. '$guy3 runto $node_x'")
    p.add_argument("--file", help="a .scr file instead")
    p.add_argument("--label", help="run from this label")
    p.add_argument("--timeout", type=float, default=5.0)
    p.set_defaults(fn=cmd_script)

    p = sub.add_parser("look", help="a turn (its JSON) or a backlog item (its id) in brief")
    p.add_argument("what")
    p.add_argument("--width", type=int, default=1280, help="the small screenshots' width")
    p.set_defaults(fn=cmd_look)

    p = sub.add_parser("wait", help="until the game is in a level")
    p.add_argument("--timeout", type=float, default=120.0)
    p.set_defaults(fn=cmd_wait)

    p = sub.add_parser("view", help="put the player at x y z, facing pitch/yaw")
    p.add_argument("pos", nargs=3, type=float)
    p.add_argument("--pitch", type=float)
    p.add_argument("--yaw", type=float)
    p.set_defaults(fn=cmd_view)

    p = sub.add_parser("frames", help="timed screenshots on one contact sheet")
    p.add_argument("count", type=int, nargs="?", default=4)
    p.add_argument("--every", type=float, default=0.5, help="seconds between frames")
    p.add_argument("--script", help="script code to run first (an explosion, a spawn)")
    p.add_argument("--crop", type=float, nargs=4, metavar=("X0", "Y0", "X1", "Y1"),
                   help="part of each frame, as fractions (0 0 1 1 is all)")
    p.add_argument("--width", type=int, default=640, help="each frame's size on the sheet")
    p.add_argument("--cols", type=int, default=2)
    p.add_argument("--name", help="file name prefix")
    p.set_defaults(fn=cmd_frames)

    p = sub.add_parser("promote", help="loose overrides in the install's home made into repo files")
    p.add_argument("--pak", help="data/<pak> or content/<pak> for files no pak of ours has")
    p.add_argument("--repo", help="the worktree to write into (default: this one)")
    p.add_argument("--apply", action="store_true")
    p.set_defaults(fn=cmd_promote)

    p = sub.add_parser("reset", help="list (--apply: remove) loose overrides in the install's home")
    p.add_argument("--apply", action="store_true")
    p.set_defaults(fn=cmd_reset)

    p = sub.add_parser("log", help="what the game logged lately")
    p.add_argument("--last", type=float, default=30.0, help="seconds back (default 30)")
    p.add_argument("--type", help="only these, comma separated: trail, gaze, hitch, damage, trigger, think, order, ...")
    p.set_defaults(fn=cmd_log)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
