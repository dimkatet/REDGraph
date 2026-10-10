#!/usr/bin/env python3
"""The self-test's GAME-DERIVED blobs, built from the game installed on this machine.

The repository carries no bytecode of the game. Two self-tests need some, so the build makes it from the game's own
shader caches with the SDK's tools - the same path a modder takes:

  patches/rt_variant_test_318cee44.hpp   RtVariantTest: the RT sun-shadow library 318cee44 reduced to one extra
                                         raygen, `rgtest_rgs_shadow_lit` = rgs_shadow_main with every store "lit"
                                         (extract -> dis -> rt_raygen_variant.py --edit lit --only -> asm -> header)
  patches/rt_swap_test_318cee44.hpp      RtSwapTest: the WHOLE library 318cee44 with rgs_shadow_main's stores "lit"
                                         IN PLACE - no variant, so the host swaps it in for the engine's (M3 swap)
  patches/selftest_vanilla_0c359133.hpp  PatchScopeTest: the sky-cube PS 0c359133 byte for byte (extract -> header)

    python scripts/make_selftest_blobs.py --game <Cyberpunk 2077 install> --dxc <DXC bin/x64> --out <dir>

--game defaults to $RG_GAME_DIR, --dxc to $RG_DXC_DIR (a DirectX Shader Compiler release: dxc.exe, dxcompiler.dll,
dxil.dll). Writes <out>/patches/*.hpp; intermediates go to <out>/work/.
"""
import argparse
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TOOLS = ROOT / "sdk" / "tools"

RT_HASH = "318cee441b4f0c3b27de656dff3c59c0"
SKY_HASH = "0c35913398007b9717872af88f5f480f"


def run(*args):
    r = subprocess.run([sys.executable, *map(str, args)], capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit(f"failed: {' '.join(map(str, args[:2]))}")


def lit_in_place(src, dst, raygen):
    """src's raygen with every textureStore "lit" (rt_raygen_variant.lit_store), everything else as is."""
    sys.path.insert(0, str(TOOLS))
    from rt_raygen_variant import lit_store, mangled
    lines = src.read_text(encoding="utf-8").split("\n")
    start = lines.index(f"define void @{mangled(raygen)}() #0 {{")
    end, stores = start, 0
    while lines[end] != "}":
        new = lit_store(lines[end])
        stores += new != lines[end]
        lines[end] = new
        end += 1
    if not stores:
        raise SystemExit(f"lit_in_place: no textureStore in {raygen}")
    dst.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--game", default=os.environ.get("RG_GAME_DIR"))
    ap.add_argument("--dxc", default=os.environ.get("RG_DXC_DIR"))
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if not a.game or not a.dxc:
        raise SystemExit("pass --game and --dxc (or set RG_GAME_DIR / RG_DXC_DIR)")
    out = pathlib.Path(a.out)
    work, patches = out / "work", out / "patches"
    work.mkdir(parents=True, exist_ok=True)
    patches.mkdir(parents=True, exist_ok=True)
    cache, dxil, variant = TOOLS / "rg_shader_cache.py", TOOLS / "rg_dxil.py", TOOLS / "rt_raygen_variant.py"

    run(cache, "extract", RT_HASH, SKY_HASH, "--game", a.game, "--out", work)
    run(dxil, "dis", work / f"{RT_HASH}.dxil", work / "318cee44_clean.ll", "--dxc", a.dxc)
    run(variant, work / "318cee44_clean.ll", work / "318cee44_lit.ll", "--clone", "rgs_shadow_main",
        "--as", "rgtest_rgs_shadow_lit", "--edit", "lit", "--only", "--prefix", "rgtest_")
    run(dxil, "asm", work / "318cee44_lit.ll", work / "318cee44_lit.dxil", "--dxc", a.dxc)
    run(dxil, "header", work / "318cee44_lit.dxil", patches / "rt_variant_test_318cee44.hpp",
        "--symbol", "RtVariantTest318cee44", "--namespace", "patches")
    lit_in_place(work / "318cee44_clean.ll", work / "318cee44_swap.ll", "rgs_shadow_main")
    run(dxil, "asm", work / "318cee44_swap.ll", work / "318cee44_swap.dxil", "--dxc", a.dxc)
    run(dxil, "header", work / "318cee44_swap.dxil", patches / "rt_swap_test_318cee44.hpp",
        "--symbol", "RtSwapTest318cee44", "--namespace", "patches")
    run(dxil, "header", work / f"{SKY_HASH}.dxil", patches / "selftest_vanilla_0c359133.hpp",
        "--symbol", "SelftestVanilla0c359133", "--namespace", "patches")
    print(f"self-test blobs -> {patches}")


if __name__ == "__main__":
    main()
