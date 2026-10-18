#!/usr/bin/env python3
"""Build m0_techdemo: write the .map, compile it, and pack it.

    tools/techdemo/build.py                  # map, bsp, vis, light, pk3
    tools/techdemo/build.py --fast           # rough vis and light, for trying things
    tools/techdemo/build.py --install "/mnt/d/Medal of Honor/orch-test/home/main"

The compilers are MOH:AA's own, from the MOHTools release (Q3map.exe for the
bsp and vis, MOHlight.exe for the light), run through WSL's interop. They read
the game's shaders and textures from the paks in a game folder: every pak in
the game's main, so the map is compiled against the mods that are installed
(their shaders' surface flags and light, their textures' colours). The game
skips what it cannot parse in a shader file, the compilers stop: HRRTM's
scripts/new_opelgray.shader starts with a stray "}". So they get a folder of
their own, hard links to the game's paks (no copy) and one more pak, sorting
last, with a repaired copy of each shader file they would stop at:

    <game>/techdemo-build/main/*.pk3
    <game>/techdemo-build/main/zzzzzzzzzzzzzzzzzzzz-techdemo-shaderfix.pk3

The pk3 (build/techdemo/zzzzzzzzz-opm-techdemo.pk3) holds the bsp, what is in
pak/ (the map's script, its precache, its water) and the benchmark lap's
length (maps/m0_techdemo_bench.scr). Nothing in it is retail or from a mod.

MOHAA_GAME (default /mnt/d/Medal of Honor) is the game folder, MOHTOOLS
(default /mnt/d/MOHTools-master) the tools.
"""

import argparse
import io
import os
import re
import shutil
import subprocess
import sys
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import techdemo  # noqa: E402

GAME = os.environ.get("MOHAA_GAME", "/mnt/d/Medal of Honor")
TOOLS = os.environ.get("MOHTOOLS", "/mnt/d/MOHTools-master")
PAK = "zzzzzzzzz-opm-techdemo.pk3"
SHADERFIX = "zzzzzzzzzzzzzzzzzzzz-techdemo-shaderfix.pk3"
RETAIL = {"Pak0.pk3", "Pak1.pk3", "Pak2.pk3", "Pak3.pk3", "Pak4.pk3", "Pak5.pk3", "Pak6EnUk.pk3"}
IMAGE_EXTS = (".tga", ".jpg", ".dds")


def winpath(p):
    return subprocess.check_output(["wslpath", "-w", p], text=True).strip()


def shader_tokens(text):
    """(start, end, token) of each token, comments skipped, as the compilers read them."""
    for m in re.finditer(r'//[^\n]*|/\*.*?\*/|"[^"]*"|[{}]|[^\s{}]+', text, re.S):
        if not m.group().startswith(("//", "/*")):
            yield m.start(), m.end(), m.group()


def repair_shader(text):
    """The text with the braces the compilers would stop at taken out (a brace
    between shaders), or None when there are none."""
    cut, depth, prev = [], 0, None
    for a, b, t in shader_tokens(text):
        if t == "{":
            if depth == 0 and (prev is None or prev in "{}"):
                cut.append((a, b))
                continue
            depth += 1
        elif t == "}":
            if depth == 0:
                cut.append((a, b))
                continue
            depth -= 1
        prev = t
    for a, b in reversed(cut):
        text = text[:a] + text[b:]
    return text if cut else None


def shader_images(text):
    """{shader name: [the images it names]} for a shader file."""
    out, name, depth, key = {}, None, 0, None
    for _, _, t in shader_tokens(text):
        if t == "{":
            depth += 1
        elif t == "}":
            depth = max(depth - 1, 0)
        elif depth == 0:
            name = t.lower()
            out[name] = []
        elif key:
            if name and not t.startswith("$"):
                out[name].append(t.lower())
            key = None
        else:
            key = t.lower() in ("qer_editorimage", "map", "clampmap")
    return out


