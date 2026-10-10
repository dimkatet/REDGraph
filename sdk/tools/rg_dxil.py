#!/usr/bin/env python3
"""DXIL text <-> container, the HASH a REDGraph registration needs, and a blob as a C++ header.

    python rg_dxil.py dis    IN.dxil OUT.ll   [--dxc DIR]   # disassemble (dxc -dumpbin)
    python rg_dxil.py asm    IN.ll   OUT.dxil [--dxc DIR]   # assemble + validate (the container checksum is written)
    python rg_dxil.py hash   FILE.dxil ...                  # the HASH chunk digest (what REDGraph matches on)
    python rg_dxil.py header IN.dxil OUT.hpp --symbol NAME [--namespace NS]
                                                            # `NS::kNAMEBytes[]` / `NS::kNAMESize` for rg_blob

DIR (--dxc, default $RG_DXC_DIR) = a DirectX Shader Compiler release's bin/x64 (github.com/microsoft/
DirectXShaderCompiler/releases): dxc.exe, dxcompiler.dll and dxil.dll. `asm` loads dxcompiler.dll (which loads dxil.dll
from the same folder) - Windows only.

Why `asm` validates: IDxcAssembler::AssembleToContainer leaves the container's own 16-byte checksum (after the "DXBC"
magic) zero, and D3D12 silently refuses such a shader - it creates nothing and binds nothing, with no error.
IDxcValidator::Validate with DxcValidatorFlags_InPlaceEdit writes the checksum and fails on an invalid module.

The HASH of an edited shader differs from the engine's: REDGraph registers a patch under the ENGINE shader's hash
(rg_patch_desc.hash) and takes your container as bytes - do not try to keep the hash.

Standard library only.
"""
import argparse
import ctypes
import os
import pathlib
import struct
import subprocess
import sys
import uuid


def dxc_dir(arg):
    d = arg or os.environ.get("RG_DXC_DIR")
    if not d:
        raise SystemExit("pass --dxc <DXC release bin/x64> or set RG_DXC_DIR")
    p = pathlib.Path(d).resolve()
    if not (p / "dxcompiler.dll").exists():
        raise SystemExit(f"{p} has no dxcompiler.dll")
    return p


def container_hash(data: bytes):
    """The HASH chunk digest (hex) of a DXBC container, or None."""
    if data[:4] != b"DXBC" or len(data) < 32:
        return None
    count, = struct.unpack_from("<I", data, 28)
    for i in range(min(count, 32)):
        co, = struct.unpack_from("<I", data, 32 + 4 * i)
        if data[co:co + 4] == b"HASH":
            return data[co + 12:co + 28].hex()
    return None


# ---- assemble: IDxcLibrary / IDxcAssembler / IDxcValidator through their vtables ----
HR = ctypes.c_long
VP = ctypes.c_void_p


def _guid(s):
    return (ctypes.c_ubyte * 16).from_buffer_copy(uuid.UUID(s).bytes_le)


def _call(obj, slot, *types_and_args):
    """obj->vtbl[slot](obj, args...) - types_and_args = (ctype, value) pairs; returns the HRESULT / value."""
    vtbl = ctypes.cast(ctypes.cast(obj, ctypes.POINTER(VP))[0], ctypes.POINTER(VP))
    types = [t for t, _ in types_and_args]
    restype = HR
    fn = ctypes.WINFUNCTYPE(restype, VP, *types)(vtbl[slot])
    return fn(obj, *[a for _, a in types_and_args])


def _blob_bytes(blob):
    vtbl = ctypes.cast(ctypes.cast(blob, ctypes.POINTER(VP))[0], ctypes.POINTER(VP))
    ptr = ctypes.WINFUNCTYPE(VP, VP)(vtbl[3])(blob)
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, VP)(vtbl[4])(blob)
    return ctypes.string_at(ptr, size) if ptr and size else b""


def _errors(result):
    err = VP()
    _call(result, 5, (ctypes.POINTER(VP), ctypes.byref(err)))
    return _blob_bytes(err).decode("utf-8", "replace").strip() if err.value else "(no error text)"


