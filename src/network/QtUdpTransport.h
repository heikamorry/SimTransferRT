#pragma once
#include "network/IUdpTransport.h"
#include <QUdpSocket>
#include <QHostAddress>
namespace rt {
    // Construct, bind, read, write and destroy on ONE dedicated worker thread.
    // Polling synchronous APIs; no readyRead or QTimer is expected on this worker.
    class QtUdpTransport:public IUdpTransport {
        public:
        void open(const StreamConfig&)override;
        bool send(const std::uint8_t*,std::size_t)override;
        SendDisposition submitDatagram(const std::uint8_t*,std::size_t,std::uint64_t)override;
        bool receive(IncomingDatagram&)override;
        ReceiveOutcome receiveFeedback(IncomingDatagram&)override;
        std::uint16_t localPort()const override {
            return socket_.localPort();
        }
        bool dfEnabled()const override {
            return df_;
        }
        std::string lastError()const override {
            return error_;
        }
        int sendBufferBytes()const override {return sendBuffer_;}
        int receiveBufferBytes()const override {return receiveBuffer_;}
        int connResetError()const override {return resetError_;}
        std::string name()const override {return "Qt QUdpSocket";}
        std::string sendApi()const override {return "QUdpSocket::writeDatagram";}
        protected:
        QUdpSocket socket_;
        QHostAddress target_;
        std::string error_;
        int sendBuffer_=0,receiveBuffer_=0,resetError_=0;
        private:
        quint16 port_=0;
        bool df_=false;
    };
    std::unique_ptr<IUdpTransport> makeV2QtUdpTransport();
}
