// [Dev] Readback (docs/internals.md "GPU readback (dev)"): "read THIS now" from CET / REDBridge, without a bespoke
// readback in a client. A request is a text spec (rg_dev.h):
//   block:<name | id>                     the NEXT write of an engine constant block (engine_blocks' CPU copy) after
//                                         the request -> <dir>/rb<id>_block_<name>.bin + .json
//   flow:<hash | mod/name> after:<node>   a RenderFlow texture at a node -> .dds + .json; before:<node> the same at the
//                                         node's start (after the clients' befores and a wrap's declarations)
//   vsout:<VS hash or prefix> [draws:n]   what an engine VS outputs at its next draw(s), by Stream Output -> .bin + .json:
//                                         a companion PSO from the engine PSO's desc (the VS the draw ran with - a patch
//                                         variant's when a clone drew - no PS, nothing rasterized, a Stream Output decl
//                                         from the VS's output signature) issues the same draw again on the engine's list,
//                                         SOSetTargets before / unset after; n draws of ONE frame (default 1)
// A flow request is ARMED until the first run of that node class (in the graph's real run) where the name resolves -
// every class is wrapped under the switch (render_nodes::wrap_all_classes, as for the graph debug module), our own
// nodes call in from node_execute. There, on the node's command list: the engine's TransitionTexture to COPY_SOURCE
// (its state tracker then starts the next bind from there - nothing is restored), CopyTextureRegion of every slice of
// one mip into a READBACK buffer, then a MARKER (WriteBufferImmediate, MARKER_OUT: written once the copy is done) -
// the proof that the copy executed on the GPU (a readback whose copy never runs reads as zeros, NOTES rule). The frame
// hook maps the buffer until the marker is there, then a worker writes the files and the summary: min / max per
// channel, NaN / Inf counts for the formats it decodes.
// Off (the default): nothing here runs, no class is wrapped for it.

#include <Windows.h>
#include <d3d12.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <RED4ext/CName.hpp>

#include "rg_dev.h"
#include "core/engine_231.hpp"
#include "core/engine_blocks.hpp"
#include "core/internal.hpp"

namespace core::readback {
namespace {

namespace fs = std::filesystem;

constexpr uint64_t kDefaultFrames = 600;  // a request not served by then fails
constexpr uint64_t kGpuFrames = 300;      // a copy whose marker is not there by then never ran
constexpr uint32_t kMaxReqs = 256;        // finished requests beyond this are forgotten, oldest first
constexpr uint32_t kMarker = 0x5EADB000u;

enum class Kind { kBlock, kFlow, kVsOut };
constexpr uint32_t kMaxDraws = 64;               // vsout: draws:<n> at most
constexpr uint64_t kMaxSoBytes = 128ull << 20;   // vsout: one draw's Stream Output buffer at most
constexpr uint64_t kSoHeader = 256;              // vsout: the filled-size counter's slot before the data

// vsout: one element of the VS's output signature, as Stream Output writes it (32-bit components, in register order).
struct SoElem {
  std::string name;
  uint32_t index = 0, reg = 0, mask = 0, sysval = 0, type = 0, minprec = 0, start = 0, count = 0, offset = 0;
};
// vsout: one draw captured.
struct SoDraw {
  ID3D12Resource *so = nullptr, *rb = nullptr;
  uint64_t bytes = 0, frame = 0;  // the SO data window
  rg_draw_info info{};
  blas_census::IaViews ia{};
  uint32_t topology = 0;
  uint8_t vs_used[16] = {};
  bool clone = false;
  const void* engine = nullptr;
};
// blas:1 - one BLAS geometry whose inputs were copied, and one NvAPI build's copy.
struct BGeo {
  uint64_t ib = 0, vb = 0, vstride = 0, tf = 0, vb_bytes = 0, ib_bytes = 0;
  uint64_t vb_off = 0, ib_off = 0, tf_off = 0;  // in the call's buffer
  uint32_t icount = 0, vcount = 0, ifmt = 0, vfmt = 0, type = 0, geom_flags = 0, geometry = 0, of = 0;
  uint32_t vb_skew = 0, ib_skew = 0, tf_skew = 0;  // the source's offset inside its 4-aligned window
};
struct BlasCall {
  ID3D12Resource *dst = nullptr, *rb = nullptr;
  uint64_t bytes = 0, frame = 0, dest = 0;
  uint32_t flags = 0;
  bool marker = false;
  std::vector<BGeo> geos;
};
enum class St { kWaitBlock, kArmed, kCopied, kWriting, kDone, kFailed };

struct Req {
  int32_t id = 0;
  std::string spec;
  Kind kind = Kind::kBlock;
  St st = St::kFailed;
  uint64_t start = 0, frames = kDefaultFrames;
  // block
  int32_t block = -1;
  uint64_t seq0 = 0;
  // flow
  uint32_t name_hash = 0;
  std::string flow, node;
  uint64_t node_hash = 0;
  bool after = true;
  uint32_t mip = 0;
  uint64_t seen = 0, not_live = 0;
  bool job = false;
  // vsout
  uint8_t vs_hash[16] = {};
  uint32_t vs_len = 0;  // bytes of vs_hash given (a prefix)
  uint32_t draws_want = 1;
  std::vector<SoElem> elems;
  uint32_t stride = 0;
  std::vector<SoDraw> draws;
  std::vector<std::pair<ID3D12PipelineState*, ID3D12PipelineState*>> so_psos;  // bound PSO -> its SO PSO
  uint64_t skipped_indirect = 0, skipped_ia = 0, skipped_frame = 0;
  std::vector<uint8_t> cb12;  // the shared constants' latest write at the first draw (its matrices map SV_Position)
  uint64_t cb12_seq = 0;
  // blas:1 - phase 0: one frame of the VS's draws gives the keys (an index range: the IB VA of its first index, the
  // index count); phase 1 = frame cap_frame: the draws' Stream Output AND every BLAS build / update of those keys
  bool with_blas = false;
  int phase = 0;
  uint64_t learn_frame = 0, cap_frame = 0;
  std::set<std::pair<uint64_t, uint32_t>> keys;
  std::set<uint64_t> ib_done;
  std::vector<BlasCall> calls;
  uint64_t blas_builds_seen = 0, blas_refused = 0;
  // the copy
  ID3D12Resource* rb = nullptr;
  bool marker = false;  // WriteBufferImmediate recorded (else: kGpuFrames are waited out)
  uint64_t marker_off = 0, copy_frame = 0;
  uint32_t gpu_id = 0;
  D3D12_RESOURCE_DESC desc{};
  std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> fp;
  std::vector<UINT> rows;
  std::vector<UINT64> row_bytes;
  rg_readback_info info{};
};

bool g_on = false;
fs::path g_dir;
std::mutex g_mu;
std::vector<std::unique_ptr<Req>> g_req;
int32_t g_next = 0;
std::atomic<uint32_t> g_armed{0};     // flow requests armed
std::atomic<uint32_t> g_vs_armed{0};  // vsout requests armed
std::atomic<uint32_t> g_blas_armed{0};  // blas:1 requests in their capture frame
// vsout: the hashes armed requests wait for, readable lock-free from the bind funnel (len 0 = a free entry).
constexpr uint32_t kMaxVsWant = 8;
struct VsWant {
  uint8_t hash[16];
  std::atomic<uint32_t> len{0};
};
VsWant g_vs_want[kMaxVsWant];
std::atomic<uint64_t> g_frame{0};
ID3D12Device* g_dev = nullptr;
std::once_flag g_dev_once;

void set_text(Req& r, const std::string& s) { std::snprintf(r.info.text, sizeof r.info.text, "%s", s.c_str()); }

// Under g_mu. Logged once, with the spec.
void vs_unwant(const Req& r);
void release_draws(Req& r);

void fail(Req& r, const std::string& why) {
  if (r.st == St::kArmed) {
    (r.kind == Kind::kVsOut ? g_vs_armed : g_armed).fetch_sub(1, std::memory_order_relaxed);
    if (r.kind == Kind::kVsOut) vs_unwant(r);
    if (r.with_blas && r.phase == 1) g_blas_armed.fetch_sub(1, std::memory_order_relaxed);
  }
  release_draws(r);
  if (r.rb) r.rb->Release();
  r.rb = nullptr;
  r.st = St::kFailed;
  r.info.state = RG_RB_FAILED;
  set_text(r, "failed: " + why);
  g_sdk->logger->WarnF(g_handle, "DEV readback #%d '%s' FAILED - %s", r.id, r.spec.c_str(), why.c_str());
}

ID3D12Device* device() {
  std::call_once(g_dev_once, [] {
    const auto* data = *reinterpret_cast<const uint8_t* const*>(g_base + e231::kDeviceData);
    auto* engine_dev = data ? *reinterpret_cast<IUnknown* const*>(data + e231::kDeviceInData) : nullptr;
    if (engine_dev && SUCCEEDED(engine_dev->QueryInterface(IID_PPV_ARGS(&g_dev)))) g_dev->Release();  // the engine's
  });
  return g_dev;
}

std::string safe(const std::string& s) {
  std::string o;
  for (char c : s) o += (std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_') ? c : '_';
  return o.size() > 60 ? o.substr(0, 60) : o;
}

std::string json_str(const std::string& s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    if (static_cast<unsigned char>(c) < 0x20) continue;
    o += c;
  }
  return o + "\"";
}

// ---- texel decoding for the summary ----

float half(uint16_t h) {
  const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
  float v;
  if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
  else if (e == 31) v = m ? NAN : INFINITY;
  else v = std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
  return s ? -v : v;
}

float small_float(uint32_t bits, uint32_t mbits) {  // R11G11B10: 5-bit exponent, no sign
  const uint32_t e = bits >> mbits, m = bits & ((1u << mbits) - 1);
  if (e == 0) return std::ldexp(static_cast<float>(m), -14 - static_cast<int>(mbits));
  if (e == 31) return m ? NAN : INFINITY;
  return std::ldexp(static_cast<float>(m | (1u << mbits)), static_cast<int>(e) - 15 - static_cast<int>(mbits));
}

// Bytes per texel and channel count of the formats decoded; 0 = not decoded (the file is still written).
uint32_t texel_info(DXGI_FORMAT f, uint32_t* ch) {
  switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: return *ch = 4, 16;
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: return *ch = 2, 8;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM: return *ch = 4, 8;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
      return *ch = 4, 4;
    case DXGI_FORMAT_R11G11B10_FLOAT: return *ch = 3, 4;
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: return *ch = 2, 4;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
      return *ch = 1, 4;
    case DXGI_FORMAT_R8G8_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: return *ch = 2, 2;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UINT: return *ch = 1, 2;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT: return *ch = 1, 1;
    default: return *ch = 0, 0;
  }
}

void decode(DXGI_FORMAT f, const uint8_t* p, float* o) {
  uint32_t u;
  uint16_t h[4];
  switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT: std::memcpy(o, p, 16); return;
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT: std::memcpy(o, p, 8); return;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_D32_FLOAT: std::memcpy(o, p, 4); return;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
      std::memcpy(h, p, 8);
      for (int i = 0; i < 4; ++i) o[i] = half(h[i]);
      return;
    case DXGI_FORMAT_R16G16B16A16_UNORM:
      std::memcpy(h, p, 8);
      for (int i = 0; i < 4; ++i) o[i] = h[i] / 65535.f;
      return;
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT:
      std::memcpy(h, p, 4);
      o[0] = half(h[0]), o[1] = half(h[1]);
      return;
    case DXGI_FORMAT_R16G16_UNORM:
      std::memcpy(h, p, 4);
      o[0] = h[0] / 65535.f, o[1] = h[1] / 65535.f;
      return;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT:
      std::memcpy(h, p, 2);
      o[0] = half(h[0]);
      return;
    case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_D16_UNORM:
      std::memcpy(h, p, 2);
      o[0] = h[0] / 65535.f;
      return;
    case DXGI_FORMAT_R16_UINT:
      std::memcpy(h, p, 2);
      o[0] = h[0];
      return;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R10G10B10A2_UNORM:
      std::memcpy(&u, p, 4);
      o[0] = (u & 1023) / 1023.f, o[1] = ((u >> 10) & 1023) / 1023.f, o[2] = ((u >> 20) & 1023) / 1023.f;
      o[3] = (u >> 30) / 3.f;
      return;
    case DXGI_FORMAT_R11G11B10_FLOAT:
      std::memcpy(&u, p, 4);
      o[0] = small_float(u & 2047, 6), o[1] = small_float((u >> 11) & 2047, 6), o[2] = small_float(u >> 22, 5);
      return;
    case DXGI_FORMAT_R32_UINT:
      std::memcpy(&u, p, 4);
      o[0] = static_cast<float>(u);
      return;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
      std::memcpy(&u, p, 4);
      o[0] = (u & 0xffffff) / 16777215.f;
      return;
    case DXGI_FORMAT_R8_UINT: o[0] = p[0]; return;
    default:  // 8-bit unorm, 1..4 channels, in memory order (B8G8R8A8: b, g, r, a)
      for (int i = 0; i < 4; ++i) o[i] = p[i] / 255.f;
  }
}

