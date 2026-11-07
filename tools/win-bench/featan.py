#!/usr/bin/env python3
"""featan.py - tables from a features.sh run directory.

    python3 tools/win-bench/featan.py ~/.cache/omfeat/<date>

For each scene, every arm's mean frame time over the measured window (from
com_frameStats; without it, the last r_gpuTimers sample), p99, worst frame and hitches a
minute (from com_frameStats), and what turning the feature off saved, overall
and per GPU pass (from r_gpuTimers), against the median of the base and base2
runs. Runs in the slow state (wall over 1.5x the base median) are dropped and
listed. Ends with where hitch time went in the base runs (com_hitchLog).
"""
import re,glob,os,sys,statistics as st,collections
if len(sys.argv)!=2: sys.exit(__doc__)
D=sys.argv[1]
def window(log):
    out=[];on=False
    for l in log.splitlines():
        if 'OMBENCH_WIN_1' in l: on=True; continue
        if 'OMBENCH_ENDWIN_1' in l: break
        if on: out.append(l)
    return out
def f(p,t,cast=float):
    m=re.search(p,t,re.M); return cast(m.group(1)) if m else None
runs=collections.defaultdict(lambda: collections.defaultdict(list))
allruns=[]
hitchparts=collections.defaultdict(lambda: collections.defaultdict(float))
nbase=collections.Counter(re.match(r'(.*?)__',os.path.basename(p)).group(1) for p in glob.glob(D+'/*__base*__r*.txt')) or {}
for txt in sorted(glob.glob(D+'/*__*__r*.txt')):
    scene,arm,rep=re.match(r'(.*)__(.*)__r(\d+)',os.path.basename(txt)[:-4]).groups()
    t=open(txt,errors='replace').read()
    if not re.search(r'^GPU',t,re.M): continue
    r={}
    r['wall']=f(r'cpu\s+([\d.]+)ms wall',t)
    for k in ['sunshadow','prepass','main3d','post','sprites','ao','volume','rtshadow']:
        r[k]=f(r'^GPU.*? '+k+r' ([\d.]+)',t)
    r['cgame']=f(r'^cgame\s*:\s*cgame\s+([\d.]+)ms',t)
    r['draws']=f(r'gpu submission: (\d+) draws',t)
    lg=txt[:-4]+'.log'
    if os.path.exists(lg):
        w=window(open(lg,errors='replace').read())
        fs=[l for l in w if 'framestats' in l][1:]  # first window straddles the start
        if fs:
            g=lambda p:[float(re.search(p,l).group(1)) for l in fs]
            n=sum(g(r'framestats (\d+) frames'))
            r['p50']=st.median(g(r'frame mean [\d.]+ p50 ([\d.]+)'))
            r['p99']=st.median(g(r' p99 ([\d.]+) p99\.9'))
            r['max']=max(g(r'frame mean.*? max ([\d.]+) \|'))
            secs=sum(g(r'frame mean ([\d.]+)'))*250/1000  # windows of 250
            # The mean over the whole window: the cpu line is one 100-frame
            # sample at the end, a fixed moment of a scripted scene, and once
            # read 2.8 ms slow for a build faster in every window.
            r['wall']=st.mean(g(r'frame mean ([\d.]+)'))
            ov=[re.search(r'over 33ms (\d+) 50ms (\d+) 100ms (\d+)',l).groups() for l in fs]
            for i,th in enumerate((33,50,100)):
                r[f'h{th}']=sum(int(o[i]) for o in ov)/secs*60
        for l in w:
            m=re.search(r'\] hitch (cgame tail|cgame|renderscene) ([\d.]+) ?ms = (.*)',l)
            if m and arm in ('base','base2'):
                for k,v in re.findall(r'([a-z]+) ([\d.]+)',m.group(3).split('|')[0]):
                    hitchparts[(scene,m.group(1))][k]+=float(v)/nbase[scene]  # per base run
            m=re.search(r'\] hitch ([\d.]+)ms = (.*)',l)
            if m and arm in ('base','base2'):
                for k,v in re.findall(r'([a-z]+) ([\d.]+)',m.group(2)):
                    hitchparts[(scene,'frame')][k]+=float(v)/nbase[scene]
    r['_name']=os.path.basename(txt)[:-4]
    allruns.append((scene,arm,r))
dropped=[]
_bw=collections.defaultdict(list); _bp=collections.defaultdict(list)
for sc,a,r in allruns:
    if a in ('base','base2'): _bw[sc].append(r['wall']); _bp[sc].append(r.get('p50') or 0)
for sc,a,r in allruns:
    bw=st.median(_bw[sc]); bp=st.median(_bp[sc])
    if r['wall']>1.5*bw or (r.get('p50') and bp and r['p50']>1.25*bp):
        dropped.append(r['_name']); continue
    for k,v in r.items():
        if k!='_name': runs[(sc,a)][k].append(v)
print('dropped (slow state):',', '.join(dropped))
def med(v):
    v=[x for x in v if x is not None]; return st.median(v) if v else None
scenes=sorted({s for s,a in runs},key=lambda s:['fight','street','m1l2a','m4l1'].index(s) if s in ['fight','street','m1l2a','m4l1'] else 9)
cols=['wall','p50','p99','max','h33','h50','h100','sunshadow','prepass','main3d','volume','ao','sprites','cgame','draws']
for s in scenes:
    base={k:med(runs[(s,'base')][k]+runs[(s,'base2')][k]) for k in cols}
    print(f"\n### {s}  (base wall {base['wall']:.2f} ms, base+base2 spread {min(runs[(s,'base')]['wall']+runs[(s,'base2')]['wall']):.2f}-{max(runs[(s,'base')]['wall']+runs[(s,'base2')]['wall']):.2f})")
    rows=[]
    for (ss,a),d in runs.items():
        if ss!=s or a in('base','base2'): continue
        m={k:med(d[k]) for k in cols}
        rows.append((base['wall']-m['wall'] if m['wall'] else 0,a,m,len(d['wall'])))
    rows.sort(reverse=True)
    print('feature off | n | saves ms | wall | p99 | max | hitch>33/min | >50/min | >100/min | Δsun | Δprepass | Δmain3d | Δvolume | Δao | Δsprites | Δcgame')
    fm=lambda x:'-' if x is None else f'{x:.2f}'
    dl=lambda k,m:'-' if m[k] is None or base[k] is None else f'{base[k]-m[k]:+.2f}'
    print(f"(base) | {len(runs[(s,'base')]['wall'])+len(runs[(s,'base2')]['wall'])} | 0 | {fm(base['wall'])} | {fm(base['p99'])} | {fm(base['max'])} | {fm(base['h33'])} | {fm(base['h50'])} | {fm(base['h100'])} | | | | | | |")
    for sv,a,m,n in rows:
        print(f"{a} | {n} | {sv:+.2f} | {fm(m['wall'])} | {fm(m['p99'])} | {fm(m['max'])} | {fm(m['h33'])} | {fm(m['h50'])} | {fm(m['h100'])} | "+' | '.join(dl(k,m) for k in ['sunshadow','prepass','main3d','volume','ao','sprites','cgame']))
print('\n### hitch time by part (base runs, ms summed per run over the window)')
for (s,kind),d in sorted(hitchparts.items()):
    print(s,kind,', '.join(f'{k} {v:.0f}' for k,v in sorted(d.items(),key=lambda x:-x[1])[:8]))
