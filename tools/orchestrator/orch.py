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
            found = [p for p in glob.glob(os.path.join(GAME_ROOT, "openmohaa-ragdoll", n))
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


def sidecar_python():
    found = glob.glob("/mnt/c/Users/*/AppData/Local/openmohaa-orch/venv/Scripts/python.exe")
    return found[0] if found else None


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


def cmd_backlog(args):
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
                    item["events"] = json.load(f).get("events", [])
            except (OSError, ValueError):
                pass
        if args.state:
            out = run_commands("orch_state", timeout=2.0)
            for line in (out or "").splitlines():
                if line.startswith("{"):
                    item["state"] = json.loads(line)
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
    if args.action == "show":
        print(json.dumps(item, indent=1))
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

    p = sub.add_parser("offline", help="game + sidecar in queue mode, no agent needed")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--save")
    g.add_argument("--map")
    p.add_argument("sidecar_args", nargs=argparse.REMAINDER,
                   help="passed on to sidecar.py (after --), e.g. -- --ptt-key mouse5")
    p.set_defaults(fn=cmd_offline)

    p = sub.add_parser("backlog", help="what was filed in queue mode")
    p.add_argument("action", nargs="?",
                   choices=["list", "show", "add", "doing", "done", "drop", "reopen"])
    p.add_argument("rest", nargs="*", help="the item id, or for add the text")
    p.add_argument("--all", action="store_true", help="list done and dropped items too")
    p.add_argument("--note", help="what was done, or why not (doing/done/drop/reopen)")
    p.add_argument("--turn", help="add: the turn JSON it came from (its shots come along)")
    p.add_argument("--state", action="store_true", help="add: record where the player is now")
    p.set_defaults(fn=cmd_backlog)

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

    p = sub.add_parser("log", help="what the game logged lately")
    p.add_argument("--last", type=float, default=30.0, help="seconds back (default 30)")
    p.add_argument("--type", help="only these, comma separated: trail, gaze, hitch, damage, trigger, think, order, ...")
    p.set_defaults(fn=cmd_log)

    args = ap.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()
