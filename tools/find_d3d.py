"""Finds a game's D3D / XDK library functions (the XD3D_* names the hooks use) by their code.

The D3D library is linked into every game from the XDK, so with the same XDK the functions are the
same instructions at other addresses. A signature is a function's instructions with the address
fields masked: branch targets (b/bl), the high halves of addresses (lis) and the low halves added
to them. Data the hooks read (XD3D_ADDR_*) is found through the same instructions in the function
that uses it.

  find_d3d.py build <reference.pe> <xexinfo.txt> <xd3d_functions.h> <ppc_func_mapping.cpp> <db.json>
      signatures from a game whose table is known (games/gta4: GTA IV, XDK 2.0.6274)
  find_d3d.py find <game.pe> <xexinfo.txt> <db.json> <xd3d_functions.h out> [ppc_func_mapping.cpp [fixes.txt]]
      the table for another game; prints what was found, missing or ambiguous. fixes.txt
      (games/<slot>/xd3d_fixes.txt) holds what was checked by hand, over what the search says:
      "<name> <address>" or "<name> -" for a function, "DeviceOffset(<n>) <m>" or "DeviceOffset(<n>) -"
      for a device field ("-": this game has none), each with the reason after a "#".
      With the game's ppc_func_mapping.cpp, each hit must start a recompiled function, and a
      function no signature matches is looked for:
      - by its calls (match_by_calls(): another XDK's build of it sits at the same place in the call
        graph, between the imports and the functions found already), marked "by calls";
      - else by likeness (similar(): the recompiled function whose instructions look most like it),
        marked "alike" and in "similar" in the output.
      A function still missing whose callers were found was probably inlined into them (a game
      built with link-time code generation): "inlined" in the output.
      Next to the header goes xd3d_device.h: where this game's D3D device keeps each field the
      native renderer uses (DeviceOffset(n) in DEVICE_SOURCES, n where the reference keeps it),
      by lining up the instructions of the functions found (device_offsets()).

The .pe files are the loaded images xexinfo writes (address - 0x82000000 = file offset).
"""
import json, os, re, struct, sys
from difflib import SequenceMatcher

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

IMAGE_BASE = 0x82000000
MAX_WORDS = 96
LOADS_STORES = set(range(32, 56)) | {58, 62}
STORES = {36, 37, 38, 39, 44, 45, 47, 52, 53, 54, 55, 62}
# Words too common to anchor a search on.
COMMON = {0x7C0802A6, 0x4E800020, 0x7C0803A6, 0x60000000, 0x38210000, 0x4E800420, 0x7D8802A6}
# The search by likeness (similar()): how alike (difflib ratio of the instruction kinds), by how
# much more than the second best, and the shortest function it is tried on.
SIMILAR, MARGIN, MIN_SIMILAR_WORDS = 0.8, 0.05, 12
# The search by calls (match_by_calls()): the reference's functions up to GRAPH_DEPTH calls away
# from the named ones go into the database (callers only of functions with at most
# GRAPH_MAX_CALLERS), their code up to GRAPH_MAX_WORDS; the likeness compared covers the first
# KINDS_WORDS instructions. Pairs are taken the surest first: each threshold until nothing more
# is found, then the next.
GRAPH_DEPTH, GRAPH_MAX_CALLERS, GRAPH_MAX_WORDS, KINDS_WORDS = 2, 16, 1500, 400
CALL_THRESHOLDS = (0.9, 0.8, 0.7, 0.6)
# The device's fields (device_offsets()): the code that uses DeviceOffset(n), and how an
# instruction pairs with the game's: the same opcode and destination register, at most
# PAIR_WINDOW from where the alignment puts it (the compiler may swap two loads). Offsets under
# SMALL_OFFSET, and addi, count only off the register holding the device (other structures are
# that small too), none off a lis (globals); a field no instruction shows takes the shift of
# the fields around it, when those agree and one is at most NEIGHBOUR bytes away.
DEVICE_SOURCES = ("rexglue_glue/xd3d/d3d_hooks.cpp", "third_party/rexglue/src/graphics/xnative/graphics_system.cpp",
                  "runtime/gpu/command_processor.cpp")
PAIR_WINDOW, SMALL_OFFSET, NEIGHBOUR = 3, 1024, 256
D_FORM = set(range(32, 56)) | {14, 58, 62}       # loads, stores, addi, ld / std
GPR_LOADS = set(range(32, 36)) | set(range(40, 44)) | {46, 58}


def sections(info_text, kind="code"):
    return [(int(m[1], 16), int(m[2], 16)) for m in
            re.finditer(r"^section \S+\s+base=0x([0-9A-F]+) size=0x([0-9A-F]+) (\w+)", info_text, re.M) if m[3] == kind]


def libraries(info_text):
    libs = dict(re.findall(r"^(\w+)\s+(\d+\.\d+\.\d+\.\d+)$", info_text, re.M))
    # a game built with link-time code generation links D3D9LTCG (Gears of War) instead of D3D9
    if "D3D9" not in libs and "D3D9LTCG" in libs:
        libs["D3D9"] = libs["D3D9LTCG"]
    return libs


