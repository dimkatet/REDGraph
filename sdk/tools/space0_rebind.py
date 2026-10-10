#!/usr/bin/env python3
"""Move a shader's resource bindings to other registers - for leases.

REDGraph leases FREE space0 registers of the engine's own descriptor tables (rg_lease_desc); a shader written against
any other register (another space, or one the vanilla shader already reads) must read the leased one instead. This
tool does the mechanical part: in `!dx.resources` the entry's
(space, lower bound) becomes (new space, new register), and every `@dx.op.createHandle(i32 57, i8 class, i32 id,
i32 index, ...)` of that entry gets index - old lower + new lower. Nothing else changes; the tool prints a diff count
and refuses:
  - a mapping that matches no entry, or more than one;
  - a target register some other entry of that class already covers in the target space;
  - a createHandle of a moved entry whose index is not a constant;
A LIBRARY (lib_6_x, the RT raygens; 2026-10-06): its handles are created from the resource GLOBALS
(createHandleForLib), with no register in the call, so only the declaration moves - the reassembled container's RDAT
carries the new (space, lower). The target must not overlap any range of that class already declared in the target
space (an unbounded range counts to infinity).

Usage:  python tools/space0_rebind.py <in.ll> <out.ll> <map> [<map> ...]
  map = <class><reg>/space<N>=<class><reg>[/space<M>]      e.g.  b0/space6=b3   u0/space6=u0   t0/space7=t26/space0
  class: t (SRV), u (UAV), b (CBV), s (sampler); the target space defaults to 0.
"""
import re
import sys

CLASS = {"t": 0, "u": 1, "b": 2, "s": 3}
CLASS_NAME = {v: k for k, v in CLASS.items()}


def parse_map(m):
    r = re.fullmatch(r"([tubs])(\d+)/space(\d+)=([tubs])(\d+)(?:/space(\d+))?", m)
    if not r:
        raise SystemExit(f"bad map '{m}' (want e.g. b0/space6=b3)")
    if r.group(1) != r.group(4):
        raise SystemExit(f"map '{m}' changes the register class")
    return CLASS[r.group(1)], int(r.group(3)), int(r.group(2)), int(r.group(6) or 0), int(r.group(5))


def main():
    if len(sys.argv) < 4:
        raise SystemExit(__doc__)
    src, dst, maps = sys.argv[1], sys.argv[2], [parse_map(m) for m in sys.argv[3:]]
    lines = open(src, encoding="utf-8").read().split("\n")
    is_lib = any(re.search(r'!\{!"lib"', l) for l in lines if l.startswith("!"))

    node = {}  # "!N" -> line index
    for i, l in enumerate(lines):
        m = re.match(r"^(!\d+) = ", l)
        if m:
            node[m.group(1)] = i
    top = next((l for l in lines if l.startswith("!dx.resources = ")), None)
    if not top:
        raise SystemExit("no !dx.resources")
    lists = re.findall(r"!\d+", top.split("=", 1)[1])
    res_node = re.match(r"^!\d+ = !\{(.*)\}$", lines[node[lists[0]]]).group(1)
    class_lists = [x.strip() for x in res_node.split(",")]  # SRV, UAV, CBV, sampler; "null" = none

    # entries[class] = [(node, line idx, id, space, lower)]
    entries = {c: [] for c in range(4)}
    entry_re = re.compile(r"^(!\d+) = !\{i32 (\d+), .+?\* (?:undef|@(?:\"[^\"]*\"|[\w.$]+)), !\"[^\"]*\", i32 (\d+), i32 (\d+), "
                          r"i32 (-?\d+), ")
    for c, ln in enumerate(class_lists):
        if ln == "null":
            continue
        members = re.findall(r"!\d+", lines[node[ln]].split("=", 1)[1])
        for mnode in members:
            i = node[mnode]
            m = entry_re.match(lines[i])
            if not m:
                raise SystemExit(f"cannot parse resource entry {lines[i]}")
            entries[c].append((mnode, i, int(m.group(2)), int(m.group(3)), int(m.group(4)), int(m.group(5))))

    moved = []  # (class, id, old lower, new lower)
    for c, sp, reg, nsp, nreg in maps:
        hit = [e for e in entries[c] if e[3] == sp and e[4] == reg]
        if len(hit) != 1:
            raise SystemExit(f"{CLASS_NAME[c]}{reg}/space{sp}: {len(hit)} matching entries (want 1)")
        def covers(e, r):  # size -1 / 0 / 0xFFFFFFFF = unbounded
            n = e[5] if e[5] > 0 and e[5] != 0xFFFFFFFF else 1 << 40
            return e[4] <= r < e[4] + n
        clash = [e for e in entries[c] if e[3] == nsp and covers(e, nreg) and e is not hit[0]]
        if clash:
            raise SystemExit(f"{CLASS_NAME[c]}{nreg}/space{nsp} is already bound by {clash[0][0]}")
        mnode, i, rid = hit[0][:3]
        new = re.sub(r"((?:undef|@(?:\"[^\"]*\"|[\w.$]+)), !\"[^\"]*\", )i32 \d+, i32 \d+, ", rf"\g<1>i32 {nsp}, i32 {nreg}, ",
                     lines[i], count=1)
        print(f"  {CLASS_NAME[c]}{reg}/space{sp} -> {CLASS_NAME[c]}{nreg}/space{nsp}  ({mnode}, id {rid})")
        lines[i] = new
        moved.append((c, rid, reg, nreg))

    ch_re = re.compile(r"@dx\.op\.createHandle\(i32 57, i8 (\d+), i32 (\d+), i32 ([^,]+), ")
    edits = {m[:2]: 0 for m in moved}
    for i, l in enumerate(lines):
        for m in list(ch_re.finditer(l)):
            key = (int(m.group(1)), int(m.group(2)))
            mv = next((x for x in moved if x[:2] == key), None)
            if not mv:
                continue
            idx = m.group(3).strip()
            if not re.fullmatch(r"\d+", idx):
                raise SystemExit(f"line {i + 1}: createHandle of a moved entry with a dynamic index '{idx}'")
            nidx = int(idx) - mv[2] + mv[3]
            l = l[: m.start(3)] + str(nidx) + l[m.end(3):]
            edits[key] += 1
        lines[i] = l
    for (c, rid), n in edits.items():
        if is_lib:
            if n:
                raise SystemExit(f"a library with createHandle on a moved entry (class {CLASS_NAME[c]} id {rid})")
            continue
        print(f"  class {CLASS_NAME[c]} id {rid}: {n} createHandle(s) re-indexed")
        if n == 0:
            print("    (declared, never created - only the declaration moved)")
    if is_lib:
        print("  library: declarations moved (handles come from the globals)")
    open(dst, "w", encoding="utf-8", newline="\n").write("\n".join(lines))
    print(f"wrote {dst}")


if __name__ == "__main__":
    main()
