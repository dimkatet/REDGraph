#!/usr/bin/env python3
"""The game's shaders, offline, from its own shader caches - names, search, and the vanilla bytecode by hash.

    python rg_shader_cache.py names   [--game DIR] [--out DIR]   # write static_names.csv + material_names.csv (~20 s)
    python rg_shader_cache.py find    TEXT ... [--out DIR]       # rows whose hash starts with / name contains TEXT
    python rg_shader_cache.py extract HASH ... [--game DIR] [--out DIR]  # HASH.dxil = the shipped DXBC container
                                                                 # (an RT library also prints its export names)

DIR for --game is the Cyberpunk 2077 install (the folder holding bin/ and engine/); default: $RG_GAME_DIR, then the
usual Steam / GOG paths. --out defaults to ./shader_names for names / find and to . for extract.

A HASH is the shader's DXIL `HASH` chunk digest (32 hex digits; a unique prefix is enough) - the same hash REDGraph
logs, its graph window shows, and rg_patch_desc.hash / rtlib_register / companion_register take. Disassemble an
extracted blob with `dxc -dumpbin HASH.dxil > HASH.ll` (DXC release from github.com/microsoft/DirectXShaderCompiler).

WHERE THE NAMES ARE. `engine/staticshader_final.cache` (engine shaders: compute, full-screen passes, RT libraries)
and `engine/shader_final.cache` (material shaders) hold the shipped bytecode as raw DXBC containers. Each blob is
preceded by an 8-byte KEY, and the cache's metadata references blobs by that key only - so a name is recovered by
following the key.

STATIC CACHE, layout of game 2.31:
  * metadata section first, binaries after. A metadata record lists its blob keys and is CLOSED by its name: keys
    belong to the NEXT name string, not the previous one.
  * strings are `0x80 | len` + ASCII (a name over 63 chars has a second length byte, flagged by 0x40). Names are the
    engine's permutation variables (`m_...`, plus NRD's `REBLUR_...`); render-target formats, vertex layouts etc. are
    skipped.
  * binary: `u64 key, u32 size, DXBC`. RT LIBRARIES are wrapped: `u64 key, u32 size, "CPOS" header + UTF-16 export
    names, DXBC` - the key sits before `CPOS`.
  * a blob can carry several names (identical code, e.g. `m_simpleCopy32_R/_RG/_RGBA`): all kept.

MATERIAL CACHE: binaries first, `u64 key, u64 ?, u32 size, DXBC`. Then technique records `"<material>
CompiledTechnique [Index: i, Pass 'renderstage_...', ..., RenderStageContext: [ID, VF: <vertex factory>; ...]]"`
followed by 4 bytes and the keys of its VS (+4) and PS (+12). One blob serves many techniques; the CSV keeps counts
of materials, passes, vertex factories.

Standard library only.
"""
import argparse
import bisect
import collections
import csv
import os
import pathlib
import re
import struct
import sys

STAGE = {0: "ps", 1: "vs", 2: "gs", 3: "hs", 4: "ds", 5: "cs", 6: "lib", 13: "as", 14: "ms"}
IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{2,}$")
NOT_A_NAME = re.compile(r"^(None|rtFormats|dsFormat|static:.*|GpuWrapApi.*|TEXFMT_.*|.*_Fullscreen|DebugVertex.*)$")
UTF16_NAME = re.compile(rb"(?:[A-Za-z0-9_]\x00){3,}")
GAME_GUESSES = [
    r"C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077",
    r"C:\Program Files\Steam\steamapps\common\Cyberpunk 2077",
    r"C:\GOG Games\Cyberpunk 2077",
    r"C:\Program Files (x86)\GOG Galaxy\Games\Cyberpunk 2077",
]


def engine_dir(game=None):
    """The game's engine/ folder (holding the two caches)."""
    for g in [game, os.environ.get("RG_GAME_DIR"), *GAME_GUESSES]:
        if not g:
            continue
        p = pathlib.Path(g)
        for e in (p / "engine", p.parent.parent / "engine"):  # the install, or its bin/x64
            if (e / "staticshader_final.cache").exists():
                return e
    raise SystemExit("no Cyberpunk 2077 install found - pass --game <install dir> or set RG_GAME_DIR")


