// compile_hlsl (rg_host.h; docs/plan-gpu-timing-and-hlsl.md section 2). The DLL loading and the signed-container check
// are ported from spuddeh/cp2077-native-shaders (Effects.cpp), with the author's permission.
//
// dxcompiler.dll + dxil.dll are loaded on first use from next to REDGraph.dll, by FULL path, dxil.dll FIRST:
// dxcompiler loads it by bare name to sign the container, and finds it already in the process - the search path is
// never touched. Not linked (the 20 MB is why runtime compile was once rejected) and never a hard dependency: without
// dxcompiler.dll every call is RG_E_UNAVAILABLE, logged once. Without dxil.dll the container comes out UNSIGNED (an
// all-zero hash at bytes 4..20) and D3D12 would reject it - refused here with the reason. Every blob is kept for the
// process: the engine and our clones keep pointers into bytecode. assemble() serves [Dev] ShaderHotReload
// (shader_reload.cpp): an edited .ll back into a signed container.

#include <Windows.h>
#include <objidl.h>  // IStream, IUnknown - Windows.h is lean in this build
#include <unknwn.h>
#include <dxcapi.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <mutex>
#include <string>
#include <vector>

#include "core/internal.hpp"

namespace core::hlsl {
namespace {

std::mutex g_mu;  // IDxcCompiler3 is one instance: one compile at a time
bool g_tried = false;
IDxcUtils* g_utils = nullptr;
IDxcCompiler3* g_dxc = nullptr;
DxcCreateInstanceProc g_create = nullptr;  // for the assembler / validator ([Dev] ShaderHotReload)
bool g_have_dxil = false;
std::vector<IDxcBlob*> g_keep;  // never released

std::wstring widen(const char* s) {
  if (!s || !*s) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
  std::wstring w(n > 0 ? n - 1 : 0, L'\0');
  if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
  return w;
}

std::filesystem::path our_dir() {
  HMODULE m = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&our_dir), &m);
  wchar_t buf[MAX_PATH];
  const DWORD n = m ? GetModuleFileNameW(m, buf, MAX_PATH) : 0;
  return n ? std::filesystem::path(buf).parent_path() : std::filesystem::path();
}

bool load() {
  if (g_tried) return g_dxc != nullptr;
  g_tried = true;
  const auto dir = our_dir();
  g_have_dxil = LoadLibraryExW((dir / L"dxil.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH) != nullptr;
  HMODULE dxc = LoadLibraryExW((dir / L"dxcompiler.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  auto create = dxc ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(dxc, "DxcCreateInstance")) : nullptr;
  g_create = create;
  if (!create) {
    g_sdk->logger->InfoF(g_handle, "hlsl: no dxcompiler.dll next to REDGraph.dll (%ls) - compile_hlsl is unavailable",
                         dir.c_str());
    return false;
  }
  if (FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&g_utils))) || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&g_dxc)))) {
    g_sdk->logger->Error(g_handle, "hlsl: dxcompiler.dll is there but DxcCreateInstance failed - compile_hlsl is "
                                   "unavailable");
    if (g_utils) g_utils->Release(), g_utils = nullptr;
    g_dxc = nullptr;
    return false;
  }
  g_sdk->logger->InfoF(g_handle, "hlsl: compiler loaded from %ls%s", dir.c_str(),
                       g_have_dxil ? "" : " - but NO dxil.dll: every container would be unsigned (refused)");
  return true;
}

bool profile_ok(const char* p) {
  if (!p || std::strlen(p) < 6 || p[2] != '_' || p[3] != '6' || p[4] != '_') return false;
  return std::strncmp(p, "cs", 2) == 0 || std::strncmp(p, "vs", 2) == 0 || std::strncmp(p, "ps", 2) == 0;
}

void put_error(char* out, uint32_t size, const char* text) {
  if (!out || !size) return;
  std::snprintf(out, size, "%s", text ? text : "");
}

}  // namespace

