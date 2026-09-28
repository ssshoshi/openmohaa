#!/usr/bin/env python3
# jv.py dump joint t0 t1 [joint2] : per frame displacement of a joint (and a second one)
import sys, math
j=int(sys.argv[2]); t0=int(sys.argv[3]); t1=int(sys.argv[4])
fr=[]; cur=None
for l in open(sys.argv[1]):
    if l.startswith('F '):
        p=l.split(); cur={'t':int(p[1]),'steps':int(p[6]),'P':{}}; fr.append(cur)
    elif l.startswith('P ') and cur is not None: p=l.split(); cur['P'][int(p[1])]=tuple(map(float,p[2:5]))
prev=None
for f in fr:
    if not (t0<=f['t']<=t1) or j not in f['P']: continue
    if prev:
        d=[f['P'][j][k]-prev['P'][j][k] for k in range(3)]
        g=[f['P'][int(sys.argv[5])][k]-prev['P'][int(sys.argv[5])][k] for k in range(3)] if len(sys.argv)>5 else None
        print(f"t={f['t']} dt={f['t']-prev['t']:2d} steps={f['steps']} d=({d[0]:6.2f},{d[1]:6.2f},{d[2]:6.2f})"+(f"  held d=({g[0]:6.2f},{g[1]:6.2f},{g[2]:6.2f})" if g else ""))
    prev=f
