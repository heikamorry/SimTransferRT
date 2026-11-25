#pragma once
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <sys/socket.h>
#endif
#include "network/IUdpTransport.h"
#include <array>

namespace rt {
// Both production adapters own a nonblocking IPv4 UDP socket. Read the OS
// result directly so a failed feedback read cannot look like an empty queue.
#ifdef _WIN32
using FeedbackSocket = SOCKET;
#else
using FeedbackSocket = int;
#endif
inline ReceiveOutcome readUdpFeedback(FeedbackSocket socket, IncomingDatagram &out,
                                      std::string &error) {
    std::array<char, MaxDatagram + 1> buffer;
    sockaddr_in source{};
#ifdef _WIN32
    int addressSize = sizeof(source);
#else
    socklen_t addressSize = sizeof(source);
#endif
    auto size = recvfrom(socket, buffer.data(), static_cast<int>(buffer.size()), 0,
                         reinterpret_cast<sockaddr *>(&source), &addressSize);
    if (size < 0) {
#ifdef _WIN32
        const int code = WSAGetLastError();
        if (code == WSAEWOULDBLOCK || code == WSAEINTR)
            return ReceiveOutcome::Empty;
        if (code != WSAEMSGSIZE) {
            error = "Winsock feedback receive error " + std::to_string(code);
            return ReceiveOutcome::Error;
        }
        // Winsock copied the prefix and discarded the rest of this datagram.
        // The extra byte preserves the oversized length for protocol rejection.
        size = static_cast<int>(buffer.size());
#else
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
            return ReceiveOutcome::Empty;
        error = std::strerror(errno);
        return ReceiveOutcome::Error;
#endif
    }
    char ip[INET_ADDRSTRLEN]{};
    if (!inet_ntop(AF_INET, &source.sin_addr, ip, sizeof(ip))) {
        error = "Cannot format feedback source IPv4 address";
        return ReceiveOutcome::Error;
    }
    out.bytes.assign(buffer.data(), buffer.data() + size);
    out.sourceIp = ip;
    out.sourcePort = ntohs(source.sin_port);
    return ReceiveOutcome::Received;
}
} // namespace rt
