#!/usr/bin/env python3
"""Where the time inside the JIT's native code goes.

    PBCONPC_HOSTPROF=hp.txt build/pbconpc ...      (writes hp.txt, hp.txt.jit, hp.txt.jit.index)
    tools/jitprof.py hp.txt [BLOCKS] [--elf fw.elf]

Lists the translated blocks by samples (guest address, instructions, share of all samples) and
disassembles the hottest BLOCKS (default 5) with the samples on each x86 instruction, beside the
guest code from the ELF if given. Needs objdump (x86-64) and, for --elf, arm-none-eabi-objdump.
"""
import bisect, collections, struct, subprocess, sys

args = [a for a in sys.argv[1:] if not a.startswith("--")]
elf = None
if "--elf" in sys.argv:
    elf = sys.argv[sys.argv.index("--elf") + 1]
    args.remove(elf)
path = args[0]
nshow = int(args[1]) if len(args) > 1 else 5

words = open(path).read().split()
addrs = [int(x, 16) for x in words[2:]] if words[0] == "base" else [int(x, 16) for x in words]
total = len(addrs)

raw = open(path + ".jit", "rb").read()
base, length = struct.unpack("<QQ", raw[:16])
code = raw[16:16 + length]
index = []
for line in open(path + ".jit.index"):
    e, pc, n, careful = line.split()
    index.append((int(e, 16), int(pc, 16), int(n), careful == "1"))
index.sort()
starts = [e for e, *_ in index]

inside = collections.Counter(a for a in addrs if base <= a < base + length)
fixed = sum(c for a, c in inside.items() if not starts or a < starts[0])
per_block = collections.Counter()
for a, c in inside.items():
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0:
        per_block[i] += c
jit_total = sum(inside.values())
print(f"{total} samples; in JIT code {jit_total} ({100 * jit_total / total:.1f} %), "
      f"of which fixed stubs {fixed}")
print(" %all   samples  guest pc  ops  careful")
for i, c in per_block.most_common(25):
    e, pc, n, careful = index[i]
    print(f"{100 * c / total:5.2f} {c:9d}  {pc:08x} {n:4d}  {'yes' if careful else ''}")

def guest(pc, n):
    if not elf:
        return ""
    out = subprocess.run(["arm-none-eabi-objdump", "-d", f"--start-address={pc:#x}",
                          f"--stop-address={pc + 4 * n + 4:#x}", elf], capture_output=True, text=True).stdout
    lines = [l for l in out.splitlines() if l[:1] == " " and ":" in l]
    return "\n".join(lines[:n])

for i, c in per_block.most_common(nshow):
    e, pc, n, careful = index[i]
    end = index[i + 1][0] if i + 1 < len(index) else base + length
    print(f"\n=== block {pc:08x}, {n} ops, {100 * c / total:.2f} % of all samples, {end - e} bytes")
    g = guest(pc, n)
    if g:
        print(g)
        print("---")
    blob = code[e - base:end - base]
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".bin") as tf:
        tf.write(blob)
        tf.flush()
        out = subprocess.run(["objdump", "-D", "-b", "binary", "-mi386:x86-64", f"--adjust-vma={e:#x}", tf.name],
                             capture_output=True).stdout.decode()
    for l in out.splitlines():
        l2 = l.strip()
        if not l2 or ":" not in l2 or not l2[0] in "0123456789abcdef":
            continue
        try:
            a = int(l2.split(":")[0], 16)
        except ValueError:
            continue
        cnt = inside.get(a, 0)
        print(f"{cnt:6d} {l2}")
