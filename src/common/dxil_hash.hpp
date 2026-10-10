// Extracts the 128-bit shader-identity hash the DirectX shader compiler embeds
// in every compiled DXBC/DXIL container - the same value PIX/dxc show as
// "shader hash" in their disassembly view.
//
// This is NOT the 16 bytes immediately following the "DXBC" magic (that's a
// whole-container integrity checksum, a different value) - it lives in a
// dedicated chunk with FourCC "HASH", found via the container's chunk offset
// table:
//   offset 0:  "DXBC" magic (4 bytes)
//   offset 4:  container checksum (16 bytes) - NOT the shader hash
//   offset 20: version (uint32)
//   offset 24: total size (uint32)
//   offset 28: chunk count (uint32)
//   offset 32: chunk_count x uint32 chunk offsets (relative to file start)
//   each chunk: 4-byte FourCC + 4-byte payload size + payload
//   "HASH" chunk payload: uint32 flags, then the 16-byte digest itself.
// This is the HASH chunk digest, NOT the container checksum; verified against
// dxc.exe -dumpbin's own reported hash.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

namespace dxil_hash {

using Hash = std::array<uint8_t, 16>;

inline std::optional<Hash> of(const void* code, size_t code_size) {
  if (code == nullptr || code_size < 32) return std::nullopt;
  const auto* bytes = static_cast<const uint8_t*>(code);
  if (std::memcmp(bytes, "DXBC", 4) != 0) return std::nullopt;

  uint32_t chunk_count = 0;
  std::memcpy(&chunk_count, bytes + 28, sizeof(chunk_count));

  const size_t offsets_start = 32;
  if (offsets_start + static_cast<size_t>(chunk_count) * sizeof(uint32_t) > code_size) return std::nullopt;

  for (uint32_t i = 0; i < chunk_count; ++i) {
    uint32_t chunk_offset = 0;
    std::memcpy(&chunk_offset, bytes + offsets_start + i * sizeof(uint32_t), sizeof(chunk_offset));
    if (static_cast<size_t>(chunk_offset) + 8 > code_size) continue;
    if (std::memcmp(bytes + chunk_offset, "HASH", 4) != 0) continue;

    uint32_t chunk_size = 0;
    std::memcpy(&chunk_size, bytes + chunk_offset + 4, sizeof(chunk_size));
    const size_t payload_offset = static_cast<size_t>(chunk_offset) + 8;
    // Payload is { uint32 flags; uint8 digest[16]; } - 20 bytes total.
    if (chunk_size < 20 || payload_offset + 20 > code_size) return std::nullopt;

    Hash hash;
    std::memcpy(hash.data(), bytes + payload_offset + 4, 16);
    return hash;
  }
  return std::nullopt;
}

}  // namespace dxil_hash