struct Stats {
  uint32_t ch = 0;
  float mn[4], mx[4];
  uint64_t nan = 0, inf = 0, n = 0;
};

void stats_add(Stats& s, const float* v) {
  for (uint32_t c = 0; c < s.ch; ++c) {
    const float x = v[c];
    if (std::isnan(x)) { ++s.nan; continue; }
    if (std::isinf(x)) { ++s.inf; continue; }
    if (!s.n || x < s.mn[c]) s.mn[c] = x;
    if (!s.n || x > s.mx[c]) s.mx[c] = x;
  }
  ++s.n;
}

std::string stats_text(const Stats& s, bool json) {
  if (!s.ch) return json ? "null" : "stats: format not decoded";
  char b[96];
  std::string mn, mx;
  for (uint32_t c = 0; c < s.ch; ++c) {
    std::snprintf(b, sizeof b, json ? "%s%.9g" : "%s%.6g", c ? (json ? ", " : " ") : "", s.mn[c]);
    mn += b;
    std::snprintf(b, sizeof b, json ? "%s%.9g" : "%s%.6g", c ? (json ? ", " : " ") : "", s.mx[c]);
    mx += b;
  }
  if (!s.n) mn = mx = json ? "" : "-";
  std::snprintf(b, sizeof b, json ? "\"nan\": %llu, \"inf\": %llu" : "nan %llu inf %llu",
                static_cast<unsigned long long>(s.nan), static_cast<unsigned long long>(s.inf));
  if (json) return "{\"channels\": " + std::to_string(s.ch) + ", \"min\": [" + mn + "], \"max\": [" + mx + "], " + b + "}";
  return "min (" + mn + ") max (" + mx + ") " + b;
}

// The typed format a DDS reader expects for a typeless resource's data.
DXGI_FORMAT dds_format(DXGI_FORMAT f) {
  switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    default: return f;
  }
}

// DDS + DX10 header (DirectXTex / numpy readers): one mip, `slices` array slices or a volume of `depth`.
std::vector<uint8_t> dds_header(DXGI_FORMAT f, uint32_t w, uint32_t h, uint32_t depth, uint32_t slices, bool volume,
                                bool one_d) {
  std::vector<uint8_t> b(4 + 124 + 20, 0);
  auto put = [&](size_t at, uint32_t v) { std::memcpy(b.data() + at, &v, 4); };
  std::memcpy(b.data(), "DDS ", 4);
  put(4, 124);
  put(8, 0x1 | 0x2 | 0x4 | 0x1000 | (volume ? 0x800000u : 0u));  // CAPS HEIGHT WIDTH PIXELFORMAT [DEPTH]
  put(12, h);
  put(16, w);
  put(24, volume ? depth : 0);
  put(28, 1);
  put(4 + 72, 32);      // ddspf.size
  put(4 + 76, 0x4);     // DDPF_FOURCC
  std::memcpy(b.data() + 4 + 80, "DX10", 4);
  put(4 + 104, 0x1000);  // DDSCAPS_TEXTURE
  put(4 + 108, volume ? 0x200000u : 0u);
  put(128, static_cast<uint32_t>(f));
  put(132, volume ? 4u : one_d ? 2u : 3u);  // D3D10_RESOURCE_DIMENSION
  put(136, 0);
  put(140, volume ? 1u : slices);
  put(144, 0);
  return b;
}

// ---- spec parsing (on the caller's thread) ----

uint32_t parse_hex(const std::string& s) {
  std::string t = s.rfind("0x", 0) == 0 || s.rfind("0X", 0) == 0 ? s.substr(2) : s;
  if (t.empty() || t.size() > 8 || t.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return 0;
  return static_cast<uint32_t>(std::strtoul(t.c_str(), nullptr, 16));
}

// Fills r from the spec; "" = ok, else why not.
std::string parse(Req& r) {
  std::string block, flow, node, err, vsout;
  bool have_phase = false;
  size_t a = 0;
  const std::string& s = r.spec;
  while (a < s.size()) {
    while (a < s.size() && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    size_t b = a;
    while (b < s.size() && !std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    if (b == a) break;
    const std::string tok = s.substr(a, b - a);
    a = b;
    const size_t colon = tok.find(':');
    if (colon == std::string::npos) return "'" + tok + "' is not key:value";
    const std::string k = tok.substr(0, colon), v = tok.substr(colon + 1);
    if (k == "block") block = v;
    else if (k == "flow") flow = v;
    else if (k == "vsout") vsout = v;
    else if (k == "blas") r.with_blas = v == "1" || v == "on" || v == "true";
    else if (k == "draws") r.draws_want = std::min<uint32_t>(kMaxDraws, std::max(1, std::atoi(v.c_str())));
    else if (k == "after" || k == "before") {
      if (have_phase) return "one of after: / before:";
      have_phase = true;
      r.after = k == "after";
      node = v;
    } else if (k == "mip") r.mip = static_cast<uint32_t>(std::atoi(v.c_str()));
    else if (k == "frames") r.frames = std::max<uint64_t>(1, std::strtoull(v.c_str(), nullptr, 10));
    else return "unknown key '" + k + "' (block: flow: vsout: after: before: mip: draws: blas: frames:)";
  }
  if (r.with_blas && vsout.empty()) return "blas:1 goes with vsout:<VS hash> (the meshes it draws)";
  if (!vsout.empty()) {
    if (!block.empty() || !flow.empty() || have_phase) return "vsout: takes no block: / flow: / after: / before:";
    if (vsout.size() < 8 || vsout.size() > 32 || vsout.size() % 2 ||
        vsout.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos)
      return "vsout:'" + vsout + "' is not 8..32 hex digits (a VS hash or its prefix)";
    r.kind = Kind::kVsOut;
    r.vs_len = static_cast<uint32_t>(vsout.size() / 2);
    for (uint32_t i = 0; i < r.vs_len; ++i)
      r.vs_hash[i] = static_cast<uint8_t>(std::strtoul(vsout.substr(2 * i, 2).c_str(), nullptr, 16));
    return {};
  }
  if (block.empty() == flow.empty()) return "name one of block:<name> / flow:<name> / vsout:<VS hash>";
  if (!block.empty()) {
    r.kind = Kind::kBlock;
    r.block = engine_blocks::find(block.c_str());
    if (r.block < 0 && !block.empty() && block.find_first_not_of("0123456789") == std::string::npos)
      r.block = std::atoi(block.c_str()) < static_cast<int>(engine_blocks::count()) ? std::atoi(block.c_str()) : -1;
    if (r.block < 0) {
      std::string names;
      for (uint32_t i = 0; i < engine_blocks::count(); ++i) names += std::string(i ? ", " : "") + engine_blocks::name(i);
      return "no engine block '" + block + "' (the catalog: " + names + ")";
    }
    return {};
  }
  r.kind = Kind::kFlow;
  r.flow = flow;
  r.name_hash = flow.find('/') != std::string::npos ? render_flow::client_name_hash(flow.c_str()) : parse_hex(flow);
  if (!r.name_hash) return "flow:'" + flow + "' is neither 8 hex digits (an engine name hash) nor a published <mod>/<name>";
  if (node.empty()) return "a flow needs after:<node> or before:<node>";
  r.node = node;
  r.node_hash = RED4ext::CName(node.c_str()).hash;
  return {};
}

// ---- the copy, at the node (g_mu held) ----

void copy_at(Req& r, ID3D12Resource* res, uint32_t id) {
  ID3D12Device* dev = device();
  const uint8_t* lctx = static_cast<const uint8_t*>(reinterpret_cast<void* (*)()>(g_base + e231::kGetCommandListContext)());
  auto* cl = lctx ? *reinterpret_cast<ID3D12GraphicsCommandList* const*>(lctx + e231::kCtxCmdList) : nullptr;
  if (!dev || !cl) return fail(r, "no engine device / no command list bound at the node");
  const D3D12_RESOURCE_DESC d = res->GetDesc();
  if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return fail(r, "the name resolves to a buffer (textures only)");
  if (d.SampleDesc.Count > 1) return fail(r, "multisampled (x" + std::to_string(d.SampleDesc.Count) + ") - not copyable");
  if (r.mip >= d.MipLevels) return fail(r, "mip " + std::to_string(r.mip) + " of " + std::to_string(d.MipLevels));
  const bool volume = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  const uint32_t slices = volume ? 1u : d.DepthOrArraySize;
  r.fp.resize(slices);
  r.rows.resize(slices);
  r.row_bytes.resize(slices);
  uint64_t total = 0;
  for (uint32_t s = 0; s < slices; ++s) {
    UINT64 bytes = 0;
    dev->GetCopyableFootprints(&d, r.mip + s * d.MipLevels, 1, 0, &r.fp[s], &r.rows[s], &r.row_bytes[s], &bytes);
    r.fp[s].Offset = total;
    total += (bytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~uint64_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  }
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = D3D12_HEAP_TYPE_READBACK;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;
  bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  const HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&r.rb));
  if (FAILED(hr)) {
    char why[96];
    std::snprintf(why, sizeof why, "readback buffer of %llu bytes not created, hr 0x%08lx",
                  static_cast<unsigned long long>(bd.Width), static_cast<unsigned long>(hr));
    r.rb = nullptr;
    return fail(r, why);
  }
  r.marker_off = total;
  void* m = nullptr;
  const D3D12_RANGE none{0, 0};
  if (SUCCEEDED(r.rb->Map(0, &none, &m))) {  // the marker's slot cleared: only the GPU's write sets it
    std::memset(static_cast<uint8_t*>(m) + total, 0, 8);
    const D3D12_RANGE wrote{static_cast<SIZE_T>(total), static_cast<SIZE_T>(total + 8)};
    r.rb->Unmap(0, &wrote);
  }
  if (!render_flow::transition_here(id, D3D12_RESOURCE_STATE_COPY_SOURCE)) return fail(r, "no command-list context");
  for (uint32_t s = 0; s < slices; ++s) {
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = r.rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = r.fp[s];
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = res;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = r.mip + s * d.MipLevels;  // plane 0
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  }
  ID3D12GraphicsCommandList2* cl2 = nullptr;
  if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&cl2)))) {
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER p{r.rb->GetGPUVirtualAddress() + total,
                                                 kMarker ^ static_cast<uint32_t>(r.id)};
    const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    cl2->WriteBufferImmediate(1, &p, &mode);
    cl2->Release();
    r.marker = true;
  }
  r.desc = d;
  r.gpu_id = id;
  r.copy_frame = g_frame.load(std::memory_order_relaxed);
  g_armed.fetch_sub(1, std::memory_order_relaxed);
  r.st = St::kCopied;
  set_text(r, "pending: copied at frame " + std::to_string(r.copy_frame) + ", waiting for the GPU");
  g_sdk->logger->InfoF(g_handle, "DEV readback #%d: %s %s '%s' - texture id %u, %llux%u x%u, %u mip(s), DXGI %u, "
                       "flags 0x%x; mip %u copied (%llu bytes, %u slice(s))%s%s", r.id, r.after ? "after" : "before",
                       r.node.c_str(), r.flow.c_str(), id, static_cast<unsigned long long>(d.Width), d.Height,
                       static_cast<unsigned>(d.DepthOrArraySize), static_cast<unsigned>(d.MipLevels),
                       static_cast<unsigned>(d.Format), static_cast<unsigned>(d.Flags), r.mip,
                       static_cast<unsigned long long>(total), slices, r.marker ? "" : " - NO marker (no List2)",
                       r.job ? " - a job-builder node: 'after' is before its job's work" : "");
}