def assemble(src: pathlib.Path, out: pathlib.Path, dxc: pathlib.Path):
    if sys.platform != "win32":
        raise SystemExit("asm needs Windows (dxcompiler.dll)")
    os.add_dll_directory(str(dxc))
    dll = ctypes.WinDLL(str(dxc / "dxcompiler.dll"))
    create = dll.DxcCreateInstance
    create.argtypes = [VP, VP, ctypes.POINTER(VP)]
    create.restype = HR

    def instance(clsid, iid, what):
        p = VP()
        c, i = _guid(clsid), _guid(iid)  # alive across the call
        hr = create(ctypes.addressof(c), ctypes.addressof(i), ctypes.byref(p))
        if hr < 0 or not p.value:
            raise SystemExit(f"{what}: 0x{hr & 0xFFFFFFFF:08X}")
        return p
    lib = instance("6245d6af-66e0-48fd-80b4-4d271796748c", "e5204dc7-d18c-4c3c-bdfb-851673980fe7", "IDxcLibrary")
    asm = instance("d728db68-f903-4f80-94cd-dccf76ec7151", "091f7a26-1c1f-4948-904b-e6e3a8a771d5", "IDxcAssembler")
    val = instance("8ca3e215-f728-4cf3-8cdd-88af917587a1", "a6e82bd2-1fd7-4826-9811-2857e797f49a", "IDxcValidator")

    text = src.read_bytes()
    buf = ctypes.create_string_buffer(text, len(text))
    blob = VP()
    hr = _call(lib, 7, (VP, ctypes.addressof(buf)), (ctypes.c_uint32, len(text)), (ctypes.c_uint32, 65001),
               (ctypes.POINTER(VP), ctypes.byref(blob)))  # CreateBlobWithEncodingOnHeapCopy, UTF-8
    if hr < 0:
        raise SystemExit(f"CreateBlobWithEncodingOnHeapCopy: 0x{hr & 0xFFFFFFFF:08X}")
    result = VP()
    hr = _call(asm, 3, (VP, blob), (ctypes.POINTER(VP), ctypes.byref(result)))  # AssembleToContainer
    if hr < 0 or not result.value:
        raise SystemExit(f"AssembleToContainer: 0x{hr & 0xFFFFFFFF:08X}")
    status = HR()
    _call(result, 3, (ctypes.POINTER(HR), ctypes.byref(status)))  # GetStatus
    if status.value != 0:
        raise SystemExit("assembly error: " + _errors(result))
    container = VP()
    _call(result, 4, (ctypes.POINTER(VP), ctypes.byref(container)))  # GetResult
    vresult = VP()
    hr = _call(val, 3, (VP, container), (ctypes.c_uint32, 1), (ctypes.POINTER(VP), ctypes.byref(vresult)))  # InPlaceEdit
    if hr < 0 or not vresult.value:
        raise SystemExit(f"Validate: 0x{hr & 0xFFFFFFFF:08X}")
    _call(vresult, 3, (ctypes.POINTER(HR), ctypes.byref(status)))
    if status.value != 0:
        raise SystemExit("validation error: " + _errors(vresult))
    data = _blob_bytes(container)
    if data[4:20] == bytes(16):
        raise SystemExit("the container checksum is still zero - validation did not write it")
    out.write_bytes(data)
    print(f"{out}: {len(data)} B, validated, HASH {container_hash(data)}")


def disassemble(src: pathlib.Path, out: pathlib.Path, dxc: pathlib.Path):
    exe = dxc / "dxc.exe"
    if not exe.exists():
        raise SystemExit(f"{dxc} has no dxc.exe")
    r = subprocess.run([str(exe), "-dumpbin", str(src), "-Fc", str(out)], capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stderr or r.stdout)
    print(f"{out}: {out.stat().st_size} B")


def header(src: pathlib.Path, out: pathlib.Path, symbol: str, namespace: str):
    data = src.read_bytes()
    if data[:4] != b"DXBC":
        raise SystemExit(f"{src} is not a DXBC container")
    lines = [f"// Generated by rg_dxil.py header from {src.name} (HASH {container_hash(data)}).", "#pragma once",
             "#include <cstddef>", "#include <cstdint>", f"namespace {namespace} {{",
             f"inline constexpr size_t k{symbol}Size = {len(data)};",
             f"inline constexpr uint8_t k{symbol}Bytes[k{symbol}Size] = {{"]
    for i in range(0, len(data), 20):
        lines.append("    " + " ".join(f"0x{b:02x}," for b in data[i:i + 20]))
    lines += ["};", f"}}  // namespace {namespace}", ""]
    out.write_text("\n".join(lines), encoding="utf-8")
    print(f"{out}: {namespace}::k{symbol}Bytes / k{symbol}Size ({len(data)} B)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("dis", "asm"):
        p = sub.add_parser(name)
        p.add_argument("src", type=pathlib.Path)
        p.add_argument("out", type=pathlib.Path)
        p.add_argument("--dxc", help="DXC release bin/x64 (default: $RG_DXC_DIR)")
    p = sub.add_parser("hash")
    p.add_argument("files", nargs="+", type=pathlib.Path)
    p = sub.add_parser("header")
    p.add_argument("src", type=pathlib.Path)
    p.add_argument("out", type=pathlib.Path)
    p.add_argument("--symbol", required=True, help="C++ name part: kSYMBOLBytes / kSYMBOLSize")
    p.add_argument("--namespace", default="blobs")
    a = ap.parse_args()
    if a.cmd == "asm":
        assemble(a.src, a.out, dxc_dir(a.dxc))
    elif a.cmd == "dis":
        disassemble(a.src, a.out, dxc_dir(a.dxc))
    elif a.cmd == "hash":
        for f in a.files:
            print(f"{container_hash(f.read_bytes()) or 'not a DXBC container':32}  {f}")
    else:
        header(a.src, a.out, a.symbol, a.namespace)


if __name__ == "__main__":
    main()
