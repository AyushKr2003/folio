#include "foreignparent.h"

#include <QEvent>
#include <QGuiApplication>
#include <QPointer>
#include <QWindow>
#include <memory>

#ifdef FOLIO_WAYLAND_FOREIGN
#include <QtGui/qguiapplication_platform.h>
#include <QtGui/qpa/qplatformwindow_p.h>
#include <cstring>
#include <wayland-client.h>
#endif

namespace ForeignParent {

ParsedHandle parse(const QString &parentWindow) {
    ParsedHandle out;
    if (parentWindow.startsWith(QLatin1String("wayland:"))) {
        const QString handle = parentWindow.mid(8).trimmed();
        if (!handle.isEmpty()) {
            out.kind = ParsedHandle::Kind::Wayland;
            out.wayland = handle;
        }
    } else if (parentWindow.startsWith(QLatin1String("x11:"))) {
        bool ok = false;
        const qulonglong id = parentWindow.mid(4).trimmed().toULongLong(&ok, 16);
        if (ok && id != 0) {
            out.kind = ParsedHandle::Kind::X11;
            out.x11 = WId(id);
        }
    }
    return out;
}

#ifdef FOLIO_WAYLAND_FOREIGN
namespace {

// Minimal xdg-foreign-unstable-v2 client bindings (matching
// xdg-foreign-unstable-v2.xml version 1) so Folio needs no build-time
// wayland-protocols dependency beyond wayland-client.
struct zxdg_importer_v2;
struct zxdg_imported_v2;

extern const wl_interface zxdg_imported_v2_interface;

const wl_interface *const importerTypes[] = {
    &zxdg_imported_v2_interface,
    nullptr,
};

const wl_message importerRequests[] = {
    {"destroy", "", importerTypes + 1},
    {"import_toplevel", "ns", importerTypes + 0},
};

const wl_interface zxdg_importer_v2_interface = {
    "zxdg_importer_v2", 1,
    2, importerRequests,
    0, nullptr,
};

const wl_interface *const importedTypes[] = {
    &wl_surface_interface,
};

const wl_message importedRequests[] = {
    {"destroy", "", importerTypes + 1},
    {"set_parent_of", "o", importedTypes + 0},
};

const wl_message importedEvents[] = {
    {"destroyed", "", importerTypes + 1},
};

const wl_interface zxdg_imported_v2_interface = {
    "zxdg_imported_v2", 1,
    2, importedRequests,
    1, importedEvents,
};

struct zxdg_imported_v2_listener {
    void (*destroyed)(void *data, zxdg_imported_v2 *imported);
};

void destroyImporter(zxdg_importer_v2 *importer) {
    wl_proxy_marshal_flags(reinterpret_cast<wl_proxy *>(importer), 0, nullptr,
                           wl_proxy_get_version(reinterpret_cast<wl_proxy *>(importer)), WL_MARSHAL_FLAG_DESTROY);
}

zxdg_imported_v2 *importToplevel(zxdg_importer_v2 *importer, const char *handle) {
    wl_proxy *id = wl_proxy_marshal_flags(reinterpret_cast<wl_proxy *>(importer), 1, &zxdg_imported_v2_interface,
                                          wl_proxy_get_version(reinterpret_cast<wl_proxy *>(importer)), 0, nullptr, handle);
    return reinterpret_cast<zxdg_imported_v2 *>(id);
}

void destroyImported(zxdg_imported_v2 *imported) {
    wl_proxy_marshal_flags(reinterpret_cast<wl_proxy *>(imported), 0, nullptr,
                           wl_proxy_get_version(reinterpret_cast<wl_proxy *>(imported)), WL_MARSHAL_FLAG_DESTROY);
}

void setParentOf(zxdg_imported_v2 *imported, wl_surface *surface) {
    wl_proxy_marshal_flags(reinterpret_cast<wl_proxy *>(imported), 1, nullptr,
                           wl_proxy_get_version(reinterpret_cast<wl_proxy *>(imported)), 0, surface);
}

class WaylandForeignParent : public QObject {
public:
    WaylandForeignParent(QWindow *window, const QString &handle)
        : QObject(window), m_window(window), m_handle(handle.toUtf8()) {
        auto *wlApp = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>() : nullptr;
        if (!wlApp || !wlApp->display())
            return;
        m_display = wlApp->display();
        bindImporter();
        if (!m_importer)
            return;
        window->installEventFilter(this);
        hookWaylandWindow();
    }