// ---- the files (a worker thread; the request is kWriting, nothing else touches it) ----

bool write_file(const fs::path& p, const void* a, size_t na, const void* b, size_t nb) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  if (na) out.write(static_cast<const char*>(a), static_cast<std::streamsize>(na));
  if (nb) out.write(static_cast<const char*>(b), static_cast<std::streamsize>(nb));
  return static_cast<bool>(out);
}

void finish(Req* r, std::vector<uint8_t> data, std::string json_extra, Stats st, const fs::path& base,
            const char* ext, std::vector<uint8_t> header) {
  std::error_code ec;
  fs::create_directories(g_dir, ec);
  const fs::path file = base.string() + ext, meta = base.string() + ".json";
  const bool ok = write_file(file, header.data(), header.size(), data.data(), data.size());
  const std::string stats = stats_text(st, true);
  std::string j = "{\n  \"id\": " + std::to_string(r->id) + ",\n  \"spec\": " + json_str(r->spec) +
                  ",\n  \"frame\": " + std::to_string(r->info.frame) + ",\n  \"file\": " + json_str(file.filename().string()) +
                  ",\n  \"header_bytes\": " + std::to_string(header.size()) + ",\n  \"bytes\": " +
                  std::to_string(data.size()) + ",\n" + json_extra + "  \"stats\": " + stats + "\n}\n";
  const bool ok_meta = write_file(meta, j.data(), j.size(), nullptr, 0);
  std::lock_guard lock(g_mu);
  if (!ok || !ok_meta) return fail(*r, "cannot write " + (ok ? meta : file).string());
  std::snprintf(r->info.path, sizeof r->info.path, "%s", file.string().c_str());
  r->info.bytes = data.size();
  r->info.state = RG_RB_DONE;
  r->st = St::kDone;
  char dims[160];
  if (r->kind == Kind::kBlock)
    std::snprintf(dims, sizeof dims, "%llu bytes (float32 view)", static_cast<unsigned long long>(data.size()));
  else if (r->kind == Kind::kVsOut)
    std::snprintf(dims, sizeof dims, "%u vertices in %u draw(s), stride %u B (stats: SV_Position x y z w)",
                  r->info.width, r->info.height, r->info.row_bytes);
  else
    std::snprintf(dims, sizeof dims, "%ux%ux%u x%u slice(s), mip %u, DXGI %u", r->info.width, r->info.height,
                  r->info.depth, r->info.slices, r->info.mip, r->info.format);
  const std::string text = std::string("done: ") + file.string() + " | " + dims + " | " + stats_text(st, false);
  set_text(*r, text);
  g_sdk->logger->InfoF(g_handle, "DEV readback #%d '%s' -> %s", r->id, r->spec.c_str(), text.c_str());
}

// The marker is there (or the wait is over): out of the buffer into a tight copy, then the worker. g_mu held.
void harvest(Req& r) {
  void* m = nullptr;
  const D3D12_RANGE all{0, static_cast<SIZE_T>(r.marker_off + 8)};
  if (FAILED(r.rb->Map(0, &all, &m))) return fail(r, "the readback buffer does not map");
  const auto* b = static_cast<const uint8_t*>(m);
  uint32_t mark = 0;
  std::memcpy(&mark, b + r.marker_off, 4);
  const uint64_t waited = g_frame.load(std::memory_order_relaxed) - r.copy_frame;
  if (r.marker ? mark != (kMarker ^ static_cast<uint32_t>(r.id)) : waited < 8) {
    const D3D12_RANGE none{0, 0};
    r.rb->Unmap(0, &none);
    if (waited > kGpuFrames) fail(r, "the copy was recorded but never executed on the GPU (no marker after " +
                                      std::to_string(kGpuFrames) + " frames)");
    return;
  }
  const auto fmt = static_cast<DXGI_FORMAT>(r.fp[0].Footprint.Format);
  uint32_t ch = 0;
  const uint32_t tb = texel_info(fmt, &ch);
  Stats st;
  st.ch = ch;
  std::vector<uint8_t> data;
  const uint32_t slices = static_cast<uint32_t>(r.fp.size());
  const uint32_t depth = r.fp[0].Footprint.Depth;
  data.reserve(size_t(r.row_bytes[0]) * r.rows[0] * depth * slices);
  float v[4];
  for (uint32_t s = 0; s < slices; ++s) {
    const auto& f = r.fp[s];
    for (uint32_t z = 0; z < f.Footprint.Depth; ++z)
      for (uint32_t y = 0; y < r.rows[s]; ++y) {
        const uint8_t* row = b + f.Offset + (uint64_t(z) * r.rows[s] + y) * f.Footprint.RowPitch;
        data.insert(data.end(), row, row + r.row_bytes[s]);
        if (tb)
          for (uint32_t x = 0; x < f.Footprint.Width; ++x) {
            decode(fmt, row + size_t(x) * tb, v);
            stats_add(st, v);
          }
      }
  }
  const D3D12_RANGE none{0, 0};
  r.rb->Unmap(0, &none);
  r.rb->Release();
  r.rb = nullptr;
  const DXGI_FORMAT out_fmt = dds_format(fmt);
  r.info.format = out_fmt;
  r.info.width = r.fp[0].Footprint.Width;
  r.info.height = r.fp[0].Footprint.Height;
  r.info.depth = depth;
  r.info.slices = slices;
  r.info.mip = r.mip;
  r.info.row_bytes = static_cast<uint32_t>(r.row_bytes[0]);
  r.info.frame = r.copy_frame;
  char extra[1024];
  std::snprintf(extra, sizeof extra,
                "  \"kind\": \"flow\",\n  \"flow\": %s,\n  \"name_hash\": \"%08x\",\n  \"node\": %s,\n  \"phase\": \"%s\",\n"
                "  \"job_builder_node\": %s,\n  \"gpuapi_texture_id\": %u,\n  \"resource\": {\"dimension\": %u, "
                "\"width\": %llu, \"height\": %u, \"depth_or_array\": %u, \"mips\": %u, \"dxgi_format\": %u, "
                "\"flags\": %u},\n  \"mip\": %u,\n  \"width\": %u,\n  \"height\": %u,\n  \"depth\": %u,\n"
                "  \"slices\": %u,\n  \"dxgi_format\": %u,\n  \"footprint_format\": %u,\n  \"row_bytes\": %u,\n"
                "  \"rows\": %u,\n  \"gpu_marker\": %s,\n  \"state_after\": \"COPY_SOURCE (the engine's tracker re-binds from there)\",\n",
                json_str(r.flow).c_str(), r.name_hash, json_str(r.node).c_str(), r.after ? "after" : "before",
                r.job ? "true" : "false", r.gpu_id, static_cast<unsigned>(r.desc.Dimension),
                static_cast<unsigned long long>(r.desc.Width), r.desc.Height,
                static_cast<unsigned>(r.desc.DepthOrArraySize), static_cast<unsigned>(r.desc.MipLevels),
                static_cast<unsigned>(r.desc.Format), static_cast<unsigned>(r.desc.Flags), r.mip, r.info.width,
                r.info.height, depth, slices, static_cast<unsigned>(out_fmt), static_cast<unsigned>(fmt),
                r.info.row_bytes, r.rows[0], r.marker ? "true" : "false");
  const fs::path base = g_dir / ("rb" + std::to_string(r.id) + "_flow_" + safe(r.flow) + "_" +
                                 (r.after ? "after_" : "before_") + safe(r.node));
  r.st = St::kWriting;
  set_text(r, "pending: writing the files");
  std::thread(finish, &r, std::move(data), std::string(extra), st, base, ".dds",
              dds_header(out_fmt, r.info.width, r.info.height, depth, slices,
                         r.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D,
                         r.desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE1D))
      .detach();
}

