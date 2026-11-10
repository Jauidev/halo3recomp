"""Makes <dst> an exact copy of <src>, touching only files whose content changed, so ninja rebuilds
just those (a fresh XenonRecomp output otherwise has new timestamps on all ~230 files).
Usage: sync_dir.py <src> <dst>"""
import filecmp, os, shutil, sys

src, dst = sys.argv[1], sys.argv[2]
os.makedirs(dst, exist_ok=True)
new = set(os.listdir(src))
if not new:
    sys.exit(f"{src} is empty: refusing to wipe {dst}")
changed = removed = 0
for name in sorted(new):
    s, d = os.path.join(src, name), os.path.join(dst, name)
    if not os.path.exists(d) or not filecmp.cmp(s, d, shallow=False):
        shutil.copyfile(s, d)
        changed += 1
for name in sorted(set(os.listdir(dst)) - new):
    os.remove(os.path.join(dst, name))
    removed += 1
print(f"{dst}: {changed} files changed, {removed} removed, {len(new)} total")
