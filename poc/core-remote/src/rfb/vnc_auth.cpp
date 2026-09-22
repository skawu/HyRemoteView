#include "rfb/vnc_auth.hpp"

#include "rfb/des.hpp"

namespace poc::core_remote::rfb {

bool vncAuthResponse(const std::uint8_t *password,
                     std::size_t passwordSize,
                     const std::array<std::uint8_t, kVncChallengeBytes> &challenge,
                     std::array<std::uint8_t, kVncChallengeBytes> &response,
                     std::string &error)
{
    response.fill(0);
    if (password == nullptr || passwordSize == 0 || passwordSize > kVncMaxPasswordBytes) {
        error = "VNC authentication password must contain 1 to 8 bytes";
        return false;
    }

    const std::array<std::uint8_t, 8> key = vncDesKey(password, passwordSize);
    for (std::size_t offset = 0; offset < kVncChallengeBytes; offset += 8) {
        std::array<std::uint8_t, 8> block{};
        for (std::size_t index = 0; index < 8; ++index)
            block[index] = challenge[offset + index];
        const std::array<std::uint8_t, 8> encrypted = desEncryptBlock(key, block);
        for (std::size_t index = 0; index < 8; ++index)
            response[offset + index] = encrypted[index];
    }

    error.clear();
    return true;
}

}  // namespace poc::core_remote::rfb