void block_tick(Req& r) {
  static uint8_t buf[4096];
  uint32_t size = 0;
  const uint64_t seq = engine_blocks::latest(static_cast<uint32_t>(r.block), buf, sizeof buf, &size);
  if (seq <= r.seq0) return;
  const uint32_t kept = size < sizeof buf ? size : static_cast<uint32_t>(sizeof buf);
  std::vector<uint8_t> data(buf, buf + kept);
  Stats st;
  st.ch = 1;
  for (uint32_t i = 0; i + 4 <= kept; i += 4) {
    float f;
    std::memcpy(&f, buf + i, 4);
    stats_add(st, &f);
  }
  r.info.frame = g_frame.load(std::memory_order_relaxed);
  char extra[512];
  std::snprintf(extra, sizeof extra,
                "  \"kind\": \"block\",\n  \"block_id\": %d,\n  \"block\": %s,\n  \"write_seq\": %llu,\n"
                "  \"engine_size\": %u,\n  \"truncated\": %s,\n", r.block, json_str(engine_blocks::name(r.block)).c_str(),
                static_cast<unsigned long long>(seq), size, size > kept ? "true" : "false");
  const fs::path base = g_dir / ("rb" + std::to_string(r.id) + "_block_" + safe(engine_blocks::name(r.block)));
  r.st = St::kWriting;
  std::thread(finish, &r, std::move(data), std::string(extra), st, base, ".bin", std::vector<uint8_t>()).detach();
}

// ---- vsout: the outputs of an engine VS at its next draw, by Stream Output (g_mu held unless said) ----

void vs_want(const Req& r) {
  for (VsWant& w : g_vs_want)
    if (!w.len.load(std::memory_order_relaxed)) {
      std::memcpy(w.hash, r.vs_hash, 16);
      w.len.store(r.vs_len, std::memory_order_release);
      return;
    }
}

void vs_unwant(const Req& r) {
  for (VsWant& w : g_vs_want)
    if (w.len.load(std::memory_order_relaxed) == r.vs_len && std::memcmp(w.hash, r.vs_hash, r.vs_len) == 0) {
      w.len.store(0, std::memory_order_release);
      return;
    }
}

void release_draws(Req& r) {
  for (SoDraw& d : r.draws) {
    if (d.so) d.so->Release();
    if (d.rb) d.rb->Release();
    d.so = d.rb = nullptr;
  }
  for (auto& p : r.so_psos)
    if (p.second) p.second->Release();
  r.so_psos.clear();
  for (BlasCall& c : r.calls) {
    if (c.dst) c.dst->Release();
    if (c.rb) c.rb->Release();
    c.dst = c.rb = nullptr;
  }
}

// The VS's output signature (DXIL "OSG1", or "OSGN") -> the elements Stream Output writes, in register order. "" = ok.
std::string so_layout(const rg_blob& vs, std::vector<SoElem>* out, uint32_t* stride) {
  const auto* b = static_cast<const uint8_t*>(vs.bytes);
  if (!b || vs.size < 32 || std::memcmp(b, "DXBC", 4) != 0) return "the VS is not a DXBC container";
  uint32_t chunks = 0;
  std::memcpy(&chunks, b + 28, 4);
  for (uint32_t c = 0; c < chunks && 32 + 4 * (c + 1) <= vs.size; ++c) {
    uint32_t off = 0, size = 0;
    std::memcpy(&off, b + 32 + 4 * c, 4);
    if (off + 8 > vs.size) continue;
    const bool sg1 = std::memcmp(b + off, "OSG1", 4) == 0;
    if (!sg1 && std::memcmp(b + off, "OSGN", 4) != 0) continue;
    std::memcpy(&size, b + off + 4, 4);
    const uint8_t* p = b + off + 8;
    if (off + 8 + size > vs.size || size < 8) return "a broken output signature";
    uint32_t n = 0, first = 0;
    std::memcpy(&n, p, 4);
    std::memcpy(&first, p + 4, 4);
    const uint32_t es = sg1 ? 32 : 24;
    if (first + uint64_t(n) * es > size) return "a broken output signature";
    for (uint32_t i = 0; i < n; ++i) {
      const uint8_t* e = p + first + i * es + (sg1 ? 4 : 0);  // OSG1: a u32 stream first
      uint32_t stream = 0, name_off = 0;
      if (sg1) std::memcpy(&stream, p + first + i * es, 4);
      if (stream) continue;
      SoElem x;
      std::memcpy(&name_off, e, 4);
      std::memcpy(&x.index, e + 4, 4);
      std::memcpy(&x.sysval, e + 8, 4);
      std::memcpy(&x.type, e + 12, 4);
      std::memcpy(&x.reg, e + 16, 4);
      x.mask = e[20];
      if (sg1) std::memcpy(&x.minprec, e + 24, 4);
      if (name_off >= size) return "a broken output signature";
      const auto* nm = reinterpret_cast<const char*>(p + name_off);
      x.name.assign(nm, strnlen(nm, size - name_off));
      if (!x.mask) continue;
      unsigned long lo = 0;
      _BitScanForward(&lo, x.mask);
      x.start = lo;
      x.count = static_cast<uint32_t>(__popcnt(x.mask));
      out->push_back(std::move(x));
    }
    std::sort(out->begin(), out->end(), [](const SoElem& a, const SoElem& c) {
      return a.reg != c.reg ? a.reg < c.reg : a.start < c.start;
    });
    uint32_t at = 0;
    for (SoElem& x : *out) {
      x.offset = at;
      at += 4 * x.count;
    }
    *stride = at;
    if (out->empty()) return "the VS outputs nothing";
    if (at > 2048 || out->size() > 64) return "the VS outputs more than Stream Output takes (2048 B / 64 entries)";
    return {};
  }
  return "no output signature in the VS";
}

ID3D12PipelineState* so_pso_for(Req& r, ID3D12PipelineState* engine, ID3D12PipelineState* bound, const rg_blob& vs,
                                 long* hr_out) {
  for (auto& p : r.so_psos)
    if (p.first == bound) return *hr_out = 0, p.second;
  std::vector<D3D12_SO_DECLARATION_ENTRY> decl;
  for (const SoElem& x : r.elems)
    decl.push_back({0, x.name.c_str(), x.index, static_cast<BYTE>(x.start), static_cast<BYTE>(x.count), 0});
  const UINT stride = r.stride;
  D3D12_STREAM_OUTPUT_DESC so{};
  so.pSODeclaration = decl.data();
  so.NumEntries = static_cast<UINT>(decl.size());
  so.pBufferStrides = &stride;
  so.NumStrides = 1;
  so.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
  ID3D12PipelineState* pso = nullptr;
  *hr_out = pso::dev_so_pso(engine, vs, &so, &pso);
  r.so_psos.emplace_back(bound, SUCCEEDED(*hr_out) ? pso : nullptr);
  return SUCCEEDED(*hr_out) ? pso : nullptr;
}

ID3D12Resource* buffer(D3D12_HEAP_TYPE heap, uint64_t bytes, D3D12_RESOURCE_STATES state, long* hr) {
  D3D12_HEAP_PROPERTIES hp{};
  hp.Type = heap;
  D3D12_RESOURCE_DESC bd{};
  bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  bd.Width = bytes;
  bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
  bd.SampleDesc.Count = 1;
  bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ID3D12Resource* res = nullptr;
  ID3D12Device* dev = device();
  *hr = dev ? dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, state, nullptr, IID_PPV_ARGS(&res)) : E_FAIL;
  return SUCCEEDED(*hr) ? res : nullptr;
}

std::string hex16(const uint8_t* h) {
  char s[33];
  for (int i = 0; i < 16; ++i) std::snprintf(s + 2 * i, 3, "%02x", h[i]);
  return s;
}

// The control for a refused SO PSO: does this runtime / driver build ANY Stream Output PSO from a DXIL VS - our own VS on
// our own root signature with ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | ALLOW_STREAM_OUTPUT.
std::string so_control() {
  static const char* kSrc = "float4 main(float4 p : POSITION) : SV_Position { return p; }";
  rg_hlsl_desc hd{sizeof(rg_hlsl_desc), kSrc, nullptr, "main", "vs_6_0", nullptr, 0, nullptr};
  rg_blob vs{};
  char err[256];
  if (hlsl::compile(&hd, &vs, err, sizeof err) != 0) return std::string("not run (compile: ") + err + ")";
  ID3D12Device* dev = device();
  D3D12_ROOT_SIGNATURE_DESC rd{0, nullptr, 0, nullptr, static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(0x41)};
  ID3DBlob *blob = nullptr, *eb = nullptr;
  ID3D12RootSignature* rs = nullptr;
  if (!dev || FAILED(D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &eb)) ||
      FAILED(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)))) {
    if (blob) blob->Release();
    if (eb) eb->Release();
    return "not run (root signature)";
  }
  blob->Release();
  const D3D12_INPUT_ELEMENT_DESC il{"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
                                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
  const D3D12_SO_DECLARATION_ENTRY e{0, "SV_Position", 0, 0, 4, 0};
  const UINT st = 16;
  D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
  d.pRootSignature = rs;
  d.VS = {vs.bytes, vs.size};
  d.StreamOutput = {&e, 1, &st, 1, D3D12_SO_NO_RASTERIZED_STREAM};
  d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  d.SampleMask = UINT_MAX;
  d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  d.InputLayout = {&il, 1};
  d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  d.SampleDesc.Count = 1;
  ID3D12PipelineState* pso = nullptr;
  const HRESULT hr = dev->CreateGraphicsPipelineState(&d, IID_PPV_ARGS(&pso));
  if (pso) pso->Release();
  rs->Release();
  char b[64];
  std::snprintf(b, sizeof b, "hr 0x%08lx", static_cast<unsigned long>(hr));
  return b;
}

