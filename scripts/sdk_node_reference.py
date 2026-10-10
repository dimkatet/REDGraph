"""The SDK's node reference (sdk/reference/): every render-graph node the census saw, the shaders it binds, the RT
libraries it dispatches, the render modes it ran in - the union of the `graph-*-shaders.txt` files that REDGraph's
`[Diagnostics] RenderGraphDump` writes (docs/internals.md "Census and graph dump"), named from the game's caches
(sdk/tools/rg_shader_cache.py).

    python scripts/sdk_node_reference.py --runs DIR [--game DIR] [--names DIR]

--runs: a folder searched recursively for census files (one sub-folder per game session is a good habit; the more
render modes, menus and places the sessions covered, the more complete the reference). --game: the Cyberpunk 2077
install (default $RG_GAME_DIR). --names: rg_shader_cache.py's CSVs (made in a temporary folder when not given).
Writes sdk/reference/node_shaders.csv + nodes.md.

Mode of a window = its header's "render mode by what ran" (raster / RT / PT); older runs have none and count as "?".
binds/frame = the most any window of >= 100 frames saw.
"""
import argparse
import collections
import csv
import os
import pathlib
import re
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "sdk" / "tools"))
import rg_shader_cache as sdk  # noqa: E402

OUT = ROOT / "sdk" / "reference"
MODE = re.compile(r"render mode by what ran: (raster|RT|PT)")
FRAMES = re.compile(r": (\d+) frames,")
LABEL = re.compile(r"^e(\d+) (.+?) / m(\d+) (\S+)( \[job\])?$")
ORDER = {"raster": 0, "RT": 1, "PT": 2, "?": 3}