int32_t compile(const rg_hlsl_desc* d, rg_blob* out, char* error, uint32_t error_size) {
  put_error(error, error_size, "");
  if (out) *out = {nullptr, 0};
  if (!d || !out || d->struct_size < sizeof(rg_hlsl_desc) || !d->entry || (!d->source && !d->path)) return RG_E_ARG;
  if (!profile_ok(d->profile)) {
    put_error(error, error_size, "profile: cs_6_x, vs_6_x or ps_6_x");
    return RG_E_UNSUPPORTED;
  }
  std::lock_guard lock(g_mu);
  if (!load()) {
    put_error(error, error_size, "the shader compiler (dxcompiler.dll next to REDGraph.dll) is not installed");
    return RG_E_UNAVAILABLE;
  }
  const std::string name = d->path ? d->path : "<source>";
  // The source: the text given, or the file.
  IDxcBlobEncoding* src = nullptr;
  std::string text;
  if (d->source) {
    text = d->source;
  } else if (FAILED(g_utils->LoadFile(widen(d->path).c_str(), nullptr, &src))) {
    const std::string why = "cannot read " + name;
    put_error(error, error_size, why.c_str());
    return RG_E_ARG;
  }
  // Arguments: the name first (errors and #line), then the include folder - given, or the file's.
  std::vector<std::wstring> a;
  a.push_back(widen(name.c_str()));
  a.push_back(L"-E");
  a.push_back(widen(d->entry));
  a.push_back(L"-T");
  a.push_back(widen(d->profile));
  a.push_back(L"-O3");
  std::wstring inc = widen(d->include_dir);
  if (inc.empty() && d->path) inc = std::filesystem::path(widen(d->path)).parent_path().wstring();
  if (!inc.empty()) {
    a.push_back(L"-I");
    a.push_back(inc);
  }
  for (uint32_t i = 0; d->defines && i < d->define_count; ++i)
    if (d->defines[i]) {
      a.push_back(L"-D");
      a.push_back(widen(d->defines[i]));
    }
  std::vector<LPCWSTR> argv;
  for (const auto& s : a) argv.push_back(s.c_str());
  DxcBuffer buf{};
  if (src) {
    buf.Ptr = src->GetBufferPointer();
    buf.Size = src->GetBufferSize();
    buf.Encoding = DXC_CP_ACP;
  } else {
    buf.Ptr = text.data();
    buf.Size = text.size();
    buf.Encoding = DXC_CP_UTF8;
  }
  IDxcIncludeHandler* includes = nullptr;
  g_utils->CreateDefaultIncludeHandler(&includes);
  IDxcResult* res = nullptr;
  HRESULT hr = g_dxc->Compile(&buf, argv.data(), static_cast<UINT32>(argv.size()), includes, IID_PPV_ARGS(&res));
  if (includes) includes->Release();
  if (src) src->Release();
  HRESULT status = E_FAIL;
  if (SUCCEEDED(hr) && res) res->GetStatus(&status);
  IDxcBlobUtf8* msgs = nullptr;
  if (res) res->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&msgs), nullptr);
  const char* msg = msgs && msgs->GetStringLength() ? msgs->GetStringPointer() : "";
  int32_t rc = 0;
  IDxcBlob* obj = nullptr;
  if (FAILED(hr) || FAILED(status) || !res || FAILED(res->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&obj), nullptr)) ||
      !obj || obj->GetBufferSize() < 20) {
    put_error(error, error_size, *msg ? msg : "the compiler failed without a message");
    g_sdk->logger->WarnF(g_handle, "hlsl: %s (%s %s) did NOT compile: %.400s", name.c_str(), d->entry, d->profile,
                         *msg ? msg : "no message");
    rc = RG_E_COMPILE;
  } else {
    const auto* b = static_cast<const uint8_t*>(obj->GetBufferPointer());
    bool signed_ = false;
    for (int i = 4; i < 20; ++i) signed_ |= b[i] != 0;
    if (std::memcmp(b, "DXBC", 4) != 0 || !signed_) {
      put_error(error, error_size, g_have_dxil ? "the container is not signed" :
                                                 "the container is not signed: dxil.dll was not loaded (put it next to "
                                                 "REDGraph.dll)");
      g_sdk->logger->ErrorF(g_handle, "hlsl: %s compiled but is UNSIGNED%s - refused", name.c_str(),
                            g_have_dxil ? "" : " (no dxil.dll)");
      rc = RG_E_COMPILE;
    } else {
      g_keep.push_back(obj);
      obj = nullptr;
      *out = {b, g_keep.back()->GetBufferSize()};
      g_sdk->logger->InfoF(g_handle, "hlsl: %s (%s %s) compiled, %zu bytes%s%.300s", name.c_str(), d->entry,
                           d->profile, out->size, *msg ? " - warnings: " : "", msg);
    }
  }
  if (obj) obj->Release();
  if (msgs) msgs->Release();
  if (res) res->Release();
  return rc;
}

