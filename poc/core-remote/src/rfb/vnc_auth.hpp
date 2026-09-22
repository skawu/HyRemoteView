#pragma once

// POC_ONLY VNC authentication (RFB security type 2).
//
// The server proves the credential by sending a 16-byte challenge and checking the client's DES
// response. The same 1..8 byte password limit as the server applies, so a credential that the
// server would refuse is refused here too instead of producing a confusing handshake failure.

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace poc::core_remote::rfb {

inline constexpr std::size_t kVncChallengeBytes = 16;
inline constexpr std::size_t kVncMaxPasswordBytes = 8;

// Returns the 16-byte response for `password` (1..8 bytes) and `challenge` (exactly 16 bytes).
[[nodiscard]] bool vncAuthResponse(const std::uint8_t *password,
                                   std::size_t passwordSize,
                                   const std::array<std::uint8_t, kVncChallengeBytes> &challenge,
                                   std::array<std::uint8_t, kVncChallengeBytes> &response,
                                   std::string &error);

}  // namespace poc::core_remote::rfb
