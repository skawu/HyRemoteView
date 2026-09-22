#include "rfb/des.hpp"

namespace poc::core_remote::rfb {
namespace {

// ---------------------------------------------------------------- bit helpers (1-based, MSB first)
//
// Every value here is *right aligned* to its own width: DES position 1 of an N-bit value is bit N-1.
// Keeping the width explicit is what makes the key schedule and the Feistel round agree on where the
// halves start; an implicit 64-bit alignment silently corrupts PC1 and every derived subkey.

// Generic permutation: `table` lists input bit positions (1 = most significant of `inputBits`) in
// output order, most significant first.
std::uint64_t permute(std::uint64_t input,
                      int inputBits,
                      const int *table,
                      int outputBits) noexcept
{
    std::uint64_t output = 0;
    for (int index = 0; index < outputBits; ++index) {
        const int source = table[index];
        const int bit = static_cast<int>((input >> (inputBits - source)) & 1ULL);
        if (bit != 0)
            output |= (1ULL << (outputBits - 1 - index));
    }
    return output;
}

constexpr int kIp[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6,
    64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7};

constexpr int kFp[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30,
    37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25};

constexpr int kExpansion[48] = {
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1};

constexpr int kRoundPermutation[32] = {
    16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
    2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25};

constexpr int kPc1[56] = {
    57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3,
    60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37,
    29, 21, 13, 5, 28, 20, 12, 4};

constexpr int kPc2[48] = {
    14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32};

constexpr int kShifts[16] = {1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1};

constexpr std::uint64_t kHalfMask = (1ULL << 28U) - 1ULL;

constexpr int kSBoxes[8][4][16] = {
    {{14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7},
     {0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8},
     {4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0},
     {15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13}},
    {{15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10},
     {3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5},
     {0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15},
     {13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9}},
    {{10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8},
     {13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1},
     {13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7},
     {1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12}},
    {{7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15},
     {13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9},
     {10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4},
     {3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14}},
    {{2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9},
     {14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6},
     {4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14},
     {11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3}},
    {{12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11},
     {10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8},
     {9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6},
     {4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13}},
    {{4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1},
     {13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6},
     {1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2},
     {6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12}},
    {{13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7},
     {1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2},
     {7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8},
     {2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11}}};

std::uint64_t readBlock(const std::array<std::uint8_t, 8> &block) noexcept
{
    std::uint64_t value = 0;
    for (std::uint8_t byte : block)
        value = (value << 8U) | byte;
    return value;
}

std::array<std::uint8_t, 8> writeBlock(std::uint64_t value) noexcept
{
    std::array<std::uint8_t, 8> block{};
    for (int index = 7; index >= 0; --index) {
        block[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(value & 0xffU);
        value >>= 8U;
    }
    return block;
}

// 28-bit halves are rotated inside their own width, so the shift is applied with wraparound.
std::uint64_t rotate28(std::uint64_t half, int amount) noexcept
{
    const std::uint64_t mask = (1ULL << 28U) - 1ULL;
    half &= mask;
    return ((half << amount) | (half >> (28 - amount))) & mask;
}

std::array<std::uint64_t, 16> keySchedule(const std::array<std::uint8_t, 8> &key) noexcept
{
    const std::uint64_t permuted = permute(readBlock(key), 64, kPc1, 56);
    std::uint64_t c = permuted >> 28U;
    std::uint64_t d = permuted & kHalfMask;

    std::array<std::uint64_t, 16> subkeys{};
    for (int round = 0; round < 16; ++round) {
        c = rotate28(c, kShifts[round]);
        d = rotate28(d, kShifts[round]);
        const std::uint64_t combined = (c << 28U) | d;
        subkeys[static_cast<std::size_t>(round)] = permute(combined, 56, kPc2, 48);
    }
    return subkeys;
}

std::uint32_t feistel(std::uint32_t right, std::uint64_t subkey) noexcept
{
    const std::uint64_t expanded = permute(right, 32, kExpansion, 48);
    const std::uint64_t mixed = expanded ^ subkey;

    std::uint32_t substituted = 0;
    for (int box = 0; box < 8; ++box) {
        // The 48-bit E/R result is right aligned, so group `box` starts at bit 42-6b.
        const int shift = 42 - 6 * box;
        const int six = static_cast<int>((mixed >> shift) & 0x3fULL);
        const int row = ((six & 0x20) >> 4) | (six & 0x01);
        const int column = (six >> 1) & 0x0f;
        substituted = (substituted << 4U) | static_cast<std::uint32_t>(kSBoxes[box][row][column]);
    }
    return static_cast<std::uint32_t>(permute(substituted, 32, kRoundPermutation, 32));
}

}  // namespace

std::uint8_t reverseBits(std::uint8_t value) noexcept
{
    value = static_cast<std::uint8_t>(((value & 0x55U) << 1U) | ((value >> 1U) & 0x55U));
    value = static_cast<std::uint8_t>(((value & 0x33U) << 2U) | ((value >> 2U) & 0x33U));
    return static_cast<std::uint8_t>((value << 4U) | (value >> 4U));
}

std::array<std::uint8_t, 8> vncDesKey(const std::uint8_t *password, std::size_t size) noexcept
{
    std::array<std::uint8_t, 8> key{};
    const std::size_t count = size > 8 ? 8 : size;
    for (std::size_t index = 0; index < count; ++index)
        key[index] = reverseBits(password[index]);
    return key;
}

std::array<std::uint8_t, 8> desEncryptBlock(const std::array<std::uint8_t, 8> &key,
                                            const std::array<std::uint8_t, 8> &block) noexcept
{
    const std::array<std::uint64_t, 16> subkeys = keySchedule(key);

    const std::uint64_t permuted = permute(readBlock(block), 64, kIp, 64);
    std::uint32_t left = static_cast<std::uint32_t>(permuted >> 32U);
    std::uint32_t right = static_cast<std::uint32_t>(permuted & 0xffffffffULL);

    for (int round = 0; round < 16; ++round) {
        const std::uint32_t previousRight = right;
        right = left ^ feistel(right, subkeys[static_cast<std::size_t>(round)]);
        left = previousRight;
    }

    const std::uint64_t preOutput = (static_cast<std::uint64_t>(right) << 32U) | left;
    return writeBlock(permute(preOutput, 64, kFp, 64));
}

}  // namespace poc::core_remote::rfb
