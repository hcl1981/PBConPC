#!/usr/bin/env python3
"""Summarise a PBCONPC_HOSTPROF sample file: hostprof.py SAMPLES build/pbconpc"""
import collections, subprocess, sys
lines = open(sys.argv[1]).read().split()
base = int(lines[1], 16) if lines[0] == "base" else 0
addrs = [int(x, 16) for x in lines[2:]]
exe = sys.argv[2]
inside = collections.Counter()
outside = 0
for a in addrs:
    off = a - base
    if 0 <= off < 64 << 20:
        inside[off] += 1
    else:
        outside += 1
uniq = sorted(inside)
names = {}
for i in range(0, len(uniq), 2000):
    chunk = uniq[i:i + 2000]
    out = subprocess.run(["addr2line", "-f", "-e", exe] + [hex(x) for x in chunk],
                         capture_output=True, text=True).stdout.split("\n")
    for j, x in enumerate(chunk):
        names[x] = out[2 * j]
per = collections.Counter()
for off, n in inside.items():
    per[names.get(off, "?")] += n
total = len(addrs)
print(f"{total} samples; outside the executable (JIT code, libraries): {100*outside/total:.1f} %")
for name, n in per.most_common(30):
    print(f"{100*n/total:6.2f} %  {name}")
