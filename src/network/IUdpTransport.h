#pragma once
#include "core/Types.h"
#include <memory>
namespace rt {
    struct IncomingDatagram {
        Bytes bytes;
        std::string sourceIp;
        std::uint16_t sourcePort=0;
    };
    enum class ReceiveOutcome { Received, Empty, Error };
    enum class SendDisposition { Complete, Failed, Backpressure, Pending, Cancelled };
    struct SendCompletion { std::uint64_t token=0; SendDisposition disposition=SendDisposition::Failed; std::uint64_t observedUs=0; };
    class IUdpTransport {
        public:
        virtual ~IUdpTransport()=default;
        virtual void open(const StreamConfig& config)=0;
        virtual bool send(const std::uint8_t* data,std::size_t size)=0;
        // Synchronous V1 callers retain bool send(). V2 owns this socket on
        // one thread, including initiation, completion observation and shutdown.
        virtual SendDisposition submitDatagram(const std::uint8_t* p,std::size_t n,std::uint64_t) {
            return send(p,n) ? SendDisposition::Complete : SendDisposition::Failed;
        }
        virtual bool pollCompletion(SendCompletion&) {return false;}
        virtual std::size_t inFlight() const noexcept {return 0;}
        virtual void cancelOutstanding() noexcept {}
        virtual std::string libraryVersion() const {return {};}
        virtual bool receive(IncomingDatagram& datagram)=0;
        virtual std::uint16_t localPort()const=0;
        virtual bool dfEnabled()const=0;
        virtual std::string lastError()const=0;
        virtual int sendBufferBytes()const {return 0;}
        virtual int receiveBufferBytes()const {return 0;}
        virtual int connResetError()const {return 0;}
        virtual std::string name()const {return "test-adapter";}
        virtual std::string sendApi()const {return {};}
        virtual ReceiveOutcome receiveFeedback(IncomingDatagram& datagram) {
            return receive(datagram) ? ReceiveOutcome::Received : ReceiveOutcome::Empty;
        }
    };
}