def dxbc_blobs(d: bytes):
    """(offset, size, hash hex, stage, shader model) of every well-formed DXBC container."""
    for m in re.finditer(b"DXBC", d):
        o = m.start()
        if o + 32 > len(d):
            continue
        size, count = struct.unpack_from("<II", d, o + 24)
        if count > 32 or size < 32 or o + size > len(d):
            continue
        h, stage, sm = None, "?", "?"
        for i in range(count):
            co, = struct.unpack_from("<I", d, o + 32 + 4 * i)
            if co + 12 > size:
                continue
            cc = d[o + co:o + co + 4]
            if cc == b"HASH":
                h = d[o + co + 12:o + co + 28].hex()
            elif cc in (b"DXIL", b"SHEX", b"SHDR"):
                ver, = struct.unpack_from("<I", d, o + co + 8)
                stage, sm = STAGE.get(ver >> 16, str(ver >> 16)), f"{(ver >> 4) & 0xF}_{ver & 0xF}"
        if h:
            yield o, size, h, stage, sm


def rt_exports(d: bytes, o: int):
    """An RT library's export names from its CPOS wrapper (the blob at offset o), or []."""
    c = d.rfind(b"CPOS", max(0, o - 8192), o)
    if c < 0 or d.find(b"DXBC", c, o) >= 0:
        return []
    return [m.group(0).decode("utf-16-le") for m in UTF16_NAME.finditer(d, c, o) if (m.start() - c) % 2 == 0]


def static_table(engine):
    """(sorted rows (hash, stage, sm, name), number of named hashes, unnamed hashes) of the static cache."""
    d = (pathlib.Path(engine) / "staticshader_final.cache").read_bytes()
    blobs = list(dxbc_blobs(d))
    meta_end = min(o for o, *_ in blobs) - 12

    def key_of(o):
        c = d.rfind(b"CPOS", max(0, o - 8192), o)
        if c >= 0 and d.find(b"DXBC", c, o) < 0:  # RT library: the key precedes the CPOS wrapper
            return d[c - 12:c - 4]
        return d[o - 12:o - 4]

    by_key = collections.defaultdict(list)
    for o, size, h, stage, sm in blobs:
        by_key[key_of(o)].append((h, stage, sm))

    names = []
    for m in re.finditer(rb"(?:[\x81-\xbf]|[\xc0-\xff][\x01-\x7f])([ -~]+)", d[:meta_end]):
        b0 = d[m.start()]
        n = (b0 & 0x3F) | ((d[m.start() + 1] << 6) if b0 & 0x40 else 0)
        if n >= 2 and len(m.group(1)) >= n:
            s = m.group(1)[:n].decode()
            # IDENT: key bytes can read as a short string (`66 82 3a 38` = ":8"); names are identifiers.
            if IDENT.match(s) and not NOT_A_NAME.match(s):
                names.append((m.start(), s))
    starts = [o for o, _ in names]

    rows = set()
    for i in range(meta_end - 8):
        k = d[i:i + 8]
        if k in by_key:
            j = bisect.bisect(starts, i)  # the record's name CLOSES it
            name = names[j][1] if j < len(names) else "<none>"
            for h, stage, sm in by_key[k]:
                rows.add((h, stage, sm, name))
    named = {r[0] for r in rows}
    missing = sorted({h for _, _, h, _, _ in blobs} - named)
    return sorted(rows), len(named), missing


def material_table(engine):
    """{(hash, stage, sm): [techniques, materials, passes, vertex factories]} of the material cache, unnamed hashes."""
    d = (pathlib.Path(engine) / "shader_final.cache").read_bytes()
    blobs = list(dxbc_blobs(d))
    by_key = collections.defaultdict(list)
    for o, size, h, stage, sm in blobs:
        by_key[d[o - 20:o - 12]].append((h, stage, sm))
    tail_start = max(o + s for o, s, *_ in blobs)
    t = d[tail_start:]
    rx = re.compile(rb"([A-Za-z0-9_.\-]+) CompiledTechnique \[Index: \d+, Pass '([^']+)', PassIndex: \d+, Fallback: \d+, "
                    rb"RenderStageContext: \[ID: \d+, VF: ([^\];]*)[^\]]*\]\]?")
    per = collections.defaultdict(lambda: [0, collections.Counter(), collections.Counter(), collections.Counter()])
    for m in rx.finditer(t):
        for off in (4, 12):
            k = t[m.end() + off:m.end() + off + 8]
            for h, stage, sm in by_key.get(k, ()):
                r = per[(h, stage, sm)]
                r[0] += 1
                r[1][m.group(1).decode()] += 1
                r[2][m.group(2).decode()] += 1
                r[3][m.group(3).decode()] += 1
    missing = sorted({h for _, _, h, _, _ in blobs} - {k[0] for k in per})
    return per, missing


