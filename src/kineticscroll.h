#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QQuickItem>
#include <QVariantAnimation>
#include <QVector>

// Adds kinetic momentum after touchpad scroll gestures (Qt::ScrollEnd) and
// smooth easing for discrete mouse-wheel notches on a Flickable (ListView,
// GridView, or horizontal columns Flickable). Ctrl+wheel is left untouched
// for zoom, and platforms that already deliver Qt::ScrollMomentum never
// double-coast.
class KineticScroll : public QObject {
    Q_OBJECT
    Q_PROPERTY(QQuickItem *target READ target WRITE setTarget NOTIFY targetChanged)
    Q_PROPERTY(bool horizontal READ horizontal WRITE setHorizontal NOTIFY horizontalChanged)

public:
    explicit KineticScroll(QObject *parent = nullptr);
    ~KineticScroll() override;

    QQuickItem *target() const { return m_target; }
    void setTarget(QQuickItem *target);
    bool horizontal() const { return m_horizontal; }
    void setHorizontal(bool horizontal);

signals:
    void targetChanged();
    void horizontalChanged();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct Sample {
        qint64 timeMs = 0;
        QPointF delta;
    };

    void recordSample(qint64 now, const QPointF &delta);
    QPointF computeVelocity(qint64 now) const;
    void stopWheelAnimation();
    bool handleDiscreteWheel(const QPoint &angleDelta);

    QPointer<QQuickItem> m_target;
    bool m_horizontal = false;
    QVector<Sample> m_samples;
    QElapsedTimer m_clock;
    QVariantAnimation m_wheelAnim;
    qreal m_wheelTarget = 0;
    qreal m_lastAnimatedValue = 0;
    bool m_animatingHorizontal = false;
    bool m_coasting = false;
};