def names(names_dir):
    out = {}
    with open(names_dir / "static_names.csv", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            out.setdefault(r["hash"], set()).add(r["name"])
    static = set(out)
    with open(names_dir / "material_names.csv", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            out.setdefault(r["hash"], set()).add(f"material {r['materials'].split(' ')[0]} {r['passes'].split(' ')[0]}")
    return {h: ", ".join(sorted(v)[:3]) + (" ..." if len(v) > 3 else "") for h, v in out.items()}, static


def rt_exports(game):
    d = (sdk.engine_dir(game) / "staticshader_final.cache").read_bytes()
    return {h: sdk.rt_exports(d, o) for o, _, h, stage, _ in sdk.dxbc_blobs(d) if stage == "lib"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", required=True, help="folder searched recursively for graph-*-shaders.txt")
    ap.add_argument("--game", default=os.environ.get("RG_GAME_DIR"))
    ap.add_argument("--names", help="rg_shader_cache.py names output (static_names.csv, material_names.csv)")
    a = ap.parse_args()
    tmp = None
    names_dir = pathlib.Path(a.names) if a.names else None
    if names_dir is None:
        tmp = tempfile.TemporaryDirectory()
        names_dir = pathlib.Path(tmp.name)
        cmd = [sys.executable, str(ROOT / "sdk" / "tools" / "rg_shader_cache.py"), "names", "--out", str(names_dir)]
        subprocess.run(cmd + (["--game", a.game] if a.game else []), check=True, capture_output=True)
    nm, static = names(names_dir)
    exports = rt_exports(a.game)
    # (group, node, job) -> (stage, hash) -> {"modes": set, "bpf": float}
    nodes = collections.defaultdict(lambda: collections.defaultdict(lambda: {"modes": set(), "bpf": 0.0}))
    node_modes = collections.defaultdict(set)
    # Frame order: every census graph orders its nodes by (e, m) - the graph's node index, the member index. Graphs
    # differ per mode (and menus are graphs of their own), so their indices are not comparable; each graph gives
    # "A before B" for its consecutive nodes, and all graphs are merged by a topological sort (ties, and the rare
    # cycle of one class seen at two places, by the mean relative position).
    succ = collections.defaultdict(set)
    rel = collections.defaultdict(list)
    size = collections.Counter()  # the largest graph a node is in: menus are graphs of ~10 nodes, gameplay ~100
    files = sorted(str(f) for f in pathlib.Path(a.runs).rglob("graph-*-shaders.txt"))
    for path in files:
        pos = {}
        for line in open(path, encoding="utf-8", errors="replace"):
            m = LABEL.match(line.split("\t")[0]) if not line.startswith("#") else None
            if m:
                k = (m.group(2), m.group(4), bool(m.group(5)))
                pos[k] = min(pos.get(k, (1 << 30, 0)), (int(m.group(1)), int(m.group(3))))
        seq = sorted(pos, key=pos.get)
        for k in seq:
            size[k] = max(size[k], len(seq))
        for i, k in enumerate(seq):
            rel[k].append(i / max(1, len(seq) - 1))
            if i:
                succ[seq[i - 1]].add(k)
    indeg = collections.Counter(b for a in succ for b in succ[a])
    mean = {k: sum(v) / len(v) for k, v in rel.items()}
    left, order = set(rel), {}
    while left:
        ready = [k for k in left if indeg[k] == 0] or list(left)  # a cycle: take the earliest by mean position
        k = min(ready, key=lambda x: (mean[x], x))
        order[k] = (size[k] < 30, len(order))  # nodes only menus have go last
        left.discard(k)
        for b in succ[k]:
            indeg[b] -= 1
    for path in files:
        text = open(path, encoding="utf-8", errors="replace").read().split("\n")
        head = "\n".join(l for l in text if l.startswith("#"))
        mode = (MODE.search(head) or [None, "?"])[1]
        frames = int((FRAMES.search(head) or [None, "0"])[1])
        for line in text:
            if not line or line.startswith("#"):
                continue
            label, shaders, binds, _ = line.split("\t")
            m = LABEL.match(label)
            if not m:
                continue  # (no node), nodes of another graph
            key = (m.group(2), m.group(4), bool(m.group(5)))
            node_modes[key].add(mode)
            for part in shaders.split(" "):
                stage, _, h = part.partition("=")
                if not h or stage not in ("vs", "ps", "cs", "rt"):
                    continue
                e = nodes[key][(stage, h)]
                e["modes"].add(mode)
                if frames >= 100:
                    e["bpf"] = max(e["bpf"], int(binds) / frames)

    def modes(s):
        known = sorted(s - {"?"}, key=ORDER.get)
        return " ".join(known) if known else "?"

    OUT.mkdir(parents=True, exist_ok=True)
    rows = 0
    with open(OUT / "node_shaders.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["group", "node", "job", "stage", "hash", "engine_name", "binds_per_frame", "modes", "rt_exports"])
        for (group, node, job), sh in sorted(nodes.items(), key=lambda kv: (order[kv[0]], kv[0])):
            for (stage, h), e in sorted(sh.items()):
                w.writerow([group, node, int(job), stage, h, nm.get(h, ""), f"{e['bpf']:.1f}", modes(e["modes"]),
                            " ".join(exports.get(h, [])) if stage == "rt" else ""])
                rows += 1

    md = ["# REDGraph node reference - Cyberpunk 2077 2.31", "",
          f"Generated by `scripts/sdk_node_reference.py` from {len(files)} census windows of REDGraph's "
          "`[Diagnostics] RenderGraphDump` (raster, RT and PT gameplay, menus, loading). Every row is something the census "
          "SAW bind / dispatch inside that node - a node can bind more in scenes we did not visit. The full table, with "
          "full hashes, is `node_shaders.csv`.", "",
          "* **node** = the name for `node_wrap`, `node_remove`, `node_register(after=...)`, `patch_scope`, "
          "`companion_scope`, `rt_variant(node=...)`. A **group** name works for `node_wrap` and the scopes too.",
          "* `[job]` = the work runs in a job the node submits (it still counts as the node's).",
          "* **modes** = the render modes it ran in (raster / RT / PT); `?` = seen only in early runs that did not record "
          "the mode.",
          "* Engine shaders are listed by hash prefix and engine name; material shaders (the world's geometry) are only "
          "counted.", "",
          "Rows are in FRAME ORDER (the order of every census graph, merged); nodes seen only in the small menu / loading "
          "graphs come last.", "",
          "| group | node | modes | engine shaders | material shaders | RT |", "|---|---|---|---|---|---|"]
    for (group, node, job), sh in sorted(nodes.items(), key=lambda kv: (order[kv[0]], kv[0])):
        eng = sum(1 for (s, h) in sh if s != "rt" and h in static)
        mat = sum(1 for (s, h) in sh if s != "rt" and h not in static)
        rt = sum(1 for (s, _) in sh if s == "rt")
        md.append(f"| {group} | `{node}`{' [job]' if job else ''} | {modes(node_modes[(group, node, job)])} | {eng} | "
                  f"{mat} | {rt or ''} |")
    md += ["", "## Per node", ""]
    for (group, node, job), sh in sorted(nodes.items(), key=lambda kv: (order[kv[0]], kv[0])):
        md.append(f"### {group} / `{node}`{' [job]' if job else ''}")
        md.append("")
        listed = [(s, h, e) for (s, h), e in sh.items() if s == "rt" or h in static]
        for s, h, e in sorted(listed, key=lambda x: (x[0] != "rt", -x[2]["bpf"], x[1])):
            extra = f" - exports: {' '.join(exports.get(h, []))}" if s == "rt" else ""
            md.append(f"* {s} `{h[:8]}` {nm.get(h, '?')} - {e['bpf']:.1f}/frame, {modes(e['modes'])}{extra}")
        mat = sum(1 for (s, h) in sh if s != "rt" and h not in static)
        if mat:
            md.append(f"* + {mat} material shader(s)")
        md.append("")
    (OUT / "nodes.md").write_text("\n".join(md), encoding="utf-8")
    print(f"{len(files)} windows -> {len(nodes)} nodes, {rows} rows -> {OUT}")


if __name__ == "__main__":
    main()
