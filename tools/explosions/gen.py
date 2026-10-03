#!/usr/bin/env python3
"""Write the opm-explosions data: every explosion effect from one template.

Explosions in the Half-Life 2 manner (backlog #12, #16, #19): a white flash
and a big light, a fireball that blooms and rises, sparks and debris flying
out, dust rolling out along the ground, a scorch mark, and a column of dark
smoke that hangs on. Each effect calls "opm_blast <size>" (cgame cg_blast.cpp)
for the shaking view and ringing ears near it; the push on the body is the
server's (g_blastpush).

Sizes: 1 a grenade, 1.5 a bazooka or shell, 2.5 to 3 the maps' scripted
demolitions and airstrikes. Only the game's own sprites and models are used,
by name. The TIKIs replaced are effect-only ones; those that carry damage
(fx_explosion, tankshellexplosion...) are left as they are.

    tools/explosions/gen.py          # writes data/opm-explosions/
"""

import math
import os
import random
import struct
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "..", "data", "opm-explosions")

# The explosions themselves, played by the client's effect manager, which
# alone can time one part after another ("delayedsfx"): the weapons' (where a
# grenade or a shell lands) and the three the maps' scripted ones play.
# Most explosions are mostly earth, not fire: fire scales the fireball (1 the
# full one), and dirt adds the geyser of earth of the mortar hits on Omaha
# beach (m3l1), which the bazookas, the shells and the Flak 88 rounds landing
# round the player (m3l2) throw up.
#   name: (size, sound, fire, dirt)
EXPLOSIONS = {
    "grenexp_base":         (1.0, "grenade_explode", 0.6, True),
    "bazookaexp_base":      (1.5, "bazooka_exp", 0.3, True),
    "opm_explosion_small":  (1.8, None, 0.6, False),
    "opm_explosion_medium": (2.4, None, 0.6, True),
    "opm_explosion_large":  (3.0, None, 0.7, True),
}

# The maps' explosion models: script models whose animation plays one of the
# above ("opm_explode", cgame). The first animation listed is the one they are
# spawned in, the second the one the map plays or that follows it.
#   name: (explosion, anims)
MODELS = {
    "fx_tank_explosion":   ("medium", ["idle", "start"]),
    "fx_flak88_explosion": ("large", ["aaaa", "idle"]),
    "fx_truck_explosion":  ("large", ["aaaa", "idle"]),
    "fx_stuka_explosion":  ("large", ["idle", "start"]),
    "fx_bike_explosion":   ("small", ["idle", "start"]),
    "exp_dynamic1":        ("medium", ["aaaa", "idle"]),
    "exp_dynamic2":        ("large", ["aaaa", "idle"]),
}


def f(x):
    return ("%.2f" % x).rstrip("0").rstrip(".")


