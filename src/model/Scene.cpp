#include "Scene.h"
#include "core/Diagnostics.h"
#include "core/SamplingClock.h"
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QOpenGLContext>
#ifdef SIM_VTK_GLES
#include <QOpenGLExtraFunctions>
#endif
#include <QPalette>
#include <QShowEvent>
#include <QScopeGuard>
#include <QSurfaceFormat>
#include <QVTKOpenGLNativeWidget.h>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vtkActor.h>
#include <vtkAxesActor.h>
#include <vtkCamera.h>
#include <vtkCellArray.h>
#include <vtkCellData.h>
#include <vtkDataArray.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkInteractorStyleTrackballCamera.h>
#include <vtkLookupTable.h>
#include <vtkObjectFactory.h>
#include <vtkOrientationMarkerWidget.h>
#include <vtkPointData.h>
#include <vtkPoints.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkUnsignedCharArray.h>

namespace rt::model {
namespace {
// VTK 9.4 checks the OpenGL error state before it asks Qt to make the context
// current. Both UI commands and the camera interactor can render outside paintGL.
class QtRenderWindow final : public vtkGenericOpenGLRenderWindow {
  public:
    static QtRenderWindow *New();
    vtkTypeMacro(QtRenderWindow, vtkGenericOpenGLRenderWindow);

    void Render() override {
        if (this->GetReadyForRendering()) {
            this->MakeCurrent();
            if (this->IsCurrent())
                this->Superclass::Render();
        }
    }

#ifdef SIM_VTK_GLES
    int GetColorBufferSizes(int *rgba) override {
        if (!rgba || !this->GetInitialized())
            return this->Superclass::GetColorBufferSizes(rgba);

        this->MakeCurrent();
        auto *context = QOpenGLContext::currentContext();
        std::fill_n(rgba, 4, 0);
        if (!context || !this->IsCurrent())
            return 0;
        auto *gl = context->extraFunctions();
        if (gl->glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            return 0;

        // ES uses GL_BACK for the default framebuffer. Qt's widget framebuffer
        // has a color attachment instead. VTK 9.4.2 asks for GL_BACK_LEFT.
        GLint framebuffer = 0, attachment = GL_BACK;
        gl->glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        if (framebuffer) {
            gl->glGetIntegerv(GL_DRAW_BUFFER0, &attachment);
            if (attachment == GL_NONE)
                attachment = GL_COLOR_ATTACHMENT0;
        }
        GLint type = GL_NONE;
        gl->glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
                                                 GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        if (type == GL_NONE)
            return 0;
        constexpr GLenum sizeQueries[]{GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE,
                                       GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE,
                                       GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE,
                                       GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE};
        for (int i = 0; i < 4; ++i)
            gl->glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment,
                                                     sizeQueries[i], &rgba[i]);
        return rgba[0] + rgba[1] + rgba[2] + rgba[3];
    }
#endif
};

vtkStandardNewMacro(QtRenderWindow);

int vtkType(Type t) {
    static const int types[]{
        VTK_SIGNED_CHAR, VTK_UNSIGNED_CHAR, VTK_SHORT,     VTK_UNSIGNED_SHORT,
        VTK_INT,         VTK_UNSIGNED_INT,  VTK_LONG_LONG, VTK_UNSIGNED_LONG_LONG,
        VTK_FLOAT,       VTK_DOUBLE};
    return types[int(t)];
}

QByteArray id(size_t n) {
    return "field_" + QByteArray::number(qulonglong(n));
}

vtkSmartPointer<vtkDataArray> copyArray(const Field &f, const std::vector<vtkIdType> &selected,
                                        const Task &t) {
    auto a = vtkSmartPointer<vtkDataArray>::Take(vtkDataArray::CreateDataArray(vtkType(f.type)));
    a->SetNumberOfComponents(f.components);
    a->SetNumberOfTuples(selected.size());
    auto dst = static_cast<char *>(a->GetVoidPointer(0));
    int stride = width(f.type) * f.components;
    for (size_t i = 0; i < selected.size(); ++i) {
        if (!(i & 65535))
            t.check();
        std::memcpy(dst + i * stride, f.data.constData() + selected[i] * stride, stride);
    }
    return a;
}

void makeTracking(const Dataset &d, Display &display) {
    vtkNew<vtkPoints> points;
    vtkNew<vtkPoints> ends;
    points->SetDataTypeToDouble();
    ends->SetDataTypeToDouble();
    vtkNew<vtkCellArray> verts;
    vtkNew<vtkCellArray> lines;
    vtkNew<vtkUnsignedCharArray> colors;
    colors->SetNumberOfComponents(3);
    const unsigned char rgb[3][3]{{216, 52, 51}, {29, 147, 80}, {48, 94, 204}};
    for (const auto &tool : d.tracking->tools) {
        if (!tool.transformUsable())
            continue;
        const auto &pos = tool.positionMm;
        vtkIdType point = points->InsertNextPoint(pos.data());
        verts->InsertNextCell(1, &point);
        auto q = tool.quaternion;
        double norm = 0;
        for (float f : q)
            norm += double(f) * f;
        if (!(norm > 0) || !std::isfinite(norm)) {
            ++display.invalidPoses;
            continue;
        }
        norm = std::sqrt(norm);
        double w = q[0] / norm;
        double x = q[1] / norm;
        double y = q[2] / norm;
        double z = q[3] / norm;
        double basis[3][3]{{1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)},
                           {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)},
                           {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)}};
        auto first = ends->InsertNextPoint(pos.data());
        for (int k = 0; k < 3; ++k) {
            vtkIdType pair[]{first, ends->InsertNextPoint(pos[0] + 5 * basis[k][0],
                                                          pos[1] + 5 * basis[k][1],
                                                          pos[2] + 5 * basis[k][2])};
            lines->InsertNextCell(2, pair);
            colors->InsertNextTypedTuple(rgb[k]);
        }
    }
    auto data = vtkSmartPointer<vtkPolyData>::New();
    data->SetPoints(points);
    data->SetVerts(verts);
    display.pieces.push_back(data);
    display.axes = vtkSmartPointer<vtkPolyData>::New();
    display.axes->SetPoints(ends);
    display.axes->SetLines(lines);
    display.axes->GetCellData()->SetScalars(colors);
}
} // namespace

