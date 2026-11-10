"""The native renderer against Xenia's GPU: where its image starts to differ.

  compare_gpu.py <game exe> <game folder> <out dir> [seconds]   boots the game twice and compares
  compare_gpu.py --analyze <out dir> [native dir xenia dir]       compares two runs already made
                                                                  (default <out>/native, <out>/xenia)

Xenia's GPU emulates the Xenos (EDRAM, resolves, every render target format); the native renderer
infers all that from the D3D calls, which is what fails with an engine it was not tuned for. Both
runs (tools/boot_test.py, <out>/native and <out>/xenia) save frames as they go, at different speeds
(Xenia's GPU is slower), so the two series are aligned in order (dynamic time warping over small
grey thumbnails: logos, legal text, loading screens, the game) instead of by time. A native frame far
from its Xenia counterpart is drawn wrong; the first of them is where to look (its render targets,
resolves and shaders in the native log).

Writes <out>/compare.md (the worst pairs and the first divergence) and <out>/compare.html (the
aligned pairs side by side).
"""
import html, json, os, sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import boot_test  # noqa: E402

THUMB = (64, 36)
DIFFERENT = 0.12  # mean absolute difference of the thumbnails (0..1) above which a pair differs
# Two runs never capture the same instants, so only gross differences count: a pair also differs
# when the native image is flat (black, one colour: spread below FLAT grey levels) and Xenia's is
# not, for FLAT_RUN native frames in a row (one alone is a fade; Xenia's own black frames, its
# loading screens, say nothing about the native renderer).
FLAT, FLAT_RUN = 4.0, 3
# Added to the cost of matching several frames of one series to a single frame of the other, so
# that a long black stretch of the native run is not hidden behind one fade to black of Xenia's.
STEP_PENALTY = 0.02


def thumbnails(cap):
    names = sorted(n for n in os.listdir(cap) if n.endswith(".png")) if os.path.isdir(cap) else []
    result = []
    for name in names:
        try:
            with Image.open(os.path.join(cap, name)) as image:
                result.append((name, image.convert("L").resize(THUMB, Image.BILINEAR).tobytes()))
        except OSError:
            pass
    return result


def distance(a, b):
    return sum(abs(x - y) for x, y in zip(a, b)) / (255 * len(a))


def flat(thumb):
    mean = sum(thumb) / len(thumb)
    return (sum((v - mean) ** 2 for v in thumb) / len(thumb)) ** 0.5 < FLAT


def align(native, xenia):
    """Monotonic alignment (DTW) of the two series; for each native frame, its closest aligned Xenia
    frame and their distance."""
    n, m = len(native), len(xenia)
    cost = [[distance(native[i][1], xenia[j][1]) for j in range(m)] for i in range(n)]
    total = [[0.0] * m for _ in range(n)]
    for i in range(n):
        for j in range(m):
            best = 0.0 if i == 0 and j == 0 else min(
                total[i - 1][j] + STEP_PENALTY if i else float("inf"),
                total[i][j - 1] + STEP_PENALTY if j else float("inf"),
                total[i - 1][j - 1] if i and j else float("inf"))
            total[i][j] = cost[i][j] + best
    i, j, matched = n - 1, m - 1, {}
    while True:
        if i not in matched or cost[i][j] < cost[i][matched[i]]:
            matched[i] = j
        if i == 0 and j == 0:
            break
        steps = []
        if i and j:
            steps.append((total[i - 1][j - 1], i - 1, j - 1))
        if i:
            steps.append((total[i - 1][j] + STEP_PENALTY, i - 1, j))
        if j:
            steps.append((total[i][j - 1] + STEP_PENALTY, i, j - 1))
        _, i, j = min(steps)
    return [(i, matched[i], cost[i][matched[i]]) for i in range(n)]


