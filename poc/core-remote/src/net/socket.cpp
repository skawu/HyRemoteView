#include "net/socket.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
using SocketHandle = SOCKET;
static constexpr SocketHandle kBadSocket = INVALID_SOCKET;
#else
#  include <arpa/inet.h>
#  include <errno.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <sys/select.h>
#  include <sys/ioctl.h>  // ioctl(FIONREAD) for Socket::available() on POSIX
#  include <sys/socket.h>
#  include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle kBadSocket = -1;
#endif

namespace poc::core_remote::net {
namespace {

#ifdef _WIN32
struct WinsockGuard
{
    WinsockGuard()
    {
        WSADATA data{};
        WSAStartup(MAKEWORD(2, 2), &data);
    }
    ~WinsockGuard() { WSACleanup(); }
};

void ensureSocketsInitialized()
{
    static WinsockGuard guard;
    (void)guard;
}

std::string lastSocketError()
{
    const int code = WSAGetLastError();
    switch (code) {
    case WSAETIMEDOUT:
        return "socket operation timed out";
    case WSAECONNRESET:
        return "connection reset by peer";
    case WSAECONNREFUSED:
        return "connection refused";
    case WSAENOTCONN:
        return "socket is not connected";
    case WSAHOST_NOT_FOUND:
        return "host not found";
    default:
        return "winsock error " + std::to_string(code);
    }
}

bool wouldBlock()
{
    const int code = WSAGetLastError();
    return code == WSAEWOULDBLOCK || code == WSAEINPROGRESS || code == WSAEALREADY;
}

void closeHandle(SocketHandle handle)
{
    if (handle != kBadSocket)
        closesocket(handle);
}

bool setNonBlocking(SocketHandle handle, bool nonBlocking)
{
    u_long mode = nonBlocking ? 1UL : 0UL;
    return ioctlsocket(handle, FIONBIO, &mode) == 0;
}
#else
void ensureSocketsInitialized() {}

std::string lastSocketError()
{
    return std::strerror(errno);
}

bool wouldBlock()
{
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS || errno == EALREADY;
}

void closeHandle(SocketHandle handle)
{
    if (handle != kBadSocket)
        ::close(handle);
}

bool setNonBlocking(SocketHandle handle, bool nonBlocking)
{
    const int flags = fcntl(handle, F_GETFL, 0);
    if (flags < 0)
        return false;
    const int updated = nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(handle, F_SETFL, updated) == 0;
}
#endif

using Clock = std::chrono::steady_clock;

int remainingMillis(const Clock::time_point &deadline)
{
    const auto now = Clock::now();
    if (now >= deadline)
        return 0;
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    return static_cast<int>(std::min<long long>(remaining, 1000));
}

// Waits for `forWrite` readiness of one socket, bounded by the deadline.
IoStatus waitOne(SocketHandle handle,
                 bool forWrite,
                 const Clock::time_point &deadline,
                 std::string &error)
{
    for (;;) {
        const int slice = remainingMillis(deadline);
        if (slice <= 0)
            return IoStatus::Timeout;

        fd_set set;
        FD_ZERO(&set);
        FD_SET(handle, &set);
        timeval tv{};
        tv.tv_sec = slice / 1000;
        tv.tv_usec = (slice % 1000) * 1000;
#ifdef _WIN32
        const int ready = ::select(0, forWrite ? nullptr : &set, forWrite ? &set : nullptr, nullptr, &tv);
#else
        const int ready = ::select(handle + 1,
                                   forWrite ? nullptr : &set,
                                   forWrite ? &set : nullptr,
                                   nullptr,
                                   &tv);
#endif
        if (ready > 0)
            return IoStatus::Ok;
        if (ready == 0)
            continue;  // the slice expired; re-check the overall deadline
        if (wouldBlock())
            continue;
        error = lastSocketError();
        return IoStatus::Error;
    }
}

std::string resolveError(int code, const std::string &host, std::uint16_t port)
{
    return "cannot resolve " + host + ":" + std::to_string(port) + " (" +
#ifdef _WIN32
           std::to_string(code)
#else
           gai_strerror(code)
#endif
           + ")";
}

}  // namespace

const char *toString(IoStatus status) noexcept
{
    switch (status) {
    case IoStatus::Ok:
        return "ok";
    case IoStatus::Timeout:
        return "timeout";
    case IoStatus::Closed:
        return "closed";
    case IoStatus::Error:
        return "error";
    }
    return "unknown";
}

Socket::~Socket() noexcept
{
    close();
}

Socket::Socket(Socket &&other) noexcept
    : m_handle(other.m_handle)
{
    other.m_handle = kInvalid;
}

Socket &Socket::operator=(Socket &&other) noexcept
{
    if (this != &other) {
        close();
        m_handle = other.m_handle;
        other.m_handle = kInvalid;
    }
    return *this;
}

bool Socket::valid() const noexcept
{
    return m_handle != kInvalid;
}

void Socket::close() noexcept
{
    if (m_handle != kInvalid) {
        closeHandle(static_cast<SocketHandle>(m_handle));
        m_handle = kInvalid;
    }
}

void Socket::shutdownSend() noexcept
{
    if (m_handle == kInvalid)
        return;
#ifdef _WIN32
    ::shutdown(static_cast<SocketHandle>(m_handle), SD_SEND);
#else
    ::shutdown(static_cast<SocketHandle>(m_handle), SHUT_WR);
#endif
}

bool Socket::setNoDelay(bool enabled, std::string &error) noexcept
{
    if (m_handle == kInvalid) {
        error = "socket is not open";
        return false;
    }
    const int value = enabled ? 1 : 0;
    if (::setsockopt(static_cast<SocketHandle>(m_handle),
                     IPPROTO_TCP,
                     TCP_NODELAY,
                     reinterpret_cast<const char *>(&value),
                     sizeof(value))
        != 0) {
        error = lastSocketError();
        return false;
    }
    return true;
}

Socket Socket::connectTcp(const std::string &host, std::uint16_t port, int timeoutMs, std::string &error)
{
    ensureSocketsInitialized();
    error.clear();

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfo *resolved = nullptr;
    const std::string service = std::to_string(port);
    const int lookup = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &resolved);
    if (lookup != 0 || resolved == nullptr) {
        error = resolveError(lookup, host, port);
        return {};
    }

    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 1));
    SocketHandle handle = kBadSocket;

    for (addrinfo *candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
        if (remainingMillis(deadline) <= 0) {
            error = "connect timed out";
            break;
        }

        handle = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (handle == kBadSocket)
            continue;

        // Non-blocking connect so the deadline is honoured even when the peer does not answer.
        const bool nonBlocking = setNonBlocking(handle, true);
        const int connectResult =
            ::connect(handle, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen));
        if (connectResult != 0) {
            if (!nonBlocking || !wouldBlock()) {
                closeHandle(handle);
                handle = kBadSocket;
                continue;
            }

            std::string waitError;
            const IoStatus status = waitOne(handle, true, deadline, waitError);
            if (status != IoStatus::Ok) {
                error = status == IoStatus::Timeout ? "connect timed out" : waitError;
                closeHandle(handle);
                handle = kBadSocket;
                continue;
            }

            int socketError = 0;
#ifdef _WIN32
            int length = sizeof(socketError);
#else
            socklen_t length = sizeof(socketError);
#endif
            if (::getsockopt(handle, SOL_SOCKET, SO_ERROR,
                             reinterpret_cast<char *>(&socketError), &length)
                    != 0
                || socketError != 0) {
                error = socketError != 0 ? lastSocketError() : "connect failed";
                closeHandle(handle);
                handle = kBadSocket;
                continue;
            }
        }

        setNonBlocking(handle, false);  // the protocol layer wants blocking-with-timeout semantics
        (void)nonBlocking;
        break;
    }

    ::freeaddrinfo(resolved);

    if (handle == kBadSocket) {
        if (error.empty())
            error = "cannot connect to " + host + ":" + std::to_string(port);
        return {};
    }

    Socket socket(static_cast<Handle>(handle));
    std::string ignored;
    (void)socket.setNoDelay(true, ignored);
    return socket;
}

