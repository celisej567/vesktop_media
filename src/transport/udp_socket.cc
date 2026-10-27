#include "transport/udp_socket.h"

#ifdef _WIN32
#include <mstcpip.h>
#include <mswsock.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <unistd.h>
#endif

#include <cstring>
#include <vector>

#include "common/log.h"

namespace vv {

#ifdef _WIN32
namespace {
// Winsock needs WSAStartup once per process before any socket call (never undone: the module stays loaded)
void EnsureWinsock()
{
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    (void)started;
}
} // namespace
#endif

std::optional<SocketAddress> SocketAddress::Resolve(const std::string& host, int port)
{
#ifdef _WIN32
    EnsureWinsock();
#endif
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_ADDRCONFIG;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        // AI_ADDRCONFIG hides loopback-only setups, retry without it
        hints.ai_flags = 0;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return std::nullopt;
    }
    SocketAddress addr;
    const addrinfo* pick = res;
    for (auto* p = res; p; p = p->ai_next)
        if (p->ai_family == AF_INET) {
            pick = p;
            break;
        }
    memcpy(&addr.storage, pick->ai_addr, pick->ai_addrlen);
    addr.length = static_cast<SocketLength>(pick->ai_addrlen);
    freeaddrinfo(res);
    return addr;
}

std::string SocketAddress::Host() const
{
    char buf[INET6_ADDRSTRLEN] = {};
    if (storage.ss_family == AF_INET)
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(&storage)->sin_addr, buf, sizeof(buf));
    else if (storage.ss_family == AF_INET6)
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_addr, buf, sizeof(buf));
    return buf;
}

int SocketAddress::Port() const
{
    if (storage.ss_family == AF_INET) return ntohs(reinterpret_cast<const sockaddr_in*>(&storage)->sin_port);
    if (storage.ss_family == AF_INET6) return ntohs(reinterpret_cast<const sockaddr_in6*>(&storage)->sin6_port);
    return 0;
}

bool SocketAddress::operator==(const SocketAddress& other) const
{
    return Host() == other.Host() && Port() == other.Port();
}

UdpSocket::~UdpSocket()
{
    Close();
}

bool UdpSocket::IsOpen() const
{
#ifdef _WIN32
    return fd_ != INVALID_SOCKET;
#else
    return fd_ >= 0;
#endif
}

bool UdpSocket::Open(int family)
{
    Close();
    family_ = family;
#ifdef _WIN32
    EnsureWinsock();
    fd_ = WSASocketW(family, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0, WSA_FLAG_NO_HANDLE_INHERIT);
    if (fd_ == INVALID_SOCKET) {
        VV_LOGE("udp", "socket() failed: %d", WSAGetLastError());
        return false;
    }
    u_long nonBlocking = 1;
    ioctlsocket(fd_, FIONBIO, &nonBlocking);
    // without this an ICMP "port unreachable" makes the next recvfrom fail with WSAECONNRESET and the socket
    // reports it instead of the packets behind it
    BOOL reportReset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(fd_, SIO_UDP_CONNRESET, &reportReset, sizeof(reportReset), nullptr, 0, &bytes, nullptr, nullptr);
#else
    fd_ = socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        VV_LOGE("udp", "socket() failed: %s", strerror(errno));
        return false;
    }
#endif
    int buf = 1 << 20;
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
    setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
    return true;
}

void UdpSocket::Close()
{
    if (!IsOpen()) return;
#ifdef _WIN32
    closesocket(fd_);
    fd_ = INVALID_SOCKET;
#else
    close(fd_);
    fd_ = -1;
#endif
}

bool UdpSocket::SendTo(const uint8_t* data, size_t length, const SocketAddress& to)
{
    if (!IsOpen()) return false;
    auto sent = sendto(fd_, reinterpret_cast<const char*>(data), static_cast<int>(length), 0,
                       reinterpret_cast<const sockaddr*>(&to.storage), to.length);
    return sent == static_cast<decltype(sent)>(length);
}

int64_t UdpSocket::RecvFrom(uint8_t* buffer, size_t capacity, SocketAddress& from)
{
    if (!IsOpen()) return -1;
    from.length = sizeof(from.storage);
    return recvfrom(fd_, reinterpret_cast<char*>(buffer), static_cast<int>(capacity), 0,
                    reinterpret_cast<sockaddr*>(&from.storage), &from.length);
}

void UdpSocket::SetDscp(int dscp)
{
    if (!IsOpen()) return;
    // Windows ignores IP_TOS without the qWAVE API (and admin rights for most values); harmless to try
    int tos = dscp << 2;
    if (family_ == AF_INET6) setsockopt(fd_, IPPROTO_IPV6, IPV6_TCLASS, reinterpret_cast<const char*>(&tos), sizeof(tos));
    else setsockopt(fd_, IPPROTO_IP, IP_TOS, reinterpret_cast<const char*>(&tos), sizeof(tos));
}

int UdpSocket::LocalPort() const
{
    sockaddr_storage s{};
    SocketLength len = sizeof(s);
    if (!IsOpen() || getsockname(fd_, reinterpret_cast<sockaddr*>(&s), &len) != 0) return 0;
    SocketAddress a;
    a.storage = s;
    a.length = len;
    return a.Port();
}

bool WaitReadable(std::initializer_list<const UdpSocket*> sockets, int timeoutMs)
{
    pollfd fds[4];
    unsigned n = 0;
    for (auto* s : sockets)
        if (s->IsOpen() && n < 4) fds[n++] = {s->Handle(), POLLIN, 0};
#ifdef _WIN32
    // WSAPoll with no sockets is an error, not a sleep
    if (!n) {
        Sleep(static_cast<DWORD>(timeoutMs));
        return false;
    }
    return WSAPoll(fds, n, timeoutMs) > 0;
#else
    return poll(fds, n, timeoutMs) > 0;
#endif
}

} // namespace vv
