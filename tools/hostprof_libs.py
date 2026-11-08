#!/usr/bin/env python3
"""Name the library samples of a PBCONPC_HOSTPROF file: hostprof_libs.py SAMPLES"""
import bisect, collections, subprocess, sys
path = sys.argv[1]
lines = open(path).read().split()
addrs = [int(x, 16) for x in lines[2:]]
maps = []
for l in open(path + ".maps"):
    f = l.split()
    if len(f) < 6 or 'x' not in f[1]:
        continue
    lo, hi = (int(x, 16) for x in f[0].split('-'))
    maps.append((lo, hi, int(f[2], 16), f[5]))
syms = {}
def symtab(lib):
    if lib not in syms:
        out = subprocess.run(["nm", "-D", "--defined-only", lib], capture_output=True, text=True).stdout
        t = sorted((int(a, 16), n) for a, _, n in (x.split()[:3] for x in out.splitlines() if len(x.split()) >= 3))
        syms[lib] = t
    return syms[lib]
count = collections.Counter()
for a in addrs:
    for lo, hi, off, lib in maps:
        if lo <= a < hi:
            if lib.endswith("pbconpc"):
                break
            t = symtab(lib)
            o = a - lo + off
            i = bisect.bisect_right([x[0] for x in t], o) - 1
            count[(lib.split('/')[-1], t[i][1] if i >= 0 else '?')] += 1
            break
tot = len(addrs)
for (lib, name), n in count.most_common(20):
    print(f"{100*n/tot:5.2f}% {lib} {name}")
