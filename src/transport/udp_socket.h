// Non-blocking IPv4/IPv6 UDP socket (on Windows with overlapped sends, see SendTo)
#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>

namespace vv {

#ifdef _WIN32
using SocketHandle = SOCKET;
using SocketLength = int;
#else
using SocketHandle = int;
using SocketLength = socklen_t;
#endif

struct SocketAddress {
    sockaddr_storage storage{};
    SocketLength length = 0;

    static std::optional<SocketAddress> Resolve(const std::string& host, int port);
    std::string Host() const;
    int Port() const;
    bool operator==(const SocketAddress& other) const;
};

class UdpSocket {
public:
    UdpSocket() = default;
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool Open(int family);
    void Close();
    bool IsOpen() const;
    SocketHandle Handle() const { return fd_; }
    bool SendTo(const uint8_t* data, size_t length, const SocketAddress& to);
    // -1 when nothing is pending
    int64_t RecvFrom(uint8_t* buffer, size_t capacity, SocketAddress& from);
    void SetDscp(int dscp);
    int LocalPort() const;

private:
#ifdef _WIN32
    void ReapSends(DWORD timeoutMs);

    SocketHandle fd_ = INVALID_SOCKET;
    // Sends are overlapped (see SendTo): their completions, sends still in the kernel
    HANDLE port_ = nullptr;
    bool skipCompletionOnSuccess_ = false;
    std::atomic<int> inFlight_{0};
    std::atomic<bool> loggedPending_{false};
#else
    SocketHandle fd_ = -1;
#endif
    int family_ = AF_INET;
};

// Waits until one of the (open) sockets has something to read, at most timeoutMs. False on timeout
bool WaitReadable(std::initializer_list<const UdpSocket*> sockets, int timeoutMs);

} // namespace vv
