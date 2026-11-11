#include "drives.h"

#include <QPointer>
#include <QTimer>

// GIO's headers use `signals` as a struct member, which Qt defines as a
// keyword.
#undef signals
#include <gio/gio.h>
#define signals Q_SIGNALS

namespace {

QString take(char *text) {
    const QString s = QString::fromUtf8(text ? text : "");
    g_free(text);
    return s;
}

QString rootOf(GMount *mount) {
    GFile *root = g_mount_get_root(mount);
    const QString path = take(g_file_get_path(root));
    g_object_unref(root);
    return path;
}

// What the callbacks carry: the Drives (gone if Folio quit first) and the
// drive's name for what they say.
struct Pending {
    QPointer<Drives> drives;
    QString name;
};

// The user dismissing the polkit prompt, or a question nobody answered.
bool cancelled(const GError *error) {
    return g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)
        || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
}

} // namespace

struct DrivesCallbacks {
    static void changed(GVolumeMonitor *, gpointer, gpointer data) {
        emit static_cast<Drives *>(data)->changed();
    }

    static void added(GVolumeMonitor *, GVolume *volume, gpointer data) {
        auto *self = static_cast<Drives *>(data);
        emit self->changed();
        // Only what udisks marks to mount by itself (removable media, not
        // another internal partition).
        if (!g_volume_should_automount(volume) || !g_volume_can_mount(volume) || !Drives::automountWanted())
            return;
        // An automounter of the desktop's own (udiskie, a shell) gets the
        // first go, so a drive isn't mounted twice, nor an encrypted one's
        // passphrase asked for twice.
        const QString id = Drives::idOf(volume);
        const QString name = take(g_volume_get_name(volume));
        QTimer::singleShot(Drives::automountDelayMs, self, [self, id, name] {
            GVolume *now = Drives::find(id);
            if (!now)
                return;
            GMount *mount = g_volume_get_mount(now);
            const bool unmounted = !mount && g_volume_can_mount(now);
            if (mount)
                g_object_unref(mount);
            g_object_unref(now);
            if (unmounted)
                emit self->plugged(id, name);
        });
    }

    // Unmounted from anywhere (here, another app, the drive pulled out):
    // nothing should keep showing a folder that's gone.
    static void mountRemoved(GVolumeMonitor *, GMount *mount, gpointer data) {
        auto *self = static_cast<Drives *>(data);
        const QString root = rootOf(mount);
        if (!root.isEmpty())
            emit self->leaving(root);
        emit self->changed();
    }

    static void mounted(GObject *source, GAsyncResult *result, gpointer data) {
        auto *pending = static_cast<Pending *>(data);
        GError *error = nullptr;
        const bool ok = g_volume_mount_finish(G_VOLUME(source), result, &error);
        if (Drives *self = pending->drives.data()) {
            if (ok)
                emit self->notice(QObject::tr("%1 is mounted").arg(pending->name));
            else if (!cancelled(error) && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED))
                emit self->failed(QObject::tr("Couldn't mount %1: %2").arg(pending->name, QString::fromUtf8(error->message)));
        }
        if (error)
            g_error_free(error);
        delete pending;
    }

    static void unmounted(GObject *source, GAsyncResult *result, gpointer data, bool ejected) {
        auto *pending = static_cast<Pending *>(data);
        GError *error = nullptr;
        bool ok;
        if (G_IS_MOUNT(source))
            ok = ejected ? g_mount_eject_with_operation_finish(G_MOUNT(source), result, &error)
                         : g_mount_unmount_with_operation_finish(G_MOUNT(source), result, &error);
        else
            ok = g_volume_eject_with_operation_finish(G_VOLUME(source), result, &error);
        if (Drives *self = pending->drives.data()) {
            if (ok && ejected)
                emit self->notice(QObject::tr("%1 can be unplugged").arg(pending->name));
            else if (!ok && !cancelled(error))
                // Busy is the usual one: a file open in another app.
                emit self->failed(QObject::tr("Couldn't eject %1: %2").arg(pending->name, QString::fromUtf8(error->message)));
        }
        if (error)
            g_error_free(error);
        delete pending;
    }
    static void unmountedOnly(GObject *s, GAsyncResult *r, gpointer d) { unmounted(s, r, d, false); }
    static void ejectedToo(GObject *s, GAsyncResult *r, gpointer d) { unmounted(s, r, d, true); }
};