Display makeDisplay(const Dataset &d, const Task &t) {
    Display result;
    if (d.tracking) {
        makeTracking(d, result);
        return result;
    }
    for (int p = 0; p < d.pieces(); ++p) {
        auto coords = d.find("Points", p);
        auto data = vtkSmartPointer<vtkPolyData>::New();
        result.pieces.push_back(data);
        Field displayCoordinates;
        if (!coords && p == 0 && d.descriptor.contains("coordinateFields")) {
            auto ids = d.descriptor["coordinateFields"].toArray();
            displayCoordinates.type = Type::F64;
            displayCoordinates.components = 3;
            displayCoordinates.data.resize(checked(d.points(p), 24));
            for (int k = 0; k < 3; ++k) {
                const auto &axis = d.fields.at(ids[k].toInt());
                for (qint64 i = 0; i < axis.size(); ++i) {
                    if (!(i & 65535))
                        t.check();
                    double value =
                        displayNumber(axis.type, axis.data.constData() + i * width(axis.type));
                    std::memcpy(displayCoordinates.data.data() + (3 * i + k) * 8, &value, 8);
                }
            }
            coords = &displayCoordinates;
        }
        if (!coords)
            continue;
        std::vector<vtkIdType> keep;
        std::vector<vtkIdType> cellOrder;
        std::vector<vtkIdType> remap(coords->size(), -1);
        keep.reserve(coords->size());
        for (qint64 i = 0; i < coords->size(); ++i) {
            if (!(i & 65535))
                t.check();
            auto ptr = coords->data.constData() + i * 3 * width(coords->type);
            bool good = true;
            for (int k = 0; k < 3; ++k)
                good = good &&
                       std::isfinite(displayNumber(coords->type, ptr + k * width(coords->type)));
            if (!good) {
                ++result.hiddenPoints;
                continue;
            }
            remap[i] = vtkIdType(keep.size());
            keep.push_back(i);
        }
        vtkNew<vtkPoints> pts;
        if (coords->type == Type::F32 || coords->type == Type::F64)
            pts->SetData(copyArray(*coords, keep, t));
        else {
            pts->SetDataTypeToDouble();
            for (auto i : keep) {
                double xyz[3];
                for (int k = 0; k < 3; ++k)
                    xyz[k] = displayNumber(coords->type, coords->data.constData() +
                                                             (3 * i + k) * width(coords->type));
                pts->InsertNextPoint(xyz);
            }
        }
        data->SetPoints(pts);
        qint64 base = 0;
        for (const auto &kind : cellKinds()) {
            auto f = d.find(kind, p);
            if (!f)
                continue;
            vtkNew<vtkCellArray> cells;
            cells->AllocateExact(f->tuples(), f->size());
            qint64 begin = 0;
            std::vector<vtkIdType> ids;
            for (qint64 i = 0; i < f->tuples(); ++i) {
                if (!(i & 32767))
                    t.check();
                auto end = f->ends[i];
                ids.resize(size_t(end - begin));
                bool good = true;
                for (auto j = begin; j < end; ++j) {
                    qint64 n;
                    std::memcpy(&n, f->data.constData() + 8 * j, 8);
                    ids[j - begin] = remap.at(n);
                    good = good && ids[j - begin] >= 0;
                }
                begin = end;
                if (good) {
                    cells->InsertNextCell(vtkIdType(ids.size()), ids.data());
                    cellOrder.push_back(base + i);
                }
            }
            if (kind == "Verts")
                data->SetVerts(cells);
            else if (kind == "Lines")
                data->SetLines(cells);
            else if (kind == "Strips")
                data->SetStrips(cells);
            else
                data->SetPolys(cells);
            base += f->tuples();
        }
        if (d.cells(p) == 0) {
            vtkNew<vtkCellArray> verts;
            verts->AllocateExact(keep.size(), keep.size());
            for (vtkIdType i = 0; i < vtkIdType(keep.size()); ++i) {
                if (!(i & 65535))
                    t.check();
                verts->InsertNextCell(1, &i);
            }
            data->SetVerts(verts);
        }
        for (size_t i = 0; i < d.fields.size(); ++i) {
            auto &f = d.fields[i];
            if (f.piece != p || f.list || (f.section != "PointData" && f.section != "CellData"))
                continue;
            auto a = copyArray(f, f.section == "PointData" ? keep : cellOrder, t);
            a->SetName(id(i).constData());
            vtkDataSetAttributes *assoc =
                f.section == "PointData" ? static_cast<vtkDataSetAttributes *>(data->GetPointData())
                                         : data->GetCellData();
            assoc->AddArray(a);
        }
    }
    return result;
}

