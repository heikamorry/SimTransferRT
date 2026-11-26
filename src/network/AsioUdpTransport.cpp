#include "network/AsioUdpTransport.h"
#include "network/AsioOperationStorage.h"
#include "network/FeedbackRead.h"
#include "core/SamplingClock.h"
#include <boost/asio/bind_allocator.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address_v4.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/version.hpp>
#include <boost/version.hpp>
#include <array>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#else
#include <cerrno>
#include <sys/socket.h>
#endif

namespace rt {
namespace {
using boost::asio::ip::address_v4;
using boost::asio::ip::udp;

// Datagram buffers have fixed capacity, bounding what a stalled socket can
// pin. One completion slot per pool slot, so a completion
// can never be dropped: it is only produced while its slot is still in flight.
constexpr std::size_t SlotCount = 64;
constexpr std::size_t SlotBytes = 256 * 1024;
static_assert(SlotCount * MaxDatagram <= SlotBytes, "send pool exceeds its byte budget");

struct Slot {
    std::array<std::uint8_t, MaxDatagram> bytes{};
    std::size_t size = 0;
    std::uint64_t token = 0;
    bool active = false;     // buffer owned by an outstanding operation
    bool unobserved = false; // completed but not yet reaped by the engine
};

class AsioUdpTransport final : public IUdpTransport {
  public:
    explicit AsioUdpTransport(bool async) : async_(async) {}

    void setOperationStorageReuse(bool enabled) override {
        if (socket_.is_open())
            throw std::logic_error("configure operation storage reuse before opening the socket");
        if (enabled && !async_)
            throw std::invalid_argument("operation storage reuse requires asynchronous Boost.Asio");
        if (enabled)
            operationStorage_ = std::make_unique<AsioOperationStorage>();
        else
            operationStorage_.reset();
    }

    OperationStorageStats operationStorageStats() const noexcept override {
        return operationStorage_ ? operationStorage_->stats() : OperationStorageStats{};
    }

    ~AsioUdpTransport() override {
        // Cancellation still delivers handlers; observe them before the buffers
        // and the socket die, otherwise an operation could reference freed memory.
        cancelOutstanding();
        SendCompletion ignoredCompletion;
        for (int i = 0; i < 100000 && inFlight(); ++i)
            (void)pollCompletion(ignoredCompletion);
        boost::system::error_code ignored;
        socket_.close(ignored);
    }

    void open(const StreamConfig &c) override {
        try {
            target_ = boost::asio::ip::make_address_v4(c.targetIp);
        } catch (const std::exception &) {
            throw std::invalid_argument("target must be numeric IPv4 unicast and nonzero port");
        }
        if (c.targetIp.empty() || target_.is_unspecified() || target_.is_multicast() ||
            target_.to_uint() == 0xFFFFFFFFu || !c.targetPort)
            throw std::invalid_argument("target must be numeric IPv4 unicast and nonzero port");
        address_v4 local;
        try {
            local = boost::asio::ip::make_address_v4(c.bindIp);
        } catch (const std::exception &) {
            throw std::invalid_argument("bind must be numeric IPv4");
        }

        boost::system::error_code ec;
        socket_.open(udp::v4(), ec);
        if (ec)
            throw std::runtime_error("Asio UDP open: " + ec.message());
#ifdef _WIN32
        const auto fd = socket_.native_handle();
        const BOOL exclusive = TRUE;
        if (setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char *>(&exclusive), sizeof(exclusive)))
            throw std::runtime_error("SO_EXCLUSIVEADDRUSE: " + std::to_string(WSAGetLastError()));
        DWORD df = 1;
        if (setsockopt(fd, IPPROTO_IP, IP_DONTFRAGMENT, reinterpret_cast<const char *>(&df),
                       sizeof(df)))
            throw std::runtime_error("IP_DONTFRAGMENT: " + std::to_string(WSAGetLastError()));
        BOOL reset = FALSE;
        DWORD ignored = 0;
        if (WSAIoctl(fd, SIO_UDP_CONNRESET, &reset, sizeof(reset), nullptr, 0, &ignored, nullptr,
                     nullptr))
            resetError_ = WSAGetLastError();
#else
        const auto fd = socket_.native_handle();
#ifdef IP_MTU_DISCOVER
        int df = IP_PMTUDISC_DO;
        if (setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &df, sizeof(df)))
            throw std::runtime_error(std::string("DF: ") + std::strerror(errno));
#else
        throw std::runtime_error("DF enforcement supported on Windows/Linux only");
#endif
#endif
        df_ = true;
        socket_.set_option(udp::socket::send_buffer_size(4 * 1024 * 1024), ec);
        socket_.set_option(udp::socket::receive_buffer_size(4 * 1024 * 1024), ec);
        socket_.bind(udp::endpoint(local, c.commandPort), ec);
        if (ec)
            throw std::runtime_error("Asio UDP bind: " + ec.message());
        socket_.set_option(udp::socket::send_buffer_size(4 * 1024 * 1024), ec);
        socket_.set_option(udp::socket::receive_buffer_size(4 * 1024 * 1024), ec);
        boost::asio::socket_base::send_buffer_size sendOption;
        socket_.get_option(sendOption, ec);
        sendBuffer_ = static_cast<int>(sendOption.value());
        boost::asio::socket_base::receive_buffer_size receiveOption;
        socket_.get_option(receiveOption, ec);
        receiveBuffer_ = static_cast<int>(receiveOption.value());
        peer_ = udp::endpoint(target_, c.targetPort);
        socket_.non_blocking(true, ec);
        if (ec)
            throw std::runtime_error("Asio non_blocking: " + ec.message());
        localPort_ = socket_.local_endpoint(ec).port();
    }

