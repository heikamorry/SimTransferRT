#include "network/AsioReceiverSocket.h"
#include "core/SamplingClock.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <windows.h>
#endif
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <system_error>
#ifndef _WIN32
#include <cerrno>
#include <poll.h>
#endif

namespace rt {
using boost::asio::ip::address_v4;
using boost::asio::ip::udp;

struct AsioReceiverSocket::Impl {
    boost::asio::io_context context;
    udp::socket socket{context, udp::v4()};
#ifdef _WIN32
    WSAEVENT readable = WSACreateEvent();
    HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_MODIFY_STATE | SYNCHRONIZE);

    ~Impl() {
        if (readable != WSA_INVALID_EVENT) {
            WSAEventSelect(socket.native_handle(), nullptr, 0);
            WSACloseEvent(readable);
        }
        if (timer)
            CloseHandle(timer);
    }
#endif
};

AsioReceiverSocket::AsioReceiverSocket(std::uint32_t bindIp, std::uint16_t port)
    : p_(std::make_unique<Impl>()) {
#ifdef _WIN32
    const auto fd = p_->socket.native_handle();
    const BOOL exclusive = TRUE;
    if (setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char *>(&exclusive),
                   sizeof(exclusive)))
        throw std::system_error(WSAGetLastError(), std::system_category(), "SO_EXCLUSIVEADDRUSE");
    BOOL reset = FALSE;
    DWORD ignored = 0;
    if (WSAIoctl(fd, SIO_UDP_CONNRESET, &reset, sizeof(reset), nullptr, 0, &ignored, nullptr,
                 nullptr))
        throw std::system_error(WSAGetLastError(), std::system_category(), "SIO_UDP_CONNRESET");
#endif
    p_->socket.bind({address_v4(bindIp), port});
    p_->socket.set_option(udp::socket::receive_buffer_size(4 * 1024 * 1024));
    p_->socket.non_blocking(true);
#ifdef _WIN32
    if (p_->timer && p_->readable != WSA_INVALID_EVENT &&
        WSAEventSelect(p_->socket.native_handle(), p_->readable, FD_READ) == SOCKET_ERROR)
        throw std::system_error(WSAGetLastError(), std::system_category(), "WSAEventSelect");
#endif
}

AsioReceiverSocket::~AsioReceiverSocket() = default;

void AsioReceiverSocket::waitReadable(std::uint64_t deadlineUs) {
    const auto now = monotonicUs();
    if (now >= deadlineUs)
        return;
    const auto fd = p_->socket.native_handle();
    const auto remaining = deadlineUs - now;
#ifdef _WIN32
    if (p_->timer && p_->readable != WSA_INVALID_EVENT) {
        HANDLE handles[]{p_->readable, p_->timer};
        LARGE_INTEGER relative{};
        relative.QuadPart = -static_cast<LONGLONG>(remaining * 10);
        if (!SetWaitableTimer(p_->timer, &relative, 0, nullptr, nullptr, FALSE))
            throw std::system_error(GetLastError(), std::system_category(), "SetWaitableTimer");
        const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
        if (result == WAIT_OBJECT_0) {
            WSANETWORKEVENTS events{};
            if (WSAEnumNetworkEvents(fd, p_->readable, &events) == SOCKET_ERROR)
                throw std::system_error(WSAGetLastError(), std::system_category(),
                                        "WSAEnumNetworkEvents");
            if (events.iErrorCode[FD_READ_BIT])
                throw std::system_error(events.iErrorCode[FD_READ_BIT], std::system_category(),
                                        "FD_READ");
        } else if (result != WAIT_OBJECT_0 + 1)
            throw std::system_error(GetLastError(), std::system_category(), "receiver wait");
        return;
    }
    fd_set ready;
    FD_ZERO(&ready);
    FD_SET(fd, &ready);
    timeval timeout{static_cast<long>(remaining / 1000000), static_cast<long>(remaining % 1000000)};
    if (::select(0, &ready, nullptr, nullptr, &timeout) == SOCKET_ERROR &&
        WSAGetLastError() != WSAEINTR)
        throw std::system_error(WSAGetLastError(), std::system_category(), "select");
#else
    pollfd ready{fd, POLLIN, 0};
    if (::poll(&ready, 1, static_cast<int>((remaining + 999) / 1000)) < 0 && errno != EINTR)
        throw std::system_error(errno, std::generic_category(), "poll");
#endif
}

std::optional<bool> AsioReceiverSocket::readableNow() const {
#ifdef _WIN32
    WSAPOLLFD fd{p_->socket.native_handle(), POLLRDNORM, 0};
    const int result = WSAPoll(&fd, 1, 0);
#else
    pollfd fd{p_->socket.native_handle(), POLLIN, 0};
    const int result = ::poll(&fd, 1, 0);
#endif
    if (result < 0 || (fd.revents & (POLLERR | POLLHUP | POLLNVAL)))
        return std::nullopt;
    return (fd.revents & fd.events) != 0;
}

int AsioReceiverSocket::read(std::uint8_t *data, std::size_t capacity, std::uint32_t &sourceIp,
                             std::uint16_t &sourcePort) {
    udp::endpoint source;
    boost::system::error_code ec;
    auto size = p_->socket.receive_from(boost::asio::buffer(data, capacity), source, 0, ec);
    if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again ||
        ec == boost::asio::error::interrupted)
        return -1;
    if (ec == boost::asio::error::message_size) {
        // Winsock copied the prefix and discarded the remainder. Preserve the
        // oversized length so the protocol decoder rejects this packet without
        // stopping the receiver. Callers reserve MaxDatagram + 1 bytes.
        size = capacity;
    } else if (ec) {
        throw boost::system::system_error(ec, "UDP receive_from");
    }
    sourceIp = source.address().to_v4().to_uint();
    sourcePort = source.port();
    return static_cast<int>(size);
}

bool AsioReceiverSocket::sendFeedback(const Datagram &packet, std::uint32_t ip,
                                      std::uint16_t port) {
    boost::system::error_code ec;
    const auto size = p_->socket.send_to(boost::asio::buffer(packet.bytes.data(), packet.size),
                                         {address_v4(ip), port}, 0, ec);
    return !ec && size == packet.size;
}
} // namespace rt