struct Scene::Impl {
    QVTKOpenGLNativeWidget *widget = nullptr;
    vtkNew<QtRenderWindow> window;
    vtkNew<vtkRenderer> renderer;
    vtkNew<vtkAxesActor> direction;
    vtkNew<vtkOrientationMarkerWidget> marker;
    std::vector<vtkSmartPointer<vtkActor>> actors;
    vtkSmartPointer<vtkActor> axesActor;
    Snapshot source;
    bool live = false;
    int pointSize = 8;
    StageProfile updateTiming;
    StageProfile renderTiming;
    StageProfile composeTiming;
    std::uint64_t composeStartedUs = 0;
};

Scene::Scene(QWidget *parent) : QWidget(parent), p(std::make_unique<Impl>()) {
    // The OpenGL surface is translucent on some drivers. Paint the same colour
    // underneath so the window watermark cannot bleed into the 3D view.
    setAutoFillBackground(true);
    QPalette opaque = palette();
    opaque.setColor(QPalette::Window, QColor(183, 213, 234));
    setPalette(opaque);
    auto layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    if (QGuiApplication::platformName() == "offscreen" ||
        QGuiApplication::platformName() == "minimal") {
        layout->addWidget(new QLabel("当前 Qt 平台没有交互式 OpenGL 视口", this));
        return;
    }
    p->widget = new QVTKOpenGLNativeWidget(this);
    p->widget->setObjectName("modelViewport");
    connect(p->widget, &QOpenGLWidget::aboutToCompose, this, [this] {
        p->composeStartedUs = monotonicUs();
    });
    connect(p->widget, &QOpenGLWidget::frameSwapped, this, [this] {
        if (p->composeStartedUs) {
            p->composeTiming.add(monotonicUs() - p->composeStartedUs);
            p->composeStartedUs = 0;
        }
    });
    // The viewport is the flexible part of the panel: it must yield space to the
    // BX table instead of forcing the whole panel past the splitter height.
    p->widget->setMinimumSize(0, 0);
    p->widget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    layout->addWidget(p->widget);
    p->window->SetMultiSamples(0);
    p->widget->setRenderWindow(p->window);
    p->window->AddRenderer(p->renderer);
    p->renderer->SetBackground(183. / 255, 213. / 255, 234. / 255);
    vtkNew<vtkInteractorStyleTrackballCamera> style;
    p->widget->interactor()->SetInteractorStyle(style);
    p->marker->SetOrientationMarker(p->direction);
    p->marker->SetInteractor(p->widget->interactor());
    p->marker->SetViewport(0, 0, 0.18, 0.22);
    p->marker->SetEnabled(true);
    p->marker->InteractiveOff();
}