IoStatus Socket::readExact(void *buffer,
                           std::size_t size,
                           int timeoutMs,
                           std::string &error) noexcept
{
    error.clear();
    if (m_handle == kInvalid)
        return IoStatus::Closed;

    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 1));
    auto *cursor = static_cast<std::uint8_t *>(buffer);
    std::size_t done = 0;

    while (done < size) {
        std::string waitError;
        const IoStatus ready = waitOne(static_cast<SocketHandle>(m_handle), false, deadline, waitError);
        if (ready != IoStatus::Ok) {
            if (ready == IoStatus::Error)
                error = waitError;
            return ready;
        }

        const int received = ::recv(static_cast<SocketHandle>(m_handle),
                                    reinterpret_cast<char *>(cursor + done),
                                    static_cast<int>(std::min<std::size_t>(size - done, 1U << 20)),
                                    0);
        if (received == 0)
            return IoStatus::Closed;
        if (received < 0) {
            if (wouldBlock())
                continue;
            error = lastSocketError();
            return IoStatus::Error;
        }
        done += static_cast<std::size_t>(received);
    }
    return IoStatus::Ok;
}

IoStatus Socket::writeAll(const void *data,
                          std::size_t size,
                          int timeoutMs,
                          std::string &error) noexcept
{
    error.clear();
    if (m_handle == kInvalid)
        return IoStatus::Closed;

    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 1));
    const auto *cursor = static_cast<const std::uint8_t *>(data);
    std::size_t done = 0;

    while (done < size) {
        std::string waitError;
        const IoStatus ready = waitOne(static_cast<SocketHandle>(m_handle), true, deadline, waitError);
        if (ready != IoStatus::Ok) {
            if (ready == IoStatus::Error)
                error = waitError;
            return ready;
        }

        const int sent = ::send(static_cast<SocketHandle>(m_handle),
                                reinterpret_cast<const char *>(cursor + done),
                                static_cast<int>(std::min<std::size_t>(size - done, 1U << 20)),
                                0);
        if (sent <= 0) {
            if (wouldBlock())
                continue;
            error = lastSocketError();
            return IoStatus::Error;
        }
        done += static_cast<std::size_t>(sent);
    }
    return IoStatus::Ok;
}

