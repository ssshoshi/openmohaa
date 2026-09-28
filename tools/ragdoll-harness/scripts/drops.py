#!/usr/bin/env python3
import sys, math
# drops.py dump : find landings (centroid vz from < -200 to > -60 u/s within 3 steps) and report the rebound
fr=[]; cur=None
for l in open(sys.argv[1], encoding='latin1'):
    if l.startswith('F '):
        p=l.split(); cur={'t':int(p[1]),'steps':int(p[6]),'sup':int(p[4]),'P':{},'C':0}
        if cur['steps']>0: fr.append(cur)
    elif l.startswith('P ') and cur is not None: p=l.split(); cur['P'][int(p[1])]=tuple(map(float,p[2:5]))
    elif l.startswith('C ') and cur is not None: cur['C']=int(l.split()[1])
fr=[f for f in fr if len(f['P'])>=23]
def cen(f): return [sum(f['P'][j][k] for j in range(23))/23 for k in range(3)]
V=[]
for a,b in zip(fr,fr[1:]):
    dt=(b['t']-a['t'])/1000.0
    if dt<=0 or dt>0.05: V.append(None); continue
    ca,cb=cen(a),cen(b); V.append(([(cb[k]-ca[k])/dt for k in range(3)], b))
i=0
while i < len(V):
    v=V[i]
    if v and v[0][2] < -200:
        # look ahead for stop
        for j in range(i+1, min(i+4,len(V))):
            if V[j] and V[j][0][2] > -60:
                vin=v[0]; b=V[j][1]
                up=0; horiz=0; dz=0
                z0=cen(b)[2]
                for k in range(j, min(j+25,len(V))):
                    if not V[k]: continue
                    up=max(up, V[k][0][2]); horiz=max(horiz, math.hypot(V[k][0][0],V[k][0][1])); dz=max(dz, cen(V[k][1])[2]-z0)
                nc=bin(b['C']).count('1')
                print(f"t={b['t']} in vz {vin[2]:6.0f} h {math.hypot(vin[0],vin[1]):5.0f} | after 400ms: max up {up:5.0f} u/s  rise {dz:5.1f}  max horiz {horiz:5.0f}  contacts {nc} sup {b['sup']}  z {z0:.0f}")
                i=j; break
    i+=1
