"""Merges functions that branch into the middle of another one (XenonRecomp emits `// ERROR <addr>`).
Usage: fix_branches.py <ppc dir> <functions.toml>  -> prints entries to add (address, size) and stats
The source function and the function containing the target become one: the earlier one is
extended to the end of the later one. Function ends are taken as the next start in
ppc_func_mapping.cpp, which is how XenonAnalyse laid them out. When that merge is already in
the config, the source is a second entry inside it, and the target is declared as an entry.
"""
import bisect, glob, os, re, sys

ppc = sys.argv[1]
mapping = sorted(int(a, 16) for a in re.findall(r"\{ 0x([0-9A-F]+), sub_", open(os.path.join(ppc, "ppc_func_mapping.cpp")).read()))
explicit = {int(a, 16): int(s, 16) for a, s in re.findall(r"address = 0x([0-9A-F]+), size = 0x([0-9A-F]+)", open(sys.argv[2]).read())}
# Import thunks (__imp__*) are never function bodies: declaring one turns the kernel call into the
# thunk's unpatched "nop; nop; nop; blr" (this broke KeTlsSetValue and 15 other imports once).
imports = {int(a, 16) for a in re.findall(r"\{ 0x([0-9A-F]+), __imp__", open(os.path.join(ppc, "ppc_func_mapping.cpp")).read())}


def extent(addr):
    k = bisect.bisect_right(mapping, addr) - 1
    start = mapping[k]
    end = mapping[k + 1] if k + 1 < len(mapping) else start + 4
    if start in explicit:
        end = max(end, start + explicit[start])
    return start, end


errors = []
for path in glob.glob(os.path.join(ppc, "ppc_recomp.*.cpp")):
    fn = None
    for line in open(path):
        m = re.match(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)", line)
        if m:
            fn = int(m.group(1), 16)
            continue
        m = re.search(r"// ERROR ([0-9A-F]+)", line)
        if m and fn is not None:
            errors.append((fn, int(m.group(1), 16)))

add = {}
starts = 0
for src, target in errors:
    if target in imports:
        continue
    s0, s1 = extent(src)
    t0, t1 = extent(target)
    lo, hi = min(s0, t0), max(s1, t1)
    if hi - lo <= 0x1000 and explicit.get(lo, 0) < hi - lo:
        # neighbours: an outlined fragment and its owner become one function
        add[lo] = max(add.get(lo, 0), hi - lo)
    elif hi - lo <= 0x1000:
        # already merged, so the source is a second entry declared inside it and is still its own
        # function: make the target an entry too, and the branch becomes a tail call into it
        add[target] = max(add.get(target, 0), hi - target)
        starts += 1
    else:
        # far away: a tail call to a real entry point that XenonAnalyse swallowed into a
        # bigger function; declare the entry (its code stays duplicated in the container)
        add[target] = max(add.get(target, 0), t1 - target)
        starts += 1

for a in sorted(add):
    print("    { address = 0x%08X, size = 0x%X }," % (a, add[a]))
print("errors: %d, entries: %d (new entry points: %d)" % (len(errors), len(add), starts), file=sys.stderr)
