"""Extract world-space joint positions per frame from a MOHAA death animation.

The harness used to seed its corpses from a standing pose nudged by hand, which
is nothing like what a death animation actually does to a body, and several
artifacts only ever showed up in game because of it. This reads the real thing:
the skeleton out of the .skd for the hierarchy and bone lengths, the .skc for
the per frame channels, and runs forward kinematics to get joint positions.

The leg bones are IK in the skeleton, so their rotations come from the "rotFK"
channels the animations carry for exactly this purpose, and the offsets from the
IK bone lengths.
"""
import zipfile, struct, sys, math

import os

# The retail Pak0.pk3. The animations are the game's own and are not shipped
# with this tree, so rd_anims.h is generated from the player's copy.
PAK = os.environ.get("MOHAA_PAK0", "/mnt/d/Medal of Honor/main/Pak0.pk3")

# The death animations rd_anims.h carries, in the order the harness runs them.
HARNESS_ANIMS = [
    "death_back1", "death_chest", "death_collapse", "death_fall_back", "death_left", "death_right",
    "death_choke", "death_backgrenade", "death_crotch", "death_fall_to_knees", "back_death01",
    "death_run01", "death_run02", "death_run03", "death_twist", "death_knockedup", "death_prone1",
]

def qmat(q):
    x, y, z, w = q
    n = math.sqrt(x*x + y*y + z*z + w*w)
    if n < 1e-9:
        return [[1,0,0],[0,1,0],[0,0,1]]
    x, y, z, w = x/n, y/n, z/n, w/n
    return [[1-2*(y*y+z*z), 2*(x*y+z*w),   2*(x*z-y*w)],
            [2*(x*y-z*w),   1-2*(x*x+z*z), 2*(y*z+x*w)],
            [2*(x*z+y*w),   2*(y*z-x*w),   1-2*(x*x+y*y)]]

