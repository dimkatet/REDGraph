// Compile-time construction of a dxil_hash::Hash from a 32-char hex string.
// Lets a shader be registered as hash_from_hex("10c9eb74...ed dbedb647e") instead
// of a hand-split 16-byte array - fewer chances to transpose a byte, and the hex
// reads the same way PIX/dxc print it. Evaluated at compile time in the registry,
// so a wrong length or a stray non-hex character is a build error, not a silent
// zero hash that would never match anything live.
#pragma once

#include <string_view>

#include "dxil_hash.hpp"

namespace hash_literal {

constexpr uint8_t hex_nibble(char c) {
  if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
  if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(c - 'A' + 10);
  throw "hash_from_hex: non-hex character in shader hash literal";
}

constexpr dxil_hash::Hash hash_from_hex(std::string_view hex) {
  if (hex.size() != 32) throw "hash_from_hex: expected exactly 32 hex chars (16 bytes)";
  dxil_hash::Hash hash{};
  for (size_t i = 0; i < 16; ++i) {
    hash[i] = static_cast<uint8_t>((hex_nibble(hex[2 * i]) << 4) | hex_nibble(hex[2 * i + 1]));
  }
  return hash;
}

}  // namespace hash_literal