// One draw of request r: the SO companion on the engine's list right after the engine's draw, then the copy + marker.
void capture_draw(Req& r, uint8_t* ctx, ID3D12PipelineState* engine, ID3D12PipelineState* bound,
                  const rg_draw_info& info) {
  auto* cl = *reinterpret_cast<ID3D12GraphicsCommandList**>(ctx + e231::kCtxCmdList);
  const uint64_t frame = g_frame.load(std::memory_order_relaxed);
  if (!r.draws.empty() && frame != r.draws[0].frame) return void(++r.skipped_frame);  // one frame: the first draw's
  if (info.kind == RG_DRAW_INDIRECT || info.kind == RG_DRAW_INDEXED_INDIRECT || !info.count || !info.instances)
    return void(++r.skipped_indirect);  // the counts are on the GPU: no size for the buffer
  SoDraw d;
  if (!blas_census::ia_views(&d.ia)) return void(++r.skipped_ia);  // the IB tracking starts with the next bind
  rg_blob vs{};
  if (!pso::dev_vs_of(engine, bound, &vs, d.vs_used)) return fail(r, "the draw's VS bytes are not known");
  if (r.elems.empty()) {
    const std::string why = so_layout(vs, &r.elems, &r.stride);
    if (!why.empty()) return fail(r, why);
  }
  long hr = 0;
  ID3D12PipelineState* so_pso = so_pso_for(r, engine, bound, vs, &hr);
  if (!so_pso) {
    // What the runtime refuses: the same PSO without Stream Output, with one SV_Position entry, with one other entry.
    std::string sig;
    for (const SoElem& x : r.elems)
      sig += " " + x.name + std::to_string(x.index) + "(r" + std::to_string(x.reg) + " m" + std::to_string(x.mask) + " sv" +
             std::to_string(x.sysval) + " t" + std::to_string(x.type) + " mp" + std::to_string(x.minprec) + ")";
    long ladder[4] = {};
    for (int k = 0; k < 4; ++k) {
      D3D12_STREAM_OUTPUT_DESC so{};
      D3D12_SO_DECLARATION_ENTRY e{};
      UINT st = 0;
      const SoElem* pick = nullptr;
      for (const SoElem& x : r.elems)
        if ((k == 1 || k == 3) == (x.sysval == 1) && !pick) pick = &x;
      if (k && !pick) {
        ladder[k] = -1;
        continue;
      }
      if (k) {
        e = {0, pick->name.c_str(), pick->index, static_cast<BYTE>(pick->start), static_cast<BYTE>(pick->count), 0};
        st = 4 * pick->count;
        so = {&e, 1, &st, 1, D3D12_SO_NO_RASTERIZED_STREAM};
      }
      ID3D12PipelineState* t = nullptr;
      void* rs = nullptr;
      ladder[k] = pso::dev_so_pso(engine, vs, &so, &t, k == 3, &rs);
      if (t) t->Release();
      if (!k) {
        const auto* dd = *reinterpret_cast<const uint8_t* const*>(g_base + e231::kDeviceData);
        const void* gfx = dd ? *reinterpret_cast<void* const*>(dd + 0x1410638) : nullptr;
        const void* cs = dd ? *reinterpret_cast<void* const*>(dd + 0x1410640) : nullptr;
        g_sdk->logger->WarnF(g_handle, "DEV readback #%d: the PSO's root signature %p; the engine's graphics one %p, "
                             "compute %p", r.id, rs, gfx, cs);
      }
    }
    g_sdk->logger->WarnF(g_handle, "DEV readback #%d: SO PSO refused - without SO hr 0x%08lx, SV_Position only 0x%08lx, "
                         "one other element 0x%08lx, SV_Position with no targets 0x%08lx; own root signature 0x41 + own "
                         "VS: %s; output signature:%s", r.id, static_cast<unsigned long>(ladder[0]),
                         static_cast<unsigned long>(ladder[1]), static_cast<unsigned long>(ladder[2]),
                         static_cast<unsigned long>(ladder[3]), so_control().c_str(), sig.c_str());
    char why[220];
    std::snprintf(why, sizeof why, "the Stream Output PSO was not created, hr 0x%08lx (the engine's root signature "
                  "without ALLOW_STREAM_OUTPUT? an output Stream Output does not take?)", static_cast<unsigned long>(hr));
    return fail(r, why);
  }
  std::memcpy(&d.topology, ctx + e231::kCtxTopology, 4);
  const uint64_t prims = d.topology == D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP ? 3 : 1;  // a strip is written as a list
  d.bytes = std::min<uint64_t>(kMaxSoBytes, uint64_t(info.count) * info.instances * prims * r.stride);
  d.so = buffer(D3D12_HEAP_TYPE_DEFAULT, kSoHeader + d.bytes, D3D12_RESOURCE_STATE_STREAM_OUT, &hr);  // zeroed: counter 0
  d.rb = d.so ? buffer(D3D12_HEAP_TYPE_READBACK, kSoHeader + d.bytes + 8, D3D12_RESOURCE_STATE_COPY_DEST, &hr) : nullptr;
  if (!d.so || !d.rb) {
    if (d.so) d.so->Release();
    char why[120];
    std::snprintf(why, sizeof why, "buffers of %llu bytes not created, hr 0x%08lx",
                  static_cast<unsigned long long>(d.bytes), static_cast<unsigned long>(hr));
    return fail(r, why);
  }
  const uint64_t va = d.so->GetGPUVirtualAddress();
  const D3D12_STREAM_OUTPUT_BUFFER_VIEW view{va + kSoHeader, d.bytes, va};
  cl->SOSetTargets(0, 1, &view);
  cl->SetPipelineState(so_pso);
  passes::issue_draw(ctx, cl, info);
  const D3D12_STREAM_OUTPUT_BUFFER_VIEW none{};
  cl->SOSetTargets(0, 1, &none);
  cl->SetPipelineState(bound);  // the list as the engine left it (ctx+0x3d0)
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = d.so;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_STREAM_OUT;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  cl->ResourceBarrier(1, &b);
  cl->CopyBufferRegion(d.rb, 0, d.so, 0, kSoHeader + d.bytes);
  ID3D12GraphicsCommandList2* cl2 = nullptr;
  if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&cl2)))) {
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER p{
        d.rb->GetGPUVirtualAddress() + kSoHeader + d.bytes,
        kMarker ^ static_cast<uint32_t>(r.id) ^ static_cast<uint32_t>(r.draws.size())};
    const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    cl2->WriteBufferImmediate(1, &p, &mode);
    cl2->Release();
    r.marker = true;
  }
  if (r.draws.empty()) {  // cb12 as the engine last wrote it - the frame these draws were recorded in
    static uint8_t cb[4096];
    uint32_t size = 0;
    r.cb12_seq = engine_blocks::latest(3, cb, sizeof cb, &size);
    r.cb12.assign(cb, cb + std::min<uint32_t>(size, sizeof cb));
  }
  d.info = info;
  d.frame = frame;
  d.clone = bound != engine;
  d.engine = engine;
  r.draws.push_back(d);
  if (r.draws.size() == 1)
    g_sdk->logger->InfoF(g_handle, "DEV readback #%d: vsout first draw - kind %u, %u x %u, topology %u, ib 0x%llx, VS %s%s; "
                         "%zu output element(s), stride %u", r.id, info.kind, info.count, info.instances, d.topology,
                         static_cast<unsigned long long>(d.ia.ib), hex16(d.vs_used).c_str(),
                         d.clone ? " (a patch's clone)" : "", r.elems.size(), r.stride);
  if (r.draws.size() >= r.draws_want && !r.with_blas) {  // blas:1 waits for its capture frame's end
    g_vs_armed.fetch_sub(1, std::memory_order_relaxed);
    vs_unwant(r);
    r.st = St::kCopied;
    r.copy_frame = frame;
    set_text(r, "pending: " + std::to_string(r.draws.size()) + " draw(s) captured at frame " + std::to_string(frame) +
                    ", waiting for the GPU");
  }
}

// ---- blas:1 - the inputs of the BLAS builds of the meshes a vsout request draws (g_mu held unless said) ----

// A copy by GPU VA - no ID3D12Resource needed: our CS reads a root SRV at the source VA (ByteAddressBuffer) and writes
// a root UAV of our buffer, on the build's own list right before the build. The inputs are in the state the build
// needs (NON_PIXEL_SHADER_RESOURCE) - a shader read, no barrier on the engine's buffers.
ID3D12RootSignature* g_copy_rs = nullptr;
ID3D12PipelineState* g_copy_pso = nullptr;
std::string g_copy_err;  // set once the setup failed

std::string copy_setup() {
  if (g_copy_pso) return {};
  if (!g_copy_err.empty()) return g_copy_err;
  static const char* kSrc =
      "ByteAddressBuffer src : register(t0); RWByteAddressBuffer dst : register(u0);\n"
      "cbuffer c : register(b0) { uint n; uint off; };\n"
      "[numthreads(64, 1, 1)] void main(uint i : SV_DispatchThreadID) { if (i < n) dst.Store(off + i * 4, src.Load(i * 4)); }\n";
  rg_hlsl_desc hd{sizeof(rg_hlsl_desc), kSrc, nullptr, "main", "cs_6_0", nullptr, 0, nullptr};
  rg_blob cs{};
  char err[512];
  if (hlsl::compile(&hd, &cs, err, sizeof err) != 0) return g_copy_err = std::string("the copy CS: ") + err;
  ID3D12Device* dev = device();
  D3D12_ROOT_PARAMETER p[3]{};
  p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
  p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
  p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  p[2].Constants.Num32BitValues = 2;
  D3D12_ROOT_SIGNATURE_DESC rd{3, p, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  ID3DBlob *blob = nullptr, *eb = nullptr;
  HRESULT hr = dev ? D3D12SerializeRootSignature(&rd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &eb) : E_FAIL;
  if (eb) eb->Release();
  if (SUCCEEDED(hr)) hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&g_copy_rs));
  if (blob) blob->Release();
  D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
  pd.pRootSignature = g_copy_rs;
  pd.CS = {cs.bytes, cs.size};
  if (SUCCEEDED(hr)) hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_copy_pso));
  if (FAILED(hr)) {
    char b[96];
    std::snprintf(b, sizeof b, "the copy CS's root signature / PSO not created, hr 0x%08lx", static_cast<unsigned long>(hr));
    return g_copy_err = b;
  }
  return {};
}

