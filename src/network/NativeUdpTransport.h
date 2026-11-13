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
        bool receive(IncomingDatagram&)override;
        std::uint16_t localPort()const override;
        bool dfEnabled()const override;
        std::string lastError()const override;
        private:struct Impl;
        std::unique_ptr<Impl> p_;
    };
}
