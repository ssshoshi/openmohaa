#!/usr/bin/env python3
"""Read a ragdoll_dump.txt written by cg_ragdoll_dump and say what went wrong.

The point of the trace is that it carries three things per frame side by side:
where the simulation put its particles, what the animation would have drawn,
and what was actually drawn. A bone that turns while its particles sit still is
a reconstruction fault, and nothing else can tell that apart from the physics
having genuinely moved something.

  ./rdtrace.py ragdoll_dump.txt            summary
  ./rdtrace.py ragdoll_dump.txt --bone 13  one bone, frame by frame
"""
import sys, math, re
from collections import defaultdict

def sub(a, b):  return [x - y for x, y in zip(a, b)]
def dot(a, b):  return sum(x * y for x, y in zip(a, b))
def cross(a, b):
    return [a[1]*b[2]-a[2]*b[1], a[2]*b[0]-a[0]*b[2], a[0]*b[1]-a[1]*b[0]]
def norm(v):
    l = math.sqrt(dot(v, v))
    return ([x / l for x in v], l) if l > 1e-9 else (list(v), 0.0)
def rot(v, ax, deg):
    t = math.radians(deg); c, s = math.cos(t), math.sin(t); cr = cross(ax, v)
    return [v[i]*c + cr[i]*s + ax[i]*dot(ax, v)*(1-c) for i in range(3)]

def roll_between(ax, ay, bx, by):
    """Roll of b about its own length relative to a, the parent's Y carried onto
    b's direction first so the bend at the joint does not leak into the number."""
    r, sn = norm(cross(ax, bx))
    cs = dot(ax, bx)
    car = ay if sn < 1e-3 else rot(ay, r, math.degrees(math.atan2(sn, cs)))
    ref, l = norm([car[i] - dot(car, bx)*bx[i] for i in range(3)])
    if l < 1e-3: return None                      # singular: not measurable
    yb, l2 = norm([by[i] - dot(by, bx)*bx[i] for i in range(3)])
    if l2 < 1e-3: return None
    return math.degrees(math.atan2(dot(yb, cross(bx, ref)), dot(yb, ref)))

def wrap(d):
    while d > 180.0:  d -= 360.0
    while d < -180.0: d += 360.0
    return d

def load(path):
    joints, bones, parent, frames = {}, {}, {}, []
    cur = None
    for ln in open(path):
        if ln.startswith('#'):
            for m in re.finditer(r'# joint (\d+) ([^#\n]+)', ln):
                joints[int(m.group(1))] = m.group(2).strip()
            m = re.match(r'# bone (\d+) parent (-?\d+) joint (\d+) aim (\d+) (.+)', ln)
            if m:
                bones[int(m.group(1))] = m.group(5).strip()
                parent[int(m.group(1))] = int(m.group(2))
            continue
        f = ln.split()
        if not f: continue
        if f[0] == 'F':
            cur = {'t': int(f[1]), 'w': float(f[2]), 'state': int(f[3]),
                   'sup': int(f[4]), 'P': {}, 'A': {}, 'B': {}}
            if len(f) >= 8:
                cur['disp'] = float(f[5]); cur['steps'] = int(f[6]); cur['quiet'] = int(f[7])
            frames.append(cur)
        elif cur is None:
            continue
        elif f[0] == 'C':
            cur['C'] = (int(f[1]), int(f[2]), int(f[3]))
        elif f[0] == 'E':
            cur['E'] = [float(x) for x in f[1:4]]
        elif f[0] == 'P':
            cur['P'][int(f[1])] = [float(x) for x in f[2:5]]
        elif f[0] in ('A', 'B'):
            i = int(f[1]); v = [float(x) for x in f[2:14]]
            cur[f[0]][i] = {'p': v[0:3], 'x': v[3:6], 'y': v[6:9], 'z': v[9:12]}
    return joints, bones, parent, frames

