#pragma once
#include "core/Types.h"
#include <QWidget>
class QLineEdit;
class QSpinBox;
class QTableWidget;
class QCheckBox;
class QComboBox;
class QTabWidget;
namespace rt {
    class ConfigPanel final:public QWidget {
        Q_OBJECT
        public:
        explicit ConfigPanel(QWidget*parent=nullptr);
        StreamConfig config()const;
        void setRunning(bool busy);
        void setNextId(std::uint32_t id);
        std::uint32_t selectedMask()const;
        signals:void maskRequested(std::uint32_t mask);
        private:
        QTabWidget* tabs_;
        QWidget* net_;
        QWidget*fault_;
        QLineEdit *target_,*bind_,*source_,*logic_,*drop_,*always_,*corrupt_,*duplicate_;
        QSpinBox *port_,*cmdPort_,*sourcePort_,*limit_;
        QComboBox *link_,*cache_;
        QCheckBox *overload_,*reverse_;
        QTableWidget*modules_;
        bool running_=false;
        void preset(std::uint32_t bytes);
        void chooseModuleFile();
    };
}
