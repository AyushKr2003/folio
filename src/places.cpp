#include "places.h"
#include "fileops.h"
#include "foldermodel.h"

#include <QCollator>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QSettings>
#include <QUrl>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QStorageInfo>
#include <algorithm>

namespace {

// Home's own folders, all but the hidden ones (as refreshPlaces lists them),
// are already under Home.
bool listedUnderHome(const QString &path, const QString &home) {
    const QString clean = QDir::cleanPath(home);
    const QFileInfo info(path);
    return path == clean || (info.absolutePath() == clean && !info.isHidden());
}

// Omarchy's own folder reads as the name it goes by.
QString displayName(const QString &name) {
    return name == "omarchy" ? QStringLiteral("Omarchy") : name;
}

// Reads bookmark lines ("file:///path Label") into pinned entries, leaving
// out folders already listed under Home and missing folders.
QVariantList parseBookmarks(const QStringList &lines, const QString &home) {
    QVariantList pinned;
    QStringList seen;
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        if (line.isEmpty())
            continue;
        const int space = line.indexOf(' ');
        const QUrl url(space < 0 ? line : line.left(space));
        const QString label = space < 0 ? QString() : line.mid(space + 1).trimmed();
        // Remote bookmarks (sftp://, smb://) stay in the file for GTK, but
        // only local folders are pinned here for now.
        if (!url.isLocalFile())
            continue;
        const QString path = QDir::cleanPath(url.toLocalFile());
        // Home's visible folders are already under Home; Omarchy also seeds
        // a few of those as bookmarks for the file dialogs.
        if (listedUnderHome(path, home) || seen.contains(path) || !QFileInfo(path).isDir())
            continue;
        seen << path;
        pinned << QVariantMap{{"name", label.isEmpty() ? displayName(QFileInfo(path).fileName()) : label},
                              {"path", path}, {"icon", "folder"}};
    }
    return pinned;
}

} // namespace

Places::Places(const QString &home, QObject *parent)
    : QObject(parent), m_home(home.isEmpty() ? QDir::homePath() : home), m_realHome(home.isEmpty()) {
    connect(&m_tailscale, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        const QVariantList peers = code == 0 && status == QProcess::NormalExit
            ? parseTailscale(m_tailscale.readAllStandardOutput()) : QVariantList();
        // The saved machines are read again on every poll too: another Folio
        // process may have opened or forgotten one.
        const QVariantList before = m_machines;
        const bool peersChanged = peers != m_peers;
        m_peers = peers;
        refreshMachines();
        if (peersChanged || m_machines != before)
            emit changed();
    });
    connect(&m_dropbox, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        const QString status = parseDropboxStatus(m_dropbox.readAllStandardOutput());
        if (status != m_dropboxStatus) {
            m_dropboxStatus = status;
            refreshPlaces();
            emit changed();
        }
    });

    // Tailnet peers come and go, drives get plugged in, Dropbox syncs: a
    // gentle poll keeps the sidebar honest without any background service.
    m_poll.setInterval(15000);
    // Drives also say at once when they come, go or mount.
    if (m_realHome) {
        m_drives = new Drives(this);
        connect(m_drives, &Drives::changed, this, [this] {
            const QVariantList before = m_devices;
            refreshDevices();
            if (m_devices != before)
                emit changed();
        });
    }
    connect(&m_poll, &QTimer::timeout, this, &Places::refresh);
    if (m_realHome)
        m_poll.start();
    refresh();
}

void Places::refresh() {
    refreshPlaces();
    refreshPinned();
    refreshDevices();
    if (m_realHome) {
        pollTailscale();
        pollDropbox();
    }
    emit changed();
}

QString Places::userDirectory(int location, const QString &fallback) const {
    if (m_realHome) {
        const QString path = QStandardPaths::writableLocation(QStandardPaths::StandardLocation(location));
        if (!path.isEmpty() && path != m_home)
            return path;
    }
    return m_home + "/" + fallback;
}