    ~WaylandForeignParent() override {
        clearImported();
        if (m_importer) {
            destroyImporter(m_importer);
            m_importer = nullptr;
        }
        if (m_registry) {
            wl_registry_destroy(m_registry);
            m_registry = nullptr;
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == m_window && event->type() == QEvent::PlatformSurface) {
            auto *surfaceEvent = static_cast<QPlatformSurfaceEvent *>(event);
            if (surfaceEvent->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated)
                hookWaylandWindow();
            else
                clearImported();
        }
        return QObject::eventFilter(watched, event);
    }

private:
    static void onGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
        auto *self = static_cast<WaylandForeignParent *>(data);
        if (!self->m_importer && std::strcmp(interface, zxdg_importer_v2_interface.name) == 0) {
            self->m_importer = static_cast<zxdg_importer_v2 *>(
                wl_registry_bind(registry, name, &zxdg_importer_v2_interface, qMin<uint32_t>(version, 1)));
        }
    }
    static void onGlobalRemove(void *, wl_registry *, uint32_t) {}

    static void onImportedDestroyed(void *data, zxdg_imported_v2 *imported) {
        auto *self = static_cast<WaylandForeignParent *>(data);
        if (self->m_imported == imported) {
            destroyImported(imported);
            self->m_imported = nullptr;
        }
    }

    void bindImporter() {
        static const wl_registry_listener registryListener = {&onGlobal, &onGlobalRemove};
        m_registry = wl_display_get_registry(m_display);
        if (!m_registry)
            return;
        wl_registry_add_listener(m_registry, &registryListener, this);
        wl_display_roundtrip(m_display);
    }

    void hookWaylandWindow() {
        if (!m_window || !m_importer)
            return;
        auto *wlWin = m_window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
        if (!wlWin)
            return;
        if (m_hooked != wlWin) {
            m_hooked = wlWin;
            connect(wlWin, &QNativeInterface::Private::QWaylandWindow::surfaceRoleCreated,
                    this, &WaylandForeignParent::apply, Qt::UniqueConnection);
            connect(wlWin, &QNativeInterface::Private::QWaylandWindow::surfaceRoleDestroyed,
                    this, &WaylandForeignParent::clearImported, Qt::UniqueConnection);
        }
        if (wlWin->surface() && m_window->isVisible())
            apply();
    }

    void apply() {
        if (!m_window || !m_importer || m_handle.isEmpty())
            return;
        auto *wlWin = m_window->nativeInterface<QNativeInterface::Private::QWaylandWindow>();
        if (!wlWin)
            return;
        wl_surface *surface = wlWin->surface();
        if (!surface)
            return;
        clearImported();
        static const zxdg_imported_v2_listener importedListener = {&onImportedDestroyed};
        m_imported = importToplevel(m_importer, m_handle.constData());
        if (!m_imported)
            return;
        wl_proxy_add_listener(reinterpret_cast<wl_proxy *>(m_imported),
                              reinterpret_cast<void (**)(void)>(const_cast<zxdg_imported_v2_listener *>(&importedListener)),
                              this);
        setParentOf(m_imported, surface);
        wl_display_flush(m_display);
    }

    void clearImported() {
        if (m_imported) {
            destroyImported(m_imported);
            m_imported = nullptr;
            if (m_display)
                wl_display_flush(m_display);
        }
    }

    QPointer<QWindow> m_window;
    QByteArray m_handle;
    wl_display *m_display = nullptr;
    wl_registry *m_registry = nullptr;
    zxdg_importer_v2 *m_importer = nullptr;
    zxdg_imported_v2 *m_imported = nullptr;
    QPointer<QObject> m_hooked;
};

} // namespace
#endif

void attach(QWindow *window, const QString &parentWindow) {
    if (!window || parentWindow.isEmpty())
        return;
    const ParsedHandle parsed = parse(parentWindow);
    if (parsed.kind == ParsedHandle::Kind::Wayland) {
#ifdef FOLIO_WAYLAND_FOREIGN
        if (QGuiApplication::platformName().startsWith(QLatin1String("wayland")))
            new WaylandForeignParent(window, parsed.wayland);
#endif
    } else if (parsed.kind == ParsedHandle::Kind::X11) {
        if (QGuiApplication::platformName() == QLatin1String("xcb")) {
            if (QWindow *foreign = QWindow::fromWinId(parsed.x11)) {
                QObject::connect(window, &QObject::destroyed, foreign, &QObject::deleteLater);
                window->setTransientParent(foreign);
            }
        }
    }
}

} // namespace ForeignParent