int32_t assemble(const std::string& ir, rg_blob* out, std::string* error) {
  *out = {nullptr, 0};
  std::lock_guard lock(g_mu);
  if (!load() || !g_create) {
    *error = "no dxcompiler.dll next to REDGraph.dll";
    return RG_E_UNAVAILABLE;
  }
  if (!g_have_dxil) {
    *error = "no dxil.dll next to REDGraph.dll: the container could not be signed";
    return RG_E_UNAVAILABLE;
  }
  IDxcAssembler* as = nullptr;
  IDxcValidator* val = nullptr;
  IDxcBlobEncoding* text = nullptr;
  IDxcOperationResult* ar = nullptr;
  IDxcOperationResult* vr = nullptr;
  IDxcBlob* obj = nullptr;
  auto errors_of = [](IDxcOperationResult* r, const char* what) {
    std::string e = what;
    IDxcBlobEncoding* b = nullptr;
    if (r && SUCCEEDED(r->GetErrorBuffer(&b)) && b && b->GetBufferSize())
      e += ": " + std::string(static_cast<const char*>(b->GetBufferPointer()), b->GetBufferSize());
    if (b) b->Release();
    return e;
  };
  int32_t rc = RG_E_COMPILE;
  HRESULT st = E_FAIL;
  if (FAILED(g_create(CLSID_DxcAssembler, IID_PPV_ARGS(&as))) || FAILED(g_create(CLSID_DxcValidator, IID_PPV_ARGS(&val)))) {
    *error = "dxcompiler.dll has no assembler / validator";
    rc = RG_E_UNAVAILABLE;
  } else if (FAILED(g_utils->CreateBlob(ir.data(), static_cast<UINT32>(ir.size()), DXC_CP_UTF8, &text))) {
    *error = "CreateBlob failed";
  } else if (FAILED(as->AssembleToContainer(text, &ar)) || !ar || FAILED(ar->GetStatus(&st)) || FAILED(st)) {
    *error = errors_of(ar, "assembly failed");
  } else if (FAILED(ar->GetResult(&obj)) || !obj || obj->GetBufferSize() < 32) {
    *error = "the assembler returned no container";
  } else if (FAILED(val->Validate(obj, DxcValidatorFlags_InPlaceEdit, &vr)) || !vr || FAILED(vr->GetStatus(&st)) ||
             FAILED(st)) {
    *error = errors_of(vr, "validation failed");  // InPlaceEdit also writes the container's checksum (the signature)
  } else {
    const auto* b = static_cast<const uint8_t*>(obj->GetBufferPointer());
    bool signed_ = false;
    for (int i = 4; i < 20; ++i) signed_ |= b[i] != 0;
    if (!signed_) {
      *error = "validated but not signed";
    } else {
      g_keep.push_back(obj);
      obj = nullptr;
      *out = {b, g_keep.back()->GetBufferSize()};
      rc = 0;
    }
  }
  for (IUnknown* u : std::initializer_list<IUnknown*>{as, val, text, ar, vr, obj})
    if (u) u->Release();
  return rc;
}

}  // namespace core::hlsl
