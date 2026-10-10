"""Static reverse-engineering of Cyberpunk2077.exe (2.31) with names from cp2077-symbols.

    python scripts/exe_re.py dis 0x1401f6f2c [N]   # disassemble N instructions (default to the first ret)
    python scripts/exe_re.py func 0x1401f6f2c      # the whole function (.pdata bounds), calls annotated
    python scripts/exe_re.py callers 0x1401f7224   # every direct E8/E9 call site, with the containing function
    python scripts/exe_re.py up 0x1407e0bb4        # the caller tree upwards (3 per level, depth 8)
    python scripts/exe_re.py sym SetupForDraw      # grep the symbol table

Calls are annotated with the symbol name; `call [reg+off]` with the ID3D12GraphicsCommandList method when `off` is
one of its vtable slots (a guess - the register may hold another interface). Symbol names can be WRONG where the
linker folded identical functions (COMDAT): `RegexMatcher::Find+0x34` was really the state-object creator. Trust
the code, not a name that does not fit it.

Needs `capstone`, `pefile`, `numpy`; the game install in the environment variable RG_GAME_DIR (the folder holding
bin/x64/Cyberpunk2077.exe); a clone of cp2077-symbols (https://codeberg.org/bartmoss/cp2077-symbols, `git clone
--depth 1`) in CP2077_SYMBOLS_DIR.
"""
import bisect
import csv
import json
import os
import re
import sys

import capstone
import numpy as np
import pefile


SYMBOLS_DIR = os.environ.get("CP2077_SYMBOLS_DIR") or sys.exit("set CP2077_SYMBOLS_DIR to a cp2077-symbols clone")
GAME = os.environ.get("RG_GAME_DIR") or sys.exit("set RG_GAME_DIR to the Cyberpunk 2077 install")

EXE = os.path.join(GAME, "bin", "x64", "Cyberpunk2077.exe")
pe = pefile.PE(EXE, fast_load=True)
IB = pe.OPTIONAL_HEADER.ImageBase
img = pe.get_memory_mapped_image()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)

SYM = {}
for ea, name in csv.reader(open(os.path.join(SYMBOLS_DIR, "data", "names_by_address.csv"), encoding="utf-8")):
    try:
        SYM.setdefault(int(ea, 16), name)
    except ValueError:
        pass
KEYS = sorted(SYM)

# ID3D12GraphicsCommandList vtable slots we have needed so far.
CL = {0x60: "DrawInstanced", 0x68: "DrawIndexedInstanced", 0x70: "Dispatch", 0xa0: "IASetPrimitiveTopology",
      0xc8: "SetPipelineState", 0xd0: "ResourceBarrier", 0xe0: "SetDescriptorHeaps", 0xe8: "SetComputeRootSignature",
      0xf0: "SetGraphicsRootSignature", 0xf8: "SetComputeRootDescriptorTable",
      0x100: "SetGraphicsRootDescriptorTable", 0x118: "SetComputeRoot32BitConstants",
      0x120: "SetGraphicsRoot32BitConstants", 0x128: "SetComputeRootCBV", 0x130: "SetGraphicsRootCBV",
      0x138: "SetComputeRootSRV", 0x140: "SetGraphicsRootSRV", 0x148: "SetComputeRootUAV",
      0x150: "SetGraphicsRootUAV", 0x178: "ExecuteIndirect", 0x258: "SetPipelineState1", 0x260: "DispatchRays"}

pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_IMPORT"],
                                       pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
IMPORTS = {i.address: f"{e.dll.decode()}!{(i.name or b'').decode()}"
           for e in pe.DIRECTORY_ENTRY_IMPORT for i in e.imports}
RF = sorted((IB + e.struct.BeginAddress, IB + e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION)
RS = [r[0] for r in RF]


def short(n):
    m = re.match(r"\?+([^@]+)@([^@]+)@", n)
    return f"{m.group(2)}::{m.group(1)}" if m else n[:90]


def name(ea):
    i = bisect.bisect(KEYS, ea) - 1
    if i < 0:
        return hex(ea)
    b = KEYS[i]
    return short(SYM[b]) + ("" if b == ea else f"+{ea - b:#x}")


def bounds(ea):
    i = bisect.bisect(RS, ea) - 1
    return RF[i] if i >= 0 and ea < RF[i][1] else None


def func_of(ea):
    b = bounds(ea)
    return b[0] if b else ea


def annotate(ins):
    if ins.mnemonic in ("call", "jmp") or ins.mnemonic.startswith("j"):
        if ins.op_str.startswith("0x"):
            return name(int(ins.op_str, 16))
        m = re.match(r"qword ptr \[rip \+ (0x[0-9a-f]+)\]", ins.op_str)
        if m:
            return IMPORTS.get(ins.address + ins.size + int(m.group(1), 16), "")
        m = re.search(r"qword ptr \[r\w+ \+ (0x[0-9a-f]+)\]", ins.op_str)
        if m and int(m.group(1), 16) in CL:
            return "CL::" + CL[int(m.group(1), 16)] + "?"
    return ""


def dis(ea, n=None, end=None):
    stop = end or ea + 0x4000
    for k, ins in enumerate(md.disasm(img[ea - IB:stop - IB], ea)):
        note = annotate(ins)
        print(f"{ins.address:x}  {ins.mnemonic:6} {ins.op_str}" + (f"   ; {note}" if note else ""))
        if n is not None and k + 1 >= n:
            break
        if n is None and end is None and ins.mnemonic in ("ret", "int3"):
            break


_text = next(s for s in pe.sections if s.Name.startswith(b".text"))
_lo = _text.VirtualAddress
_buf = np.frombuffer(img[_lo:_lo + _text.Misc_VirtualSize], dtype=np.uint8)
_idx = np.nonzero((_buf == 0xE8) | (_buf == 0xE9))[0]
_idx = _idx[_idx + 5 < len(_buf)]
_rel = (_buf[_idx + 1].astype(np.int64) | (_buf[_idx + 2].astype(np.int64) << 8) |
        (_buf[_idx + 3].astype(np.int64) << 16) | (_buf[_idx + 4].astype(np.int64) << 24))
_rel = np.where(_rel >= 2**31, _rel - 2**32, _rel)
_tgt = IB + _lo + _idx + 5 + _rel


def callers(target):
    return [IB + _lo + int(h) for h in _idx[_tgt == target]]


def up(ea, depth=0, seen=None):
    seen = set() if seen is None else seen
    f = func_of(ea)
    if f in seen or depth > 8:
        return
    seen.add(f)
    cs = callers(f)
    print("  " * depth + f"{f:x} {name(f)} ({len(cs)} callers)")
    for c in cs[:3]:
        up(c, depth + 1, seen)


if __name__ == "__main__":
    cmd, *args = sys.argv[1:] or ["help"]
    if cmd == "dis":
        dis(int(args[0], 16), int(args[1]) if len(args) > 1 else None)
    elif cmd == "func":
        b = bounds(int(args[0], 16))
        print(f"== {name(b[0])} {b[0]:x}..{b[1]:x}") if b else print("no .pdata entry")
        if b:
            dis(b[0], end=b[1])
    elif cmd == "callers":
        for c in callers(int(args[0], 16)):
            print(f"{c:x}  {name(func_of(c))}")
    elif cmd == "up":
        up(int(args[0], 16))
    elif cmd == "sym":
        for k in KEYS:
            if all(a.lower() in SYM[k].lower() for a in args):
                print(f"{k:x}  {SYM[k][:160]}")
    else:
        print(__doc__)
