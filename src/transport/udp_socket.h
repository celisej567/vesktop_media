// Plain non-blocking IPv4/IPv6 UDP socket
#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace vv {

struct SocketAddress {
    sockaddr_storage storage{};
    socklen_t length = 0;

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
    int Fd() const { return fd_; }
    bool SendTo(const uint8_t* data, size_t length, const SocketAddress& to);
    // -1 when nothing is pending
    ssize_t RecvFrom(uint8_t* buffer, size_t capacity, SocketAddress& from);
    void SetDscp(int dscp);
    int LocalPort() const;

private:
    int fd_ = -1;
    int family_ = AF_INET;
};

} // namespace vv