def main():
    path = sys.argv[1] if len(sys.argv) > 1 else 'ragdoll_dump.txt'
    only = None
    if '--bone' in sys.argv: only = int(sys.argv[sys.argv.index('--bone') + 1])
    joints, bones, parent, frames = load(path)
    if not frames:
        print('no frames in', path); return
    n = len(frames)
    print(f'{path}: {n} frames, {frames[0]["t"]}..{frames[-1]["t"]} ms, '
          f'{len(bones)} bones, {len(joints)} joints')
    for ln in open(path):
        if not ln.startswith('#'): break
        if ln.startswith('# entity') or ln.startswith('# blendtime') or ln.startswith('# sleepvel'):
            print('   ' + ln[2:].rstrip())
    blend = next((f['t'] for f in frames if f['w'] >= 1.0), None)
    print(f'blend ends at {blend} ms; final state {frames[-1]["state"]}, '
          f'supports {frames[-1]["sup"]}')

    # When the body actually stopped. Everything after this is the interesting
    # part: a bone that still moves once no particle does is the reconstruction
    # talking, not the physics.
    move = [0.0] * n
    for i in range(1, n):
        pa, pb = frames[i - 1]['P'], frames[i]['P']
        move[i] = max((norm(sub(pb[k], pa[k]))[1] for k in pb if k in pa), default=0.0)
    # Per rendered frame, not per physics step. The game draws several frames
    # to each 1/60 s step, so this is a much weaker test than the solver's own
    # sleep check and must not be compared against cg_ragdoll_sleepvel. Where
    # the trace carries the solver's own figure, prefer it.
    if 'disp' in frames[-1]:
        solver = [f['disp'] for f in frames if f.get('steps', 0) > 0]
        still = n
        for i in range(n - 1, 0, -1):
            if frames[i].get('steps', 0) > 0 and frames[i]['disp'] > 0.25:
                still = i + 1
                break
        what = "the solver's own per-step figure"
    else:
        still = n
        for i in range(n - 1, 0, -1):
            if move[i] > 0.25:
                still = i + 1
                break
        what = 'rendered-frame movement (weaker than the solver test)'
    if still >= n:
        print(f'still moving at the end, by {what}')
    else:
        print(f'stops moving at {frames[still]["t"]} ms, by {what}')

    if only is not None:
        b = only
        print(f'\n-- bone {b} {bones.get(b,"?")}, parent '
              f'{parent.get(b)} {bones.get(parent.get(b),"-")} --')
        print(f'{"t":>7} {"w":>5} {"drawnRoll":>10} {"animRoll":>9} '
              f'{"step":>7} {"partMove":>9}')
        prev = None; prevP = None
        for f in frames:
            if b not in f['B']: continue
            p = parent.get(b)
            dr = roll_between(f['B'][p]['x'], f['B'][p]['y'], f['B'][b]['x'], f['B'][b]['y']) if p in f['B'] else None
            ar = roll_between(f['A'][p]['x'], f['A'][p]['y'], f['A'][b]['x'], f['A'][b]['y']) if p in f.get('A',{}) and b in f['A'] else None
            step = abs(wrap(dr - prev)) if (prev is not None and dr is not None) else 0.0
            pm = 0.0
            if prevP is not None:
                pm = max(norm(sub(f['P'][k], prevP[k]))[1] for k in f['P'])
            flag = '  <-- bone turns, particles do not' if step > 15 and pm < 0.3 else ''
            print(f'{f["t"]:7d} {f["w"]:5.2f} '
                  f'{dr if dr is not None else float("nan"):10.1f} '
                  f'{ar if ar is not None else float("nan"):9.1f} '
                  f'{step:7.1f} {pm:9.3f}{flag}')
            prev = dr; prevP = f['P']
        return

    # why a corpse is still awake: the sleep test needs the body supported and
    # its worst particle under cg_ragdoll_sleepvel for cg_ragdoll_sleeptime
    if 'disp' in frames[-1]:
        # Only the part that was solved: since traces carry on past sleep, the
        # tail is entity records with no physics behind them and would swamp
        # this.
        solved = [f for f in frames if f['P']]
        half = solved[len(solved) // 2:] if solved else []
        stepped = [f for f in half if f.get('steps', 0) > 0]
        if stepped:
            over = [f for f in stepped if f['disp'] > 0.25]
            print(f'\n-- sleep, over the last half --')
            print(f'   worst particle: median {sorted(f["disp"] for f in stepped)[len(stepped)//2]:.3f}, '
                  f'max {max(f["disp"] for f in stepped):.3f}  (sleeps under 0.25)')
            print(f'   {len(over)} of {len(stepped)} stepped frames over the threshold')
            print(f'   longest quiet run reached: {max(f["quiet"] for f in half)} ms (needs 400)')

    if any('C' in f for f in frames):
        cf = [f for f in frames if 'C' in f]
        print('\n-- contact --')
        nb = (max(joints) + 1) if joints else 32
        anyb = [f for f in cf if f['C'][2]]
        print(f'   {len(anyb)} of {len(cf)} frames had a joint buried in solid geometry')
        if anyb:
            per = {}
            for f in anyb:
                for j in range(nb):
                    if f['C'][2] & (1 << j): per[j] = per.get(j, 0) + 1
            for j, c in sorted(per.items(), key=lambda kv: -kv[1])[:6]:
                print(f'      {joints.get(j, j):20} buried on {c} frames')
        last = cf[-1]
        held = [joints.get(j, j) for j in range(nb) if last['C'][0] & (1 << j)]
        grnd = [joints.get(j, j) for j in range(nb) if last['C'][1] & (1 << j)]
        print(f'   at the end: {len(held)} joints holding a contact plane, {len(grnd)} on the ground')
        if held and len(held) <= 8:
            print(f'      holding: {", ".join(held)}')
        if grnd and len(grnd) <= 8:
            print(f'      ground:  {", ".join(grnd)}')

    if any('E' in f for f in frames):
        es = [(f['t'], f['E']) for f in frames if 'E' in f]
        e0, e1 = es[0][1], es[-1][1]
        print(f'\n-- entity --')
        print(f'   origin moved {norm(sub(e1, e0))[1]:.1f} units over the trace')
        # Once asleep the matrices are frozen in model space, so from then on the
        # drawn body follows the entity and not the solver.
        sl = [f for f in frames if f['state'] == 3 and 'E' in f]
        if sl:
            eSleep = sl[0]['E']
            drift = norm(sub(e1, eSleep))[1]
            print(f'   asleep from {sl[0]["t"]} ms; entity has moved {drift:.1f} units since')
            if drift > 4.0:
                print(f'   the drawn corpse rides that: it is {drift:.1f} units from where it settled')
        pel = [k for k in joints if joints[k].endswith('Pelvis')]
        last = next((f for f in reversed(frames) if f['P']), None)
        if pel and last:
            print(f'   pelvis sat {norm(sub(last["P"][pel[0]], last["E"] if "E" in last else e1))[1]:.1f} '
                  f'units from the entity origin while solving')

    # worst roll step per bone, and whether the particles moved when it happened
    print('\n-- roll about its own length, per bone --')
    print(f'{"bone":22} {"worst step":>11} {"at ms":>7} {"partMove":>9} {"settled":>8}')
    rows = []
    for b in sorted(bones):
        p = parent.get(b)
        if p is None or p < 0: continue
        prev = None; worst = 0.0; at = 0; pmAt = 0.0; late = 0.0; lateAt = 0
        for i, f in enumerate(frames):
            if b not in f['B'] or p not in f['B']: continue
            dr = roll_between(f['B'][p]['x'], f['B'][p]['y'], f['B'][b]['x'], f['B'][b]['y'])
            if dr is not None and prev is not None:
                st = abs(wrap(dr - prev))
                if st > worst: worst, at, pmAt = st, f['t'], move[i]
                if i >= still and st > late: late, lateAt = st, f['t']
            prev = dr if dr is not None else prev
        rows.append((worst, bones[b], at, pmAt, late, lateAt))
    for worst, nm, at, pm, late, lateAt in sorted(rows, reverse=True)[:10]:
        mark = ''
        if late > 5.0:
            mark = f'  <-- {late:.0f} deg at {lateAt} ms with the body at rest'
        elif worst > 15 and pm < 0.3:
            mark = '  <-- bone turns, particles do not'
        print(f'{nm:22} {worst:11.1f} {at:7d} {pm:9.3f} {late:8.1f}{mark}')

    # limbs left in the air, measured against the lowest thing in the body
    print('\n-- last frame --')
    # The last frame that still carries particles: once the corpse is asleep
    # nothing is solved and only the entity is recorded.
    f = next((g for g in reversed(frames) if g['P']), frames[-1])
    if not f['P']:
        print('   no particle records'); return
    ground = min(p[2] for p in f['P'].values())
    for k in sorted(joints):
        if any(w in joints[k] for w in ('Foot', 'Hand', 'Toe')):
            print(f'   {joints[k]:20} {f["P"][k][2] - ground:6.1f} above lowest point')
    lt = [k for k in joints if joints[k].endswith('L Thigh')]
    lc = [k for k in joints if joints[k].endswith('L Calf')]
    rt = [k for k in joints if joints[k].endswith('R Thigh')]
    rc = [k for k in joints if joints[k].endswith('R Calf')]
    if lt and lc and rt and rc:
        a, _ = norm(sub(f['P'][lc[0]], f['P'][lt[0]]))
        b2, _ = norm(sub(f['P'][rc[0]], f['P'][rt[0]]))
        print(f'   legs {math.degrees(math.acos(max(-1,min(1,dot(a,b2))))):.0f} deg apart at the hip')

main()