    bool send(const std::uint8_t *data, std::size_t size) override {
        return submitDatagram(data, size, 0) == SendDisposition::Complete;
    }

    SendDisposition submitDatagram(const std::uint8_t *data, std::size_t size,
                                   std::uint64_t token) override {
        if (size > MaxDatagram) {
            error_ = "datagram exceeds 1472";
            return SendDisposition::Failed;
        }
        boost::system::error_code ec;
        if (!async_) {
            const auto sent = socket_.send_to(boost::asio::buffer(data, size), peer_, 0, ec);
            if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again)
                return SendDisposition::Backpressure;
            if (ec) {
                error_ = "Asio send_to: " + ec.message();
                return SendDisposition::Failed;
            }
            if (sent != size) {
                error_ = "short Asio send_to";
                return SendDisposition::Failed;
            }
            return SendDisposition::Complete;
        }
        Slot *slot = acquire();
        if (!slot)
            return SendDisposition::Backpressure;
        std::memcpy(slot->bytes.data(), data, size);
        slot->size = size;
        slot->token = token;
        const auto index = static_cast<std::size_t>(slot - slots_.data());
        auto onComplete = [this, index](const boost::system::error_code &error, std::size_t sent) {
            auto &done = slots_[index];
            done.active = false;
            done.unobserved = true;
            if (error) {
                if (error == boost::asio::error::operation_aborted)
                    error_ = "Asio send cancelled";
                else
                    error_ = "Asio async_send_to: " + error.message();
                completions_[completionWrite_++ % completions_.size()] = {
                    {done.token,
                     error == boost::asio::error::operation_aborted ? SendDisposition::Cancelled
                                                                    : SendDisposition::Failed,
                     rt::monotonicUs()},
                    index};
            } else if (sent != done.size) {
                error_ = "short Asio async_send_to";
                completions_[completionWrite_++ % completions_.size()] = {
                    {done.token, SendDisposition::Failed, rt::monotonicUs()}, index};
            } else {
                completions_[completionWrite_++ % completions_.size()] = {
                    {done.token, SendDisposition::Complete, rt::monotonicUs()}, index};
            }
        };
        const auto buffer = boost::asio::buffer(slot->bytes.data(), size);
        try {
            if (operationStorage_) {
                std::pmr::polymorphic_allocator<std::byte> allocator(operationStorage_.get());
                socket_.async_send_to(buffer, peer_,
                                      boost::asio::bind_allocator(allocator, std::move(onComplete)));
            } else {
                socket_.async_send_to(buffer, peer_, std::move(onComplete));
            }
        } catch (...) {
            // Initiation failed before a handler took ownership of this slot.
            slot->active = false;
            --inFlight_;
            throw;
        }
        return SendDisposition::Pending;
    }

    bool pollCompletion(SendCompletion &out) override {
        if (!async_)
            return false;
        // Observe one real handler without an extra idle OS poll per packet.
        if (completionRead_ == completionWrite_)
            context_.poll_one();
        if (completionRead_ == completionWrite_)
            return false;
        const auto pending = completions_[completionRead_++ % completions_.size()];
        out = pending.value;
        auto &slot = slots_[pending.slot];
        if (slot.unobserved) {
            slot.unobserved = false;
            --inFlight_;
        }
        return true;
    }

    std::size_t inFlight() const noexcept override {
        return inFlight_;
    }

    void cancelOutstanding() noexcept override {
        boost::system::error_code ignored;
        socket_.cancel(ignored);
    }

    bool receive(IncomingDatagram &out) override {
        udp::endpoint source;
        boost::system::error_code ec;
        const auto size = socket_.receive_from(boost::asio::buffer(buffer_), source, 0, ec);
        if (ec == boost::asio::error::would_block || ec == boost::asio::error::try_again)
            return false;
        if (ec) {
            error_ = "Asio receive_from: " + ec.message();
            return false;
        }
        out.bytes.assign(buffer_.data(), buffer_.data() + size);
        out.sourceIp = source.address().to_v4().to_string();
        out.sourcePort = source.port();
        return true;
    }

    ReceiveOutcome receiveFeedback(IncomingDatagram &out) override {
        // The same bound socket carries feedback; read the OS result directly so
        // an error cannot masquerade as an empty queue.
        return readUdpFeedback(static_cast<FeedbackSocket>(socket_.native_handle()), out, error_);
    }

    std::uint16_t localPort() const override {
        return localPort_;
    }

    bool dfEnabled() const override {
        return df_;
    }

    std::string lastError() const override {
        return error_;
    }

    int sendBufferBytes() const override {
        return sendBuffer_;
    }

    int receiveBufferBytes() const override {
        return receiveBuffer_;
    }

    int connResetError() const override {
        return resetError_;
    }

    std::string name() const override {
        return async_ ? "Boost.Asio UDP (bounded async)" : "Boost.Asio UDP (sync)";
    }

    std::string sendApi() const override {
        return async_ ? "asio::ip::udp::socket::async_send_to" : "asio::ip::udp::socket::send_to";
    }

    std::string libraryVersion() const override {
        return "Boost " BOOST_LIB_VERSION " / Asio " + std::to_string(BOOST_ASIO_VERSION);
    }

  private:
    Slot *acquire() {
        if (inFlight_ >= SlotCount)
            return nullptr;
        for (auto &slot : slots_) {
            if (slot.active || slot.unobserved)
                continue;
            slot.active = true;
            ++inFlight_;
            return &slot;
        }
        return nullptr;
    }

    bool async_;
    udp::endpoint peer_;
    address_v4 target_;
    std::array<std::uint8_t, MaxDatagram + 1> buffer_{};
    std::array<Slot, SlotCount> slots_{};

    struct PendingCompletion {
        SendCompletion value;
        std::size_t slot;
    };

    std::array<PendingCompletion, SlotCount> completions_{};
    std::size_t completionRead_ = 0;
    std::size_t completionWrite_ = 0;
    std::size_t inFlight_ = 0;
    std::uint16_t localPort_ = 0;
    bool df_ = false;
    int sendBuffer_ = 0;
    int receiveBuffer_ = 0;
    int resetError_ = 0;
    std::string error_;
    // Both the data slots and the handler allocator must outlive io_context
    // teardown, including handlers destroyed without invocation after close.
    std::unique_ptr<AsioOperationStorage> operationStorage_;
    boost::asio::io_context context_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_{
        context_.get_executor()};
    udp::socket socket_{context_};
};
} // namespace

std::unique_ptr<IUdpTransport> makeAsioUdpTransport(bool async) {
    return std::make_unique<AsioUdpTransport>(async);
}
} // namespace rt
