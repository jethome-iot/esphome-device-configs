import rd_recursive_descent as rd, json, time, collections
d = rd.Decoder()
t0 = time.time()
out = {}
dups = collections.Counter()
for a, n in sorted(d.heads.items()):
    dups[n] += 1
# walk by address so duplicate names are kept apart
res = {}
for a, n in sorted(d.heads.items()):
    end = d.fn_end[a]
    r = None
    for _ in range(16):
        r = d._once(a, end)
        if not r['orphans']:
            break
        grew = False
        for o in sorted(r['orphans']):
            if d.resync(o, end):
                grew = True
        if not grew:
            break
    res[f'{a:08x}'] = dict(name=n, frame=r['frame'], entry=r['entry'], extra=r['extra'],
                           dyn=r['dyn'], calls=r['calls'], tails=r['tails'],
                           indirect=r['indirect'], orphans=sorted(hex(x) for x in r['orphans']),
                           bogus=len(r['bogus']), insns=r['n'])
json.dump(res, open('rdgraph.json', 'w'))
print('functions', len(res), 'time %.1fs' % (time.time() - t0))
print('dup names', sum(1 for k, v in dups.items() if v > 1))
print('with orphans', sum(1 for v in res.values() if v['orphans']))
print('with bogus', sum(1 for v in res.values() if v['bogus']))
print('dyn frames', [ (k, v['name']) for k, v in res.items() if v['dyn']][:30])
