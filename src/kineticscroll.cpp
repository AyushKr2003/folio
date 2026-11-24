#include "kineticscroll.h"

#include <QMetaObject>
#include <QWheelEvent>
#include <QtMath>

namespace {

constexpr qint64 kVelocityWindowMs = 80;
constexpr qreal kMinFlickVelocity = 120.0;
constexpr qreal kWheelStepPixels = 96.0;
constexpr int kWheelDurationMs = 110;

qreal propReal(const QObject *obj, const char *name) {
    return obj ? obj->property(name).toReal() : 0.0;
}

} // namespace

KineticScroll::KineticScroll(QObject *parent) : QObject(parent) {
    m_clock.start();
    m_wheelAnim.setDuration(kWheelDurationMs);
    m_wheelAnim.setEasingCurve(QEasingCurve::OutCubic);
    connect(&m_wheelAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        if (!m_target) {
            m_wheelAnim.stop();
            return;
        }
        const char *prop = m_animatingHorizontal ? "contentX" : "contentY";
        const char *sizeProp = m_animatingHorizontal ? "contentWidth" : "contentHeight";
        const char *viewProp = m_animatingHorizontal ? "width" : "height";
        const char *originProp = m_animatingHorizontal ? "originX" : "originY";
        const qreal current = propReal(m_target, prop);
        // If something else moved the view (scrollbar drag, keyboard navigation,
        // programmatic jump), yield immediately instead of pulling it back.
        if (qAbs(current - m_lastAnimatedValue) > 1.5) {
            m_wheelAnim.stop();
            return;
        }
        const qreal origin = propReal(m_target, originProp);
        const qreal maxPos = origin + qMax<qreal>(0.0, propReal(m_target, sizeProp) - propReal(m_target, viewProp));
        const qreal raw = value.toReal();
        const qreal next = qBound(origin, raw, maxPos);
        m_lastAnimatedValue = next;
        m_target->setProperty(prop, next);
        if (next != raw) {
            m_wheelTarget = next;
            m_wheelAnim.stop();
        }
    });
}

KineticScroll::~KineticScroll() {
    if (m_target)
        m_target->removeEventFilter(this);
}

void KineticScroll::setTarget(QQuickItem *target) {
    if (m_target == target)
        return;
    stopWheelAnimation();
    if (m_target)
        m_target->removeEventFilter(this);
    m_target = target;
    if (m_target)
        m_target->installEventFilter(this);
    emit targetChanged();
}

void KineticScroll::setHorizontal(bool horizontal) {
    if (m_horizontal == horizontal)
        return;
    m_horizontal = horizontal;
    emit horizontalChanged();
}

void KineticScroll::stopWheelAnimation() {
    if (m_wheelAnim.state() == QAbstractAnimation::Running)
        m_wheelAnim.stop();
}

void KineticScroll::recordSample(qint64 now, const QPointF &delta) {
    m_samples.push_back({now, delta});
    qsizetype drop = 0;
    while (drop < m_samples.size() && (now - m_samples[drop].timeMs > kVelocityWindowMs || m_samples.size() - drop > 8))
        ++drop;
    if (drop > 0)
        m_samples.remove(0, drop);
}

QPointF KineticScroll::computeVelocity(qint64 now) const {
    if (m_samples.isEmpty())
        return {};
    if (now - m_samples.last().timeMs > kVelocityWindowMs)
        return {};
    QPointF total;
    qint64 first = m_samples.first().timeMs;
    for (const Sample &s : m_samples) {
        if (now - s.timeMs <= kVelocityWindowMs) {
            if (total.isNull())
                first = s.timeMs;
            total += s.delta;
        }
    }
    const qint64 spanMs = qMax<qint64>(16, now - first);
    const qreal seconds = spanMs / 1000.0;
    return QPointF(total.x() / seconds, total.y() / seconds);
}

