#include "ui/FrameTableModel.h"
#include <QColor>
namespace rt {
    QVariant FrameTableModel::headerData(int i,Qt::Orientation o,int role)const {
        if(role!=Qt::DisplayRole||o!=Qt::Horizontal)return {
        };
        static const QStringList names= {
            "logic_id",QStringLiteral("片号"),QStringLiteral("载荷 B"),QStringLiteral("本地状态（非送达确认）"),QStringLiteral("重传尝试"),QStringLiteral("采样 TS / us"),QStringLiteral("末次发出 / us")
        };
        return names.value(i);
    }
    QVariant FrameTableModel::data(const QModelIndex&i,int role)const {
        if(!i.isValid()||i.row()<0||std::size_t(i.row())>=rows_.size())return {
        };
        const auto&r=rows_[static_cast<std::size_t>(i.row())];
        if(role==Qt::ForegroundRole) {
            if(r.f.state.find("drop")!=std::string::npos||r.f.state.find("error")!=std::string::npos)return QColor("#B45309");
            if(r.f.retries)return QColor("#2563EB");
            return {
            };
        }
        if(role!=Qt::DisplayRole)return {
        };
        switch(i.column()) {
            case 0:return r.frame;
            case 1:return r.f.id;
            case 2:return r.f.bytes;
            case 3: {
                const auto&s=r.f.state;
                if(s=="Queued")return QStringLiteral("待发");
                if(s=="Injected drop")return QStringLiteral("注入丢片");
                if(s=="Local send error")return QStringLiteral("本地发送错误");
                if(r.f.retries)return QStringLiteral("已重传 / 对端未知");
                return QStringLiteral("本地已发 / 对端未知");
            }
            case 4:return r.f.retries;
            case 5:return QString::number(r.ts);
            case 6:return r.f.lastSendUs?QString::number(r.f.lastSendUs):QStringLiteral("—");
            default:return {
            };
        }
    }
    void FrameTableModel::apply(const Snapshot&s) {
        beginResetModel();
        rows_.clear();
        for(const auto&frame:s.frames)for(const auto&f:frame.fragments)rows_.push_back( {
            frame.id,frame.timestampUs,f
        });
        endResetModel();
    }
}