def mul(a, b):  # a then b
    return [[sum(a[i][k]*b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]

def xform(m, v):
    return [sum(v[k]*m[k][i] for k in range(3)) for i in range(3)]

def load_skeleton(z, path):
    d = z.read(path)
    numSurf, numBones, ofsBones, _, _ = struct.unpack_from("<5i", d, 72)
    o, bones = ofsBones, []
    for _ in range(numBones):
        name   = d[o:o+32].split(b'\0')[0].decode()
        parent = d[o+32:o+64].split(b'\0')[0].decode()
        bt, ofsBase, _, _, bEnd = struct.unpack_from("<5i", d, o+64)
        b = o + ofsBase
        off, length = (0.0, 0.0, 0.0), 0.0
        if bt == 0:                                    # rotation
            off = struct.unpack_from("<3f", d, b)
        elif bt == 2:                                  # ik shoulder
            off = struct.unpack_from("<3f", d, b+16)
        elif bt in (3, 4):                             # ik elbow / wrist
            v = struct.unpack_from("<3f", d, b)
            length = math.sqrt(sum(c*c for c in v))
            off = (length, 0.0, 0.0)
        bones.append(dict(name=name, parent=parent, type=bt, off=off))
        o += bEnd
    return bones

def load_anim(z, path):
    d = z.read(path)
    _, _, _, frameTime = struct.unpack_from("<iiif", d, 4)
    _, numChannels, ofsNames, numFrames = struct.unpack_from("<fiii", d, 32)
    names = [d[ofsNames+i*32:ofsNames+i*32+32].split(b'\0')[0].decode() for i in range(numChannels)]
    frames = []
    for i in range(numFrames):
        ofs = struct.unpack_from("<i", d, 48 + i*48 + 44)[0]
        frames.append([struct.unpack_from("<4f", d, ofs + c*16) for c in range(numChannels)])
    return names, frames, frameTime

def fk(bones, names, frame):
    """World transform per bone.

    The rotations are transposed relative to the obvious reading of the
    quaternion, and a child concatenates as local-then-parent. Both were settled
    by checking the result against the skeleton: only this combination stands the
    figure upright and reproduces every bone length in the .skd exactly.

    A "pos" channel is honoured only for bones that really are animated by
    position. The feet carry one as well, because they are the IK targets, but
    using it here puts the foot wherever the IK wanted it rather than a shin's
    length from the knee, and the shin then stretches to two or three times its
    length as the animation runs.
    """
    ch = {n: v for n, v in zip(names, frame)}
    out = {}
    for b in bones:
        n = b["name"]
        rot = ch.get(n + " rotFK") or ch.get(n + " rot")
        pos = ch.get(n + " pos") if b["type"] == 1 else None
        m = qmat(rot) if rot else [[1,0,0],[0,1,0],[0,0,1]]
        m = [[m[j][i] for j in range(3)] for i in range(3)]
        local = list(pos[:3]) if pos else list(b["off"])
        if b["parent"] in out:
            pm, pp = out[b["parent"]]
            wm = mul(m, pm)
            wp = [pp[k] + xform(pm, local)[k] for k in range(3)]
        else:
            wm, wp = m, local
        out[n] = (wm, wp)
    return out

# ---- harness table generation -------------------------------------------

HARNESS_BONES = [
    "Bip01","Bip01 Pelvis","Bip01 Spine","Bip01 Spine1","Bip01 Spine2","Bip01 Neck","Bip01 Head",
    "Bip01 L Clavicle","Bip01 L UpperArm","Bip01 L Forearm","Bip01 L Hand",
    "Bip01 R Clavicle","Bip01 R UpperArm","Bip01 R Forearm","Bip01 R Hand",
    "Bip01 L Thigh","Bip01 L Calf","Bip01 L Foot","Bip01 L Toe0",
    "Bip01 R Thigh","Bip01 R Calf","Bip01 R Foot","Bip01 R Toe0",
]

# Same factor the synthetic body used, so the real poses land at the stature the
# scenarios and thresholds were written against.
SCALE = 0.42

def emit(anims, out):
    z = zipfile.ZipFile(PAK)
    bones = load_skeleton(z, "models/human/allied_army_soldier/usarmy.skd")
    f = open(out, "w")
    f.write("// Generated by skcdump.py -- real MOHAA death animations.\n")
    f.write("// Joint positions and bone axes per frame, run through forward\n")
    f.write("// kinematics from the shipped .skd and .skc files. Do not edit.\n\n")
    f.write("#define RD_ANIM_BONES %d\n\n" % len(HARNESS_BONES))
    table = []
    for path in anims:
        short = path.rsplit("/", 1)[-1][:-4]
        names, frames, ft = load_anim(z, path)
        sym = "anim_" + short.replace("-", "_")
        f.write("static const float %s[] = {\n" % sym)
        for fr in frames:
            p = fk(bones, names, fr)
            for bn in HARNESS_BONES:
                m, pos = p[bn]
                vals = [pos[k] * SCALE for k in range(3)] + [m[r][c] for r in range(3) for c in range(3)]
                f.write("    " + ",".join("%.5ff" % v for v in vals) + ",\n")
        f.write("};\n")
        table.append((short, len(frames), ft, sym))
    f.write("\nstatic const struct { const char *name; int numFrames; float frameTime; const float *data; }\n")
    f.write("rd_anims[] = {\n")
    for short, n, ft, sym in table:
        f.write('    {"%s", %d, %.6ff, %s},\n' % (short, n, ft, sym))
    f.write("};\n\n#define RD_NUM_ANIMS ((int)(sizeof(rd_anims)/sizeof(rd_anims[0])))\n")
    f.close()
    print("wrote %s: %d animations" % (out, len(table)))


if __name__ == "__main__" and len(sys.argv) > 2 and sys.argv[1] == "--emit":
    # ./skcdump.py --emit rd_anims.h
    emit(["models/human/animation/deaths/%s.skc" % a for a in HARNESS_ANIMS], sys.argv[2])
elif __name__ == "__main__":
    z = zipfile.ZipFile(PAK)
    bones = load_skeleton(z, "models/human/allied_army_soldier/usarmy.skd")
    names, frames, ft = load_anim(z, sys.argv[1] if len(sys.argv) > 1
                                  else "models/human/animation/deaths/death_back1.skc")
    print("bones %d  frames %d  frameTime %.4f" % (len(bones), len(frames), ft))
    want = ["Bip01 Pelvis","Bip01 Spine2","Bip01 Head","Bip01 L Thigh","Bip01 L Calf","Bip01 L Foot","Bip01 L Hand"]
    for fi in (0, len(frames)//2, len(frames)-1):
        p = fk(bones, names, frames[fi])
        print(" frame %2d:" % fi, "  ".join("%s(%.0f,%.0f,%.0f)" % (w.split()[-1], *p[w][1]) for w in want))
