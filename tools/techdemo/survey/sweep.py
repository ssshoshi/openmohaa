"""Screenshots across maps, for studying how they are built: for each map in
a views file (survey/views.py or views_techdemo.py makes one), the camera
held at each view in turn in the orch-test game, one shot each, and a
contact sheet per map. The pictures go outside the repository (retail
content): $DESIGN_REF (default D:\\Medal of Honor\\design-ref)/$OUTDIR.

    ORCH_LIVE=... python3 tools/techdemo/survey/sweep.py [map ...]
    VIEWS=survey/views_techdemo.json OUTDIR=td python3 .../sweep.py

The multiplayer maps hang the game while loading in this build (2026-11-28),
so the views file puts them last."""
import subprocess, json, sys, os, time, shutil
from PIL import Image
os.environ.setdefault("ORCH_LIVE", "/mnt/d/Medal of Honor/orch-test")
HERE = os.path.dirname(os.path.abspath(__file__))
O = ["python3", os.path.join(HERE, "..", "..", "orchestrator", "orch.py")]
SS = os.environ["ORCH_LIVE"] + "/home/main/screenshots/"
OUT = os.path.join(os.environ.get("DESIGN_REF", "/mnt/d/Medal of Honor/design-ref"), os.environ.get("OUTDIR", "retail")) + "/"
os.makedirs(OUT, exist_ok=True)
def run(*a, t=30):
    try: return subprocess.run(O + list(a), capture_output=True, text=True, timeout=t).stdout
    except subprocess.TimeoutExpired: return ""
def state():
    out = run("state")
    try: return json.loads(out[out.index("{"):])
    except Exception: return {}
def cam(x, y, z, p, yw):
    run("cmd", f"tele {x} {y} {z-56}")
    for _ in range(4):
        out = run("cam", str(x), str(y), str(z), "--pitch", str(p), "--yaw", str(yw))
        try: v = json.loads(out[out.index("{"):])["view_origin"]
        except Exception: continue
        if abs(v[0]-x) < 2 and abs(v[1]-y) < 2: return True
    return False
VF = os.environ.get("VIEWS", os.path.join(HERE, "views.json")); views = json.load(open(VF))
MAPOF = lambda m: "m0_techdemo" if m.startswith("td_") else m
only = sys.argv[1:] or list(views)
for m in only:
    tag = m.replace("/", "_")
    if os.path.exists(OUT + tag + "_sheet.jpg"): continue
    st = state()
    if st.get("map", "").lower() != MAPOF(m).split("/")[-1].lower():
        mp = "/" in m
        if not st or mp:
            run("stop", t=30); time.sleep(3)
            if mp: run("launch", "--extra", "g_gametype 1", "--extra", f"map {m}", t=60)
            else: run("launch", "--map", MAPOF(m), t=60)
        else:
            run("cmd", f"devmap {MAPOF(m)}", t=10)
        time.sleep(10)
        for _ in range(60):
            st = state()
            if st.get("in_game") and st.get("map", "").lower() == MAPOF(m).split("/")[-1].lower(): break
            time.sleep(3)
        else:
            print(m, "did not load", flush=True); run("stop", t=30); continue
        time.sleep(4)
    run("cmd", "dog 1; notarget 1; cg_drawhud 0; ui_hud 0; cg_hud 0; r_drawviewmodel 0")
    cam(*views[m][0][:5])  # warm-up: first cam parks at 0,0,0
    run("frames", "1", "--width", "320", "--name", "warm")
    ims = []
    for i, v in enumerate(views[m]):
        name = f"{tag}_{i:02d}"
        src = SS + name + "_0.jpg"
        for attempt in range(3):
            ok = cam(*v[:5]); time.sleep(0.6 + attempt)
            run("frames", "1", "--width", "960", "--name", name)
            try:
                from PIL import ImageStat
                if max(ImageStat.Stat(Image.open(src).convert("L")).mean) > 2: break
            except Exception: pass
        if os.path.exists(src):
            for _ in range(20):
                try:
                    shutil.copyfile(src, OUT + name + ".jpg")
                    break
                except OSError:      # still being written
                    time.sleep(0.5)
            ims.append(Image.open(OUT + name + ".jpg"))
        print(m, i, v, ok, flush=True)
    if ims:
        w, h = 640, int(640 * ims[0].size[1] / ims[0].size[0])
        cols = 3; rows = (len(ims) + cols - 1) // cols
        s = Image.new("RGB", (cols*w, rows*h))
        for i, im in enumerate(ims): s.paste(im.resize((w, h)), ((i % cols)*w, (i//cols)*h))
        s.save(OUT + tag + "_sheet.jpg", quality=85)
print("done")