def emitters(s, fire=1.0, dirt=False):
    """The commands of one explosion of size s, without their prefix."""
    n = lambda k: max(1, int(round(k)))
    c = []

    # the feel of it: shake, ringing ears
    c.append("opm_blast %s" % f(s))

    # a big, sudden light, going orange as it dies
    c.append("""blockdlight %s 0.7
(
	color 1.0 0.8 0.5
	colorvelocity -1.2 -1.1 -0.8
	scalerate -0.7
)""" % f(380 * s))

    # the white flash
    c.append("""originspawn
(
	model grenexp_flash.spr
	count 1
	color 1.0 0.95 0.85
	scale %s
	scalerate 3.0
	life 0.15
	fade
)""" % f(2.6 * s))

    # the fireball: several blooms that rise
    c.append("""originspawn
(
	model gren_explosion.spr
	count %d
	color 1.0 0.62 0.34
	scalemin %s
	scalemax %s
	scalerate 0.7
	life 0.6
	offset crandom %s crandom %s random %s
	randvel crandom %s crandom %s range %s %s
	accel 0 0 140
	friction 1.5
	randomroll
)""" % (n((2 + s) * fire), f(0.9 * s * (0.4 + 0.6 * fire)), f(1.5 * s * (0.4 + 0.6 * fire)),
        f(14 * s), f(14 * s), f(18 * s), f(50 * s), f(50 * s), f(40 * s), f(90 * s)))

    # fire thrown further out
    if fire >= 0.5:
        c.append("DELAY 0.05 " + """originspawn
(
	model fgrenexplosion.spr
	count %d
	color 1.0 0.55 0.25
	scalemin %s
	scalemax %s
	life 0.45
	offset crandom %s crandom %s random %s
	randvel crandom %s crandom %s range %s %s
	friction 2.0
	randomroll
)""" % (n(2 + s), f(0.5 * s), f(0.9 * s), f(36 * s), f(36 * s), f(30 * s),
        f(140 * s), f(140 * s), f(60 * s), f(180 * s)))

    if dirt:
        c += geyser(s)

    # the shock ring
    c.append("""originspawn
(
	model fgrenshock.spr
	count 1
	scale %s
	scalerate 7.0
	life 0.22
	alpha 0.8
	fade
)""" % f(0.4 * s))

    # sparks, streaking
    c.append("""originspawn
(
	model models/fx/bh_metal_fastpiece.tik
	count %d
	color 1.0 0.8 0.5
	scalemin 2.0
	scalemax 4.0
	scalerate -0.6
	life 0.35
	randvel crandom %s crandom %s range %s %s
	accel 0 0 -1500
	align
	fade
)""" % (n(16 * s), f(650 * s), f(650 * s), f(250 * s), f(800 * s)))

    # debris, tumbling and bouncing
    c.append("""originspawn
(
	model bh_stone_piece.spr
	count %d
	scalemin 0.4
	scalemax 1.0
	life 2.5
	fadedelay 1.8
	randvel crandom %s crandom %s range %s %s
	accel 0 0 -800
	collision
	bouncefactor 0.3
	randomroll
	fade
)""" % (n(6 * s), f(300 * s), f(300 * s), f(250 * s), f(700 * s)))

    # clods of dirt, thrown up and falling back
    c.append("""originspawn
(
	model bh_dirt_piece.spr
	count %d
	scalemin 0.35
	scalemax 0.8
	life 1.8
	randvel crandom %s crandom %s range %s %s
	accel 0 0 -900
	collision
	dietouch
	randomroll
	fade
)""" % (n(10 * s), f(260 * s), f(260 * s), f(300 * s), f(800 * s)))

    # dust rolling out along the ground
    c.append("DELAY 0.15 " + """originspawn
(
	model vsssource.spr
	count %d
	circle
	radius %s
	radialvelocity 0 %s 20
	friction 2.2
	color 0.55 0.48 0.40
	alpha 0.55
	fadein 0.3
	scalemin %s
	scalemax %s
	scalerate 1.1
	life 4.0
	randomroll
	fade
)""" % (n(14 + 4 * s), f(18 * s), f(300 * s), f(0.6 * s), f(1.1 * s)))

    # the column of dark smoke that hangs on
    c.append("DELAY 0.5 " + """originspawn
(
	model vsssource.spr
	count %d
	color 0.16 0.15 0.14
	alpha 0.8
	fadein 0.8
	offset crandom %s crandom %s random %s
	randvel crandom %s crandom %s range %s %s
	friction 0.6
	accel 0 0 6
	scalemin %s
	scalemax %s
	scalerate 0.3
	life %s
	randomroll
	fade
)""" % (n(6 * s), f(18 * s), f(18 * s), f(40 * s), f(14 * s), f(14 * s),
        f(15 * s), f(40 * s), f(2.0 * s), f(3.2 * s), f(6 + 2.5 * s)))

    # paler dust smoke round its foot
    c.append("DELAY 0.4 " + """originspawn
(
	model vsssource.spr
	count %d
	color 0.45 0.42 0.38
	alpha 0.5
	fadein 0.3
	offset crandom %s crandom %s 0
	randvel crandom %s crandom %s range 10 %s
	friction 0.8
	scalemin %s
	scalemax %s
	scalerate 0.5
	life %s
	randomroll
	fade
)""" % (n(5 * s), f(30 * s), f(30 * s), f(30 * s), f(30 * s), f(40 * s),
        f(1.6 * s), f(2.6 * s), f(4 + 1.5 * s)))

    # the scorch mark: an unseen piece dropped onto what is below
    c.append("""originspawn
(
	model vsssource.spr
	count 1
	alpha 0.0
	life 0.4
	randvel 0 0 -700
	collision
	dietouch
	bouncedecal
	decalshader blastmark
	decalradius %s
)""" % f(36 * s))

    return c


def geyser(s):
    """Earth thrown up as on Omaha beach: a jet of dirt, clods raining back
    down round it, and a cloud of dust that rolls and hangs."""
    n = lambda k: max(1, int(round(k)))
    c = []

    # the jets: two columns of earth shooting up and spreading
    c.append("""originspawn
(
	model mortar_dirthit.spr
	count 1
	scale %s
	scalerate 7.0
	life 1.8
	randvel 0 0 %s
	accel 0 0 -320
	offset 0 0 %s
	fadedelay 0.8
	physicsrate every
)""" % (f(0.18 * s), f(330 * s), f(30 * s)))
    c.append("""originspawn
(
	model mortar_dirthit2.spr
	count 1
	scale %s
	scalerate 7.0
	life 1.1
	randvel 0 0 %s
	offset 0 0 %s
	fadedelay 0.6
	physicsrate every
)""" % (f(0.18 * s), f(350 * s), f(26 * s)))

    # clods, thrown out in rings and falling back
    for count, speed, side, spread in ((14, 340, 110, 12), (10, 210, 80, 40), (7, 70, 50, 40)):
        c.append("""originspawn
(
	model dirtplume.spr
	count %d
	offsetalongaxis crandom %s crandom %s %s
	angles crandom 45 crandom 45 crandom 180
	velocity %s
	randvel crandom %s crandom %s random %s
	clampvel %s %s %s %s -450 99999
	avelocity 0 0 crandom 90
	accel 0 0 -800
	life 3.2 0.5
	scalemin %s
	scalemax %s
	scalerate 0.2
	fadedelay 1.4
)""" % (n(count * s), f(spread * s), f(spread * s), f(22 * s), f(speed * s), f(side * s), f(side * s),
            f(270 * s), f(-side * s), f(side * s), f(-side * s), f(side * s), f(0.28 * s), f(0.42 * s)))

    # the dust cloud that rolls and hangs over the hole
    c.append("""originspawn
(
	volumetric
	model mist
	collision
	count %d
	life 12
	color 0.75 0.72 0.66 0.5
	scale %s
	randvel %s %s random %s
	offsetalongaxis crandom %s crandom %s random %s
	smokeparms 0 2.5 2.5
)""" % (n(12 * s), f(4 * s), f(22 * s), f(22 * s), f(22 * s), f(44 * s), f(44 * s), f(22 * s)))
    return c