// The sidebar's Home section is every folder in home but the hidden ones,
// the way they appear in the home folder itself, so whatever someone keeps
// at the top of their home (Work, omarchy, Dropbox) is one click away
// without pinning it.
void Places::refreshPlaces() {
    QVariantList places;
    auto add = [&](const QString &name, const QString &path, const QString &icon, const QString &status = {}) {
        places << QVariantMap{{"name", name}, {"path", path}, {"icon", icon}, {"status", status}};
    };
    // The XDG folders keep their own glyphs, wherever user-dirs points.
    const QHash<QString, QString> icons{
        {userDirectory(QStandardPaths::DocumentsLocation, "Documents"), "documents"},
        {userDirectory(QStandardPaths::DownloadLocation, "Downloads"), "downloads"},
        {userDirectory(QStandardPaths::PicturesLocation, "Pictures"), "pictures"},
        {userDirectory(QStandardPaths::MusicLocation, "Music"), "music"},
        {userDirectory(QStandardPaths::MoviesLocation, "Videos"), "videos"},
        {m_home + "/Dropbox", "dropbox"},
    };

    QStringList names = QDir(m_home).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) { return collator.compare(a, b) < 0; });
    for (const QString &name : names) {
        const QString path = m_home + "/" + name;
        const QString icon = icons.value(path, "folder");
        add(displayName(name), path, icon,
            icon == "dropbox" ? m_dropboxStatus : QString());
    }

    m_places = places;

    // Views: lists that aren't a folder of their own.
    m_views = {QVariantMap{{"name", tr("Recents")}, {"path", FolderModel::recentPath()}, {"icon", "recent"}},
               QVariantMap{{"name", tr("Trash")}, {"path", QDir::cleanPath(FileOps::homeTrash() + "/files")}, {"icon", "trash"}}};
}

QString Places::bookmarksPath() const {
    // Tests use a temporary home, whose bookmarks live inside it.
    const QString config = m_realHome ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
                                      : m_home + "/.config";
    return config + "/gtk-3.0/bookmarks";
}

QStringList Places::readBookmarkLines() const {
    QFile file(bookmarksPath());
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QStringList lines = QString::fromUtf8(file.readAll()).split('\n');
    lines.removeAll(QString());
    return lines;
}

bool Places::writeBookmarkLines(const QStringList &lines) {
    QDir().mkpath(QFileInfo(bookmarksPath()).absolutePath());
    // Written beside the file and renamed over it, so GTK never reads half
    // a list.
    QSaveFile file(bookmarksPath());
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write((lines.join('\n') + '\n').toUtf8());
    return file.commit();
}

void Places::refreshPinned() {
    m_pinned = parseBookmarks(readBookmarkLines(), m_home);
}

bool Places::isPinned(const QString &path) const {
    const QString clean = QDir::cleanPath(path);
    for (const QVariant &p : m_pinned)
        if (p.toMap().value("path").toString() == clean)
            return true;
    return false;
}

QString Places::pin(const QString &path) {
    const QString clean = QDir::cleanPath(path);
    const QFileInfo info(clean);
    if (!info.isDir())
        return tr("Only folders can be pinned.");
    if (listedUnderHome(clean, m_home))
        return tr("%1 is already under Home.").arg(displayName(info.fileName()));
    if (isPinned(clean))
        return {};
    QStringList lines = readBookmarkLines();
    lines << QString::fromUtf8(QUrl::fromLocalFile(clean).toEncoded());
    if (!writeBookmarkLines(lines))
        return tr("Couldn't save the pin to %1.").arg(bookmarksPath());
    refreshPinned();
    emit changed();
    return {};
}

void Places::unpin(const QString &path) {
    const QString clean = QDir::cleanPath(path);
    QStringList lines = readBookmarkLines();
    const int before = lines.size();
    lines.erase(std::remove_if(lines.begin(), lines.end(), [&](const QString &line) {
        const QUrl url(line.section(' ', 0, 0));
        return url.isLocalFile() && QDir::cleanPath(url.toLocalFile()) == clean;
    }), lines.end());
    if (lines.size() != before && writeBookmarkLines(lines)) {
        refreshPinned();
        emit changed();
    }
}

