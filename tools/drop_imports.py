"""Removes UnleashedRecomp import implementations that the the title runtime replaces.
Usage: drop_imports.py <file.cpp> Name [Name ...]
Deletes the top-level function `... Name(...) { ... }` and its GUEST_FUNCTION_HOOK/STUB(__imp__Name...) line."""
import re, sys
path = sys.argv[1]
src = open(path, encoding="utf-8").read()
for name in sys.argv[2:]:
    src, n_hook = re.subn(r"^GUEST_FUNCTION_(?:HOOK|STUB)\(__imp__%s\b[^\n]*\n" % re.escape(name), "", src, flags=re.M)
    # top-level definition: starts at column 0, body ends with a closing brace at column 0
    m = re.search(r"^[A-Za-z_][^\n;{}]*\b%s\s*\([^;{]*?\)\s*\n\{.*?\n\}\n\n?" % re.escape(name), src, flags=re.M | re.S)
    n_def = 0
    if m:
        src = src[:m.start()] + src[m.end():]
        n_def = 1
    print(f"{name}: hook {n_hook}, definition {n_def}")
open(path, "w", encoding="utf-8").write(src)
