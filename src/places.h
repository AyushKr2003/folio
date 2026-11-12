#pragma once

#include <QFutureWatcher>
#include <QObject>
#include <QTimer>
#include <QVariantList>

#include "drives.h"

// The sidebar's own rows: the user's places, pinned folders, views and
// drives (plugins add the rest). Each list is small, so it is handed to QML
// as plain maps rather than a model.
class Places : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString home READ home CONSTANT)
    Q_PROPERTY(QVariantList places READ places NOTIFY changed)
    Q_PROPERTY(QVariantList pinned READ pinned NOTIFY changed)
    Q_PROPERTY(QVariantList views READ views NOTIFY changed)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)

public:
    // `home` is overridable for tests; the real home also consults the XDG
    // user-dirs, so renamed or translated folders are found.
    explicit Places(const QString &home = {}, QObject *parent = nullptr);

    QString home() const { return m_home; }
    QVariantList places() const { return m_places; }
    QVariantList pinned() const { return m_pinned; }
    QVariantList views() const { return m_views; }
    QVariantList devices() const { return m_devices; }

    Q_INVOKABLE void refresh();

    // Pins are GTK's bookmarks (~/.config/gtk-3.0/bookmarks), so a folder
    // pinned here is also in every GTK file dialog, and the other way round.
    // Returns an empty string, or why it couldn't be pinned.
    Q_INVOKABLE QString pin(const QString &path);
    Q_INVOKABLE void unpin(const QString &path);
    Q_INVOKABLE bool isPinned(const QString &path) const;

    // Lets go of the drive at `path`: ejects it when it can be (so a stick
    // or card is safe to pull out), otherwise unmounts it. Off the UI
    // thread, in GIO; `failed` says why when it can't.
    Q_INVOKABLE void eject(const QString &path);
    // Whatever shows a folder on the drive mounted at `root` leaves it,
    // before the drive is let go of or changed.
    Q_INVOKABLE void leave(const QString &root);

    struct Mount {
        QString root;
        QString device;
        QString type;
        QString label;
        // What GIO can do with it, and the bytes left on it for the user;
        // -1 when that isn't known.
        bool canEject = false;
        bool canUnmount = false;
        qint64 free = -1;
    };
    // The physical drives among the mounts, one entry per drive: removable
    // and extra internal disks, never the system's own partitions. Each
    // says how it's let go of ("release": "eject", "unmount", or empty when
    // it can't be) and how much is free.
    static QVariantList physicalDrives(const QList<Mount> &mounts, const QString &systemDevice);
    // The Devices list: GIO's volumes in its order, each mounted one as its
    // drive among `drives` and each unmounted one by `volume` (no path, and
    // nothing to let go of, until it's mounted), then the drives GIO
    // doesn't list (an fstab mount). A mounted volume `drives` leaves out
    // (the system's own) stays out.
    static QVariantList devicesFrom(const QList<Drives::Volume> &volumes, const QVariantList &drives);

    // GIO's drives; null for a test's home, which has none.
    Drives *drives() const { return m_drives; }

signals:
    void changed();
    // A drive is about to be let go of: whatever shows a folder on it
    // leaves, so Folio itself doesn't keep it busy.
    void releasing(const QString &root);
    void failed(const QString &message);

private:
    void refreshPlaces();
    void refreshDevices();
    // Every mount and its free space, read off the UI thread: reading them
    // asks each filesystem, and a network one (an NFS or SMB server that's
    // gone away) can take its time answering, or never answer.
    static QList<Mount> readMounts();
    void mountsRead();
    void refreshPinned();
    QString bookmarksPath() const;
    QStringList readBookmarkLines() const;
    bool writeBookmarkLines(const QStringList &lines);
    QString userDirectory(int location, const QString &fallback) const;

    QString m_home;
    bool m_realHome;
    QVariantList m_places, m_pinned, m_views, m_devices;
    QTimer m_poll;
    QFutureWatcher<QList<Mount>> m_mounts;
    // Another refresh was asked for while one was reading.
    bool m_rereadMounts = false;
    // The device the system runs from, read once: it doesn't change.
    QString m_systemDevice;
    Drives *m_drives = nullptr;
};
