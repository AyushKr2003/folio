#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantList>

#include "drives.h"

// The sidebar: the user's places (with Dropbox first-class when it's set up),
// the drives, and the tailnet machines opened in Folio. Each list is small,
// so it is handed to QML as plain maps rather than a model.
class Places : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString home READ home CONSTANT)
    Q_PROPERTY(QVariantList places READ places NOTIFY changed)
    Q_PROPERTY(QVariantList pinned READ pinned NOTIFY changed)
    Q_PROPERTY(QVariantList views READ views NOTIFY changed)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
    // Every online machine on the tailnet that can hold files.
    Q_PROPERTY(QVariantList peers READ peers NOTIFY changed)
    // The few of those opened in Folio, most recently opened first. A
    // tailnet can have hundreds of machines; these are the ones that matter.
    Q_PROPERTY(QVariantList machines READ machines NOTIFY changed)
    Q_PROPERTY(QString dropboxStatus READ dropboxStatus NOTIFY changed)

public:
    // `home` is overridable for tests; the real home also consults the XDG
    // user-dirs, so renamed or translated folders are found.
    explicit Places(const QString &home = {}, QObject *parent = nullptr);

    QString home() const { return m_home; }
    QVariantList places() const { return m_places; }
    QVariantList pinned() const { return m_pinned; }
    QVariantList views() const { return m_views; }
    QVariantList devices() const { return m_devices; }
    QVariantList peers() const { return m_peers; }
    QVariantList machines() const { return m_machines; }
    QString dropboxStatus() const { return m_dropboxStatus; }

    Q_INVOKABLE void refresh();

    // Pins are GTK's bookmarks (~/.config/gtk-3.0/bookmarks), so a folder
    // pinned here is also in every GTK file dialog, and the other way round.
    // Returns an empty string, or why it couldn't be pinned.
    Q_INVOKABLE QString pin(const QString &path);
    Q_INVOKABLE void unpin(const QString &path);
    Q_INVOKABLE bool isPinned(const QString &path) const;

    // Opening a machine records its host, and when, in Folio's settings;
    // forgetting it takes it off the sidebar again.
    Q_INVOKABLE void remember(const QString &host);
    Q_INVOKABLE void forget(const QString &host);
    // The hosts recorded, most recently opened first.
    QStringList opened() const;
    // The peers among `opened`, in its order, at most `limit` of them.
    static QVariantList onlineOf(const QStringList &opened, const QVariantList &peers, int limit = 6);

    // Unmounts a drive from the Devices list, or ejects it when removable.
    Q_INVOKABLE void eject(const QString &volume);

    // GIO's drives; null for a test's home, which has none.
    Drives *drives() const { return m_drives; }

    static QVariantList parseTailscale(const QByteArray &json);
    static QString parseDropboxStatus(const QByteArray &output);

    struct Mount {
        QString root;
        QString device;
        QString type;
        QString label;
    };
    // The physical drives among the mounts, one entry per drive: removable
    // and extra internal disks, never the system's own partitions.
    static QVariantList physicalDrives(const QList<Mount> &mounts, const QString &systemDevice);
    // The Devices list: GIO's volumes in its order, each mounted one at its
    // folder and each unmounted one by `volume` (no path until it's
    // mounted), then any mounted drive GIO doesn't list (an fstab mount).
    static QVariantList devicesFrom(const QList<Drives::Volume> &volumes, const QVariantList &drives);

signals:
    void changed();

private:
    void refreshPlaces();
    void refreshDevices();
    void refreshPinned();
    QString bookmarksPath() const;
    QStringList readBookmarkLines() const;
    bool writeBookmarkLines(const QStringList &lines);
    // A machine opened, and when, as ISO 8601 text, which keeps the
    // settings file readable.
    struct Opened {
        QString host;
        QString when;
    };
    QString settingsPath() const;
    QList<Opened> readOpened() const;
    void writeOpened(const QList<Opened> &opened);
    void refreshMachines();
    void pollTailscale();
    void pollDropbox();
    QString userDirectory(int location, const QString &fallback) const;

    QString m_home;
    bool m_realHome;
    QVariantList m_places, m_pinned, m_views, m_devices, m_peers, m_machines;
    QString m_dropboxStatus;
    QProcess m_tailscale, m_dropbox;
    QTimer m_poll;
    Drives *m_drives = nullptr;
};
