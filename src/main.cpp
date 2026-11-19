#include "ui/MainWindow.h"
#include "ui/V2Window.h"
#include <QApplication>
#include <QFile>
#include <QTimer>
#include <QPixmap>
int main(int argc,char**argv) {
    QApplication app(argc,argv);
    app.setOrganizationName("SimTransferLab");
    app.setApplicationName("SimTransferRT");
    qRegisterMetaType<rt::Snapshot>();
    QFile style(":/styles/light.qss");
    if(style.open(QIODevice::ReadOnly))app.setStyleSheet(QString::fromUtf8(style.readAll()));
    const auto args=app.arguments();
    std::unique_ptr<QMainWindow> selected;
    if(args.contains("--v1"))selected=std::make_unique<rt::MainWindow>();
    else selected=std::make_unique<rt::V2Window>();
    auto& window=*selected;
    window.show();
    if(args.contains("--smoke-test"))QTimer::singleShot(400,&app,&QApplication::quit);
    const int i=args.indexOf("--screenshot");
    if(i>=0&&i+1<args.size())QTimer::singleShot(500,&window,[&window,&app,args,i] {
        window.grab().save(args[i+1]);app.quit();
    });
    return app.exec();
}
