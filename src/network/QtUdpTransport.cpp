#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <mstcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <cerrno>
#include <cstring>
#endif
#include "network/QtUdpTransport.h"
#include "network/FeedbackRead.h"
#include <QVariant>
#include <array>
namespace rt {
#ifdef _WIN32
    namespace {
        // QUdpSocket owns and binds this unconnected socket. The fixed peer
        // is prepared once; feedback still arrives on the same local port.
        class WindowsV2QtUdpTransport final:public QtUdpTransport {
        public:
            void open(const StreamConfig& config)override {
                QtUdpTransport::open(config);
                descriptor_=static_cast<SOCKET>(socket_.socketDescriptor());
                // QUdpSocket can leave the Winsock send buffer at its 64 KiB
                // default. Set the same 4 MiB budget on the native descriptor
                // so a fair comparison does not measure a socket-option gap.
                int buffer=4*1024*1024;
                setsockopt(descriptor_,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer));
                setsockopt(descriptor_,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&buffer),sizeof(buffer));
                int actual=0;int length=sizeof(actual);
                if(!getsockopt(descriptor_,SOL_SOCKET,SO_SNDBUF,reinterpret_cast<char*>(&actual),&length))sendBuffer_=actual;
                length=sizeof(actual);
                if(!getsockopt(descriptor_,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&actual),&length))receiveBuffer_=actual;
                peer_.sin_family=AF_INET;
                peer_.sin_port=htons(config.targetPort);
                peer_.sin_addr.s_addr=htonl(target_.toIPv4Address());
            }
            bool send(const std::uint8_t* data,std::size_t size)override {
                return submitDatagram(data,size,0)==SendDisposition::Complete;
            }
            SendDisposition submitDatagram(const std::uint8_t* data,std::size_t size,std::uint64_t)override {
                if(size>MaxDatagram){
                    error_="datagram exceeds 1472";
                    return SendDisposition::Failed;
                }
                const auto sent=::sendto(descriptor_,reinterpret_cast<const char*>(data),
                                         static_cast<int>(size),0,reinterpret_cast<const sockaddr*>(&peer_),sizeof(peer_));
                if(sent==SOCKET_ERROR){
                    const int code=WSAGetLastError();
                    if(code==WSAEWOULDBLOCK)return SendDisposition::Backpressure;
                    error_="Winsock sendto error "+std::to_string(code);
                    return SendDisposition::Failed;
                }
                if(sent!=static_cast<int>(size)){
                    error_="short UDP sendto: "+std::to_string(sent)+" of "+std::to_string(size)+" bytes";
                    return SendDisposition::Failed;
                }
                return SendDisposition::Complete;
            }
            std::string sendApi()const override {return "Winsock sendto";}
        private:
            SOCKET descriptor_=INVALID_SOCKET; // Borrowed for the lifetime of socket_.
            sockaddr_in peer_{};
        };
    }
#endif
    std::unique_ptr<IUdpTransport> makeV2QtUdpTransport() {
#ifdef _WIN32
        return std::make_unique<WindowsV2QtUdpTransport>();
#else
        return std::make_unique<QtUdpTransport>();
#endif
    }
    void QtUdpTransport::open(const StreamConfig& c) {
        target_=QHostAddress(QString::fromStdString(c.targetIp));
        QHostAddress local(QString::fromStdString(c.bindIp));
        port_=c.targetPort;
        if(target_.protocol()!=QAbstractSocket::IPv4Protocol||target_.isNull()||target_.isMulticast()||target_.toIPv4Address()==0||target_.toIPv4Address()==0xFFFFFFFFu||!port_)
        throw std::invalid_argument("target must be numeric IPv4 unicast and nonzero port");
        if(local.protocol()!=QAbstractSocket::IPv4Protocol)throw std::invalid_argument("bind must be numeric IPv4");
        if(!socket_.bind(local,c.commandPort,QAbstractSocket::DontShareAddress))throw std::runtime_error(socket_.errorString().toStdString());
        socket_.setSocketOption(QAbstractSocket::SendBufferSizeSocketOption,4*1024*1024);
        socket_.setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption,4*1024*1024);
        sendBuffer_=socket_.socketOption(QAbstractSocket::SendBufferSizeSocketOption).toInt();
        receiveBuffer_=socket_.socketOption(QAbstractSocket::ReceiveBufferSizeSocketOption).toInt();
#ifdef _WIN32
        const auto fd=static_cast<SOCKET>(socket_.socketDescriptor());
        DWORD df=1;
        if(setsockopt(fd,IPPROTO_IP,IP_DONTFRAGMENT,reinterpret_cast<const char*>(&df),sizeof(df)))throw std::runtime_error("IP_DONTFRAGMENT failed: "+std::to_string(WSAGetLastError()));
        BOOL reset=FALSE;
        DWORD ignored=0;
        if(WSAIoctl(fd,SIO_UDP_CONNRESET,&reset,sizeof(reset),nullptr,0,&ignored,nullptr,nullptr))resetError_=WSAGetLastError();
#elif defined(IP_MTU_DISCOVER)
        const auto fd=static_cast<int>(socket_.socketDescriptor());
        int df=IP_PMTUDISC_DO;
        if(setsockopt(fd,IPPROTO_IP,IP_MTU_DISCOVER,&df,sizeof(df)))throw std::runtime_error(std::string("DF: ")+std::strerror(errno));
#else
        throw std::runtime_error("DF enforcement supported on Windows/Linux only");
#endif
        df_=true;
    }
    bool QtUdpTransport::send(const std::uint8_t* p,std::size_t n) {
        if(n>MaxDatagram) {
            error_="datagram exceeds 1472";
            return false;
        }
        if(socket_.writeDatagram(reinterpret_cast<const char*>(p),static_cast<qint64>(n),target_,port_)!=static_cast<qint64>(n)) {
            error_=socket_.errorString().toStdString();
            return false;
        }
        return true;
    }
    bool QtUdpTransport::receive(IncomingDatagram& out) {
        if(!socket_.hasPendingDatagrams())return false;
        std::array<char,MaxDatagram+1> buffer {
        };
        QHostAddress from;
        quint16 port=0;
        auto n=socket_.readDatagram(buffer.data(),static_cast<qint64>(buffer.size()),&from,&port);
        if(n<0) {
            error_=socket_.errorString().toStdString();
            return false;
        }
        out.bytes.assign(reinterpret_cast<const std::uint8_t*>(buffer.data()),reinterpret_cast<const std::uint8_t*>(buffer.data())+n);
        out.sourceIp=from.toString().toStdString();
        out.sourcePort=port;
        return true;
    }
    SendDisposition QtUdpTransport::submitDatagram(const std::uint8_t* p,std::size_t n,std::uint64_t) {
        if(send(p,n))return SendDisposition::Complete;
        return n<=MaxDatagram && socket_.error()==QAbstractSocket::TemporaryError ? SendDisposition::Backpressure : SendDisposition::Failed;
    }
    ReceiveOutcome QtUdpTransport::receiveFeedback(IncomingDatagram& out) {
        return readUdpFeedback(static_cast<FeedbackSocket>(socket_.socketDescriptor()),out,error_);
    }
}