def analyze(out, native_dir=None, xenia_dir=None):
    """native_dir, xenia_dir: the two boot_test.py folders (default <out>/native and <out>/xenia)."""
    native_dir, xenia_dir = native_dir or os.path.join(out, "native"), xenia_dir or os.path.join(out, "xenia")
    native, xenia = thumbnails(os.path.join(native_dir, "cap")), thumbnails(os.path.join(xenia_dir, "cap"))
    result = {"native_frames": len(native), "xenia_frames": len(xenia), "pairs": [], "first_divergence": None,
              "native_cap": os.path.relpath(os.path.join(native_dir, "cap"), out).replace(os.sep, "/"),
              "xenia_cap": os.path.relpath(os.path.join(xenia_dir, "cap"), out).replace(os.sep, "/")}
    if native and xenia:
        pairs = align(native, xenia)
        lone = [flat(native[i][1]) and not flat(xenia[j][1]) for i, j, _ in pairs]
        for k, (i, j, d) in enumerate(pairs):
            pair = {"native": native[i][0], "xenia": xenia[j][0], "distance": round(d, 3)}
            run = lone[k] and any(all(lone[s:s + FLAT_RUN]) for s in range(max(0, k - FLAT_RUN + 1), k + 1)
                                  if s + FLAT_RUN <= len(lone))
            if run:
                pair["flat"] = "native"
            result["pairs"].append(pair)
        different = [p for p in result["pairs"] if p["distance"] > DIFFERENT or p.get("flat")]
        result["different"] = len(different)
        result["first_divergence"] = different[0] if different else None
        result["worst"] = sorted(different, key=lambda p: (not p.get("flat"), -p["distance"]))[:10]
    with open(os.path.join(out, "compare.json"), "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1)
    with open(os.path.join(out, "compare.md"), "w", encoding="utf-8") as f:
        f.write(markdown(result))
    with open(os.path.join(out, "compare.html"), "w", encoding="utf-8") as f:
        f.write(page(result))
    return result


def markdown(r):
    lines = ["## Nativo frente a la GPU de Xenia", ""]
    if not r["native_frames"] or not r["xenia_frames"]:
        return "\n".join(lines + [f"Sin capturas para comparar (nativo {r['native_frames']}, Xenia {r['xenia_frames']}).", ""])
    lines.append(f"- Capturas: nativo {r['native_frames']}, Xenia {r['xenia_frames']}; "
                 f"{r['different']} de {len(r['pairs'])} nativas se alejan de su pareja de Xenia")
    first = r["first_divergence"]
    if first:
        lines.append(f"- Primera diferencia: `{r['native_cap']}/{first['native']}` frente a `{r['xenia_cap']}/{first['xenia']}` "
                     f"(distancia {first['distance']}): ahí empieza lo que el renderizador nativo dibuja mal")
        lines += ["- Las más distintas:", ""] + [
            f"  - `{p['native']}` / `{p['xenia']}`: {p['distance']}"
            + (" (la nativa es plana: negra o de un color)" if p.get("flat") else "")
            for p in r["worst"]]
    else:
        lines.append("- Ninguna se aleja: el renderizador nativo no dibuja nada muy distinto de la GPU emulada")
    lines += ["", "Solo cuenta lo grueso (negro donde Xenia dibuja, capas o colores muy distintos): las dos partidas "
              "no capturan los mismos instantes. Las parejas, lado a lado: `compare.html`.", ""]
    return "\n".join(lines)


def page(r):
    rows = []
    for p in r["pairs"]:
        mark = ' style="background:#fdd"' if p["distance"] > DIFFERENT or p.get("flat") else ""
        rows.append(f'<tr{mark}><td>{p["distance"]}</td>'
                    f'<td><img src="{r["native_cap"]}/{html.escape(p["native"])}" width="480"><br>{html.escape(p["native"])}</td>'
                    f'<td><img src="{r["xenia_cap"]}/{html.escape(p["xenia"])}" width="480"><br>{html.escape(p["xenia"])}</td></tr>')
    return ("<!DOCTYPE html><meta charset=\"utf-8\"><title>Nativo frente a Xenia</title>"
            "<table border=1 cellspacing=0><tr><th>distancia</th><th>nativo</th><th>Xenia</th></tr>"
            + "".join(rows) + "</table>\n")


def main():
    argv = sys.argv[1:]
    sys.stdout.reconfigure(encoding="utf-8")
    if len(argv) in (2, 4) and argv[0] == "--analyze":
        print(markdown(analyze(*argv[1:])))
        return
    if len(argv) not in (3, 4):
        raise SystemExit(__doc__)
    exe, game, out = argv[0], argv[1], argv[2]
    seconds = int(argv[3]) if len(argv) == 4 else 300
    for gpu in ("native", "xenia"):
        result = boot_test.boot(exe, game, os.path.join(out, gpu), seconds, gpu)
        print(f"{gpu}: {boot_test.verdict(result)}", flush=True)
    print(markdown(analyze(out)))


if __name__ == "__main__":
    main()
