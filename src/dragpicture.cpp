#include "dragpicture.h"

#include <QQuickItem>
#include <QQuickWindow>

bool DragPicture::capture(QQuickItem *item) {
    if (!item)
        return false;
    auto grab = item->grabToImage();
    if (!grab)
        return false;
    // A replaced or discarded delegate can let go between Qt's setup and
    // render callbacks. Keep that frame's work alive until it has completed;
    // queue the release after ready returns. The window also releases it on
    // teardown, after its render thread has stopped.
    connect(grab.data(), &QQuickItemGrabResult::ready, item->window(), [grab] {},
            Qt::ConnectionType(Qt::QueuedConnection | Qt::SingleShotConnection));
    m_source = item;
    m_grab = grab;
    connect(grab.data(), &QQuickItemGrabResult::ready, this, [this, request = grab.data()] {
        if (m_grab.data() == request && m_source)
            emit ready();
    });
    return true;
}

QUrl DragPicture::url() const {
    return m_grab ? m_grab->url() : QUrl();
}