uint64_t rd64v(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}
uint32_t rd32v(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

uint32_t format_bytes(uint32_t dxgi) {
  switch (dxgi) {
    case DXGI_FORMAT_R16_UINT: return 2;
    case DXGI_FORMAT_R32_UINT: return 4;
    case DXGI_FORMAT_R32G32B32_FLOAT: return 12;
    case DXGI_FORMAT_R32G32_FLOAT: return 8;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_SNORM: return 8;
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_SNORM: return 4;
    default: return 0;
  }
}

std::atomic<bool> g_blas_ctx_logged{false};

// One NvAPI build: every geometry whose (IB VA, index count) a draw of the request's VS used -> its vertices, its indices
// (once per request) and its transform (when it has one), copied by the CS into one buffer, then into a READBACK one.
void blas_copy(Req& r, ID3D12GraphicsCommandList* cl, const uint8_t* d, uint64_t frame) {
  const uint32_t flags = rd32v(d + 0xc), count = rd32v(d + 0x10), layout = rd32v(d + 0x14);
  const uint64_t gstride = rd64v(d + 0x18), geoms = rd64v(d + 0x20);
  BlasCall call;
  uint64_t total = 0;
  struct Range {
    uint64_t src, dst;
    uint32_t dwords;
  };
  std::vector<Range> ranges;
  // [src, src + bytes) -> a 4-aligned window at `total`; returns its offset and the skew of src inside it.
  auto add = [&](uint64_t src, uint64_t bytes, uint64_t* off, uint32_t* skew) {
    const uint64_t a = src & ~3ull;
    *skew = static_cast<uint32_t>(src - a);
    const uint64_t n = (*skew + bytes + 3) / 4;
    *off = total;
    ranges.push_back({a, total, static_cast<uint32_t>(n)});
    total += n * 4;
  };
  ++r.blas_builds_seen;
  for (uint32_t i = 0; i < count && geoms; ++i) {
    const uint8_t* g = layout == 0 ? reinterpret_cast<const uint8_t*>(geoms + i * gstride)
                                   : reinterpret_cast<const uint8_t*>(rd64v(reinterpret_cast<const uint8_t*>(geoms) + 8ull * i));
    if (!g) continue;
    const uint32_t gt = rd32v(g);
    if (gt != 0 && gt != 2) continue;  // triangles, triangles + OMM
    BGeo x;
    x.ib = rd64v(g + 0x20);
    x.icount = rd32v(g + 0x18);
    if (!x.ib || !x.icount || !r.keys.count({x.ib, x.icount})) continue;
    x.geometry = i;
    x.of = count;
    x.type = gt;
    x.geom_flags = rd32v(g + 4);
    x.tf = rd64v(g + 0x08);
    x.ifmt = rd32v(g + 0x10);
    x.vfmt = rd32v(g + 0x14);
    x.vcount = rd32v(g + 0x1c);
    x.vb = rd64v(g + 0x28);
    x.vstride = rd64v(g + 0x30);
    x.vb_bytes = x.vcount ? (x.vcount - 1) * x.vstride + std::max<uint64_t>(format_bytes(x.vfmt), 4) : 0;
    if (x.vb && x.vb_bytes) add(x.vb, x.vb_bytes, &x.vb_off, &x.vb_skew);
    const uint32_t isz = format_bytes(x.ifmt);
    if (isz && r.ib_done.insert(x.ib).second) {
      x.ib_bytes = uint64_t(x.icount) * isz;
      add(x.ib, x.ib_bytes, &x.ib_off, &x.ib_skew);
    }
    if (x.tf) add(x.tf, 48, &x.tf_off, &x.tf_skew);
    call.geos.push_back(x);
  }
  if (call.geos.empty()) return;
  auto* ctx = reinterpret_cast<uint8_t*>(reinterpret_cast<void* (*)()>(g_base + e231::kGetCommandListContext)());
  if (!ctx || *reinterpret_cast<ID3D12GraphicsCommandList**>(ctx + e231::kCtxCmdList) != cl) {
    ++r.blas_refused;  // the build's list is not this thread's context: its state could not be restored
    if (!g_blas_ctx_logged.exchange(true))
      g_sdk->logger->WarnF(g_handle, "DEV readback #%d: a BLAS build on a list that is not the thread's context's - not "
                           "copied (counted in blas_refused)", r.id);
    return;
  }
  long hr = 0;
  call.bytes = total;
  call.dst = nullptr;
  {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = device()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                           IID_PPV_ARGS(&call.dst));
  }
  call.rb = SUCCEEDED(hr) ? buffer(D3D12_HEAP_TYPE_READBACK, total + 8, D3D12_RESOURCE_STATE_COPY_DEST, &hr) : nullptr;
  if (!call.dst || !call.rb) {
    if (call.dst) call.dst->Release();
    call.dst = nullptr;
    char why[120];
    std::snprintf(why, sizeof why, "BLAS copy buffers of %llu bytes not created, hr 0x%08lx",
                  static_cast<unsigned long long>(total), static_cast<unsigned long>(hr));
    return fail(r, why);
  }
  const uint64_t dva = call.dst->GetGPUVirtualAddress();
  cl->SetComputeRootSignature(g_copy_rs);
  cl->SetPipelineState(g_copy_pso);
  for (const Range& g : ranges)
    for (uint32_t done = 0; done < g.dwords;) {
      const uint32_t n = std::min<uint32_t>(g.dwords - done, 65535u * 64u);
      cl->SetComputeRootShaderResourceView(0, g.src + uint64_t(done) * 4);
      cl->SetComputeRootUnorderedAccessView(1, dva);
      const uint32_t c[2] = {n, static_cast<uint32_t>(g.dst + uint64_t(done) * 4)};
      cl->SetComputeRoot32BitConstants(2, 2, c, 0);
      cl->Dispatch((n + 63) / 64, 1, 1);
      done += n;
    }
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = call.dst;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
  cl->ResourceBarrier(1, &b);
  cl->CopyBufferRegion(call.rb, 0, call.dst, 0, total);
  ID3D12GraphicsCommandList2* cl2 = nullptr;
  if (SUCCEEDED(cl->QueryInterface(IID_PPV_ARGS(&cl2)))) {
    const D3D12_WRITEBUFFERIMMEDIATE_PARAMETER p{call.rb->GetGPUVirtualAddress() + total,
                                                 kMarker ^ 0xB1A5u ^ static_cast<uint32_t>(r.calls.size())};
    const D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    cl2->WriteBufferImmediate(1, &p, &mode);
    cl2->Release();
    call.marker = true;
  }
  // The engine's state back, as after a pass (passes.cpp): both root signatures re-ensured, the PSO re-set by the next
  // dispatch / draw.
  const D3D12_COMMAND_LIST_TYPE type = cl->GetType();
  if (r.calls.empty())
    g_sdk->logger->InfoF(g_handle, "DEV readback #%d: BLAS copy recorded on a list of type %u - the engine's state back "
                         "next", r.id, static_cast<unsigned>(type));
  if (type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
    reinterpret_cast<void (*)(ID3D12GraphicsCommandList*)>(g_base + e231::kRestoreState)(cl);
    ctx[e231::kCtxGraphicsDirty] = 1;
  } else {  // a compute list: only the compute root signature (RestoreState would set a graphics one on it)
    *reinterpret_cast<void**>(ctx + e231::kCtxComputeRootSig) = nullptr;
    reinterpret_cast<void (*)(uint8_t*, bool)>(g_base + e231::kEnsureComputeRootSig)(ctx, false);
  }
  *reinterpret_cast<void**>(ctx + e231::kCtxPso) = nullptr;
  ctx[e231::kCtxComputeDirty] = 1;
  call.frame = frame;
  call.dest = rd64v(d);
  call.flags = flags;
  if (r.calls.empty())
    g_sdk->logger->InfoF(g_handle, "DEV readback #%d: first BLAS %s copied - %zu matching geometr%s of %u, %llu bytes; "
                         "list type %u", r.id, flags & 0x20 ? "UPDATE" : "build", call.geos.size(),
                         call.geos.size() == 1 ? "y" : "ies", count, static_cast<unsigned long long>(total),
                         static_cast<unsigned>(type));
  r.calls.push_back(std::move(call));
}

// The BLAS side of a blas:1 request, once every copy's marker is there: data + the json body. false = not yet / failed.
bool harvest_blas(Req& r, uint64_t waited, std::vector<uint8_t>* data, std::string* json) {
  const D3D12_RANGE none{0, 0};
  for (size_t i = 0; i < r.calls.size(); ++i) {
    BlasCall& c = r.calls[i];
    if (!c.marker) continue;
    void* m = nullptr;
    const D3D12_RANGE tail{static_cast<SIZE_T>(c.bytes), static_cast<SIZE_T>(c.bytes + 8)};
    if (FAILED(c.rb->Map(0, &tail, &m))) return fail(r, "a BLAS readback buffer does not map"), false;
    uint32_t mark = 0;
    std::memcpy(&mark, static_cast<const uint8_t*>(m) + c.bytes, 4);
    c.rb->Unmap(0, &none);
    if (mark != (kMarker ^ 0xB1A5u ^ static_cast<uint32_t>(i))) {
      if (waited > kGpuFrames) fail(r, "BLAS copy " + std::to_string(i) + " was recorded but never executed on the GPU");
      return false;
    }
  }
  std::string j;
  for (size_t i = 0; i < r.calls.size(); ++i) {
    BlasCall& c = r.calls[i];
    void* m = nullptr;
    const D3D12_RANGE all{0, static_cast<SIZE_T>(c.bytes)};
    if (FAILED(c.rb->Map(0, &all, &m))) return fail(r, "a BLAS readback buffer does not map"), false;
    const uint64_t at = data->size();
    data->insert(data->end(), static_cast<const uint8_t*>(m), static_cast<const uint8_t*>(m) + c.bytes);
    c.rb->Unmap(0, &none);
    for (const BGeo& x : c.geos) {
      char line[1100];
      std::snprintf(line, sizeof line,
                    "%s    {\"call\": %zu, \"frame\": %llu, \"dest\": \"0x%llx\", \"build_flags\": %u, \"update\": %s, "
                    "\"geometry\": %u, \"of\": %u, \"type\": %u, \"geom_flags\": %u, \"ib_va\": \"0x%llx\", \"icount\": %u, "
                    "\"ifmt\": %u, \"vb_va\": \"0x%llx\", \"vcount\": %u, \"vfmt\": %u, \"vstride\": %llu, "
                    "\"transform_va\": \"0x%llx\", \"vb_offset\": %llu, \"vb_bytes\": %llu, \"vb_skew\": %u, "
                    "\"ib_offset\": %llu, \"ib_bytes\": %llu, \"ib_skew\": %u, \"transform_offset\": %llu, "
                    "\"transform_skew\": %u}",
                    j.empty() ? "" : ",\n", i, static_cast<unsigned long long>(c.frame),
                    static_cast<unsigned long long>(c.dest), c.flags & ~0x20u, c.flags & 0x20 ? "true" : "false",
                    x.geometry, x.of, x.type, x.geom_flags, static_cast<unsigned long long>(x.ib), x.icount, x.ifmt,
                    static_cast<unsigned long long>(x.vb), x.vcount, x.vfmt, static_cast<unsigned long long>(x.vstride),
                    static_cast<unsigned long long>(x.tf), static_cast<unsigned long long>(at + x.vb_off),
                    static_cast<unsigned long long>(x.vb_bytes), x.vb_skew,
                    static_cast<unsigned long long>(x.ib_bytes ? at + x.ib_off : 0),
                    static_cast<unsigned long long>(x.ib_bytes), x.ib_skew,
                    static_cast<unsigned long long>(x.tf ? at + x.tf_off : 0), x.tf_skew);
      j += line;
    }
  }
  char head[400];
  std::snprintf(head, sizeof head,
                "{\n  \"id\": %d,\n  \"spec\": %s,\n  \"kind\": \"blas\",\n  \"capture_frame\": %llu,\n  \"keys\": %zu,\n"
                "  \"builds_seen\": %llu,\n  \"copies\": %zu,\n  \"refused_other_list\": %llu,\n  \"geometries\": [\n",
                r.id, json_str(r.spec).c_str(), static_cast<unsigned long long>(r.cap_frame), r.keys.size(),
                static_cast<unsigned long long>(r.blas_builds_seen), r.calls.size(),
                static_cast<unsigned long long>(r.blas_refused));
  *json = std::string(head) + j + "\n  ]\n}\n";
  return true;
}

