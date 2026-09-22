#pragma once

// POC_ONLY DES-ECB single-block encryption.
//
// RFB VNC authentication is defined in terms of DES, and the PoC client must not depend on OpenSSL
// or on any other crypto library to reach parity with the server. This is therefore a plain,
// table-driven DES implementation of the published algorithm, used for nothing except the 16-byte
// VNC challenge response. It is not a general-purpose crypto library and must not be treated as
// one.

#include <array>
#include <cstdint>

namespace poc::core_remote::rfb {

// Encrypts one 8-byte block. `key` must be exactly 8 bytes; parity bits are ignored, which matches
// the server's use of DES_set_key_unchecked.
[[nodiscard]] std::array<std::uint8_t, 8> desEncryptBlock(const std::array<std::uint8_t, 8> &key,
                                                          const std::array<std::uint8_t, 8> &block) noexcept;

// VNC authentication reverses the bits of each password byte before using it as the DES key. This is
// the only piece of VNC-specific key handling, kept next to DES so the two are read together.
[[nodiscard]] std::array<std::uint8_t, 8> vncDesKey(const std::uint8_t *password,
                                                    std::size_t size) noexcept;

[[nodiscard]] std::uint8_t reverseBits(std::uint8_t value) noexcept;

}  // namespace poc::core_remote::rfb