def indent(text, tabs):
    pad = "\t" * tabs
    return "\n".join(pad + line if line else line for line in text.splitlines())


HEADER = [
    "TIKI",
    "// OPM: generated by tools/explosions/gen.py (opm-explosions); edit that.",
]

SETUP = [
    "setup",
    "{",
    "\tscale 1.0",
    "\tpath models/fx/dummy",
    "\tskelmodel dummy2.skd",
    "}",
    "",
]


def explosion_tiki(size, sound, fire, dirt):
    lines = HEADER + ["// An explosion of size %s in the manner of Half-Life 2." % f(size)] + SETUP
    lines += [
        "init",
        "{",
        "\tclient",
        "\t{",
        "\t\tcache models/fx/bh_metal_fastpiece.tik",
        "\t\tcache gren_explosion.spr",
        "\t\tcache fgrenexplosion.spr",
        "\t\tcache mortar_dirthit.spr",
        "\t\tcache mortar_dirthit2.spr",
        "\t\tcache dirtplume.spr",
    ]
    if sound:
        lines.append("\t\tsfx sound %s" % sound)
    for cmd in emitters(size, fire, dirt):
        if cmd.startswith("DELAY "):
            _, delay, cmd = cmd.split(" ", 2)
            lines.append(indent("delayedsfx %s %s" % (delay, cmd), 2))
        else:
            lines.append(indent("sfx " + cmd, 2))
    lines += ["\t}", "}", "", "animations", "{", "\tidle dummy2.skc", "}"]
    return "\n".join(lines) + "\n"


def model_tiki(explosion, anims):
    lines = HEADER + ["// A map's explosion: plays opm_explosion_%s where it is spawned." % explosion] + SETUP
    lines += [
        "init",
        "{",
        "\tserver",
        "\t{",
        "\t\tclassname Animate",
        "\t\tnotsolid",
        "\t\trendereffects +dontdraw",
        "\t}",
        "}",
        "",
        "animations",
        "{",
        "\t%s dummy2.skc" % anims[0],
        "\t%s dummy2.skc" % anims[1],
        "\t{",
        "\t\tclient",
        "\t\t{",
        "\t\t\tenter opm_explode %s" % explosion,
        "\t\t}",
        "\t}",
        "}",
    ]
    return "\n".join(lines) + "\n"


def ear_ring(path):
    """A ringing in the ears: a high tone and a beat, rising and dying away."""
    rate, secs = 22050, 4.5
    random.seed(7)
    frames = bytearray()
    for i in range(int(rate * secs)):
        t = i / rate
        env = min(1.0, t / 0.08) * math.exp(-t / 1.6)
        v = 0.6 * math.sin(2 * math.pi * 3150 * t) + 0.25 * math.sin(2 * math.pi * 3190 * t)
        v += 0.05 * (random.random() * 2 - 1)
        frames += struct.pack("<h", int(max(-1, min(1, v * env * 0.8)) * 32767))
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(bytes(frames))


def main():
    fx = os.path.join(OUT, "models", "fx")
    os.makedirs(fx, exist_ok=True)
    for name, (size, sound, fire, dirt) in EXPLOSIONS.items():
        with open(os.path.join(fx, name + ".tik"), "w", newline="\r\n") as out:
            out.write(explosion_tiki(size, sound, fire, dirt))
    for name, (explosion, anims) in MODELS.items():
        with open(os.path.join(fx, name + ".tik"), "w", newline="\r\n") as out:
            out.write(model_tiki(explosion, anims))

    snd = os.path.join(OUT, "sound", "opm")
    os.makedirs(snd, exist_ok=True)
    ear_ring(os.path.join(snd, "ear_ring.wav"))
    print("wrote %d effects and the ear ring into %s" % (len(EXPLOSIONS) + len(MODELS), os.path.normpath(OUT)))


if __name__ == "__main__":
    main()
