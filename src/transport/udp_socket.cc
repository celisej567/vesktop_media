#include "transport/udp_socket.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <unistd.h>

#include <cstring>

#include "common/log.h"

namespace vv {

std::optional<SocketAddress> SocketAddress::Resolve(const std::string& host, int port)
{
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
    addr.length = pick->ai_addrlen;
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

bool UdpSocket::Open(int family)
{
    Close();
    family_ = family;
    fd_ = socket(family, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
        VV_LOGE("udp", "socket() failed: %s", strerror(errno));
        return false;
    }
    int buf = 1 << 20;
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));
    setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    return true;
}

void UdpSocket::Close()
{
    if (fd_ >= 0) close(fd_);
    fd_ = -1;
}

bool UdpSocket::SendTo(const uint8_t* data, size_t length, const SocketAddress& to)
{
    if (fd_ < 0) return false;
    return sendto(fd_, data, length, 0, reinterpret_cast<const sockaddr*>(&to.storage), to.length) == static_cast<ssize_t>(length);
}

ssize_t UdpSocket::RecvFrom(uint8_t* buffer, size_t capacity, SocketAddress& from)
{
    if (fd_ < 0) return -1;
    from.length = sizeof(from.storage);
    return recvfrom(fd_, buffer, capacity, 0, reinterpret_cast<sockaddr*>(&from.storage), &from.length);
}

void UdpSocket::SetDscp(int dscp)
{
    if (fd_ < 0) return;
    int tos = dscp << 2;
    if (family_ == AF_INET6) setsockopt(fd_, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof(tos));
    else setsockopt(fd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
}

int UdpSocket::LocalPort() const
{
    sockaddr_storage s{};
    socklen_t len = sizeof(s);
    if (fd_ < 0 || getsockname(fd_, reinterpret_cast<sockaddr*>(&s), &len) != 0) return 0;
    SocketAddress a;
    a.storage = s;
    a.length = len;
    return a.Port();
}

} // namespace vv
