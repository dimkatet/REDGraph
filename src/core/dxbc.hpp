#pragma once

// The resource bindings a DXIL shader declares, from its container's PSV0 chunk (DxilPipelineStateValidation.h):
//   u32 runtime_info_size, runtime info, u32 resource_count, [u32 entry_size, entries {u32 type, space, lo, hi, ...}].
// Checked offline on the vanilla wetness PS 20664d7f / 4706078c (CBV b0 b6 b12) and on patched copies reading b1 too -
// the broker's lease test (docs/internals.md "Leases (M2)").

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace core::dxbc {

enum class Bound { kNo, kYes, kUnknown };  // kUnknown = no / malformed PSV0

// PSV0 resource types -> rg_kind (0 CBV, 1 SRV, 2 UAV), -1 = sampler / other.
inline int kind_of(uint32_t type) {
  if (type == 2) return 0;
  if (type >= 3 && type <= 5) return 1;
  if (type >= 6 && type <= 9) return 2;
  return -1;
}

// Does the shader bind register `reg` of `kind` in `space`?
inline Bound binds(const void* code, size_t size, uint32_t kind, uint32_t space, uint32_t reg) {
  const auto* b = static_cast<const uint8_t*>(code);
  auto u32 = [&](size_t off, uint32_t& v) {
    if (off + 4 > size) return false;
    std::memcpy(&v, b + off, 4);
    return true;
  };
  if (!b || size < 32 || std::memcmp(b, "DXBC", 4) != 0) return Bound::kUnknown;
  uint32_t parts = 0;
  if (!u32(28, parts)) return Bound::kUnknown;
  for (uint32_t i = 0; i < parts; ++i) {
    uint32_t off = 0, psize = 0;
    if (!u32(32 + 4ull * i, off) || off + 8ull > size) return Bound::kUnknown;
    if (std::memcmp(b + off, "PSV0", 4) != 0) continue;
    if (!u32(off + 4ull, psize)) return Bound::kUnknown;
    const size_t end = off + 8ull + psize;
    if (end > size) return Bound::kUnknown;
    size_t p = off + 8ull;
    uint32_t rsz = 0, count = 0, esz = 0;
    if (!u32(p, rsz)) return Bound::kUnknown;
    p += 4ull + rsz;
    if (p + 4 > end || !u32(p, count)) return Bound::kUnknown;
    p += 4;
    if (count == 0) return Bound::kNo;
    if (p + 4 > end || !u32(p, esz) || esz < 16) return Bound::kUnknown;
    p += 4;
    if (p + static_cast<size_t>(count) * esz > end) return Bound::kUnknown;
    for (uint32_t k = 0; k < count; ++k, p += esz) {
      uint32_t type, sp, lo, hi;
      std::memcpy(&type, b + p, 4);
      std::memcpy(&sp, b + p + 4, 4);
      std::memcpy(&lo, b + p + 8, 4);
      std::memcpy(&hi, b + p + 12, 4);
      if (kind_of(type) == static_cast<int>(kind) && sp == space && reg >= lo && reg <= hi) return Bound::kYes;
    }
    return Bound::kNo;
  }
  return Bound::kUnknown;
}

// The same for a LIBRARY (lib_6_x - no PSV0): its RDAT chunk's resource table (DxilRuntimeData.h, version 0x10):
//   u32 version, u32 part_count, u32 part_offsets[]; part = {u32 type, u32 size, data}; type 3 = ResourceTable =
//   {u32 count, u32 stride, records {u32 class, kind, id, space, lower, upper, ...}}, class 0 SRV 1 UAV 2 CBV 3 sampler.
// Checked offline 2026-10-06: all 68 RT libraries of staticshader_final.cache carry it (stride 32), and our
// space0_rebind'ed libraries declare the moved registers there.
inline Bound lib_binds(const void* code, size_t size, uint32_t kind, uint32_t space, uint32_t reg) {
  const auto* b = static_cast<const uint8_t*>(code);
  auto u32 = [&](size_t off, uint32_t& v) {
    if (off + 4 > size) return false;
    std::memcpy(&v, b + off, 4);
    return true;
  };
  static constexpr uint32_t kClassOf[3] = {2, 0, 1};  // rg_kind CBV, SRV, UAV -> RDAT class
  if (!b || size < 32 || std::memcmp(b, "DXBC", 4) != 0 || kind > 2) return Bound::kUnknown;
  uint32_t parts = 0;
  if (!u32(28, parts)) return Bound::kUnknown;
  for (uint32_t i = 0; i < parts; ++i) {
    uint32_t off = 0, psize = 0;
    if (!u32(32 + 4ull * i, off) || off + 8ull > size) return Bound::kUnknown;
    if (std::memcmp(b + off, "RDAT", 4) != 0) continue;
    if (!u32(off + 4ull, psize)) return Bound::kUnknown;
    const size_t base = off + 8ull, end = base + psize;
    uint32_t ver = 0, np = 0;
    if (end > size || !u32(base, ver) || !u32(base + 4, np) || ver != 0x10) return Bound::kUnknown;
    for (uint32_t k = 0; k < np; ++k) {
      uint32_t po = 0, type = 0, count = 0, stride = 0;
      if (!u32(base + 8 + 4ull * k, po) || base + po + 16 > end || !u32(base + po, type)) return Bound::kUnknown;
      if (type != 3) continue;
      if (!u32(base + po + 8, count) || !u32(base + po + 12, stride) || stride < 24) return Bound::kUnknown;
      size_t p = base + po + 16;
      if (p + static_cast<size_t>(count) * stride > end) return Bound::kUnknown;
      for (uint32_t r = 0; r < count; ++r, p += stride) {
        uint32_t cls, sp, lo, hi;
        std::memcpy(&cls, b + p, 4);
        std::memcpy(&sp, b + p + 12, 4);
        std::memcpy(&lo, b + p + 16, 4);
        std::memcpy(&hi, b + p + 20, 4);
        if (cls == kClassOf[kind] && sp == space && reg >= lo && reg <= hi) return Bound::kYes;  // hi ~0u = unbounded
      }
      return Bound::kNo;
    }
    return Bound::kNo;  // an RDAT without a resource table declares none
  }
  return Bound::kUnknown;
}

}  // namespace core::dxbc
