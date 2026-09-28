#!/usr/bin/env python3
# ev.py dump threshold t0 t1 : steps where a joint moves more than threshold units, with buried joints and limb middles
import sys, math
fr=[]; cur=None; names={}
for l in open(sys.argv[1]):
    if l.startswith('# joint '):
        p=l.split(None,3); names[int(p[2])]=p[3].strip()[6:]
    elif l.startswith('F '):
        p=l.split(); cur={'t':int(p[1]),'state':int(p[3]),'sup':int(p[4]),'steps':int(p[6]),'P':{},'C':(0,0,0),'L':0}
        if cur['steps']>0 and cur['state']==2: fr.append(cur)
    elif l.startswith('P ') and cur is not None: p=l.split(); cur['P'][int(p[1])]=tuple(map(float,p[2:5]))
    elif l.startswith('C ') and cur is not None: cur['C']=tuple(int(x) for x in l.split()[1:4])
    elif l.startswith('L ') and cur is not None: cur['L']=int(l.split()[1])
fr=[f for f in fr if len(f['P'])>=23]
thr=float(sys.argv[2]) if len(sys.argv)>2 else 3.0
t0=int(sys.argv[3]) if len(sys.argv)>3 else 0; t1=int(sys.argv[4]) if len(sys.argv)>4 else 10**9
for a,b in zip(fr,fr[1:]):
    if not (t0<=b['t']<=t1) or b['t']-a['t']>40: continue
    ds={j:math.dist(a['P'][j],b['P'][j]) for j in range(23)}
    mean=sum(ds.values())/23
    top=sorted(ds,key=lambda j:-ds[j])[:3]
    bur=b['C'][2]; lb=b['L']
    if ds[top[0]]-mean>thr or bur or lb:
        print(f"t={b['t']} sup={b['sup']:2d} con={bin(b['C'][0]).count('1'):2d} mean={mean:5.2f} "+", ".join(f"{names[j]} {ds[j]:.1f}" for j in top)+(f" BURIED {[names[j] for j in range(23) if bur>>j&1]}" if bur else "")+(f" LIMBBURIED {lb:#x}" if lb else ""))