bool KineticScroll::handleDiscreteWheel(const QPoint &angleDelta) {
    if (!m_target)
        return false;
    const bool horiz = (angleDelta.y() == 0 && angleDelta.x() != 0);
    if (m_horizontal && !horiz)
        return false;
    const int delta = horiz ? angleDelta.x() : angleDelta.y();
    if (delta == 0)
        return false;

    const char *posProp = horiz ? "contentX" : "contentY";
    const char *sizeProp = horiz ? "contentWidth" : "contentHeight";
    const char *viewProp = horiz ? "width" : "height";
    const char *originProp = horiz ? "originX" : "originY";

    const qreal origin = propReal(m_target, originProp);
    const qreal maxPos = origin + qMax<qreal>(0.0, propReal(m_target, sizeProp) - propReal(m_target, viewProp));
    if (maxPos <= origin)
        return false;

    const qreal current = propReal(m_target, posProp);
    const qreal step = -(qreal(delta) / 120.0) * kWheelStepPixels;
    const qreal remaining = m_wheelTarget - current;
    const bool sameDirection = (remaining > 0 && step > 0) || (remaining < 0 && step < 0);
    const bool continuing = m_wheelAnim.state() == QAbstractAnimation::Running
                            && m_animatingHorizontal == horiz && sameDirection;
    const qreal base = continuing ? m_wheelTarget : current;
    const qreal target = qBound(origin, base + step, maxPos);

    // Already at the edge in this direction: let a parent Flickable handle it.
    if (qFuzzyCompare(target + 1.0, current + 1.0) && !continuing)
        return false;

    QMetaObject::invokeMethod(m_target, "cancelFlick");
    m_animatingHorizontal = horiz;
    m_wheelTarget = target;
    m_lastAnimatedValue = current;
    m_wheelAnim.stop();
    m_wheelAnim.setStartValue(current);
    m_wheelAnim.setEndValue(target);
    m_wheelAnim.start();
    return true;
}

bool KineticScroll::eventFilter(QObject *watched, QEvent *event) {
    if (watched != m_target)
        return QObject::eventFilter(watched, event);

    if (event->type() == QEvent::MouseButtonPress) {
        stopWheelAnimation();
        m_samples.clear();
        return false;
    }

    if (event->type() != QEvent::Wheel)
        return false;

    auto *we = static_cast<QWheelEvent *>(event);
    // Leave Ctrl+wheel alone for zooming the grid/list.
    if (we->modifiers() & Qt::ControlModifier)
        return false;

    const Qt::ScrollPhase phase = we->phase();
    const qint64 now = we->timestamp() > 0 ? qint64(we->timestamp()) : m_clock.elapsed();

    // Platform already supplies OS momentum (e.g. macOS): never double-coast.
    if (phase == Qt::ScrollMomentum) {
        m_samples.clear();
        m_coasting = false;
        stopWheelAnimation();
        return false;
    }

    // Touchpad gesture start/update: stop any active coast we started so
    // touching the pad arrests motion immediately, track velocity, and let Qt
    // Quick handle 1:1 finger movement natively.
    if (phase == Qt::ScrollBegin) {
        stopWheelAnimation();
        if (m_coasting) {
            m_coasting = false;
            QMetaObject::invokeMethod(m_target, "cancelFlick");
        }
        m_samples.clear();
        if (!we->pixelDelta().isNull())
            recordSample(now, QPointF(we->pixelDelta()));
        return false;
    }

    if (phase == Qt::ScrollUpdate || (phase == Qt::NoScrollPhase && !we->pixelDelta().isNull())) {
        stopWheelAnimation();
        if (m_coasting) {
            m_coasting = false;
            QMetaObject::invokeMethod(m_target, "cancelFlick");
        }
        if (!we->pixelDelta().isNull())
            recordSample(now, QPointF(we->pixelDelta()));
        return false;
    }

    if (phase == Qt::ScrollEnd) {
        const QPointF v = computeVelocity(now);
        m_samples.clear();
        const bool canX = propReal(m_target, "contentWidth") > propReal(m_target, "width") + 1.0;
        const bool canY = propReal(m_target, "contentHeight") > propReal(m_target, "height") + 1.0;
        const qreal maxV = propReal(m_target, "maximumFlickVelocity");
        const qreal limit = maxV > 0 ? maxV : 5000.0;
        const qreal vx = canX ? qBound(-limit, v.x(), limit) : 0.0;
        const qreal vy = canY ? qBound(-limit, v.y(), limit) : 0.0;
        if (qHypot(vx, vy) >= kMinFlickVelocity) {
            m_coasting = true;
            QMetaObject::invokeMethod(m_target, "cancelFlick");
            QMetaObject::invokeMethod(m_target, "flick", Q_ARG(qreal, vx), Q_ARG(qreal, vy));
            we->accept();
            return true;
        }
        return false;
    }

    // Discrete mouse wheel (no phase, no pixelDelta): smooth ease-out scroll.
    if (phase == Qt::NoScrollPhase && we->pixelDelta().isNull() && !we->angleDelta().isNull()) {
        if (handleDiscreteWheel(we->angleDelta())) {
            we->accept();
            return true;
        }
    }
    return false;
}
