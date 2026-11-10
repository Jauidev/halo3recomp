"""Merges `{ address, size }` entries into the `functions = [...]` block of a XenonRecomp TOML.
Usage: merge_functions.py <config.toml> <entries.toml>   (rewrites config.toml; same address keeps the larger size)"""
import re, sys
cfg = open(sys.argv[1]).read()
pat = r"address = 0x([0-9A-F]+), size = 0x([0-9A-F]+)"
m = re.search(r"functions = \[\n(.*?)\n\]", cfg, re.S)
cur = {int(a, 16): int(s, 16) for a, s in re.findall(pat, m.group(1))}
new = {int(a, 16): int(s, 16) for a, s in re.findall(pat, open(sys.argv[2]).read())}
for a, s in new.items():
    cur[a] = max(cur.get(a, 0), s)
block = "functions = [\n" + "\n".join("    { address = 0x%08X, size = 0x%X }," % (a, cur[a]) for a in sorted(cur)) + "\n]"
open(sys.argv[1], "w").write(cfg[:m.start()] + block + cfg[m.end():])
print("functions:", len(cur), "added/updated:", len(new))
