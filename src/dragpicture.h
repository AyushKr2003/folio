#pragma once

#include <QObject>
#include <QPointer>
#include <QQuickItemGrabResult>
#include <QSharedPointer>
#include <QUrl>

class QQuickItem;

// A drag's picture, taken ahead of time. The C++ grab API keeps a render
// callback's result alive even if its item disappears while it is running.
class DragPicture : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl url READ url NOTIFY ready)

public:
    explicit DragPicture(QObject *parent = nullptr) : QObject(parent) {}
    Q_INVOKABLE bool capture(QQuickItem *item);
    QUrl url() const;

signals:
    void ready();

private:
    QPointer<QQuickItem> m_source;
    QSharedPointer<QQuickItemGrabResult> m_grab;
};