QString Places::settingsPath() const {
    // The window's own settings (the view, the zoom) are in the same file;
    // tests keep theirs inside their temporary home.
    return m_realHome ? QSettings().fileName() : m_home + "/.config/folio/folio.conf";
}

QList<Places::Opened> Places::readOpened() const {
    QSettings settings(settingsPath(), QSettings::IniFormat);
    QList<Opened> opened;
    const int count = settings.beginReadArray("opened-machines");
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        opened << Opened{settings.value("host").toString(), settings.value("when").toString()};
    }
    settings.endArray();
    return opened;
}

void Places::writeOpened(const QList<Opened> &opened) {
    QSettings settings(settingsPath(), QSettings::IniFormat);
    // Rewritten whole, so a shorter list leaves none of the old entries.
    settings.remove("opened-machines");
    settings.beginWriteArray("opened-machines", opened.size());
    for (int i = 0; i < opened.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue("host", opened[i].host);
        settings.setValue("when", opened[i].when);
    }
    settings.endArray();
}

QStringList Places::opened() const {
    QStringList hosts;
    for (const Opened &o : readOpened())
        hosts << o.host;
    return hosts;
}

void Places::remember(const QString &host) {
    QList<Opened> opened = readOpened();
    opened.removeIf([&](const Opened &o) { return o.host == host; });
    opened.prepend({host, QDateTime::currentDateTime().toString(Qt::ISODate)});
    // Enough to come back to the machines that went to sleep for a while,
    // without the settings growing with every machine ever opened.
    opened = opened.mid(0, 20);
    writeOpened(opened);
    refreshMachines();
    emit changed();
}

void Places::forget(const QString &host) {
    QList<Opened> opened = readOpened();
    if (opened.removeIf([&](const Opened &o) { return o.host == host; }) == 0)
        return;
    writeOpened(opened);
    refreshMachines();
    emit changed();
}

QVariantList Places::onlineOf(const QStringList &opened, const QVariantList &peers, int limit) {
    QVariantList online;
    for (const QString &host : opened) {
        for (const QVariant &peer : peers)
            if (peer.toMap().value("host").toString() == host)
                online << peer;
        if (online.size() == limit)
            break;
    }
    return online;
}

void Places::refreshMachines() {
    m_machines = onlineOf(opened(), m_peers);
}

QVariantList Places::physicalDrives(const QList<Mount> &mounts, const QString &systemDevice) {
    // Where the system itself lives; a drive mounted here is the computer,
    // not a device to browse to.
    static const QStringList systemRoots{"/", "/boot", "/efi", "/home", "/var", "/usr", "/opt", "/srv",
                                         "/root", "/tmp", "/nix", "/snap", "/swap"};
    static const QStringList pseudo{"tmpfs", "devtmpfs", "squashfs", "overlay", "ramfs", "fuse.portal",
                                    "fuse.gvfsd-fuse", "efivarfs", "autofs"};
    QVariantList drives;
    QStringList seen;
    for (const Mount &m : mounts) {
        const bool system = systemRoots.contains(m.root)
            || std::any_of(systemRoots.begin() + 1, systemRoots.end(),
                           [&](const QString &r) { return m.root.startsWith(r + "/"); })
            || m.root.startsWith("/.");
        // Only block devices; loop images (snaps, AppImages) and zram aren't
        // drives. Btrfs subvolumes of the system disk share its device.
        const bool block = m.device.startsWith("/dev/") && !m.device.startsWith("/dev/loop")
                           && !m.device.startsWith("/dev/zram");
        if (system || !block || pseudo.contains(m.type) || m.device == systemDevice || seen.contains(m.device))
            continue;
        seen << m.device;
        const QString name = m.label.isEmpty() ? QFileInfo(m.root).fileName() : m.label;
        drives << QVariantMap{{"name", name.isEmpty() ? m.root : name}, {"path", m.root}, {"icon", "drive"}};
    }
    return drives;
}

