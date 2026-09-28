#!/usr/bin/env python3
import sys, math
# cv.py dump t0 t1 : per step centroid velocity, spread, and the joint moving fastest relative to the centroid, with buried flags
f,t0,t1=sys.argv[1],int(sys.argv[2]),int(sys.argv[3])
names={}; fr=[]; cur=None
for l in open(f, encoding='latin1'):
    if l.startswith('# joint '): p=l.split(None,3); names[int(p[2])]=p[3].strip()[6:]
    elif l.startswith('F '):
        p=l.split(); cur={'t':int(p[1]),'steps':int(p[6]),'sup':int(p[4]),'P':{},'C':(0,0,0),'L':0}
        if cur['steps']>0 and t0-50<=cur['t']<=t1: fr.append(cur)
    elif l.startswith('P ') and cur is not None: p=l.split(); cur['P'][int(p[1])]=tuple(map(float,p[2:5]))
    elif l.startswith('C ') and cur is not None: cur['C']=tuple(int(x) for x in l.split()[1:4])
    elif l.startswith('L ') and cur is not None: cur['L']=int(l.split()[1])
fr=[x for x in fr if len(x['P'])>=23]
for a,b in zip(fr,fr[1:]):
    dt=(b['t']-a['t'])/1000
    ca=[sum(a['P'][j][k] for j in range(23))/23 for k in range(3)]; cb=[sum(b['P'][j][k] for j in range(23))/23 for k in range(3)]
    v=[(cb[k]-ca[k])/dt for k in range(3)]
    rel={j:math.dist([b['P'][j][k]-a['P'][j][k] for k in range(3)],[cb[k]-ca[k] for k in range(3)]) for j in range(23)}
    top=max(rel,key=rel.get)
    bur=[names[j] for j in range(23) if b['C'][2]>>j&1]
    print(f"t={b['t']} v=({v[0]:6.0f},{v[1]:6.0f},{v[2]:6.0f}) h={math.hypot(v[0],v[1]):5.0f} con={bin(b['C'][0]).count('1'):2d} sup={b['sup']:2d} top {names[top]} {rel[top]:.1f}" + (f" BUR {bur}" if bur else "") + (f" L {b['L']:#x}" if b['L'] else ""))