def gamedir(used):
    """The compilers' own game folder: the game's paks, and one more pak that
    sorts last with what the compilers need instead of the mods' files.

    Shader files the compilers stop at are repaired. And the retail image goes
    in for each of the map's textures a mod replaces: the compilers turn a
    face's texture scale into coordinates by the image's size, so a texture
    pack's images four times the size would make the textures four times as
    big. Compiled against the retail sizes, as every retail map was, the
    game draws the mod's images at the retail scale, only sharper."""
    d = os.path.join(GAME, "techdemo-build")
    main = os.path.join(d, "main")
    os.makedirs(os.path.join(main, "maps"), exist_ok=True)
    paks = sorted((p for p in os.listdir(os.path.join(GAME, "main")) if p.lower().endswith(".pk3") and p != PAK), key=str.lower)
    for p in os.listdir(main):
        if p.lower().endswith(".pk3") and p not in paks and p != SHADERFIX:
            os.remove(os.path.join(main, p))
    shaders, images, images_of = {}, {}, {}
    for p in paks:
        src, dst = os.path.join(GAME, "main", p), os.path.join(main, p)
        if not os.path.exists(dst) or os.stat(dst).st_ino != os.stat(src).st_ino:
            if os.path.exists(dst):
                os.remove(dst)
            os.link(src, dst)
        with zipfile.ZipFile(src) as z:
            for n in z.namelist():
                base, ext = os.path.splitext(n.lower())
                if base.startswith("scripts/") and ext == ".shader":
                    text = z.read(n).decode("latin-1")
                    # a later pak's file of the same name is the one read
                    shaders[n.lower()] = (p, n, repair_shader(text))
                    images_of.update(shader_images(text))
                elif base.startswith("textures/") and ext in IMAGE_EXTS:
                    images.setdefault(base, []).append((p, n, ext))
    want = set()
    for t in used:
        t = "textures/" + t.lower()
        want.add(t)
        want.update(os.path.splitext(i)[0] for i in images_of.get(t, []))
    with zipfile.ZipFile(os.path.join(main, SHADERFIX), "w", zipfile.ZIP_DEFLATED) as z:
        for p, n, text in shaders.values():
            if text is not None:
                print("  repaired for the compilers: %s (%s)" % (n, p))
                z.writestr(n, text.encode("latin-1"))
        swapped = 0
        for base in sorted(want):
            have = images.get(base, [])
            retail = [(p, n, e) for p, n, e in have if p in RETAIL]
            mods = {e: n for p, n, e in have if p not in RETAIL}
            if not retail or not mods:
                continue
            by_ext = {e: (p, n) for p, n, e in retail}
            for ext, n in mods.items():
                if ext in by_ext:
                    p, rn = by_ext[ext]
                    with zipfile.ZipFile(os.path.join(GAME, "main", p)) as r:
                        z.writestr(n, r.read(rn))
                elif ext != ".dds":
                    p, rn = next((by_ext[e] for e in (".tga", ".jpg") if e in by_ext), (None, None))
                    if p:
                        from PIL import Image
                        with zipfile.ZipFile(os.path.join(GAME, "main", p)) as r:
                            im = Image.open(io.BytesIO(r.read(rn)))
                        buf = io.BytesIO()
                        im.convert("RGBA" if ext == ".tga" else "RGB").save(buf, "TGA" if ext == ".tga" else "JPEG", quality=95)
                        z.writestr(n, buf.getvalue())
            swapped += 1
    print("compiling against %d paks; %d of the map's textures at their retail size" % (len(paks), swapped))
    return d


def run(exe, args, log):
    t = time.time()
    p = subprocess.run([os.path.join(TOOLS, exe)] + args, capture_output=True, text=True, errors="replace")
    out = p.stdout + p.stderr
    with open(log, "a") as f:
        f.write("==== %s %s\n%s\n" % (exe, " ".join(args), out))
    # progress lines are carriage-return soup; keep the rest
    lines = [l for l in out.replace("\r", "\n").split("\n") if l.strip() and "% complete" not in l and not l.startswith("entering ")]
    bad = [l for l in lines if re.search(r"ERROR|LEAK|leaked|WARNING|not found|couldn't", l, re.I)]
    print("  %s %s: %.0f s" % (exe, args[0] if args[0].startswith("-") else "", time.time() - t))
    for l in bad[:25]:
        print("    " + l)
    if p.returncode != 0 or any("ERROR" in l for l in bad):
        sys.exit("%s failed; the log is %s" % (exe, log))
    return lines


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fast", action="store_true", help="rough vis and light")
    ap.add_argument("--no-light", action="store_true", help="skip the light pass (fullbright test)")
    ap.add_argument("--install", help="copy the pk3 into this main folder")
    ap.add_argument("--out", default=os.path.join(REPO, "build", "techdemo"))
    a = ap.parse_args()

    os.makedirs(a.out, exist_ok=True)
    log = os.path.join(a.out, "compile.log")
    open(log, "w").close()

    name = techdemo.NAME
    m = techdemo.build()
    text = m.text()
    # a face line ends in: ) texture xoff yoff rot xscale yscale ...
    d = gamedir(set(re.findall(r"\) (\S+) -?[\d.]+ -?[\d.]+ ", text)))
    mapfile = os.path.join(d, "main", "maps", name + ".map")
    with open(mapfile, "w") as f:
        f.write(text)
    shutil.copy(mapfile, os.path.join(a.out, name + ".map"))
    print("%s: %d entities, %d world brushes" % (name, len(m.ents), len(m.world.brushes)))

    wg, wm = winpath(d) + "\\", winpath(mapfile)
    fast = ["-fast"] if a.fast else []
    run("Q3map.exe", ["-v", "-gamedir", wg, wm], log)
    run("Q3map.exe", ["-vis", "-v"] + fast + ["-gamedir", wg, wm], log)
    if not a.no_light:
        run("MOHlight.exe", ["-v"] + fast + ["-gamedir", wg, wm], log)

    bsp = os.path.join(d, "main", "maps", name + ".bsp")
    pk3 = os.path.join(a.out, PAK)
    with zipfile.ZipFile(pk3, "w", zipfile.ZIP_DEFLATED) as z:
        z.write(bsp, "maps/%s.bsp" % name)
        z.writestr("maps/%s_bench.scr" % name, techdemo.bench_script(m.bench_lap))
        pak = os.path.join(HERE, "pak")
        for root, _, files in os.walk(pak):
            for fn in files:
                p = os.path.join(root, fn)
                z.write(p, os.path.relpath(p, pak).replace(os.sep, "/"))
    print("%s (%.1f MB)" % (pk3, os.path.getsize(pk3) / 1e6))
    if a.install:
        dst = os.path.join(a.install, PAK)
        shutil.copy(pk3, dst)
        print("installed: " + dst)


if __name__ == "__main__":
    main()