QVariantList Places::devicesFrom(const QList<Drives::Volume> &volumes, const QVariantList &drives) {
    QVariantList devices;
    QStringList roots;
    for (const Drives::Volume &v : volumes) {
        QVariantMap device{{"name", v.name}, {"path", v.root}, {"icon", "drive"}, {"volume", v.id},
                           {"ejectable", v.ejectable}};
        devices << device;
        if (!v.root.isEmpty())
            roots << v.root;
    }
    for (const QVariant &drive : drives) {
        if (!roots.contains(drive.toMap().value("path").toString()))
            devices << drive;
    }
    return devices;
}

void Places::eject(const QString &volume) {
    if (m_drives)
        m_drives->eject(volume);
}

void Places::refreshDevices() {
    QList<Mount> mounts;
    auto volumes = QStorageInfo::mountedVolumes();
    // Shortest path first, so a drive mounted twice is listed at its top.
    std::sort(volumes.begin(), volumes.end(),
              [](const QStorageInfo &a, const QStorageInfo &b) { return a.rootPath().size() < b.rootPath().size(); });
    for (const QStorageInfo &v : volumes) {
        if (v.isValid() && v.isReady())
            mounts << Mount{v.rootPath(), QString::fromUtf8(v.device()), QString::fromUtf8(v.fileSystemType()),
                            v.name()};
    }
    const QVariantList mounted = physicalDrives(mounts, QString::fromUtf8(QStorageInfo::root().device()));
    m_devices = m_drives ? devicesFrom(m_drives->volumes(), mounted) : mounted;
}

void Places::pollTailscale() {
    if (m_tailscale.state() != QProcess::NotRunning)
        return;
    const QString tailscale = QStandardPaths::findExecutable("tailscale");
    if (tailscale.isEmpty())
        return;
    m_tailscale.start(tailscale, {"status", "--json"});
}

void Places::pollDropbox() {
    if (m_dropbox.state() != QProcess::NotRunning || !QFileInfo(m_home + "/Dropbox").isDir())
        return;
    const QString cli = QStandardPaths::findExecutable("dropbox-cli");
    if (cli.isEmpty())
        return;
    m_dropbox.start(cli, {"status"});
}

QVariantList Places::parseTailscale(const QByteArray &json) {
    const QJsonObject root = QJsonDocument::fromJson(json).object();
    QVariantList peers;
    const QJsonObject all = root.value("Peer").toObject();
    for (auto it = all.begin(); it != all.end(); ++it) {
        const QJsonObject peer = it.value().toObject();
        QString host = peer.value("DNSName").toString();
        if (host.endsWith('.'))
            host.chop(1);
        const QString name = peer.value("HostName").toString();
        // Only machines that can hold files and can be reached right now:
        // phones can't be browsed over SFTP, and offline machines would only
        // be dead ends. The 15-second poll brings them back when they wake.
        // Each keeps its system's name, as its maker writes it.
        static const QHash<QString, QString> systems{{"linux", "Linux"}, {"macOS", "macOS"}, {"windows", "Windows"}};
        const QString os = systems.value(peer.value("OS").toString());
        if (name.isEmpty() || host.isEmpty() || os.isEmpty() || !peer.value("Online").toBool())
            continue;
        peers << QVariantMap{{"name", name}, {"host", host}, {"os", os}};
    }
    std::sort(peers.begin(), peers.end(), [](const QVariant &a, const QVariant &b) {
        return QString::localeAwareCompare(a.toMap().value("name").toString(), b.toMap().value("name").toString()) < 0;
    });
    return peers;
}

QString Places::parseDropboxStatus(const QByteArray &output) {
    const QString text = QString::fromUtf8(output).trimmed();
    if (text.isEmpty() || text.contains("isn't running", Qt::CaseInsensitive))
        return "stopped";
    if (text.startsWith("Up to date", Qt::CaseInsensitive))
        return "synced";
    return "syncing";
}
