#include "application/TransferController.h"
#include <QHostAddress>
namespace rt {
    TransferController::TransferController(QObject* p):QObject(p),manager_(this) {
        connect(&manager_,&SessionManager::snapshotChanged,this,&TransferController::snapshotChanged);
        connect(&manager_,&SessionManager::logMessage,this,&TransferController::logMessage);
        connect(&manager_,&SessionManager::runEnded,this,&TransferController::runEnded);
    }
    void TransferController::start(const StreamConfig& c) {
        try {
            QHostAddress target(QString::fromStdString(c.targetIp)),local(QString::fromStdString(c.bindIp));
            if(target.protocol()!=QAbstractSocket::IPv4Protocol||target.toIPv4Address()==0||target.toIPv4Address()>=0xE0000000u||!c.targetPort)
            throw std::invalid_argument("target must be numeric IPv4 unicast / nonzero port");
            if(local.protocol()!=QAbstractSocket::IPv4Protocol||!c.commandPort)throw std::invalid_argument("NACK listen address must be IPv4 and port must be nonzero");
            if(!c.allowedNackIp.empty()&&c.allowedNackIp!="*"&&QHostAddress(QString::fromStdString(c.allowedNackIp)).protocol()!=QAbstractSocket::IPv4Protocol)throw std::invalid_argument("NACK source filter must be IPv4 or *");
            manager_.begin(c);
        }
        catch(const std::exception&e) {
            emit rejected(QString::fromUtf8(e.what()));
        }
    }
    void TransferController::stop() {
        manager_.stop();
    }
    void TransferController::applyMask(std::uint32_t m) {
        manager_.applyMask(m);
    }
    void TransferController::shutdown() {
        manager_.shutdown();
    }
}
