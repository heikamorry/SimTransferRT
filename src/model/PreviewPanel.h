#pragma once
#include <QJsonObject>
#include <QWidget>
#include <memory>

namespace rt::model {
class PreviewPanel final : public QWidget {
    Q_OBJECT
  public:
    // Install the Qt/VTK surface request before constructing QApplication.
    static void setDefaultSurfaceFormat();
    explicit PreviewPanel(QWidget *parent = nullptr);
    ~PreviewPanel() override;
    void openFile(const QString &, const QString &expected = {});
    // Decodes a complete BX object received over UDP through the same path as a
    // local file. The bytes are copied, so the transport buffer may be reused.
    void showBxObject(const QByteArray &, const QString &sourceLabel);
    void beginReceiveSession();
    bool fileTaskActive() const;
    void saveFile(const QString &, const QString &archiveDirectory = {});
    void cancel();
    void unload();
    void stop();
    bool busy() const;
    double cameraDistance() const;
    // GUI-thread wall times since the last receiver session began.
    QJsonObject timingReport() const;
  signals:
    void localActionStarted();
    void fileTaskFinished(bool success);
    void bxObjectFinished(bool success, const QString &detail);

  private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
} // namespace rt::model
