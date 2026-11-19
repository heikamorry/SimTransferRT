#include "application/TransferController.h"
#include "protocol/PacketCodec.h"
#include "network/QtUdpTransport.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QTimer>
#include <iostream>
static bool testBoundDatagrams(rt::IUdpTransport& transport) {
    rt::IncomingDatagram packet;
    if (transport.receiveFeedback(packet) != rt::ReceiveOutcome::Error || transport.lastError().empty())
        return false;
    const QHostAddress loopback(QHostAddress::LocalHost);
    QUdpSocket receiver;
    if (!receiver.bind(loopback, 0)) return false;
    rt::StreamConfig config;
    config.commandPort = 0;
    config.targetIp = "127.0.0.1";
    config.targetPort = receiver.localPort();
    transport.open(config);
    if (!transport.dfEnabled() || !transport.localPort() || transport.receiveFeedback(packet) != rt::ReceiveOutcome::Empty)
        return false;
    QByteArray body(rt::MaxDatagram + 1, '\0');
    body[0] = 'a';
    body[rt::MaxDatagram - 1] = 'z';
    const auto* data = reinterpret_cast<const std::uint8_t*>(body.constData());
    if (transport.send(data, rt::MaxDatagram + 1) || transport.lastError().empty()) return false;
    if (!transport.send(data, rt::MaxDatagram)) return false;
    if (!receiver.hasPendingDatagrams() && !receiver.waitForReadyRead(1000)) return false;
    const auto received = receiver.receiveDatagram();
    if (received.senderPort() != transport.localPort() || received.senderAddress().toIPv4Address() != loopback.toIPv4Address()
            || received.data() != body.left(rt::MaxDatagram)) return false;
    // A different feedback source port must reach the bound, unconnected socket;
    // the runner applies the configured endpoint policy after receiving.
    QUdpSocket peer;
    if (!peer.bind(QHostAddress::LocalHost, 0)) return false;
    const QByteArray payload("feedback");
    if (peer.writeDatagram(payload, QHostAddress::LocalHost, transport.localPort()) != payload.size())
        return false;
    QElapsedTimer timer;
    timer.start();
    rt::ReceiveOutcome outcome;
    do {
        outcome = transport.receiveFeedback(packet);
    } while (outcome == rt::ReceiveOutcome::Empty && timer.elapsed() < 1000);
    return outcome == rt::ReceiveOutcome::Received && packet.sourcePort == peer.localPort()
        && packet.sourceIp == "127.0.0.1" && std::string(packet.bytes.begin(), packet.bytes.end()) == "feedback";
}
int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);
    rt::QtUdpTransport legacy;
    auto v2 = rt::makeV2QtUdpTransport();
#ifdef _WIN32
    const auto expectedV2Api = "Winsock sendto";
#else
    const auto expectedV2Api = "QUdpSocket::writeDatagram";
#endif
    if (legacy.sendApi() != "QUdpSocket::writeDatagram" || v2->name() != legacy.name() ||
            v2->sendApi() != expectedV2Api || !testBoundDatagrams(legacy) || !testBoundDatagrams(*v2)) {
        std::cerr << "Bound Qt UDP transport selection, datagram delivery or feedback failed\n";
        return 1;
    }
    rt::TransferController controller;
    QUdpSocket receiver;
    if(!receiver.bind(QHostAddress(QHostAddress::LocalHost),quint16(0))) {
        std::cerr<<"bind failed\n";
        return 1;
    }
    rt::StreamConfig c;
    c.targetPort=receiver.localPort();
    QUdpSocket probe;
    if(!probe.bind(QHostAddress(QHostAddress::LocalHost),quint16(0)))return 1;
    c.commandPort=probe.localPort();
    probe.close();
    c.frameLimit=3;
    c.modules[0].dataBytes=5000;
    std::uint64_t received=0,retries=0;
    bool error=false,nackSent=false;
    rt::Snapshot last;
    QObject::connect(&receiver,&QUdpSocket::readyRead,&app,[&] {
        while(receiver.hasPendingDatagrams()) {
            auto d=receiver.receiveDatagram();try {
                auto p=rt::decodeData(reinterpret_cast<const std::uint8_t*>(d.data().constData()),static_cast<std::size_t>(d.data().size()));++received;if(p.retransmission)++retries;
                if(!nackSent) {
                    auto n=rt::encodeNack( {
                        p.logicId, {
                            {
                                0,1
                            }
                        }
                    });receiver.writeDatagram(reinterpret_cast<const char*>(n.bytes.data()),static_cast<qint64>(n.size),QHostAddress::LocalHost,c.commandPort);nackSent=true;
                }
            }
            catch(...) {
                error=true;
            }
        }
    });
    QObject::connect(&controller,&rt::TransferController::snapshotChanged,&app,[&](const rt::Snapshot&s) {
        last=s;
    });
    QObject::connect(&controller,&rt::TransferController::rejected,&app,[&](const QString&s) {
        std::cerr<<s.toStdString()<<'\n';error=true;app.quit();
    });
    QObject::connect(&controller,&rt::TransferController::runEnded,&app,&QCoreApplication::quit);
    QTimer::singleShot(4000,&app,[&] {
        error=true;controller.stop();app.quit();
    });
    QTimer::singleShot(0,&app,[&] {
        controller.start(c);
    });
    app.exec();
    controller.shutdown();
    if(error||received<12||!retries||last.stats.built!=3||!last.dfEnabled) {
        std::cerr<<"Qt app test failed packets="<<received<<" retries="<<retries<<'\n';
        return 1;
    }
    std::cout<<"PASS Qt three managers + worker QUdpSocket DATA/NACK + DF + local lifecycle\n";
    return 0;
}