// vsout: every draw's marker is there (or the wait is over) -> one .bin (the draws one after another) + the .json.
void harvest_vs(Req& r) {
  const uint64_t waited = g_frame.load(std::memory_order_relaxed) - r.copy_frame;
  const D3D12_RANGE none{0, 0};
  for (size_t i = 0; i < r.draws.size() && r.marker; ++i) {  // all there first: else the next frame tries again
    SoDraw& d = r.draws[i];
    void* m = nullptr;
    const D3D12_RANGE tail{static_cast<SIZE_T>(kSoHeader + d.bytes), static_cast<SIZE_T>(kSoHeader + d.bytes + 8)};
    if (FAILED(d.rb->Map(0, &tail, &m))) return fail(r, "a readback buffer does not map");
    uint32_t mark = 0;
    std::memcpy(&mark, static_cast<const uint8_t*>(m) + kSoHeader + d.bytes, 4);
    d.rb->Unmap(0, &none);
    if (mark != (kMarker ^ static_cast<uint32_t>(r.id) ^ static_cast<uint32_t>(i))) {
      if (waited > kGpuFrames) fail(r, "draw " + std::to_string(i) + "'s copy was recorded but never executed on the GPU "
                                       "(no marker after " + std::to_string(kGpuFrames) + " frames)");
      return;
    }
  }
  if (!r.marker && waited < 8) return;
  std::vector<uint8_t> bdata;
  std::string bjson;
  if (r.with_blas && !harvest_blas(r, waited, &bdata, &bjson)) return;
  const SoElem* pos = nullptr;
  for (const SoElem& x : r.elems)
    if (x.sysval == 1 /* SV_Position */ && x.count == 4) pos = &x;
  Stats st;
  st.ch = pos ? 4 : 1;
  std::vector<uint8_t> data;
  std::string jd;
  uint64_t verts = 0;
  for (size_t i = 0; i < r.draws.size(); ++i) {
    SoDraw& d = r.draws[i];
    void* m = nullptr;
    const D3D12_RANGE all{0, static_cast<SIZE_T>(kSoHeader + d.bytes)};
    if (FAILED(d.rb->Map(0, &all, &m))) return fail(r, "a readback buffer does not map");
    const auto* b = static_cast<const uint8_t*>(m);
    uint32_t filled = 0;
    std::memcpy(&filled, b, 4);
    const uint64_t got = std::min<uint64_t>(filled, d.bytes);
    const uint64_t at = data.size();
    data.insert(data.end(), b + kSoHeader, b + kSoHeader + got);
    d.rb->Unmap(0, &none);
    const uint64_t v = r.stride ? got / r.stride : 0;
    verts += v;
    for (uint64_t k = 0; k < v; ++k) {
      float f[4] = {};
      std::memcpy(f, data.data() + at + k * r.stride + (pos ? pos->offset : 0), pos ? 16 : 4);
      stats_add(st, f);
    }
    char line[900];
    std::snprintf(line, sizeof line,
                  "%s    {\"offset\": %llu, \"bytes\": %llu, \"vertices\": %llu, \"so_filled\": %u, \"window\": %llu, "
                  "\"kind\": %u, \"system\": %s, \"count\": %u, \"instances\": %u, \"start\": %u, \"base_vertex\": %d, "
                  "\"start_instance\": %u, \"topology\": %u, \"ib_va\": \"0x%llx\", \"ib_bytes\": %u, \"index_bytes\": %u, "
                  "\"vb0_va\": \"0x%llx\", \"vb0_bytes\": %u, \"vb0_stride\": %u, \"frame\": %llu, \"engine_pso\": \"%p\", "
                  "\"clone\": %s, \"vs_used\": \"%s\"}",
                  i ? ",\n" : "", static_cast<unsigned long long>(at), static_cast<unsigned long long>(got),
                  static_cast<unsigned long long>(v), filled, static_cast<unsigned long long>(d.bytes), d.info.kind,
                  d.info.flags & RG_DRAW_SYSTEM ? "true" : "false", d.info.count, d.info.instances, d.info.start,
                  d.info.base_vertex, d.info.start_instance, d.topology, static_cast<unsigned long long>(d.ia.ib),
                  d.ia.ib_size, d.ia.index_bytes, static_cast<unsigned long long>(d.ia.vb0), d.ia.vb0_size,
                  d.ia.vb0_stride, static_cast<unsigned long long>(d.frame), d.engine, d.clone ? "true" : "false",
                  hex16(d.vs_used).c_str());
    jd += line;
  }
  std::string je;
  for (size_t i = 0; i < r.elems.size(); ++i) {
    const SoElem& x = r.elems[i];
    char line[300];
    std::snprintf(line, sizeof line,
                  "%s    {\"name\": %s, \"index\": %u, \"register\": %u, \"mask\": %u, \"start\": %u, \"count\": %u, "
                  "\"system_value\": %u, \"component_type\": %u, \"min_precision\": %u, \"offset\": %u}",
                  i ? ",\n" : "", json_str(x.name).c_str(), x.index, x.reg, x.mask, x.start, x.count, x.sysval, x.type,
                  x.minprec, x.offset);
    je += line;
  }
  const std::string want = hex16(r.vs_hash).substr(0, 2 * r.vs_len);
  char head[600];
  std::snprintf(head, sizeof head,
                "  \"kind\": \"vsout\",\n  \"vs\": \"%s\",\n  \"stride\": %u,\n  \"vertices\": %llu,\n  \"gpu_marker\": %s,\n"
                "  \"skipped\": {\"indirect\": %llu, \"ia_tracking_started\": %llu, \"another_frame\": %llu},\n"
                "  \"cb12\": {\"file\": \"rb%d_cb12.bin\", \"bytes\": %zu, \"write_seq\": %llu},\n",
                want.c_str(), r.stride, static_cast<unsigned long long>(verts), r.marker ? "true" : "false",
                static_cast<unsigned long long>(r.skipped_indirect), static_cast<unsigned long long>(r.skipped_ia),
                static_cast<unsigned long long>(r.skipped_frame), r.id, r.cb12.size(),
                static_cast<unsigned long long>(r.cb12_seq));
  {
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    write_file(g_dir / ("rb" + std::to_string(r.id) + "_cb12.bin"), r.cb12.data(), r.cb12.size(), nullptr, 0);
  }
  const std::string extra = std::string(head) + "  \"elements\": [\n" + je + "\n  ],\n  \"draws\": [\n" + jd + "\n  ],\n";
  r.info.frame = r.copy_frame;
  r.info.width = static_cast<uint32_t>(verts);
  r.info.height = static_cast<uint32_t>(r.draws.size());
  r.info.row_bytes = r.stride;
  release_draws(r);
  const fs::path base = g_dir / ("rb" + std::to_string(r.id) + "_vsout_" + want);
  r.st = St::kWriting;
  set_text(r, "pending: writing the files");
  if (!r.with_blas) {
    std::thread(finish, &r, std::move(data), extra, st, base, ".bin", std::vector<uint8_t>()).detach();
    return;
  }
  // blas:1 - the BLAS side next to it (rb<id>_blas_<hash>.bin + .json), then the vsout files and the result.
  const fs::path bbase = g_dir / ("rb" + std::to_string(r.id) + "_blas_" + want);
  const std::string sum = ", \"blas\": {\"file\": " + json_str(bbase.filename().string() + ".bin") + ", \"copies\": " +
                          std::to_string(r.calls.size()) + ", \"capture_frame\": " + std::to_string(r.cap_frame) + "},\n";
  std::thread([&r, data = std::move(data), extra = extra + "  " + sum.substr(2), st, base, bbase,
               bdata = std::move(bdata), bjson = std::move(bjson)]() mutable {
    std::error_code ec;
    fs::create_directories(g_dir, ec);
    write_file(bbase.string() + ".bin", bdata.data(), bdata.size(), nullptr, 0);
    write_file(bbase.string() + ".json", bjson.data(), bjson.size(), nullptr, 0);
    finish(&r, std::move(data), extra, st, base, ".bin", std::vector<uint8_t>());
  }).detach();
}

std::string pending_text(const Req& r) {
  char b[256];
  std::snprintf(b, sizeof b, "pending: waiting for %s '%s' (%llu run(s) seen, the resource not live in %llu)",
                r.after ? "after" : "before", r.node.c_str(), static_cast<unsigned long long>(r.seen),
                static_cast<unsigned long long>(r.not_live));
  return b;
}

// vsout needs a graphics root signature that allows Stream Output: the engine's global one gets the flag (its creators'
// immediates, checked byte for byte first). At Load - InitDevice creates the root signatures later.
void allow_stream_output() {
  uint32_t done = 0;
  for (const e231::CodePatchSite& c : e231::kGfxRootSigFlags) {
    auto* p = reinterpret_cast<uint8_t*>(g_base + c.rva);
    DWORD old = 0;
    if (std::memcmp(p, c.bytes, c.size) != 0 || !VirtualProtect(p + c.imm, 1, PAGE_EXECUTE_READWRITE, &old)) continue;
    p[c.imm] = 0x01 | 0x40;  // D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT | ALLOW_STREAM_OUTPUT
    VirtualProtect(p + c.imm, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, c.size);
    ++done;
  }
  if (done == 2)
    g_sdk->logger->Info(g_handle, "DEV: readback - the engine's graphics root signature gets ALLOW_STREAM_OUTPUT (vsout; "
                        "both creators patched)");
  else
    g_sdk->logger->WarnF(g_handle, "DEV: readback - only %u of 2 creators of the graphics root signature found (another "
                         "exe?): vsout may fail to build its Stream Output PSO", done);
}

}  // namespace

