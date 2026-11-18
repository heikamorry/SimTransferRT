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
    class IUdpTransport {
        public:
        virtual ~IUdpTransport()=default;
        virtual void open(const StreamConfig& config)=0;
        virtual bool send(const std::uint8_t* data,std::size_t size)=0;
        virtual bool receive(IncomingDatagram& datagram)=0;
        virtual std::uint16_t localPort()const=0;
        virtual bool dfEnabled()const=0;
        virtual std::string lastError()const=0;
        virtual int sendBufferBytes()const {return 0;}
        virtual int receiveBufferBytes()const {return 0;}
        virtual int connResetError()const {return 0;}
        virtual std::string name()const {return "test-adapter";}
        virtual ReceiveOutcome receiveFeedback(IncomingDatagram& datagram) {
            return receive(datagram) ? ReceiveOutcome::Received : ReceiveOutcome::Empty;
        }
    };
}
