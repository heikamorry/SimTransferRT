#pragma once
// Medical identity for the simulator: a pale clinical gradient, a faint Rod of
// Asclepius watermark and an ECG baseline. Header-only so every GUI target that
// compiles the windows picks it up without another translation unit.
#include <QColor>
#include <QIcon>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QPixmap>
#include <QRadialGradient>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace rt::ui {
namespace detail {
// Draw in a square coordinate system so the icon and watermark scale together.
inline void paintAsclepius(QPainter &p, const QRectF &box, const QColor &color) {
    const qreal side = std::min(box.width(), box.height());
    if (side <= 1)
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    p.translate(box.center().x() - side / 2, box.center().y() - side / 2);
    p.scale(side / 100, side / 100);

    QPainterPath staff;
    staff.moveTo(46, 8);
    staff.cubicTo(46, 4, 52, 4, 52, 8);
    staff.lineTo(50, 94);
    staff.quadTo(49, 98, 48, 94);
    staff.closeSubpath();
    p.fillPath(staff, color);

    // Gaps where the serpent passes behind the staff separate the two coils.
    QPainterPath coils;
    coils.moveTo(63, 16);
    coils.cubicTo(79, 21, 66, 29, 49, 33);
    coils.cubicTo(27, 39, 25, 45, 43, 50);
    coils.moveTo(55, 53);
    coils.cubicTo(73, 59, 64, 64, 47, 68);
    coils.cubicTo(33, 72, 34, 77, 45, 80);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(color, 4.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPath(coils);

    QPainterPath tail;
    tail.moveTo(53, 80);
    tail.cubicTo(65, 83, 61, 89, 53, 91);
    tail.cubicTo(57, 87, 58, 85, 53, 84);
    tail.closeSubpath();
    p.fillPath(tail, color);

    QPainterPath head;
    head.moveTo(66, 18);
    head.cubicTo(62, 20, 55, 18, 54, 14);
    head.quadTo(57, 10, 64, 11);
    head.cubicTo(69, 11, 71, 16, 66, 18);
    p.fillPath(head, color);
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
    detail::paintAsclepius(p,
                           QRectF(logical * 0.17, logical * 0.10, logical * 0.66, logical * 0.80),
                           QColor(240, 252, 255, 236));
    p.end();
    return pm;
}
} // namespace detail

using detail::medicalMark;

// Window and taskbar icon. The PNG renditions are decoded by QtGui's built-in
// PNG reader, so the icon still appears when the optional ICO image-format
// plugin is missing from a deployment. Each size is registered explicitly so
// Windows picks the crisp 16/20/24/32 pixel rendition instead of downscaling a
// large bitmap for the title bar.
inline QIcon appIcon() {
    QIcon icon;
    for (int size : {16, 20, 24, 32, 40, 48, 64, 128, 256}) {
        const QPixmap pixmap(QStringLiteral(":/icons/app-%1.png").arg(size));
        if (!pixmap.isNull())
            icon.addPixmap(pixmap, QIcon::Normal, QIcon::Off);
    }
    if (icon.isNull()) {
        // Last resort: paint the mark directly, no image plugin involved.
        for (int size : {16, 24, 32, 48, 64, 128, 256}) {
            QImage image = detail::medicalMark(size).toImage().scaled(
                size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            image.setDevicePixelRatio(1.0);
            icon.addPixmap(QPixmap::fromImage(image), QIcon::Normal, QIcon::Off);
        }
    }
    return icon;
}

// A reserved stretch area in the header keeps the decorative mark visible
// without painting over the title or the usage badge. Vector painting follows
// the screen's device pixel ratio, including when moving between monitors.
class MedicalWatermark final : public QWidget {
  public:
    explicit MedicalWatermark(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName("headerWatermark");
        setMinimumSize(60, 48);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }

    QSize sizeHint() const override {
        return QSize(84, 48);
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        const qreal side = std::min<qreal>(72, height() - 4);
        const QRectF mark(width() - side - 4, (height() - side) / 2, side, side);
        detail::paintAsclepius(painter, mark, QColor(14, 116, 144, 78));
    }
};

class MedicalBackdrop final : public QWidget {
  public:
    explicit MedicalBackdrop(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName("medicalBackdrop");
        setAttribute(Qt::WA_OpaquePaintEvent, true);
    }

  protected:
    void paintEvent(QPaintEvent *event) override {
        QPainter p(this);
        const qreal dpr = devicePixelRatioF();
        const auto transform = p.deviceTransform();
        if (transform.type() > QTransform::TxScale || transform.m11() != dpr ||
            transform.m22() != dpr || std::floor(transform.dx()) != transform.dx() ||
            std::floor(transform.dy()) != transform.dy()) {
            paintBackground(p);
            return;
        }
        if (cachedSize_ != size() || cachedDpr_ != dpr) {
            background_ = QImage();
            // A resize can invalidate just a sliver. Do not rasterize the full
            // window for a small repaint, including disjoint dirty rectangles.
            qint64 dirtyArea = 0;
            for (const QRect &area : event->region())
                dirtyArea += qint64(area.width()) * area.height();
            if (2 * dirtyArea < qint64(width()) * height()) {
                // A later return to an old size still needs a fresh cache.
                cachedSize_ = {};
                paintBackground(p);
                return;
            }
            cachedSize_ = size();
            cachedDpr_ = dpr;
            // Hold at most one 32 MiB image; very large surfaces retain vector painting.
            constexpr int maxPixels = 8 * 1024 * 1024;
            const qreal w = std::ceil(width() * dpr);
            const qreal h = std::ceil(height() * dpr);
            if (w >= 1 && h >= 1 && w * h <= maxPixels) {
                background_ = QImage(static_cast<int>(w), static_cast<int>(h),
                                     QImage::Format_ARGB32_Premultiplied);
                if (!background_.isNull()) {
                    background_.setDevicePixelRatio(dpr);
                    background_.fill(Qt::transparent);
                    QPainter raster(&background_);
                    paintBackground(raster);
                }
            }
        }
        if (background_.isNull()) {
            paintBackground(p);
            return;
        }
        // A partially covered edge must blend with the actual backing store.
        const QRect interior(0, 0, width() - (width() * dpr != background_.width()),
                             height() - (height() * dpr != background_.height()));
        if (!interior.contains(event->region().boundingRect())) {
            p.save();
            p.setClipRect(interior, Qt::IntersectClip);
            p.drawImage(QPointF(), background_);
            p.restore();
            p.setClipRegion(QRegion(rect()).subtracted(interior), Qt::IntersectClip);
            paintBackground(p);
        } else {
            p.drawImage(QPointF(), background_);
        }
    }

  private:
    QSize cachedSize_;
    qreal cachedDpr_ = 0;
    QImage background_;

    void paintBackground(QPainter &p) const {
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
        const qreal base = height() * 0.90;
        const qreal unit = std::max<qreal>(26, width() / 26);
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