Scene::~Scene() {
    if (!p->widget)
        return;
    disconnect(p->widget, nullptr, this, nullptr);
    p->marker->SetEnabled(false);
    p->marker->SetInteractor(nullptr);
    p->widget->makeCurrent();
    p->renderer->ReleaseGraphicsResources(p->window);
    p->renderer->RemoveAllViewProps();
    p->widget->setRenderWindow(static_cast<vtkGenericOpenGLRenderWindow *>(nullptr));
    p->widget->doneCurrent();
}

void Scene::render() {
    if (p->widget && p->widget->isValid() && isVisible() && !window()->isMinimized()) {
        const auto started = monotonicUs();
        p->window->Render();
        p->renderTiming.add(monotonicUs() - started);
    }
}

void Scene::resetTiming() {
    p->updateTiming = {};
    p->renderTiming = {};
    p->composeTiming = {};
    p->composeStartedUs = 0;
}

QJsonObject Scene::timingReport() const {
    auto row = [](const StageProfile &stage) {
        return QJsonObject{{"calls", static_cast<qint64>(stage.calls)},
                           {"total_us", static_cast<qint64>(stage.totalUs)},
                           {"max_us", static_cast<qint64>(stage.maxUs)}};
    };
    QJsonObject report{{"scene_update", row(p->updateTiming)},
                       {"scene_render", row(p->renderTiming)},
                       {"qt_compose_to_swap", row(p->composeTiming)},
                       {"requested_swap_interval", QSurfaceFormat::defaultFormat().swapInterval()},
                       {"top_level_window_swap_interval", QJsonValue::Null},
                       {"widget_context_swap_interval", QJsonValue::Null}};
    if (const auto *native = window()->windowHandle(); native && native->handle())
        report["top_level_window_swap_interval"] = native->format().swapInterval();
    if (p->widget && p->widget->isValid()) {
        const auto *context = p->widget->context();
        const auto format = context->format();
        report["widget_context_swap_interval"] = format.swapInterval();
        report["widget_context_api"] = context->isOpenGLES() ? "OpenGL ES" : "OpenGL";
        report["widget_context_major"] = format.majorVersion();
        report["widget_context_minor"] = format.minorVersion();
    }
    return report;
}

void Scene::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    // Refresh data that arrived while the viewport was hidden or minimized.
    // Rendering stays on the GUI thread after Qt has exposed its OpenGL widget.
    QMetaObject::invokeMethod(
        this,
        [this] {
            render();
        },
        Qt::QueuedConnection);
}

double Scene::cameraDistance() const {
    return p->renderer->GetActiveCamera()->GetDistance();
}

void Scene::showData(const Display &display, Snapshot source, bool live) {
    const auto started = monotonicUs();
    const auto timing = qScopeGuard([this, started] {
        p->updateTiming.add(monotonicUs() - started);
    });
    const bool continuingLive =
        live && p->live && source && p->source &&
        bool(source->tracking) == bool(p->source->tracking) &&
        source->descriptor.value("kind") == p->source->descriptor.value("kind");
    if (continuingLive && p->actors.size() == display.pieces.size() &&
        bool(p->axesActor) == bool(display.axes)) {
        bool hadPoints = false;
        for (size_t i = 0; i < p->actors.size(); ++i) {
            auto *mapper = vtkPolyDataMapper::SafeDownCast(p->actors[i]->GetMapper());
            hadPoints = hadPoints || mapper->GetInput()->GetNumberOfPoints() > 0;
            mapper->SetInputData(display.pieces[i]);
        }
        if (p->axesActor)
            vtkPolyDataMapper::SafeDownCast(p->axesActor->GetMapper())->SetInputData(display.axes);
        p->source = std::move(source);
        // Keep the live camera and GPU objects. Replacing mapper input updates
        // positions without tearing down graphics resources for every BX object.
        if (!hadPoints) {
            p->renderer->ResetCamera();
            p->renderer->GetActiveCamera()->Zoom(0.9);
        }
        p->renderer->ResetCameraClippingRange();
        return;
    }
    if (p->widget && p->widget->isValid() && !p->actors.empty()) {
        p->widget->makeCurrent();
        p->renderer->ReleaseGraphicsResources(p->window);
        p->widget->doneCurrent();
    }
    p->renderer->RemoveAllViewProps();
    p->actors.clear();
    p->axesActor = nullptr;
    p->source = std::move(source);
    p->live = live;
    for (const auto &piece : display.pieces) {
        vtkNew<vtkPolyDataMapper> mapper;
        mapper->SetInputData(piece);
        mapper->ScalarVisibilityOff();
        const bool cloud =
            p->source && p->source->descriptor.value("kind").toString() == "BxPointCloud";
        auto actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);
        if (cloud)
            actor->GetProperty()->SetColor(0.05, 0.43, 0.53);
        else
            actor->GetProperty()->SetColor(.29, .38, .47);
        actor->GetProperty()->SetPointSize(p->source && p->source->tracking ? 9
                                           : cloud                          ? p->pointSize
                                                                            : 3);
        p->renderer->AddActor(actor);
        p->actors.push_back(actor);
    }
    if (display.axes) {
        vtkNew<vtkPolyDataMapper> m;
        m->SetInputData(display.axes);
        m->SetColorModeToDirectScalars();
        vtkNew<vtkActor> a;
        a->SetMapper(m);
        a->GetProperty()->SetLineWidth(3);
        p->renderer->AddActor(a);
        p->axesActor = a;
    }
    auto camera = p->renderer->GetActiveCamera();
    if (!continuingLive) {
        camera->SetPosition(0, -1, .55);
        camera->SetFocalPoint(0, 0, 0);
        camera->SetViewUp(0, 0, 1);
        p->renderer->ResetCamera();
        camera->Zoom(0.9); // keep a margin so extreme points are not on the edge
    }
    p->renderer->ResetCameraClippingRange();
}

