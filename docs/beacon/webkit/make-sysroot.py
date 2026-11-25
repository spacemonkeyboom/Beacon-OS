#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Beacon: build a riscv64 sysroot for cross-compiling HaikuWebKit from the
# official HaikuPorts riscv64 packages. Every file is checked against the
# SHA-256 in the repository index before it is unpacked.
# Usage: make-sysroot.py <verbose-repo-listing.txt> <dl-dir> <sysroot> name...
import hashlib, os, subprocess, sys, urllib.request
listing, dl, root = sys.argv[1:4]
names = sys.argv[4:]
BASE = 'https://haikuports-repository.cdn.haiku-os.org/master/riscv64/current/packages/'
idx, cur = {}, None
for line in open(listing, encoding='utf-8', errors='replace'):
    t = line.strip()
    if line.startswith('\tname: '):
        cur = {'name': t[6:].strip()}
        idx.setdefault(cur['name'], cur)
    elif cur is not None and t.startswith('version: ') and 'version' not in cur:
        cur['version'] = t[9:].strip()
    elif cur is not None and t.startswith('checksum: ') and 'sum' not in cur:
        cur['sum'] = t[10:].strip()
os.makedirs(dl, exist_ok=True); os.makedirs(root, exist_ok=True)
pkgtool = os.environ['PKGTOOL']
for n in names:
    e = idx.get(n)
    if not e or 'version' not in e or 'sum' not in e:
        print('NOT IN INDEX:', n); continue
    f = '%s-%s-riscv64.hpkg' % (n, e['version'])
    path = os.path.join(dl, f)
    if not os.path.exists(path):
        urllib.request.urlretrieve(BASE + f, path)
    h = hashlib.sha256(open(path, 'rb').read()).hexdigest()
    if h != e['sum']:
        print('CHECKSUM MISMATCH, removed:', f); os.remove(path); continue
    subprocess.run([pkgtool, 'extract', '-C', root, path], check=True,
                   stdout=subprocess.DEVNULL)
    print('ok', f)
