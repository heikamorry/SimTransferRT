#pragma once
#include "Model.h"
#include <QWidget>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

namespace rt::model {
struct Display {
    std::vector<vtkSmartPointer<vtkPolyData>> pieces;
    vtkSmartPointer<vtkPolyData> axes;
    qint64 hiddenPoints = 0;
    qint64 invalidPoses = 0;
};

Display makeDisplay(const Dataset &, const Task &);

class Scene final : public QWidget {
  public:
    explicit Scene(QWidget *parent = nullptr);
    ~Scene() override;
    void showData(const Display &, Snapshot);
    void select(int field, int representation);
    void setPointSize(int);
    void reset();
    double cameraDistance() const;
    void render();

  private:
    struct Impl;
    std::unique_ptr<Impl> p;
};
} // namespace rt::model
