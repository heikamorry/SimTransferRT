#pragma once
// Medical identity for the simulator: a pale clinical gradient, a faint Rod of
// Asclepius watermark and an ECG baseline. Header-only so every GUI target that
// compiles the windows picks it up without another translation unit.
#include <QColor>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QRadialGradient>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace rt::ui {
namespace detail {
constexpr qreal pi = 3.14159265358979323846;
inline qreal coilPhase(qreal t) {
    return t * 3.25 * 2 * pi + 0.35;
}
// Staff first, then the front half of the serpent: the crossings read as a coil.
inline void paintAsclepius(QPainter &p, const QRectF &box, const QColor &color) {
    if (box.width() <= 1 || box.height() <= 1)
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal s = std::min(box.width(), box.height());
    const qreal cx = box.center().x();
    const qreal top = box.center().y() - s * 0.44, bottom = box.center().y() + s * 0.46;
    const qreal amplitude = s * 0.118;
    const qreal snakeWidth = std::max<qreal>(1.0, s * 0.032);
    const qreal staffWidth = std::max<qreal>(0.8, s * 0.020);
    auto xAt = [&](qreal t) { return cx + std::sin(coilPhase(t)) * amplitude; };
    auto yAt = [&](qreal t) { return top + (bottom - top) * t; };
    QPainterPath back, front;
    bool backOpen = false, frontOpen = false, lastNear = false;
    const int steps = 240;
    for (int i = 0; i <= steps; ++i) {
        const qreal t = qreal(i) / steps;
        const QPointF point(xAt(t), yAt(t));
        const bool near = std::sin(coilPhase(t)) >= 0;
        if (i && near != lastNear)
            backOpen = frontOpen = false; // never bridge the hidden half
        lastNear = near;
        QPainterPath &run = near ? front : back;
        bool &open = near ? frontOpen : backOpen;
        if (!open) {
            run.moveTo(point);
            open = true;
        } else {
            run.lineTo(point);
        }
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(color, snakeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(back);
    p.setPen(QPen(color, staffWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawLine(QPointF(cx, top - s * 0.02), QPointF(cx, bottom));
    p.setPen(QPen(color, snakeWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(front);
    // A single head at the top of the coil; the staff ends in a rounded cap so
    // the two shapes never merge into a blob.
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QPointF(xAt(0) + snakeWidth * 0.55, yAt(0) - snakeWidth * 0.55),
                  snakeWidth * 0.62, snakeWidth * 0.42);
    p.restore();
}
inline QPixmap medicalMark(int logical) {
    const int ratio = 2;
    QPixmap pm(logical * ratio, logical * ratio);
    pm.setDevicePixelRatio(ratio);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    QLinearGradient g(0, 0, logical, logical);
    g.setColorAt(0, QColor("#0F7C99"));
    g.setColorAt(1, QColor("#0A4358"));
    p.setPen(Qt::NoPen);
    p.setBrush(g);
    p.drawRoundedRect(QRectF(0, 0, logical, logical), logical * 0.28, logical * 0.28);
    detail::paintAsclepius(p, QRectF(logical * 0.17, logical * 0.10, logical * 0.66, logical * 0.80),
                           QColor(240, 252, 255, 236));
    p.end();
    return pm;
}
} // namespace detail
using detail::medicalMark;
class MedicalBackdrop final : public QWidget {
  public:
    explicit MedicalBackdrop(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName("medicalBackdrop");
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        QLinearGradient bg(0, 0, width() * 0.4, height());
        bg.setColorAt(0.0, QColor("#F8FCFE"));
        bg.setColorAt(0.55, QColor("#EEF6FA"));
        bg.setColorAt(1.0, QColor("#E3EFF6"));
        p.fillRect(rect(), bg);
        QRadialGradient glow(QPointF(width() * 0.10, 0), std::max(width(), height()) * 0.85);
        glow.setColorAt(0, QColor(255, 255, 255, 165));
        glow.setColorAt(1, QColor(255, 255, 255, 0));
        p.fillRect(rect(), glow);
        const qreal s = std::min(width(), height()) * 0.86;
        detail::paintAsclepius(p,
                               QRectF(width() - s * 0.30, height() - s * 0.90, s * 0.42, s * 0.84),
                               QColor(12, 92, 118, 16));
        // ECG baseline: a quiet monitor trace behind the lower band.
        QPainterPath ecg;
        const qreal base = height() * 0.90, unit = std::max<qreal>(26, width() / 26);
        ecg.moveTo(-unit, base);
        for (qreal x = -unit; x < width() + unit; x += unit) {
            ecg.lineTo(x + unit * 0.30, base);
            ecg.lineTo(x + unit * 0.42, base - height() * 0.006);
            ecg.lineTo(x + unit * 0.50, base + height() * 0.020);
            ecg.lineTo(x + unit * 0.60, base - height() * 0.052);
            ecg.lineTo(x + unit * 0.70, base + height() * 0.030);
            ecg.lineTo(x + unit * 0.80, base);
        }
        p.setPen(QPen(QColor(12, 92, 118, 26), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(ecg);
    }
};
} // namespace rt::ui
