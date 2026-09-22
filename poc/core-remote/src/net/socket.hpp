#pragma once

// POC_ONLY minimal blocking TCP layer.
//
// The RFB client must be HyRemoteView-owned and free of GPL client dependencies, so it does not use
// Qt network classes or any third-party socket library. Only the C++ standard library plus the
// platform socket API is used, which is what keeps one source tree buildable on Windows and Linux.
//
// Every call is bounded by an explicit millisecond timeout, and every status is returned instead of
// thrown, because the negative tests assert *deterministic* failure rather than exception types.

#include <cstddef>
#include <cstdint>
#include <string>

namespace poc::core_remote::net {

enum class IoStatus {
    Ok,
    Timeout,  // the deadline expired before the requested bytes arrived
    Closed,   // the peer closed the connection (or the local socket is gone)
    Error,    // a socket-level error; `error` carries the platform message
};

[[nodiscard]] const char *toString(IoStatus status) noexcept;

class Socket
{
public:
    Socket() noexcept = default;
    ~Socket() noexcept;

    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    Socket(Socket &&other) noexcept;
    Socket &operator=(Socket &&other) noexcept;

    // Connects to a numeric IPv4/IPv6 address or a host name, bounded by `timeoutMs`.
    [[nodiscard]] static Socket connectTcp(const std::string &host,
                                          std::uint16_t port,
                                          int timeoutMs,
                                          std::string &error);

    [[nodiscard]] bool valid() const noexcept;
    void close() noexcept;
    void shutdownSend() noexcept;
    [[nodiscard]] bool setNoDelay(bool enabled, std::string &error) noexcept;

    // Reads exactly `size` bytes. Ok means the whole request was satisfied.
    [[nodiscard]] IoStatus readExact(void *buffer,
                                     std::size_t size,
                                     int timeoutMs,
                                     std::string &error) noexcept;

    [[nodiscard]] IoStatus writeAll(const void *data,
                                    std::size_t size,
                                    int timeoutMs,
                                    std::string &error) noexcept;

    // Bytes already buffered by the kernel for this socket; used by the sniffing paths.
    [[nodiscard]] std::size_t available() const noexcept;

    // Waits (bounded) until at least one byte can be read without blocking, or the peer closed.
    [[nodiscard]] IoStatus waitReadable(int timeoutMs, std::string &error) noexcept;

private:
    friend class Listener;
    using Handle = std::uintptr_t;
    static constexpr Handle kInvalid = static_cast<Handle>(~Handle{0});

    explicit Socket(Handle handle) noexcept
        : m_handle(handle)
    {
    }

    Handle m_handle = kInvalid;
};

class Listener
{
public:
    Listener() noexcept = default;
    ~Listener() noexcept;

    Listener(const Listener &) = delete;
    Listener &operator=(const Listener &) = delete;
    Listener(Listener &&other) noexcept;
    Listener &operator=(Listener &&other) noexcept;

    [[nodiscard]] static Listener bindTcp(const std::string &host,
                                          std::uint16_t port,
                                          int backlog,
                                          std::string &error);

    [[nodiscard]] bool valid() const noexcept;
    // The bound port, which is the OS-assigned one when the request asked for port 0.
    [[nodiscard]] std::uint16_t port() const noexcept;
    void close() noexcept;

    [[nodiscard]] IoStatus accept(Socket &out, int timeoutMs, std::string &error) noexcept;

private:
    Socket::Handle m_handle = Socket::kInvalid;
    std::uint16_t m_port = 0;
};

}  // namespace poc::core_remote::net
