#!/usr/bin/env python3
import sys, math
# bone.py dump jointA jointB t0 t1 : distance between two joints per stepped frame, with z of each
f,a,b,t0,t1=sys.argv[1],int(sys.argv[2]),int(sys.argv[3]),int(sys.argv[4]),int(sys.argv[5])
cur=None; P={}
def flush():
    if cur and cur[1]>0 and a in P and b in P and t0<=cur[0]<=t1:
        print(f"t={cur[0]} len={math.dist(P[a],P[b]):6.1f}  {a}: {P[a][0]:.1f} {P[a][1]:.1f} {P[a][2]:.2f}   {b}: {P[b][0]:.1f} {P[b][1]:.1f} {P[b][2]:.2f}  C={cur[2]}")
for l in open(f):
    if l.startswith('F '):
        flush(); p=l.split(); cur=[int(p[1]),int(p[6]),'']; P={}
    elif l.startswith('C ') and cur: cur[2]=' '.join(l.split()[1:])
    elif l.startswith('P ') and cur: p=l.split(); P[int(p[1])]=tuple(map(float,p[2:5]))
flush()