def top(c, n):
    return " ".join(f"{k}({v})" for k, v in c.most_common(n)) + (" ..." if len(c) > n else "")


def write_names(engine, out, extra_static=None):
    """Write static_names.csv + material_names.csv into out. extra_static(name) -> list of extra column values
    (with their headers in extra_static.headers) - for a caller holding more per-name data."""
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    rows, n_named, missing = static_table(engine)
    heads = getattr(extra_static, "headers", [])
    with open(out / "static_names.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["hash", "stage", "sm", "name", *heads])
        for h, stage, sm, name in rows:
            w.writerow([h, stage, sm, name, *(extra_static(name) if extra_static else [])])
    print(f"static:   {n_named} hashes named, {len(missing)} unnamed {missing[:4]}")
    per, missing = material_table(engine)
    with open(out / "material_names.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["hash", "stage", "sm", "techniques", "materials", "passes", "vertex_factories"])
        for (h, stage, sm), (n, mats, passes, vfs) in sorted(per.items()):
            w.writerow([h, stage, sm, n, top(mats, 6), top(passes, 4), top(vfs, 4)])
    print(f"material: {len({k[0] for k in per})} hashes named, {len(missing)} unnamed {missing[:4]}")
    print(f"-> {out}")
    return rows, per


def find(texts, out):
    """Print the CSV rows whose hash starts with, or whose text contains, one of texts (case-insensitive)."""
    texts = [t.lower() for t in texts]
    for fname in ("static_names.csv", "material_names.csv"):
        path = pathlib.Path(out) / fname
        if not path.exists():
            raise SystemExit(f"{path} is missing - run `names` first")
        with open(path, encoding="utf-8") as f:
            rows = csv.reader(f)
            next(rows, None)
            for row in rows:
                if any(row[0].startswith(t) or t in " ".join(row[1:]).lower() for t in texts):
                    print(f"{fname.split('_')[0]:8} " + " | ".join(x for x in row if x))


def extract(hashes, engine, out):
    """Write <hash>.dxil for every hash prefix (each must match one blob); print an RT library's exports."""
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    want = {h.lower(): [] for h in hashes}
    caches = {}
    for cache in ("staticshader_final.cache", "shader_final.cache"):
        d = (pathlib.Path(engine) / cache).read_bytes()
        caches[cache] = d
        for o, size, h, stage, sm in dxbc_blobs(d):
            for p in want:
                if h.startswith(p) and all(x[2] != h for x in want[p]):
                    want[p].append((cache, o, h, size, stage, sm))
    bad = False
    for p, hits in want.items():
        if len(hits) != 1:
            print(f"{p}: {'no blob' if not hits else f'{len(hits)} blobs - give more digits'}"
                  + "".join(f"\n    {x[2]}" for x in hits))
            bad = True
            continue
        cache, o, h, size, stage, sm = hits[0]
        d = caches[cache]
        path = out / f"{h}.dxil"
        path.write_bytes(d[o:o + size])
        line = f"{h}  {stage}_{sm}  {size} B  ({cache}) -> {path}"
        exports = rt_exports(d, o) if stage == "lib" else []
        if exports:
            line += "\n    exports: " + " ".join(exports)
        print(line)
    if bad:
        sys.exit(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("command", choices=["names", "find", "extract"])
    ap.add_argument("args", nargs="*")
    ap.add_argument("--game", help="the Cyberpunk 2077 install (default: $RG_GAME_DIR, then the usual paths)")
    ap.add_argument("--out", help="output folder")
    a = ap.parse_args()
    if a.command == "names":
        write_names(engine_dir(a.game), a.out or "shader_names")
    elif a.command == "find":
        if not a.args:
            ap.error("find needs a hash prefix or a text")
        find(a.args, a.out or "shader_names")
    else:
        if not a.args:
            ap.error("extract needs a hash (prefix)")
        extract(a.args, engine_dir(a.game), a.out or ".")


if __name__ == "__main__":
    main()
