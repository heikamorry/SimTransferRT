#pragma once
#include <QWidget>
#include <memory>

namespace rt::model {
class PreviewPanel final : public QWidget {
    Q_OBJECT
  public:
    explicit PreviewPanel(QWidget *parent = nullptr);
    ~PreviewPanel() override;
    void openFile(const QString &, const QString &expected = {});
    // Decodes a complete BX object received over UDP through the same path as a
    // local file. The bytes are copied, so the transport buffer may be reused.
    void showBxObject(const QByteArray &, const QString &sourceLabel);
    void saveFile(const QString &, const QString &archiveDirectory = {});
    void cancel();
    void unload();
    void stop();
    bool busy() const;
    double cameraDistance() const;
  signals:
    void fileTaskFinished(bool success);
    void bxObjectFinished(bool success, const QString &detail);

  private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
} // namespace rt::model