void Scene::setPointSize(int size) {
    p->pointSize = std::clamp(size, 1, 20);
    for (auto &actor : p->actors)
        actor->GetProperty()->SetPointSize(p->pointSize);
    render();
}

void Scene::reset() {
    auto cam = p->renderer->GetActiveCamera();
    cam->SetPosition(0, -1, .55);
    cam->SetFocalPoint(0, 0, 0);
    cam->SetViewUp(0, 0, 1);
    p->renderer->ResetCamera();
    cam->Zoom(0.9);
    p->renderer->ResetCameraClippingRange();
    render();
}

void Scene::select(int field, int rep) {
    for (size_t i = 0; i < p->actors.size(); ++i) {
        auto &a = p->actors[i];
        auto m = vtkPolyDataMapper::SafeDownCast(a->GetMapper());
        m->ScalarVisibilityOff();
        a->GetProperty()->LightingOn();
#ifdef SIM_VTK_GLES
        a->GetProperty()->SetAmbient(0.0);
        a->GetProperty()->SetDiffuse(1.0);
#endif
        a->GetProperty()->SetRepresentation(rep == 1   ? VTK_POINTS
                                            : rep == 2 ? VTK_WIREFRAME
                                                       : VTK_SURFACE);
        if (!p->source)
            continue;
        int choice = field;
        if (choice == -1) {
            for (size_t j = 0; j < p->source->fields.size(); ++j) {
                auto &f = p->source->fields[j];
                if (f.piece == int(i) && f.role == "Colors" && !f.list &&
                    (f.components == 3 || f.components == 4)) {
                    choice = int(j);
                    break;
                }
            }
        }
        if (choice < 0 || size_t(choice) >= p->source->fields.size())
            continue;
        const auto &f = p->source->fields[choice];
        if (f.piece != int(i))
            continue;
        auto name = id(choice);
        auto attrs = f.section == "PointData"
                         ? static_cast<vtkDataSetAttributes *>(m->GetInput()->GetPointData())
                         : m->GetInput()->GetCellData();
        auto arr = attrs->GetArray(name.constData());
        if (!arr || !arr->GetNumberOfTuples())
            continue;
        if (f.section == "PointData")
            m->SetScalarModeToUsePointFieldData();
        else
            m->SetScalarModeToUseCellFieldData();
        m->SelectColorArray(name.constData());
        m->ScalarVisibilityOn();
        if (field == -1) {
            m->SetColorModeToDirectScalars();
#ifdef SIM_VTK_GLES
            // The 9.4.2 GLES mapper skips its material and opacity uniforms when
            // lighting is off. Ambient-only shading preserves vertex RGBA.
            a->GetProperty()->SetAmbient(1.0);
            a->GetProperty()->SetDiffuse(0.0);
#else
            a->GetProperty()->LightingOff();
#endif
        } else {
            m->SetColorModeToMapScalars();
            m->SetArrayComponent(0);
            double range[2];
            arr->GetFiniteRange(range, 0);
            if (std::isfinite(range[0]) && std::isfinite(range[1]))
                m->SetScalarRange(range);
            a->GetProperty()->LightingOn();
        }
    }
    render();
}
} // namespace rt::model
