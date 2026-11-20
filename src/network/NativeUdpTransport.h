#pragma once
#include "network/IUdpTransport.h"
namespace rt {
    // Portable CLI/test adapter. Production Qt GUI uses QtUdpTransport with the SAME stream engine.
    class NativeUdpTransport final:public IUdpTransport {
        public:
        NativeUdpTransport();
        ~NativeUdpTransport()override;
        void open(const StreamConfig&)override;
        bool send(const std::uint8_t*,std::size_t)override;
        SendDisposition submitDatagram(const std::uint8_t*,std::size_t,std::uint64_t)override;
        int sendBufferBytes()const override;
        int receiveBufferBytes()const override;
        int connResetError()const override;
        bool receive(IncomingDatagram&)override;
        ReceiveOutcome receiveFeedback(IncomingDatagram&)override;
        std::uint16_t localPort()const override;
        bool dfEnabled()const override;
        std::string lastError()const override;
        std::string name()const override {return "Native IPv4 UDP";}
        std::string sendApi()const override {
#ifdef _WIN32
            return "Winsock sendto";
#else
            return "sendto";
#endif
        }
        private:struct Impl;
        std::unique_ptr<Impl> p_;
    };
}
