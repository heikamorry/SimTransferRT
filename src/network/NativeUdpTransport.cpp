#include "network/NativeUdpTransport.h"
#include "network/FeedbackRead.h"
#include <array>
#include <cstring>
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>
#include <cerrno>
#endif
namespace rt {
struct NativeUdpTransport::Impl {
#ifdef _WIN32
    SOCKET fd = INVALID_SOCKET;
    bool wsa = false;
#else
    int fd = -1;
#endif
    sockaddr_in peer{};
    std::uint16_t port = 0;
    bool df = false;
    int sendBuffer = 0;
    int receiveBuffer = 0;
    int resetError = 0;
    std::string error;
};

NativeUdpTransport::NativeUdpTransport() : p_(std::make_unique<Impl>()) {}

NativeUdpTransport::~NativeUdpTransport() {
#ifdef _WIN32
    if (p_->fd != INVALID_SOCKET)
        closesocket(p_->fd);
    if (p_->wsa)
        WSACleanup();
#else
    if (p_->fd >= 0)
        ::close(p_->fd);
#endif
}

namespace {
std::string socketError() {
#ifdef _WIN32
    return "Winsock error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

bool wouldBlock() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}
} // namespace

void NativeUdpTransport::open(const StreamConfig &c) {
#ifdef _WIN32
    WSADATA d{};
    if (WSAStartup(MAKEWORD(2, 2), &d))
        throw std::runtime_error("WSAStartup failed");
    p_->wsa = true;
#endif
    p_->fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
#ifdef _WIN32
    if (p_->fd == INVALID_SOCKET)
        throw std::runtime_error(socketError());
    u_long nonblock = 1;
    if (ioctlsocket(p_->fd, FIONBIO, &nonblock))
        throw std::runtime_error(socketError());
    BOOL exclusive = TRUE;
    setsockopt(p_->fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive),
               sizeof(exclusive));
    DWORD df = 1;
    if (setsockopt(p_->fd, IPPROTO_IP, IP_DONTFRAGMENT, reinterpret_cast<const char *>(&df),
                   sizeof(df)))
        throw std::runtime_error("DF: " + socketError());
    BOOL reset = FALSE;
    DWORD ignored = 0;
    if (WSAIoctl(p_->fd, SIO_UDP_CONNRESET, &reset, sizeof(reset), nullptr, 0, &ignored, nullptr,
                 nullptr))
        p_->resetError = WSAGetLastError();
#else
    if (p_->fd < 0)
        throw std::runtime_error(socketError());
    if (fcntl(p_->fd, F_SETFL, fcntl(p_->fd, F_GETFL, 0) | O_NONBLOCK) < 0)
        throw std::runtime_error(socketError());
#ifdef IP_MTU_DISCOVER
    int df = IP_PMTUDISC_DO;
    if (setsockopt(p_->fd, IPPROTO_IP, IP_MTU_DISCOVER, &df, sizeof(df)))
        throw std::runtime_error("DF: " + socketError());
#else
    throw std::runtime_error("DF enforcement is implemented for Windows/Linux only");
#endif
#endif
    p_->df = true;
    int buffer = 4 * 1024 * 1024;
    setsockopt(p_->fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char *>(&buffer),
               sizeof(buffer));
    setsockopt(p_->fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char *>(&buffer),
               sizeof(buffer));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(c.commandPort);
    if (inet_pton(AF_INET, c.bindIp.c_str(), &local.sin_addr) != 1)
        throw std::invalid_argument("invalid bind IPv4");
    if (::bind(p_->fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)))
        throw std::runtime_error("bind: " + socketError());
    p_->peer.sin_family = AF_INET;
    p_->peer.sin_port = htons(c.targetPort);
    if (!c.targetPort || inet_pton(AF_INET, c.targetIp.c_str(), &p_->peer.sin_addr) != 1)
        throw std::invalid_argument("invalid target IPv4/port");
    const auto ip = ntohl(p_->peer.sin_addr.s_addr);
    if (!ip || ip >= 0xE0000000u)
        throw std::invalid_argument("target must be IPv4 unicast");
#ifdef _WIN32
    int len = sizeof(local);
#else
    socklen_t len = sizeof(local);
#endif
    if (getsockname(p_->fd, reinterpret_cast<sockaddr *>(&local), &len))
        throw std::runtime_error(socketError());
    p_->port = ntohs(local.sin_port);
    len = sizeof(int);
    getsockopt(p_->fd, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<char *>(&p_->sendBuffer), &len);
    len = sizeof(int);
    getsockopt(p_->fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char *>(&p_->receiveBuffer), &len);
}

int NativeUdpTransport::sendBufferBytes() const {
    return p_->sendBuffer;
}

int NativeUdpTransport::receiveBufferBytes() const {
    return p_->receiveBuffer;
}

int NativeUdpTransport::connResetError() const {
    return p_->resetError;
}

bool NativeUdpTransport::send(const std::uint8_t *bytes, std::size_t n) {
    return submitDatagram(bytes, n, 0) == SendDisposition::Complete;
}

SendDisposition NativeUdpTransport::submitDatagram(const std::uint8_t *bytes, std::size_t n,
                                                   std::uint64_t) {
    if (n > MaxDatagram) {
        p_->error = "datagram exceeds 1472";
        return SendDisposition::Failed;
    }
    auto result = sendto(p_->fd, reinterpret_cast<const char *>(bytes), static_cast<int>(n), 0,
                         reinterpret_cast<const sockaddr *>(&p_->peer), sizeof(p_->peer));
    if (result < 0 && wouldBlock())
        return SendDisposition::Backpressure;
    if (result != static_cast<decltype(result)>(n)) {
        p_->error = result < 0 ? socketError() : "short UDP send";
        return SendDisposition::Failed;
    }
    return SendDisposition::Complete;
}

bool NativeUdpTransport::receive(IncomingDatagram &out) {
    std::array<std::uint8_t, MaxDatagram + 1> b{};
    sockaddr_in from{};
#ifdef _WIN32
    int len = sizeof(from);
#else
    socklen_t len = sizeof(from);
#endif
    auto n = recvfrom(p_->fd, reinterpret_cast<char *>(b.data()), static_cast<int>(b.size()), 0,
                      reinterpret_cast<sockaddr *>(&from), &len);
    if (n < 0) {
        if (!wouldBlock())
            p_->error = socketError();
        return false;
    }
    char ip[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip)))
        return false;
    out.bytes.assign(b.begin(), b.begin() + n);
    out.sourceIp = ip;
    out.sourcePort = ntohs(from.sin_port);
    return true;
}

std::uint16_t NativeUdpTransport::localPort() const {
    return p_->port;
}

ReceiveOutcome NativeUdpTransport::receiveFeedback(IncomingDatagram &out) {
    return readUdpFeedback(p_->fd, out, p_->error);
}

bool NativeUdpTransport::dfEnabled() const {
    return p_->df;
}

std::string NativeUdpTransport::lastError() const {
    return p_->error;
}
} // namespace rt