std::size_t Socket::available() const noexcept
{
    if (m_handle == kInvalid)
        return 0;
    u_long count = 0;
#ifdef _WIN32
    if (ioctlsocket(static_cast<SocketHandle>(m_handle), FIONREAD, &count) != 0)
        return 0;
#else
    int countSigned = 0;
    if (ioctl(static_cast<SocketHandle>(m_handle), FIONREAD, &countSigned) != 0)
        return 0;
    count = static_cast<u_long>(countSigned < 0 ? 0 : countSigned);
#endif
    return static_cast<std::size_t>(count);
}

IoStatus Socket::waitReadable(int timeoutMs, std::string &error) noexcept
{
    error.clear();
    if (m_handle == kInvalid)
        return IoStatus::Closed;
    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 1));
    return waitOne(static_cast<SocketHandle>(m_handle), false, deadline, error);
}

Listener::~Listener() noexcept
{
    close();
}

Listener::Listener(Listener &&other) noexcept
    : m_handle(other.m_handle)
    , m_port(other.m_port)
{
    other.m_handle = Socket::kInvalid;
    other.m_port = 0;
}

Listener &Listener::operator=(Listener &&other) noexcept
{
    if (this != &other) {
        close();
        m_handle = other.m_handle;
        m_port = other.m_port;
        other.m_handle = Socket::kInvalid;
        other.m_port = 0;
    }
    return *this;
}

bool Listener::valid() const noexcept
{
    return m_handle != Socket::kInvalid;
}

std::uint16_t Listener::port() const noexcept
{
    return m_port;
}

void Listener::close() noexcept
{
    if (m_handle != Socket::kInvalid) {
        closeHandle(static_cast<SocketHandle>(m_handle));
        m_handle = Socket::kInvalid;
    }
}

Listener Listener::bindTcp(const std::string &host,
                           std::uint16_t port,
                           int backlog,
                           std::string &error)
{
    ensureSocketsInitialized();
    error.clear();

    addrinfo hints{};
    hints.ai_family = AF_INET;  // the PoC only ever binds loopback
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo *resolved = nullptr;
    const std::string service = std::to_string(port);
    const int lookup = ::getaddrinfo(host.empty() ? "127.0.0.1" : host.c_str(),
                                     service.c_str(),
                                     &hints,
                                     &resolved);
    if (lookup != 0 || resolved == nullptr) {
        error = resolveError(lookup, host, port);
        return {};
    }

    Listener listener;
    for (addrinfo *candidate = resolved; candidate != nullptr; candidate = candidate->ai_next) {
        SocketHandle handle = ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
        if (handle == kBadSocket)
            continue;

        const int reuse = 1;
        (void)::setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
                           reinterpret_cast<const char *>(&reuse), sizeof(reuse));

        if (::bind(handle, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) != 0
            || ::listen(handle, backlog <= 0 ? 8 : backlog) != 0) {
            error = lastSocketError();
            closeHandle(handle);
            continue;
        }

        sockaddr_storage bound{};
#ifdef _WIN32
        int boundLength = sizeof(bound);
#else
        socklen_t boundLength = sizeof(bound);
#endif
        if (::getsockname(handle, reinterpret_cast<sockaddr *>(&bound), &boundLength) == 0) {
            const auto *in4 = reinterpret_cast<const sockaddr_in *>(&bound);
            listener.m_port = ntohs(in4->sin_port);
        }

        listener.m_handle = static_cast<Socket::Handle>(handle);
        break;
    }

    ::freeaddrinfo(resolved);

    if (!listener.valid() && error.empty())
        error = "cannot bind " + host + ":" + std::to_string(port);
    return listener;
}

IoStatus Listener::accept(Socket &out, int timeoutMs, std::string &error) noexcept
{
    error.clear();
    out.close();
    if (m_handle == Socket::kInvalid)
        return IoStatus::Closed;

    const auto deadline = Clock::now() + std::chrono::milliseconds(std::max(timeoutMs, 1));
    for (;;) {
        std::string waitError;
        const IoStatus ready = waitOne(static_cast<SocketHandle>(m_handle), false, deadline, waitError);
        if (ready != IoStatus::Ok) {
            if (ready == IoStatus::Error)
                error = waitError;
            return ready;
        }

        sockaddr_storage peer{};
#ifdef _WIN32
        int peerLength = sizeof(peer);
#else
        socklen_t peerLength = sizeof(peer);
#endif
        const SocketHandle accepted =
            ::accept(static_cast<SocketHandle>(m_handle), reinterpret_cast<sockaddr *>(&peer), &peerLength);
        if (accepted == kBadSocket) {
            if (wouldBlock())
                continue;
            error = lastSocketError();
            return IoStatus::Error;
        }

        Socket socket(static_cast<Socket::Handle>(accepted));
        std::string ignored;
        (void)socket.setNoDelay(true, ignored);
        out = std::move(socket);
        return IoStatus::Ok;
    }
}

}  // namespace poc::core_remote::net
