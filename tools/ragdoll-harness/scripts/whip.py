#!/usr/bin/env python3
# whip.py dump : steps where a joint moves more than 6 units off the body's mean motion, and who
import sys, math, collections
names={}; fr=[]; cur=None
for l in open(sys.argv[1]):
    if l.startswith('# joint '):
        p=l.split(None,3); names[int(p[2])]=p[3].strip()[6:]
    elif l.startswith('F '):
        p=l.split(); cur={'t':int(p[1]),'state':int(p[3]),'sup':int(p[4]),'steps':int(p[6]),'P':{},'C':(0,0,0)}
        if cur['steps']==1 and cur['state']==2: fr.append(cur)
    elif l.startswith('P ') and cur is not None: p=l.split(); cur['P'][int(p[1])]=tuple(map(float,p[2:5]))
    elif l.startswith('C ') and cur is not None: cur['C']=tuple(int(x) for x in l.split()[1:4])
fr=[f for f in fr if len(f['P'])>=23]
whips=0; steps=0; worst=0; who=collections.Counter(); ctx=collections.Counter()
for a,b in zip(fr,fr[1:]):
    if b['t']-a['t']>40: continue
    d={j:[b['P'][j][k]-a['P'][j][k] for k in range(3)] for j in range(23)}
    m=[sum(d[j][k] for j in d)/23 for k in range(3)]
    steps+=1
    for j in d:
        r=math.dist(d[j],m)
        if r>worst: worst=r
        if r>6: whips+=1; who[names[j]]+=1; ctx['contact' if (b['C'][0]>>j)&1 or (a['C'][0]>>j)&1 else 'free']+=1
print(f" steps {steps} whips(>6u rel) {whips} ({1000*whips/max(1,steps):.0f}/1000 steps) worst {worst:.1f}  who {who.most_common(4)}  ctx {dict(ctx)}")
