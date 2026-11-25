#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Beacon: find which newer HaikuPorts packages (pool dir) are needed on top of
# an image's package set (base dir) to satisfy a list of requirements.
# Usage: resolve-deps.py <base-dir> <pool-dir> req...   (PKGTOOL env = package tool)
import os, re, subprocess, sys
pkgtool = os.environ['PKGTOOL']
def info(path):
    out = subprocess.run([pkgtool, 'list', '-i', path], capture_output=True, text=True).stdout
    name = re.search(r'^\tname: (\S+)', out, re.M).group(1)
    prov = re.findall(r'^\tprovides: (\S+)(?: = (\S+))?', out, re.M)
    req = re.findall(r'^\trequires: ([^\s>=<]+)(?:\s*>=\s*(\S+))?', out, re.M)
    return name, prov, req
def ver(v):
    return [int(x) for x in re.findall(r'\d+', (v or '0').split('~')[0].split('_')[0])] or [0]
def load(d):
    db = {}
    for f in sorted(os.listdir(d)):
        if f.endswith('.hpkg') and '_devel' not in f and '_debuginfo' not in f and '_source' not in f:
            n, p, r = info(os.path.join(d, f)); db[f] = (n, p, r)
    return db
base, pool = load(sys.argv[1]), load(sys.argv[2])
def provides(db, cap):
    return [(f, v) for f, (n, p, r) in db.items() for c, v in p if c == cap]
need = [(c, v) for c, v in (a.partition('>=')[::2] for a in sys.argv[3:])]
chosen, seen = {}, set()
while need:
    cap, minv = need.pop()
    if (cap, minv) in seen or cap == 'haiku' or cap.startswith('haiku'):
        continue
    seen.add((cap, minv))
    ok = lambda lst: [f for f, v in lst if ver(v) >= ver(minv)]
    if ok(provides({k: pool[k] for k in chosen}, cap)) or ok(provides(base, cap)):
        continue
    cands = ok(provides(pool, cap))
    if not cands:
        print('UNSATISFIED', cap, '>=', minv, '| base has:', provides(base, cap)); continue
    f = cands[-1]; chosen[f] = pool[f]
    print('need', f, 'for', cap, '>=', minv, '(base has %s)' % ([v for _, v in provides(base, cap)] or 'none'))
    need += [(c, v) for c, v in pool[f][2]]
print('TOTAL', ' '.join(sorted(chosen)))
