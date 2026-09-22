#include "rfb/wire.hpp"

#include <algorithm>
#include <limits>

namespace poc::core_remote::rfb {

std::optional<std::uint64_t> checkedMul(std::uint64_t lhs, std::uint64_t rhs) noexcept
{
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs)
        return std::nullopt;
    return lhs * rhs;
}

std::optional<std::uint64_t> checkedAdd(std::uint64_t lhs, std::uint64_t rhs) noexcept
{
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs)
        return std::nullopt;
    return lhs + rhs;
}

std::size_t Reader::remaining() const noexcept
{
    return m_offset <= m_size ? m_size - m_offset : 0;
}

bool Reader::atEnd() const noexcept
{
    return remaining() == 0;
}

bool Reader::failed() const noexcept
{
    return m_failed;
}

bool Reader::require(std::size_t count) noexcept
{
    if (m_failed)
        return false;
    if (count > remaining()) {
        m_failed = true;
        return false;
    }
    return true;
}

std::uint8_t Reader::u8() noexcept
{
    if (!require(1))
        return 0;
    return m_data[m_offset++];
}

std::uint16_t Reader::u16() noexcept
{
    if (!require(2))
        return 0;
    const std::uint16_t value = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(m_data[m_offset]) << 8U) | m_data[m_offset + 1]);
    m_offset += 2;
    return value;
}

std::uint32_t Reader::u32() noexcept
{
    if (!require(4))
        return 0;
    const std::uint32_t value = (static_cast<std::uint32_t>(m_data[m_offset]) << 24U)
                                | (static_cast<std::uint32_t>(m_data[m_offset + 1]) << 16U)
                                | (static_cast<std::uint32_t>(m_data[m_offset + 2]) << 8U)
                                | static_cast<std::uint32_t>(m_data[m_offset + 3]);
    m_offset += 4;
    return value;
}

std::int32_t Reader::i32() noexcept
{
    return static_cast<std::int32_t>(u32());
}

std::vector<std::uint8_t> Reader::bytes(std::size_t count) noexcept
{
    std::vector<std::uint8_t> out;
    if (!require(count))
        return out;
    out.assign(m_data + m_offset, m_data + m_offset + count);
    m_offset += count;
    return out;
}

bool Reader::readInto(std::uint8_t *destination, std::size_t count) noexcept
{
    if (count == 0)
        return !m_failed;
    if (destination == nullptr || !require(count))
        return false;
    std::copy(m_data + m_offset, m_data + m_offset + count, destination);
    m_offset += count;
    return true;
}

void Reader::skip(std::size_t count) noexcept
{
    if (require(count))
        m_offset += count;
}

void Writer::u8(std::uint8_t value)
{
    m_data.push_back(value);
}

void Writer::u16(std::uint16_t value)
{
    m_data.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    m_data.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void Writer::u32(std::uint32_t value)
{
    m_data.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
    m_data.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    m_data.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    m_data.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void Writer::i32(std::int32_t value)
{
    u32(static_cast<std::uint32_t>(value));
}

void Writer::bytes(const void *data, std::size_t size)
{
    if (data == nullptr || size == 0)
        return;
    const auto *first = static_cast<const std::uint8_t *>(data);
    m_data.insert(m_data.end(), first, first + size);
}

void Writer::fill(std::uint8_t value, std::size_t count)
{
    m_data.insert(m_data.end(), count, value);
}

const std::vector<std::uint8_t> &Writer::data() const noexcept
{
    return m_data;
}

std::vector<std::uint8_t> Writer::take()
{
    return std::move(m_data);
}

std::size_t Writer::size() const noexcept
{
    return m_data.size();
}

std::string toHex(const std::uint8_t *data, std::size_t size)
{
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t index = 0; index < size; ++index) {
        out.push_back(kDigits[(data[index] >> 4U) & 0x0fU]);
        out.push_back(kDigits[data[index] & 0x0fU]);
    }
    return out;
}

std::string versionString(std::uint8_t minor)
{
    std::string out = "RFB 003.00";
    out.push_back(static_cast<char>('0' + (minor % 10)));
    out.push_back('\n');
    return out;
}

}  // namespace poc::core_remote::rfb
