#pragma once
#include "core/Types.h"
#include <QAbstractTableModel>
namespace rt {
    class FrameTableModel final:public QAbstractTableModel {
        Q_OBJECT
        public:
        explicit FrameTableModel(QObject*p=nullptr):QAbstractTableModel(p) {
        }
        int rowCount(const QModelIndex& parent= {
        })const override {
            return parent.isValid()?0:static_cast<int>(rows_.size());
        }
        int columnCount(const QModelIndex&parent= {
        })const override {
            return parent.isValid()?0:7;
        }
        QVariant data(const QModelIndex&,int role=Qt::DisplayRole)const override;
        QVariant headerData(int,Qt::Orientation,int role)const override;
        void apply(const Snapshot&);
        private:struct Row {
            std::uint32_t frame;
            std::uint64_t ts;
            FragmentStatus f;
        };
        std::vector<Row>rows_;
    };
}