void enable(const std::string& dir) {
  g_on = true;
  allow_stream_output();
  g_dir = fs::u8path(dir);
  render_nodes::wrap_all_classes();  // every class's Execute passes through wrap_execute - at_node sees every node
  render_flow::want();               // the node context on the recording thread (names resolve per node)
  pso::dev_readback_enable();        // vsout: every graphics PSO's desc kept (light), every graphics bind tracked
  std::error_code ec;
  fs::create_directories(g_dir, ec);
  g_sdk->logger->WarnF(g_handle, "DEV: readback ON ([Dev] Readback) - files into %s; every node class is wrapped. Not "
                       "for players.", g_dir.string().c_str());
}

bool armed() { return g_armed.load(std::memory_order_relaxed) != 0; }

bool on() { return g_on; }

bool vs_armed() { return g_vs_armed.load(std::memory_order_relaxed) != 0; }

bool blas_armed() { return g_blas_armed.load(std::memory_order_relaxed) != 0; }

void on_blas_build(void* cmdlist, const uint8_t* desc) {
  if (!desc || rd32v(desc + 8) != 1 /* BLAS */) return;
  const uint64_t f = g_frame.load(std::memory_order_relaxed);
  std::lock_guard lock(g_mu);
  for (auto& p : g_req) {
    Req& r = *p;
    if (r.kind == Kind::kVsOut && r.with_blas && r.st == St::kArmed && r.phase == 1 && f == r.cap_frame)
      blas_copy(r, static_cast<ID3D12GraphicsCommandList*>(cmdlist), desc, f);
  }
}

bool vs_wanted(const uint8_t* hash16) {
  for (const VsWant& w : g_vs_want) {
    const uint32_t n = w.len.load(std::memory_order_acquire);
    if (n && std::memcmp(w.hash, hash16, n) == 0) return true;
  }
  return false;
}

void at_draw(uint8_t* ctx, ID3D12PipelineState* engine, ID3D12PipelineState* bound, const rg_draw_info& info) {
  if (*reinterpret_cast<ID3D12PipelineState* const*>(ctx + e231::kCtxPso) != bound) return;  // the list moved on
  rg_blob evs{};
  uint8_t eh[16];
  if (!pso::dev_vs_of(engine, engine, &evs, eh)) return;  // the ENGINE's VS hash is what a request names
  std::lock_guard lock(g_mu);
  for (auto& p : g_req) {
    Req& r = *p;
    if (r.kind != Kind::kVsOut || r.st != St::kArmed || std::memcmp(r.vs_hash, eh, r.vs_len) != 0) continue;
    if (r.with_blas) {
      const uint64_t f = g_frame.load(std::memory_order_relaxed);
      if (r.phase == 0) {  // learn: this frame's index ranges of the VS
        blas_census::IaViews ia{};
        if (info.kind != RG_DRAW_INDEXED || (!r.keys.empty() && f != r.learn_frame)) continue;
        if (!blas_census::ia_views(&ia) || !ia.ib) {
          ++r.skipped_ia;
          continue;
        }
        if (r.keys.empty()) r.learn_frame = f;
        r.keys.insert({ia.ib + uint64_t(info.start) * ia.index_bytes, info.count});
        continue;
      }
      if (r.phase != 1 || f != r.cap_frame || r.draws.size() >= r.draws_want) continue;
    }
    capture_draw(r, ctx, engine, bound, info);
    return;  // one request per draw
  }
}

void at_node(const void* node, bool after, bool job) {
  const uint64_t name = render_flow::node_name_hash(node);
  std::lock_guard lock(g_mu);
  for (auto& p : g_req) {
    Req& r = *p;
    if (r.kind != Kind::kFlow || r.st != St::kArmed || r.node_hash != name || r.after != after) continue;
    ++r.seen;
    void* res = nullptr;
    const uint32_t id = render_flow::resolve_here(r.name_hash, &res);
    if (!id) {
      ++r.not_live;
      set_text(r, pending_text(r));
      continue;
    }
    r.job = job;
    copy_at(r, static_cast<ID3D12Resource*>(res), id);
  }
}

void frame_tick() {
  if (!g_on) return;
  const uint64_t f = g_frame.fetch_add(1, std::memory_order_relaxed) + 1;
  std::lock_guard lock(g_mu);
  for (auto& p : g_req) {
    Req& r = *p;
    switch (r.st) {
      case St::kWaitBlock:
        if (f - r.start > r.frames) fail(r, "the block was not written in " + std::to_string(r.frames) +
                                                " frames (its writer did not run, or it is not captured - see the log)");
        else block_tick(r);
        break;
      case St::kArmed:
        if (r.kind == Kind::kVsOut && r.with_blas) {
          if (r.phase == 0 && !r.keys.empty() && f > r.learn_frame) {  // the keys are known: capture this frame
            r.phase = 1;
            r.cap_frame = f;
            g_blas_armed.fetch_add(1, std::memory_order_relaxed);
            set_text(r, "pending: " + std::to_string(r.keys.size()) + " index range(s) learnt; capturing frame " +
                            std::to_string(f));
          } else if (r.phase == 1 && f > r.cap_frame) {
            g_blas_armed.fetch_sub(1, std::memory_order_relaxed);
            g_vs_armed.fetch_sub(1, std::memory_order_relaxed);
            vs_unwant(r);
            r.phase = 2;
            if (r.draws.empty()) {
              r.st = St::kFailed;  // (not kArmed any more: fail() must not count it out twice)
              fail(r, "the capture frame " + std::to_string(r.cap_frame) + " drew none of the learnt meshes");
            } else {
              r.st = St::kCopied;
              r.copy_frame = r.cap_frame;
              set_text(r, "pending: frame " + std::to_string(r.cap_frame) + " - " + std::to_string(r.draws.size()) +
                              " draw(s), " + std::to_string(r.calls.size()) + " BLAS copy(ies); waiting for the GPU");
            }
          } else if (f - r.start > r.frames) {
            fail(r, "no draw of VS " + hex16(r.vs_hash).substr(0, 2 * r.vs_len) + " in " + std::to_string(r.frames) +
                        " frames (IB tracking start " + std::to_string(r.skipped_ia) + ")");
          }
        } else if (r.kind == Kind::kVsOut) {
          if (!r.draws.empty() && f > r.draws[0].frame + 1) {  // fewer draws than asked: the frame is over
            g_vs_armed.fetch_sub(1, std::memory_order_relaxed);
            vs_unwant(r);
            r.st = St::kCopied;
            r.copy_frame = r.draws[0].frame;
          } else if (f - r.start > r.frames) {
            fail(r, "no draw of VS " + hex16(r.vs_hash).substr(0, 2 * r.vs_len) + " captured in " +
                        std::to_string(r.frames) + " frames (indirect draws skipped " +
                        std::to_string(r.skipped_indirect) + ", IB tracking start " + std::to_string(r.skipped_ia) +
                        "; not drawn in this scene / mode?)");
          }
        } else if (f - r.start > r.frames)
          fail(r, r.seen ? "node '" + r.node + "' ran " + std::to_string(r.seen) + " time(s), the resource was never "
                               "live there (another node / not this render mode?)"
                         : "node '" + r.node + "' never ran in " + std::to_string(r.frames) + " frames (a group? "
                               "name a member; not in this render mode?)");
        break;
      case St::kCopied: r.kind == Kind::kVsOut ? harvest_vs(r) : harvest(r); break;
      default: break;
    }
  }
  // Forget the oldest finished requests beyond kMaxReqs.
  while (g_req.size() > kMaxReqs) {
    auto it = g_req.begin();
    while (it != g_req.end() && (*it)->st != St::kDone && (*it)->st != St::kFailed) ++it;
    if (it == g_req.end()) break;
    g_req.erase(it);
  }
}

int32_t request(const char* spec) {
  if (!g_on) return RG_E_UNAVAILABLE;
  auto r = std::make_unique<Req>();
  r->spec = spec ? spec : "";
  r->start = g_frame.load(std::memory_order_relaxed);
  std::string why = parse(*r);
  std::lock_guard lock(g_mu);
  r->id = ++g_next;
  r->info.state = RG_RB_PENDING;
  Req& q = *r;
  g_req.push_back(std::move(r));
  if (!why.empty()) {
    fail(q, why);
    return q.id;
  }
  if (q.kind == Kind::kBlock) {
    engine_blocks::want(static_cast<uint32_t>(q.block));
    q.seq0 = engine_blocks::latest(static_cast<uint32_t>(q.block), nullptr, 0, nullptr);
    q.st = St::kWaitBlock;
    set_text(q, std::string("pending: waiting for the next write of ") + engine_blocks::name(q.block));
  } else if (q.kind == Kind::kVsOut) {
    if (q.with_blas) {
      const std::string why2 = copy_setup();
      if (!why2.empty()) {
        fail(q, why2);
        return q.id;
      }
    }
    q.st = St::kArmed;
    vs_want(q);
    g_vs_armed.fetch_add(1, std::memory_order_relaxed);
    set_text(q, "pending: waiting for a draw of VS " + hex16(q.vs_hash).substr(0, 2 * q.vs_len));
  } else {
    q.st = St::kArmed;
    g_armed.fetch_add(1, std::memory_order_relaxed);
    set_text(q, pending_text(q));
  }
  g_sdk->logger->InfoF(g_handle, "DEV readback #%d '%s' - %s", q.id, q.spec.c_str(), q.info.text);
  return q.id;
}

int32_t result(int32_t id, rg_readback_info* out) {
  std::lock_guard lock(g_mu);
  for (auto& p : g_req)
    if (p->id == id) {
      if (out) *out = p->info;
      return p->info.state;
    }
  if (out) {
    *out = {};
    out->state = RG_RB_UNKNOWN;
    std::snprintf(out->text, sizeof out->text, "%s", g_on ? "unknown id" : "[Dev] Readback is off");
  }
  return RG_RB_UNKNOWN;
}

}  // namespace core::readback

extern "C" RG_DEV_EXPORT int32_t rg_dev_readback(const char* spec) { return core::readback::request(spec); }
extern "C" RG_DEV_EXPORT int32_t rg_dev_readback_result(int32_t id, rg_readback_info* out) {
  return core::readback::result(id, out);
}