Drives::Drives(QObject *parent) : QObject(parent), m_monitor(g_volume_monitor_get()) {
    // Qt runs on GLib's main loop on Linux, so these arrive on the UI
    // thread, as soon as udisks sees the change.
    for (const char *signal : {"volume-removed", "volume-changed", "mount-added", "mount-changed",
                               "drive-connected", "drive-disconnected", "drive-changed"})
        g_signal_connect(m_monitor, signal, G_CALLBACK(DrivesCallbacks::changed), this);
    g_signal_connect(m_monitor, "volume-added", G_CALLBACK(DrivesCallbacks::added), this);
    g_signal_connect(m_monitor, "mount-removed", G_CALLBACK(DrivesCallbacks::mountRemoved), this);
}

Drives::~Drives() {
    g_signal_handlers_disconnect_by_data(m_monitor, this);
    g_object_unref(m_monitor);
}

QString Drives::idOf(GVolume *volume) {
    // The device first: two disks copied from one image share a filesystem
    // UUID, but never a device while both are connected.
    for (const char *kind : {G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE, G_VOLUME_IDENTIFIER_KIND_UUID}) {
        const QString id = take(g_volume_get_identifier(volume, kind));
        if (!id.isEmpty())
            return id;
    }
    return take(g_volume_get_name(volume));
}

GVolume *Drives::find(const QString &id) {
    GVolumeMonitor *monitor = g_volume_monitor_get();
    GList *volumes = g_volume_monitor_get_volumes(monitor);
    GVolume *found = nullptr;
    for (GList *l = volumes; l && !found; l = l->next) {
        if (idOf(G_VOLUME(l->data)) == id)
            found = G_VOLUME(g_object_ref(l->data));
    }
    g_list_free_full(volumes, g_object_unref);
    g_object_unref(monitor);
    return found;
}

QList<Drives::Volume> Drives::volumes() const {
    QList<Volume> list;
    GList *volumes = g_volume_monitor_get_volumes(m_monitor);
    for (GList *l = volumes; l; l = l->next) {
        GVolume *volume = G_VOLUME(l->data);
        Volume v{take(g_volume_get_name(volume)), idOf(volume), {}, false};
        if (GMount *mount = g_volume_get_mount(volume)) {
            v.root = rootOf(mount);
            v.ejectable = g_mount_can_unmount(mount) || g_mount_can_eject(mount);
            g_object_unref(mount);
        } else {
            // A stick that's plugged in but not mounted can still be ejected.
            v.ejectable = g_volume_can_eject(volume);
            if (!g_volume_can_mount(volume))
                continue;
        }
        list << v;
    }
    g_list_free_full(volumes, g_object_unref);
    return list;
}

void Drives::eject(const QString &id) {
    GVolume *volume = find(id);
    if (!volume)
        return;
    const QString name = take(g_volume_get_name(volume));
    // An operation with nothing answering it: udisks still asks polkit
    // (it's only left out without one), while a "files are open, unmount
    // anyway?" question goes unanswered, so nothing is cut off mid-write.
    GMountOperation *operation = g_mount_operation_new();
    if (GMount *mount = g_volume_get_mount(volume)) {
        emit leaving(rootOf(mount));
        if (g_mount_can_eject(mount))
            g_mount_eject_with_operation(mount, G_MOUNT_UNMOUNT_NONE, operation, nullptr,
                                         DrivesCallbacks::ejectedToo, new Pending{this, name});
        else
            g_mount_unmount_with_operation(mount, G_MOUNT_UNMOUNT_NONE, operation, nullptr,
                                           DrivesCallbacks::unmountedOnly, new Pending{this, name});
        g_object_unref(mount);
    } else if (g_volume_can_eject(volume)) {
        g_volume_eject_with_operation(volume, G_MOUNT_UNMOUNT_NONE, operation, nullptr,
                                      DrivesCallbacks::ejectedToo, new Pending{this, name});
    }
    g_object_unref(operation);
    g_object_unref(volume);
}

void Drives::mountQuietly(const QString &id) {
    GVolume *volume = find(id);
    if (!volume)
        return;
    GMountOperation *operation = g_mount_operation_new();
    g_volume_mount(volume, G_MOUNT_MOUNT_NONE, operation, nullptr, DrivesCallbacks::mounted,
                   new Pending{this, take(g_volume_get_name(volume))});
    g_object_unref(operation);
    g_object_unref(volume);
}

bool Drives::automountWanted() {
    GSettingsSchemaSource *source = g_settings_schema_source_get_default();
    GSettingsSchema *schema = source ? g_settings_schema_source_lookup(source, "org.gnome.desktop.media-handling", TRUE)
                                     : nullptr;
    if (!schema)
        return true;
    bool wanted = true;
    if (g_settings_schema_has_key(schema, "automount")) {
        GSettings *settings = g_settings_new_full(schema, nullptr, nullptr);
        wanted = g_settings_get_boolean(settings, "automount");
        g_object_unref(settings);
    }
    g_settings_schema_unref(schema);
    return wanted;
}