def words_at(pe, address, count):
    offset = address - IMAGE_BASE
    count = max(0, min(count, (len(pe) - offset) // 4))
    return list(struct.unpack_from(f">{count}I", pe, offset))


def signature(words):
    """(value, mask) per word, and the (index, reg) of each lis so data references can be decoded."""
    out, address_regs = [], set()
    for w in words:
        op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
        mask = 0xFFFFFFFF
        if op == 18:                      # b / bl: target
            mask = 0xFC000003
        elif op == 15:                    # lis / addis
            if ra == 0 or ra in address_regs:
                mask = 0xFFFF0000
                address_regs.add(rd)
            else:
                address_regs.discard(rd)
        elif op == 14:                    # addi
            if ra in address_regs or ra == 2:
                mask = 0xFFFF0000
                address_regs.add(rd)
            else:
                address_regs.discard(rd)
        elif op == 24 and rd in address_regs:   # ori rA, rS, lo
            mask = 0xFFFF0000
            address_regs.add(ra)
        elif op in LOADS_STORES:
            if ra in address_regs or ra == 2:
                mask = 0xFFFF0000 if op not in (58, 62) else 0xFFFF0003
            if op not in STORES:
                address_regs.discard(rd)
        out.append((w & mask, mask))
    return out


def data_references(words, base):
    """{address: [(hi index, lo index)]} for every lis rX + (addi / load / store) off rX."""
    refs, his = {}, {}
    for i, w in enumerate(words):
        op, rd, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 15 and ra == 0:
            his[rd] = (i, imm << 16)
            continue
        if (op == 14 or op in LOADS_STORES) and ra in his:
            hi_index, hi = his[ra]
            if op in (58, 62):
                simm &= ~3
            refs.setdefault((hi + simm) & 0xFFFFFFFF, []).append((hi_index, i))
        if op == 14 or (op in LOADS_STORES and op not in STORES):
            his.pop(rd, None)
    return refs


def decode_reference(words, hi_index, lo_index):
    hi, w = (words[hi_index] & 0xFFFF) << 16, words[lo_index]
    imm = w & 0xFFFF
    simm = imm - 0x10000 if imm & 0x8000 else imm
    if w >> 26 in (58, 62):
        simm &= ~3
    return (hi + simm) & 0xFFFFFFFF


def read_table(path):
    text = open(path, encoding="utf-8").read()
    functions = dict(re.findall(r"^#define XD3D_(\w+) sub_([0-9A-F]{8})$", text, re.M))
    data = dict(re.findall(r"^#define XD3D_ADDR_(\w+) 0x([0-9A-F]{8})$", text, re.M))
    return {k: int(v, 16) for k, v in functions.items()}, {k: int(v, 16) for k, v in data.items()}


def read_mapping(path):
    return sorted({int(a, 16) for a in re.findall(r"\{ 0x([0-9A-F]{8}), sub_", open(path, encoding="utf-8").read())})


def thunks(info_text):
    """{thunk address: name} of the imported functions."""
    return {int(t, 16): n for n, t in
            re.findall(r" func 0x[0-9A-F]+ (?:__imp__)?(\S+) var=0x[0-9A-F]+ thunk=0x([0-9A-F]+)", info_text)}


def import_variables(info_text):
    """{name: address} of the imported variables (VdGlobalDevice...)."""
    return {n: int(a, 16) for n, a in re.findall(r" var\s+0x[0-9A-F]+ (?:__imp__)?(\S+) var=0x([0-9A-F]+)", info_text)}


def function_sizes(starts):
    """{start: words} up to the next start."""
    return {s: ((starts[k + 1] if k + 1 < len(starts) else s + 4 * MAX_WORDS) - s) // 4 for k, s in enumerate(starts)}


def calls_of(words, start):
    """[(target, link)]: a function's bl, and its b out of it (tail calls)."""
    end, out = start + 4 * len(words), []
    for i, w in enumerate(words):
        if w >> 26 != 18:
            continue
        offset = w & 0x03FFFFFC
        if offset & 0x02000000:
            offset -= 0x04000000
        target = (offset if w & 2 else start + 4 * i + offset) & 0xFFFFFFFF
        if w & 1 or not start <= target < end:
            out.append((target, bool(w & 1)))
    return out


class Graph:
    """Functions ({start: words}, read on demand), their calls and callers, and the import thunks."""

    def __init__(self, words_of, sizes, thunks):
        self.words_of, self.thunks = words_of, thunks
        self.calls = {s: calls_of(words_of(s), s) for s in sizes}
        self.callers = {}
        for s, calls in self.calls.items():
            for target, _ in calls:
                self.callers.setdefault(target, set()).add(s)
        self._kinds = {}

    def kinds(self, start):
        if start not in self._kinds:
            self._kinds[start] = kinds(self.words_of(start)[:KINDS_WORDS])
        return self._kinds[start]


def match_by_calls(ref, game, seeds):
    """{reference start: game start}, grown from the seeds and the imports (by name): the functions
    at the same place in the calls of a matched pair, and the callers of a matched function, while
    their instructions are alike enough."""
    match = dict(seeds)
    by_name = {n: a for a, n in game.thunks.items()}
    match.update({a: by_name[n] for a, n in ref.thunks.items() if n in by_name})
    alike = {}

    def likeness(a, b):
        if (a, b) not in alike:
            alike[a, b] = SequenceMatcher(None, ref.kinds(a), game.kinds(b), autojunk=False).ratio()
        return alike[a, b]

    for threshold in CALL_THRESHOLDS:
        while grow(ref, game, match, threshold, likeness):
            pass
    return match


def grow(ref, game, match, threshold, likeness):
    """One round of match_by_calls(); how many pairs it added."""
    inverse = {b: a for a, b in match.items()}
    votes = {}

    def vote(a, b, weight):
        votes.setdefault(a, {})
        votes[a][b] = votes[a].get(b, 0) + weight

    def tokens(calls, known):
        return [("M", known[t]) if t in known else ("?", link) for t, link in calls]

    for a, b in list(match.items()):
        if a not in ref.calls or b not in game.calls:
            continue
        ca, cb = ref.calls[a], game.calls[b]
        ta, tb = tokens(ca, {t: t for t in match}), tokens(cb, inverse)
        # The same unknown call at the same place on both sides.
        for tag, i1, i2, j1, j2 in SequenceMatcher(None, ta, tb, autojunk=False).get_opcodes():
            if tag == "equal" or (tag == "replace" and i2 - i1 == j2 - j1):
                for i, j in zip(range(i1, i2), range(j1, j2)):
                    x, y = ca[i][0], cb[j][0]
                    if ta[i][0] == "?" and ta[i] == tb[j] and x in ref.calls and y in game.calls and y not in inverse:
                        vote(x, y, 2 if tag == "equal" else 1)
        # The callers: each reference one to the game one most like it, if clearly.
        xs = [c for c in ref.callers.get(a, ()) if c not in match]
        ys = [c for c in game.callers.get(b, ()) if c not in inverse]
        if not xs or not ys or len(xs) > 12 or len(ys) > 12:
            continue
        for x in xs:
            tx = tokens(ref.calls[x], {t: t for t in match})
            scored = sorted(((0.4 * SequenceMatcher(None, tx, tokens(game.calls[y], inverse), autojunk=False).ratio()
                              + 0.6 * likeness(x, y), y) for y in ys), reverse=True)
            if scored[0][0] >= 0.6 and (len(scored) == 1 or scored[0][0] - scored[1][0] >= 0.08):
                vote(x, scored[0][1], 1 if len(xs) == len(ys) == 1 else 0.6)
    # The votes say where to look, the instructions decide.
    claimed = {}
    for x, candidates in votes.items():
        ranked = sorted(((v * (0.25 + likeness(x, y)), y) for y, v in candidates.items()), reverse=True)
        score, y = ranked[0]
        if (len(ranked) > 1 and ranked[1][0] * 1.5 > score) or likeness(x, y) < threshold:
            continue
        claimed.setdefault(y, []).append((score, x))
    added = 0
    for y, wanted in claimed.items():
        wanted.sort(reverse=True)
        if len(wanted) == 1 or wanted[1][0] < wanted[0][0]:
            match[wanted[0][1]] = y
            added += 1
    return added


def aligned(ref_words, game_words):
    """{reference index: game index} of the instructions that line up (runs of the same kinds)."""
    matcher = SequenceMatcher(None, kinds(ref_words), kinds(game_words), autojunk=False)
    return {block.a + k: block.b + k for block in matcher.get_matching_blocks() for k in range(block.size)}


def displacement(w):
    d = w & 0xFFFF
    if w >> 26 in (58, 62):
        d &= ~3
    return d - 0x10000 if d & 0x8000 else d


def written(w):
    """The general registers an instruction writes (near enough to follow one value)."""
    op, rd, ra = w >> 26, (w >> 21) & 31, (w >> 16) & 31
    if op in (7, 8, 12, 13, 14, 15) or op in GPR_LOADS:
        return {rd}
    if op in (20, 21, 23, 24, 25, 26, 27, 28, 29, 30):
        return {ra}
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo in (24, 26, 27, 28, 58, 60, 124, 284, 316, 412, 444, 476, 536, 539, 792, 794, 824, 826, 827, 922, 954, 986):
            return {ra}           # logical, shifts, counts, sign extensions
        if xo in (0, 32, 144, 146, 178, 467, 54, 86, 149, 150, 151, 181, 183, 214, 215, 246, 247, 278, 407,
                  439, 598, 662, 663, 695, 727, 759, 854, 918, 982, 983, 1014):
            return set()          # compares, moves to special registers, stores, cache, sync
        return {rd}
    return set()


MFLR_R12 = 0x7D8802A6


def holding(words, regs, source=lambda w: None):
    """Per instruction, the registers holding a value before it: regs at the start, the register
    source(w) says an instruction sets, their copies (mr), until written or lost in a call (not
    the prologue's, bl after mflr r12: it only saves registers)."""
    out, last = [], 0
    for w in words:
        out.append(regs)
        op, rs, ra, rb = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        if source(w) is not None:
            regs = regs | {source(w)}
        elif op == 31 and (w >> 1) & 0x3FF == 444 and rs == rb:    # mr ra, rs
            regs = regs | {ra} if rs in regs else regs - {ra}
        elif (op == 18 or op == 19) and w & 1:                     # a call: r0, r3-r12 lost
            if last != MFLR_R12:
                regs = regs - {0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}
        else:
            regs = regs - written(w)
        last = w
    return out


def device_registers(words):
    """Per instruction, the registers holding the device before it: r3 at the start (a D3D function
    takes it first)."""
    return holding(words, {3})


def global_registers(words):
    """Per instruction, the registers holding a lis before it (an address in the image: what is
    read off them is a global, not a field)."""
    return holding(words, set(), lambda w: (w >> 21) & 31 if w >> 26 == 15 and (w >> 16) & 31 == 0 else None)


def paired(ref_words, game_words):
    """(reference index, game index) of the D-form instructions that line up: the same opcode and
    destination register within PAIR_WINDOW of the aligned place."""
    for i, j in aligned(ref_words, game_words).items():
        x = ref_words[i]
        if x >> 26 not in D_FORM:
            continue
        for k in sorted(range(max(0, j - PAIR_WINDOW), min(len(game_words), j + PAIR_WINDOW + 1)), key=lambda k: abs(k - j)):
            y = game_words[k]
            if y >> 26 == x >> 26 and (y >> 21) & 31 == (x >> 21) & 31:
                yield i, k
                break


def copy_length(words, i):
    """The length of the copy the address addi words[i] makes is passed to: li r5, n before the
    next call (memcpy(r3, r4, r5))."""
    if (words[i] >> 21) & 31 not in (3, 4):
        return None
    for w in words[i + 1:i + 8]:
        if w >> 26 == 14 and (w >> 21) & 31 == 5 and (w >> 16) & 31 == 0:
            return displacement(w)
        if w >> 26 == 18 and w & 1:
            return None
    return None


def device_votes(ref, game, match):
    """What the matched functions' displacements ("disp") and addi immediates ("addi") became in the
    game: {kind: {reference value: {game value: [reference functions]}}}; "block": the addi that
    are copied from or to, {reference value: {(game value, bytes both copy): [functions]}}."""
    votes = {"disp": {}, "addi": {}, "block": {}}
    for a, b in match.items():
        if a not in ref.calls or b not in game.calls:
            continue
        rw, gw = ref.words_of(a), game.words_of(b)
        rr, gr = device_registers(rw), device_registers(gw)
        rg, gg = global_registers(rw), global_registers(gw)
        for i, j in paired(rw, gw):
            x, y = rw[i], gw[j]
            bx, by = (x >> 16) & 31, (y >> 16) & 31
            dx, dy = displacement(x), displacement(y)
            if bx in (0, 1, 13) or by in (0, 1, 13):     # an immediate, the stack, the thread block
                continue
            if bx in rg[i] or by in gg[j]:
                continue
            kind = "addi" if x >> 26 == 14 else "disp"
            # An addi off anything else is arithmetic.
            if (abs(dx) < SMALL_OFFSET or kind == "addi") and (bx not in rr[i] or by not in gr[j]):
                continue
            votes[kind].setdefault(dx, {}).setdefault(dy, []).append(a)
            lengths = copy_length(rw, i), copy_length(gw, j)
            if kind == "addi" and None not in lengths:
                votes["block"].setdefault(dx, {}).setdefault((dy, min(lengths)), []).append(a)
    return votes


def clear(candidates):
    """The game value most instructions agree on, if at least twice the next: (value, count)."""
    ranked = sorted(((len(set(fs)), dy) for dy, fs in candidates.items()), reverse=True)
    if len(ranked) > 1 and ranked[1][0] * 2 > ranked[0][0]:
        return None
    return ranked[0][1], ranked[0][0]


def in_order(known):
    """The hits that keep the fields' order, the chain with the most votes: a field does not move
    past another between XDKs, an instruction lined up with the wrong one, or off another
    structure, does."""
    keys = sorted(known)
    best = []                 # per key: (votes of the best chain ending there, the key before)
    for i, n in enumerate(keys):
        m, votes = known[n][0], known[n][2]
        best.append(max([(best[j][0] + votes, j) for j in range(i) if known[keys[j]][0] < m], default=(votes, None)))
    chain, i = {}, max(range(len(keys)), key=lambda k: best[k][0], default=None)
    while i is not None:
        chain[keys[i]] = known[keys[i]]
        i = best[i][1]
    return chain


def device_offsets(votes, wanted):
    """{reference offset: (game offset, how)} for the wanted offsets, and those not found (with
    why, when they lined up out of order)."""
    known = {}
    for kind in ("disp", "addi"):
        for dx, candidates in votes[kind].items():
            hit = clear(candidates)
            if hit and dx >= 0 and hit[0] >= 0 and dx not in known:
                known[dx] = (hit[0], f"{hit[1]} instructions" if kind == "disp" else f"{hit[1]} addi", hit[1])
    chain = in_order(known)

    def between(n, m):
        """m keeps the order of the chain's fields around n."""
        below = max((k for k in chain if k < n), default=None)
        above = min((k for k in chain if k > n), default=None)
        return (below is None or chain[below][0] < m) and (above is None or m < chain[above][0])

    # A block copied whole (a structure): its fields move with its start, as far as both copy
    # and the fields known inside agree.
    blocks = {}
    for b, candidates in votes["block"].items():
        hit = clear(candidates)
        if hit and b in chain and chain[b][0] == hit[0][0] and \
                all(chain[k][0] - k == chain[b][0] - b for k in chain if b < k < b + hit[0][1]):
            blocks[b] = hit[0][1]
    table, missing = {}, []
    for n in wanted:
        if n in chain:
            table[n] = chain[n][:2]
            continue
        block = max((b for b, length in blocks.items() if b < n < b + length), default=None)
        if block is not None:
            table[n] = (chain[block][0] + n - block, f"in the {blocks[block]} bytes copied from {block}")
            continue
        # An index into a word or doubleword array (addi rX, rI, n / 4, then shifted).
        for scale in (4, 8):
            hit = clear(votes["addi"][n // scale]) if n % scale == 0 and n // scale in votes["addi"] else None
            if hit and between(n, hit[0] * scale):
                table[n] = (hit[0] * scale, f"index x{scale}, {hit[1]} addi")
                break
        else:
            # One of them may be further (an array no instruction indexes by a constant lies
            # between), when they agree.
            below = max((k for k in chain if k < n), default=None)
            above = min((k for k in chain if k > n), default=None)
            if below is not None and above is not None and min(n - below, above - n) > NEIGHBOUR:
                below = above = None
            shifts = {chain[k][0] - k for k in (below, above) if k is not None}
            # An instruction that lined up out of order paired with another field's.
            wrong = f" (lined up with {known[n][0]}, out of order)" if n in known else ""
            if below is not None and above is not None and len(shifts) == 1:
                table[n] = (n + shifts.pop(), f"guessed: {below} and {above} moved as much{wrong}")
            else:
                missing.append((n, wrong[2:-1]) if wrong else n)
    return table, missing


def device_size(game, reference_size, table):
    """The game's device size: the smallest li rX, n past the last field known (it allocates or
    clears the device) in the functions that store to that field; else the reference's room after
    it. (size, how)."""
    last = max((n for n in table if n < reference_size), default=None)
    if last is None:
        return reference_size, "the reference's"
    field, room = table[last][0], reference_size - last
    sizes_seen = set()
    for start in game.calls:
        words = game.words_of(start)
        if not any(w >> 26 in STORES and displacement(w) == field for w in words):
            continue
        sizes_seen |= {displacement(w) for w in words if w >> 26 == 14 and (w >> 16) & 31 == 0
                       and field < displacement(w) <= field + 2 * room + 128}
    if sizes_seen:
        return min(sizes_seen), f"cleared or allocated next to field {last}"
    return field + room, f"guessed: {room} bytes after field {last}, as in the reference"


def ibm_mask(mb, me):
    """Bits mb to me, numbered from the top as PowerPC does, as an integer."""
    return sum(1 << (63 - b) for b in range(mb, me + 1))


def flush_units(words, start):
    """What a draw function's prologue sends to the GPU of each dirty word (device + 0 to 32, a bit
    per thing changed): [(word, its bits from the top in the order they are sent, what gets
    them)], what being ("reg", the first register, li r5) or ("call", the function); bits None
    when the mask is read from the device. It tests each bit range (and / clrldi / rlwinm), then
    passes it, rotated (rldicr r4), to the function that writes the registers."""
    device, word_of, const = {3}, {}, {}
    units, test, r4, r5 = [], None, None, None
    for i, w in enumerate(words):
        op, rs, ra, rb = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        if op == 31 and (w >> 1) & 0x3FF == 444 and rs == rb:          # mr ra, rs
            device = device | {ra} if rs in device else device - {ra}
            if rs in word_of:
                word_of[ra] = word_of[rs]
                if ra == 4:
                    r4 = ("word", word_of[rs])
            continue
        if op == 58 and w & 3 == 0 and ra in device and displacement(w) in (0, 8, 16, 24, 32):
            word_of[rs] = displacement(w) // 8
            if rs == 4:
                r4 = ("word", word_of[rs])
            continue
        if op == 62 and ra in device and displacement(w) == 32:     # the last word cleared
            return units
        made = None                                    # the register this sets to a known value
        if op == 14 and ra == 0:
            made = rs
            const[rs] = displacement(w) & (2 ** 64 - 1)
            r5 = displacement(w) if rs == 5 else r5
        elif op == 15 and ra == 0:
            made = rs
            const[rs] = (displacement(w) << 16) & (2 ** 64 - 1)
            r4 = ("const",) if rs == 4 else r4
        elif op == 24 and rs in const:                                  # ori
            made = ra
            const[ra] = const[rs] | (w & 0xFFFF)
        elif op == 30 and (w >> 2) & 7 in (0, 1):                       # rldicl / rldicr
            sh = ((w >> 11) & 31) | ((w >> 1) & 1) << 5
            m = ((w >> 6) & 31) | ((w >> 5) & 1) << 5
            if (w >> 2) & 7 == 1 and rs in word_of and ra == 4:
                r4 = ("bits", word_of[rs], [(k + sh) % 64 for k in range(m + 1)])
            elif (w >> 2) & 7 == 0 and sh == 0 and rs in word_of:
                test = (word_of[rs], ibm_mask(m, 63))
            elif rs in const:
                v = ((const[rs] << sh) | (const[rs] >> (64 - sh))) & (2 ** 64 - 1) if sh else const[rs]
                made = ra
                const[ra] = v & (ibm_mask(0, m) if (w >> 2) & 7 == 1 else ibm_mask(m, 63))
        elif op == 21 and (w >> 11) & 31 == 0 and rs in word_of:        # rlwinm rA, rW, 0, mb, me
            test = (word_of[rs], ibm_mask(((w >> 6) & 31) + 32, ((w >> 1) & 31) + 32))
        elif op == 31 and (w >> 1) & 0x3FF == 28:                       # and
            if rs in word_of or rb in word_of:
                word = word_of.get(rs, word_of.get(rb))
                other = rb if rs in word_of else rs
                test = (word, const.get(other))
        elif op == 10 and (w >> 21) & 1 and ra in word_of:              # cmpldi rW, 0
            test = (word_of[ra], 2 ** 64 - 1)
        elif op == 18 and w & 1 and i and words[i - 1] == MFLR_R12:   # the prologue's register save
            continue
        elif op == 18 and w & 1:
            device = device - {0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}
            if test:
                word, mask = test
                if mask is None:
                    bits = None
                elif r4 and r4[0] == "bits" and r4[1] == word:
                    bits = r4[2]
                else:
                    bits = [b for b in range(64) if mask >> (63 - b) & 1]
                target = start + 4 * i + ((w & 0x03FFFFFC) ^ 0x02000000) - 0x02000000
                units.append((word, bits, ("reg", r5) if r5 is not None else ("call", target)))
            test, r4, r5 = None, None, None
            const = {}
        # What it writes holds neither a word nor a constant any more.
        for r in written(w):
            word_of.pop(r, None)
            if r != made:
                const.pop(r, None)
    return units


def dirty_runs(ref, game, match, ref_start, game_start):
    """Where the game keeps each of the reference's dirty bits: [(reference word, its lowest bit,
    game word, its lowest bit, how many)], from the draw function's prologue in both (the same
    registers, or the functions match_by_calls() paired, get the same bits in the same order);
    None when they do not line up."""
    theirs = {}
    for word, bits, what in flush_units(game.words_of(game_start), game_start):
        theirs.setdefault(what, []).append((word, bits))
    pairs = {}
    for word, bits, what in flush_units(ref.words_of(ref_start), ref_start):
        key = ("call", match.get(what[1])) if what[0] == "call" else what
        if bits is None or not theirs.get(key):
            if bits is not None:
                return None
            continue
        game_word, game_bits = theirs[key].pop(0)
        if game_bits is None or len(game_bits) != len(bits):
            return None
        for b, g in zip(bits, game_bits):
            if pairs.setdefault((word, 63 - b), (game_word, 63 - g)) != (game_word, 63 - g):
                return None
    if not pairs:
        return None
    runs = []
    for (word, bit), (game_word, game_bit) in sorted(pairs.items()):
        last = runs[-1] if runs else None
        if last and last[0] == word and last[2] == game_word and last[1] + last[4] == bit and last[3] + last[4] == game_bit:
            runs[-1] = (word, last[1], game_word, last[3], last[4] + 1)
        else:
            runs.append((word, bit, game_word, game_bit, 1))
    return runs


def pair_flush_calls(ref_units, game_units, match):
    """match[reference helper] = game helper for the calls two draw prologues make for the same
    group: the same dirty word and as many bits, in order (only those match lacks)."""
    taken = set()
    for word, bits, what in ref_units:
        if what[0] != "call" or what[1] in match:
            continue
        for k, (game_word, game_bits, game_what) in enumerate(game_units):
            if k in taken or game_what[0] != "call" or game_word != word or                     (bits is None) != (game_bits is None) or (bits is not None and len(bits) != len(game_bits)):
                continue
            taken.add(k)
            match[what[1]] = game_what[1]
            break


def wanted_offsets():
    """The n of every DeviceOffset(n) in DEVICE_SOURCES."""
    found = set()
    for path in DEVICE_SOURCES:
        with open(os.path.join(ROOT, path), encoding="utf-8", errors="replace") as f:
            found |= {int(n, 0) for n in re.findall(r"DeviceOffset\((0x[0-9A-Fa-f]+|\d+)\)", f.read())}
    return sorted(found)


def write_device_header(path, table, missing, size, how_size, same, dirty, absent=()):
    lines = ["#pragma once", "",
             "// Where this game's D3D device keeps the fields the native renderer uses, given where GTA IV's",
             "// (XDK 2.0.6274, which rexglue_glue/xd3d/d3d_hooks.cpp and the renderer follow) keeps them.",
             "// Generated by tools/find_d3d.py, which lines up the same D3D functions of both games: check",
             "// the report before trusting a line.",
             "#include <cstdint>", "", "namespace xd3d {", "",
             "// GTA IV's XDK: the hooks stand in for its D3D functions. Another one: they run the game's",
             "// own and tell the renderer what they did.",
             f"inline constexpr bool kReferenceXdk = {'true' if same else 'false'};", ""]
    if same and all(n == m for n, (m, _) in table.items()):
        lines += ["// The same XDK as the reference: the same device.",
                  f"inline constexpr uint32_t kDeviceSize = {size};", "",
                  "constexpr uint32_t DeviceOffset(uint32_t reference) { return reference; }"]
    else:
        lines += [f"inline constexpr uint32_t kDeviceSize = {size};  // {how_size}", "",
                  "constexpr uint32_t DeviceOffset(uint32_t reference) {", "  switch (reference) {"]
        lines += [f"    case {n}: return {m};  // {how}" for n, (m, how) in sorted(table.items())]
        lines += ["    default: return reference;", "  }", "}"]
    # The fields this game has none of (xd3d_fixes.txt): the code that uses one asks first.
    if absent:
        lines += ["", "constexpr bool HasDeviceOffset(uint32_t reference) {", "  switch (reference) {"]
        lines += [f"    case {n}:  // {why}" if why else f"    case {n}:" for n, why in sorted(absent)]
        lines += ["      return false;", "    default:", "      return true;", "  }", "}"]
    else:
        lines += ["", "constexpr bool HasDeviceOffset(uint32_t) { return true; }"]
    lines += ["", "// The dirty words (device + 0 to 32, a bit per thing changed since the last draw): count bits",
              "// of this game's word from bit (from the bottom) hold GTA IV's reference_word from reference_bit.",
              "struct DirtyRun {", "  uint32_t reference_word, reference_bit, word, bit, count;", "};",
              "inline constexpr DirtyRun kDirtyRuns[] = {"]
    lines += [f"    {{{w}, {b}, {gw}, {gb}, {n}}}," for w, b, gw, gb, n in dirty or [(0, 0, 0, 0, 0)]]
    lines += ["};"]
    if not dirty:
        missing = missing + ["kDirtyRuns"]
    for n in missing:
        n, why = n if isinstance(n, tuple) else (n, None)
        name = f"DeviceOffset({n})" if isinstance(n, int) else n
        lines.append(f"// {name}: not found" + (f" ({why})" if why else ""))
    lines += ["", "}  // namespace xd3d", ""]
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def build(pe_path, info_path, table_path, mapping_path, db_path):
    pe = open(pe_path, "rb").read()
    info = open(info_path, encoding="utf-8", errors="replace").read()
    functions, data = read_table(table_path)
    starts = read_mapping(mapping_path)
    import bisect
    db = {"xdk": libraries(info), "functions": {}, "data": {}}
    raw = {}
    for name, address in sorted(functions.items()):
        k = bisect.bisect_right(starts, address)
        end = starts[k] if k < len(starts) else address + 4 * MAX_WORDS
        length = (end - address) // 4
        words = words_at(pe, address, min(length, MAX_WORDS))
        raw[name] = words
        db["functions"][name] = {"address": f"{address:08X}", "length": length,
                                 "signature": [[v, m] for v, m in signature(words)]}
    variables = {a: n for n, a in import_variables(info).items()}
    for name, address in sorted(data.items()):
        if address in variables:      # an imported variable: by its name in the other game
            db["data"][name] = {"address": f"{address:08X}", "import": variables[address]}
            continue
        for function, words in raw.items():
            refs = data_references(words, functions[function]).get(address)
            if refs:
                db["data"][name] = {"address": f"{address:08X}", "function": function, "hi": refs[0][0], "lo": refs[0][1]}
                break
        else:
            db["data"][name] = {"address": f"{address:08X}", "function": None}
    # The call graph around the named functions, for match_by_calls().
    sizes = function_sizes(starts)
    graph = Graph(lambda s: words_at(pe, s, min(sizes[s], GRAPH_MAX_WORDS)), sizes, thunks(info))
    keep = set(functions.values()) & set(sizes)
    frontier = set(keep)
    for _ in range(GRAPH_DEPTH):
        reached = set()
        for a in frontier:
            reached |= {t for t, _ in graph.calls.get(a, ()) if t in sizes}
            callers = graph.callers.get(a, set())
            if len(callers) <= GRAPH_MAX_CALLERS:
                reached |= callers
        frontier = reached - keep
        keep |= frontier
    db["graph"] = {f"{s:08X}": "".join(f"{w:08X}" for w in graph.words_of(s)) for s in sorted(keep)}
    db["thunks"] = {f"{a:08X}": n for a, n in sorted(graph.thunks.items())}
    # The device's size: what CreateDevice allocates (li r3, size).
    db["device_size"] = next((displacement(w) for w in graph.words_of(functions["CreateDevice"])
                              if w >> 26 == 14 and (w >> 21) & 31 == 3 and (w >> 16) & 31 == 0
                              and displacement(w) > 4096), None) if "CreateDevice" in functions else None
    # Data no D3D function reads: the first other function that does and whose signature is unique
    # in the reference too, kept as an anchor.
    wanted = {data[n]: n for n, d in db["data"].items() if d.get("function") is None and not d.get("import")}
    for k, start in enumerate(starts):
        if not wanted:
            break
        end = starts[k + 1] if k + 1 < len(starts) else start + 4 * MAX_WORDS
        words = words_at(pe, start, min((end - start) // 4, MAX_WORDS))
        for address, refs in data_references(words, start).items():
            name = wanted.get(address)
            if not name:
                continue
            sig = [[v, m] for v, m in signature(words)]
            if search(pe, sections(info), sig) != [start]:
                continue
            anchor = f"anchor_{name}"
            db.setdefault("anchors", {})[anchor] = {"address": f"{start:08X}", "length": (end - start) // 4, "signature": sig}
            db["data"][name] = {"address": f"{address:08X}", "function": anchor, "hi": refs[0][0], "lo": refs[0][1]}
            del wanted[address]
    json.dump(db, open(db_path, "w"), indent=1)
    found = sum(1 for d in db["data"].values() if d.get("function") or d.get("import"))
    print(f"{len(db['functions'])} functions, {found}/{len(db['data'])} data addresses reachable from them, "
          f"{len(db['graph'])} in the call graph, XDK {db['xdk'].get('D3D9', '?')}")


def search(pe, code, sig, starts=None):
    """Every address in the code sections where the signature matches (at a function start, if given)."""
    anchor = next((j for j, (v, m) in enumerate(sig) if m == 0xFFFFFFFF and v not in COMMON and j >= 2), None)
    if anchor is None:
        anchor = next((j for j, (v, m) in enumerate(sig) if m == 0xFFFFFFFF), 0)
    needle = struct.pack(">I", sig[anchor][0])
    found = []
    for base, size in code:
        lo, hi = base - IMAGE_BASE, base - IMAGE_BASE + size
        at = pe.find(needle, lo, hi)
        while at >= 0:
            if at % 4 == 0:
                start = IMAGE_BASE + at - 4 * anchor
                words = words_at(pe, start, len(sig))
                if len(words) == len(sig) and all(w & m == v for w, (v, m) in zip(words, sig)):
                    if starts is None or start in starts:
                        found.append(start)
            at = pe.find(needle, at + 4, hi)
    return found


def kinds(words):
    """The instruction kinds of a function (opcode, extended opcode, b/bl), without registers,
    offsets or targets: what another XDK's build of the same function keeps."""
    out = []
    for w in words:
        op = w >> 26
        if op in (19, 31, 59, 63):
            out.append(op << 10 | (w >> 1) & 0x3FF)
        elif op == 30:
            out.append(op << 10 | (w >> 2) & 7)
        elif op in (16, 18, 58, 62):
            out.append(op << 10 | w & 3)
        else:
            out.append(op << 10)
    return out


def similar(pe, starts, db, names, taken):
    """For functions the signatures missed (another XDK: a register, an offset or a few instructions
    differ): the recompiled function whose instruction kinds are most alike, when it is alike
    enough (SIMILAR) and clearly more than the next one (MARGIN). {name: (address, score)}."""
    from difflib import SequenceMatcher
    starts = sorted(starts)
    candidates = []
    for k, start in enumerate(starts):
        length = ((starts[k + 1] if k + 1 < len(starts) else start + 4 * MAX_WORDS) - start) // 4
        candidates.append((start, length, kinds(words_at(pe, start, min(length, MAX_WORDS)))))
    found = {}
    for name in names:
        entry = db["functions"][name]
        reference = kinds([v for v, _ in entry["signature"]])
        if len(reference) < MIN_SIMILAR_WORDS:
            continue
        matcher = SequenceMatcher(None, autojunk=False)
        matcher.set_seq2(reference)
        scores = []
        for start, length, candidate in candidates:
            if start in taken or not 0.6 * entry["length"] <= length <= 1.6 * entry["length"]:
                continue
            matcher.set_seq1(candidate)
            if matcher.real_quick_ratio() < SIMILAR or matcher.quick_ratio() < SIMILAR:
                continue
            scores.append((matcher.ratio(), start))
        scores.sort(reverse=True)
        if scores and scores[0][0] >= SIMILAR and (len(scores) == 1 or scores[0][0] - scores[1][0] >= MARGIN):
            found[name] = (scores[0][1], round(scores[0][0], 3))
    return found


def search_by_calls(pe, info, starts, db, exact):
    """match_by_calls() between the database's reference graph and this game's, seeded with the
    exact hits: (reference graph, game graph, {reference start: game start})."""
    words = {int(a, 16): struct.unpack(f">{len(w) // 8}I", bytes.fromhex(w)) for a, w in db["graph"].items()}
    ref = Graph(words.__getitem__, words, {int(a, 16): n for a, n in db["thunks"].items()})
    sizes = function_sizes(sorted(starts))
    game = Graph(lambda s: words_at(pe, s, min(sizes[s], GRAPH_MAX_WORDS)), sizes, thunks(info))
    seeds = {int(db["functions"][n]["address"], 16): a for n, a in exact.items()}
    return ref, game, match_by_calls(ref, game, seeds)


def init_vertex_shader(pe, create, starts):
    """The function CreateVertexShader fills the object with: its last bl before it marks the object
    D3D-made (oris rX, rX, 0x10). Not in the database: an engine that allocates the shader itself
    (Unreal Engine 3) calls only that one."""
    following = [s for s in starts if s > create] if starts else []
    words = words_at(pe, create, (min(following) - create) // 4 if following else MAX_WORDS * 2)
    last = None
    for i, w in enumerate(words):
        if w >> 26 == 25 and w & 0xFFFF == 0x10 and (w >> 21) & 31 == (w >> 16) & 31:
            return last if last and (not starts or last in starts) else None
        last = next((t for t, link in calls_of([w], create + 4 * i) if link), last)
    return None


def read_fixes(path):
    """xd3d_fixes.txt: ({function: (address or None, why)}, {device field: (offset or None, why)})."""
    functions, fields = {}, {}
    for number, line in enumerate(open(path, encoding="utf-8") if path and os.path.exists(path) else (), 1):
        text, _, why = line.partition("#")
        if not text.strip():
            continue
        parts = text.split()
        field = re.fullmatch(r"DeviceOffset\((0x[0-9A-Fa-f]+|\d+)\)", parts[0])
        if len(parts) != 2 or not (field or re.fullmatch(r"[A-Za-z0-9_]+", parts[0])) or \
                not re.fullmatch(r"-|0x[0-9A-Fa-f]+|[0-9A-Fa-f]+", parts[1]):
            raise SystemExit(f"{path}:{number}: expected '<name> <address>', 'DeviceOffset(<n>) <offset>' or '-' for none")
        if field:
            fields[int(field[1], 0)] = (None if parts[1] == "-" else int(parts[1], 0), why.strip())
        else:
            functions[parts[0].removeprefix("XD3D_")] = (None if parts[1] == "-" else int(parts[1], 16), why.strip())
    return functions, fields


def find(pe_path, info_path, db_path, out_path, mapping_path=None, fixes_path=None):
    pe = open(pe_path, "rb").read()
    info = open(info_path, encoding="utf-8", errors="replace").read()
    db = json.load(open(db_path))
    code = sections(info)
    starts = set(read_mapping(mapping_path)) if mapping_path else None
    hits, missing, ambiguous = {}, [], {}
    anchors = {}
    for name, entry in db.get("anchors", {}).items():
        found = search(pe, code, entry["signature"], starts)
        if len(found) == 1:
            anchors[name] = found[0]
    for name, entry in sorted(db["functions"].items()):
        found = search(pe, code, entry["signature"], starts)
        if len(found) == 1:
            hits[name] = found[0]
        elif not found:
            missing.append(name)
        else:
            ambiguous[name] = found
    ref = game = None
    match, by_calls, inlined = {}, {}, {}
    if starts and db.get("graph"):
        ref, game, match = search_by_calls(pe, info, starts, db, hits)
        taken = set(hits.values())
        for name in missing + sorted(ambiguous):
            a = int(db["functions"][name]["address"], 16)
            b = match.get(a)
            if b and b not in taken:
                by_calls[name] = (b, round(SequenceMatcher(None, ref.kinds(a), game.kinds(b), autojunk=False).ratio(), 3))
                hits[name] = b
                taken.add(b)
                if name in missing:
                    missing.remove(name)
                else:
                    del ambiguous[name]
    likes = similar(pe, starts, db, missing, set(hits.values())) if starts and missing else {}
    for name, (address, _) in likes.items():
        hits[name] = address
        missing.remove(name)
    if ref:
        # Missing, but its callers are here: the compiler put it inside them.
        for name in missing:
            callers = ref.callers.get(int(db["functions"][name]["address"], 16), ())
            inside = sorted({match[c] for c in callers if c in match})
            if inside:
                inlined[name] = inside
    # What was checked by hand wins over the search.
    fixed_functions, fixed_fields = read_fixes(fixes_path)
    # "ADDR_<name> <address>": a data address checked by hand (Gears of War 3's NullClearColor is a
    # vector the reference's lis / lfs pair does not line up with).
    fixed_data = {n.removeprefix("ADDR_"): v for n, v in fixed_functions.items() if n.startswith("ADDR_")}
    fixed_functions = {n: v for n, v in fixed_functions.items() if not n.startswith("ADDR_")}
    # "#!votes-by-hand" in the fixes: the functions found by hand vote on the device fields too
    # (device_votes() only lines up the pairs search_by_calls() matched). Opt-in, so a game whose
    # fixes lack the line gets the table it always had.
    votes_by_hand = bool(fixes_path and os.path.exists(fixes_path) and
                         "#!votes-by-hand" in open(fixes_path, encoding="utf-8").read())
    by_hand, none = {}, {}
    for name, (address, why) in sorted(fixed_functions.items()):
        if name in missing:
            missing.remove(name)
        for found_as in (ambiguous, inlined, by_calls, likes, hits):
            found_as.pop(name, None)
        if address is None:
            none[name] = why
        else:
            hits[name], by_hand[name] = address, why
            if votes_by_hand and name in db["functions"]:
                match[int(db["functions"][name]["address"], 16)] = address
    derived = {}
    if "CreateVertexShader" in hits and "InitVertexShader" not in fixed_functions:
        target = init_vertex_shader(pe, hits["CreateVertexShader"], starts)
        if target:
            hits["InitVertexShader"] = derived["InitVertexShader"] = target
    data, data_missing = {}, []
    variables = import_variables(info)
    for name, entry in sorted(db["data"].items()):
        if entry.get("import"):
            if entry["import"] in variables:
                data[name] = variables[entry["import"]]
            else:
                data_missing.append(name)
            continue
        function = entry.get("function")
        located = {**hits, **anchors}
        hi, lo = entry.get("hi"), entry.get("lo")
        reference = db.get("graph", {}).get(db["functions"].get(function, {}).get("address", ""))
        if function in located and (function in by_calls or function in likes or function in by_hand) and reference and game:
            # Another build of the function: the pair is where its instructions line up.
            pairs = aligned(struct.unpack(f">{len(reference) // 8}I", bytes.fromhex(reference)), game.words_of(located[function]))
            hi, lo = pairs.get(hi), pairs.get(lo)
        words = words_at(pe, located[function], max(hi, lo) + 1) if function in located and hi is not None and lo is not None else []
        # A lis and an addi / load / store off its register, or nothing.
        if words and words[hi] >> 26 == 15 and (words[lo] >> 26 == 14 or words[lo] >> 26 in LOADS_STORES) \
                and (words[lo] >> 16) & 31 == (words[hi] >> 21) & 31:
            data[name] = decode_reference(words, hi, lo)
        else:
            data_missing.append(name)
    for name, (address, _) in fixed_data.items():
        if name in data_missing:
            data_missing.remove(name)
        if address is None:
            data.pop(name, None)
        else:
            data[name] = address
    xdk = libraries(info)
    same_xdk = xdk.get("D3D9") == db["xdk"].get("D3D9")
    wanted = wanted_offsets()
    size = db.get("device_size") or 0
    dirty = None
    if same_xdk:
        table, device_missing, how_size = {n: (n, "") for n in wanted}, [], ""
        dirty = [(w, 0, w, 0, 64) for w in range(5)]
    elif ref:
        if votes_by_hand:
            # The draw functions' prologues send the same groups in both: the helpers they call
            # for a group (vertex declaration, shader constants...) are the same functions.
            for n in ("DrawVertices", "DrawIndexedVertices"):
                if n in hits and n in db["functions"]:
                    ref_start = int(db["functions"][n]["address"], 16)
                    pair_flush_calls(flush_units(ref.words_of(ref_start), ref_start),
                                     flush_units(game.words_of(hits[n]), hits[n]), match)
        table, device_missing = device_offsets(device_votes(ref, game, match), wanted)
        size, how_size = device_size(game, size, table) if size else (0, "unknown")
        # Both draw functions flush the same way: they must agree.
        layouts = [dirty_runs(ref, game, match, int(db["functions"][n]["address"], 16), hits[n])
                   for n in ("DrawVertices", "DrawIndexedVertices") if n in hits and n in db["functions"]]
        dirty = layouts[0] if layouts and all(layout == layouts[0] for layout in layouts) else None
    else:
        table, device_missing, how_size = {}, wanted, "unknown"
    absent = []
    for n, (offset, why) in sorted(fixed_fields.items()):
        device_missing = [m for m in device_missing if (m[0] if isinstance(m, tuple) else m) != n]
        table.pop(n, None)
        if offset is None:
            absent.append((n, why))
        else:
            table[n] = (offset, "by hand" + (f": {why}" if why else ""))
    if not size:
        # A database built before device_size: the native renderer cannot size the device.
        size, how_size = 0x5780, "unknown (GTA IV's)"
        device_missing = device_missing + ["kDeviceSize"]
    write_device_header(os.path.join(os.path.dirname(os.path.abspath(out_path)), "xd3d_device.h"),
                        table, device_missing, size, how_size, same_xdk, dirty, absent)
    lines = ["#pragma once", "",
             "// This game's D3D and XDK library functions, by name (tools/find_d3d.py; the hooks and the",
             "// runtime use only these names). Generated: check the report before trusting a line.",
             '#include "ppc_recomp_shared.h"', "",
             "#define XD3D_PASTE2(a, b) a##b", "#define XD3D_PASTE(a, b) XD3D_PASTE2(a, b)",
             "#define XD3D_IMP(name) XD3D_PASTE(__imp__, XD3D_##name)", ""]
    lines += [f"#define XD3D_{n} sub_{a:08X}" + (f"  // alike, {likes[n][1]}: check it" if n in likes else
                                                 f"  // by calls, {by_calls[n][1]}: check it" if n in by_calls else
                                                 "  // called by CreateVertexShader" if n in derived else
                                                 "  // by hand" + (f": {by_hand[n]}" if by_hand[n] else "") if n in by_hand else "")
              for n, a in sorted(hits.items())]
    for n in missing + sorted(ambiguous):
        lines.append(f"// XD3D_{n}: " + ("several: " + ", ".join(f"sub_{a:08X}" for a in ambiguous[n]) if n in ambiguous else
                                         "inlined in " + ", ".join(f"sub_{a:08X}" for a in inlined[n]) if n in inlined else
                                         "not found"))
    # None in this game (xd3d_fixes.txt): its hook is left out, and CMake does not count it missing.
    lines += [f"// XD3D_{n}: none" + (f" ({why})" if why else "") for n, why in sorted(none.items())]
    lines += [""] + [f"#define XD3D_ADDR_{n} 0x{a:08X}" for n, a in sorted(data.items())]
    lines += [f"// XD3D_ADDR_{n}: not found" for n in data_missing]
    lines += [""] + [f"PPC_FUNC_IMPL(__imp__sub_{a:08X});" for _, a in sorted(hits.items())]
    open(out_path, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
    total = len(db["functions"]) + len(derived)
    result = {"xdk": xdk.get("D3D9", "?"), "reference_xdk": db["xdk"].get("D3D9", "?"), "same_xdk": same_xdk,
              "found": len(hits), "total": total, "missing": missing,
              "derived": {k: f"{a:08X}" for k, a in derived.items()},
              "by_calls": {k: {"address": f"{a:08X}", "score": s} for k, (a, s) in by_calls.items()},
              "similar": {k: {"address": f"{a:08X}", "score": s} for k, (a, s) in likes.items()},
              "inlined": {k: [f"{a:08X}" for a in v] for k, v in inlined.items()},
              "by_hand": {k: f"{hits[k]:08X}" for k in by_hand}, "none": sorted(none),
              "ambiguous": {k: [f"{a:08X}" for a in v] for k, v in ambiguous.items()},
              "data_found": len(data), "data_total": len(db["data"]), "data_missing": data_missing,
              "device": {"size": size, "offsets": len(wanted), "found": len(table), "missing": device_missing,
                         "absent": [n for n, _ in absent],
                         "dirty_runs": len(dirty) if dirty else None,
                         "guessed": sorted(n for n, (_, how) in table.items() if how.startswith("guessed"))}}
    print(json.dumps(result, indent=1))
    return result


if __name__ == "__main__":
    if len(sys.argv) == 7 and sys.argv[1] == "build":
        build(*sys.argv[2:])
    elif len(sys.argv) in (6, 7, 8) and sys.argv[1] == "find":
        find(*sys.argv[2:])
    else:
        raise SystemExit(__doc__)
