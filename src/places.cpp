#include "places.h"
#include "disks.h"
#include "fileops.h"
#include "foldermodel.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QUrl>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QtConcurrent>
#include <QPointer>
#include <algorithm>
#include <memory>

// GIO's headers use `signals` as a struct member, which Qt defines as a
// keyword.
#undef signals
#include <gio/gio.h>
#define signals Q_SIGNALS

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

// GIO's mount at `path`, with a reference for the caller, or null when GIO
// doesn't list one there (a drive only fstab knows of, which a user can't
// unmount anyway). The monitor is kept: let go of, it's made again on the
// next call, asking each of GVfs's volume monitors for its mounts over
// D-Bus, once for every drive on every refresh.
GMount *mountAt(const QString &path) {
    static GVolumeMonitor *const monitor = g_volume_monitor_get();
    GList *mounts = g_volume_monitor_get_mounts(monitor);
    GMount *found = nullptr;
    for (GList *l = mounts; l && !found; l = l->next) {
        GFile *root = g_mount_get_root(G_MOUNT(l->data));
        char *rootPath = g_file_get_path(root);
        if (rootPath && QString::fromUtf8(rootPath) == path)
            found = G_MOUNT(g_object_ref(l->data));
        g_free(rootPath);
        g_object_unref(root);
    }
    g_list_free_full(mounts, g_object_unref);
    return found;
}

// What an eject or unmount needs to report back once GIO is done. The
// sidebar can be gone by then, so it's held by a guard.
struct Release {
    QPointer<Places> places;
    QString name;
    bool eject;
};

void released(GObject *source, GAsyncResult *result, gpointer data) {
    std::unique_ptr<Release> release(static_cast<Release *>(data));
    GError *error = nullptr;
    const bool ok = release->eject ? g_mount_eject_with_operation_finish(G_MOUNT(source), result, &error)
                                   : g_mount_unmount_with_operation_finish(G_MOUNT(source), result, &error);
    if (Places *places = release->places.data()) {
        if (ok) {
            places->refresh();
        } else if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_BUSY)) {
            emit places->failed(release->eject ? Places::tr("%1 is in use and can't be ejected.").arg(release->name)
                                               : Places::tr("%1 is in use and can't be unmounted.").arg(release->name));
        } else if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) {
            const QString why = QString::fromUtf8(error ? error->message : "");
            emit places->failed(release->eject ? Places::tr("Couldn't eject %1: %2").arg(release->name, why)
                                               : Places::tr("Couldn't unmount %1: %2").arg(release->name, why));
        }
    }
    if (error)
        g_error_free(error);
}

} // namespace

Places::Places(const QString &home, QObject *parent)
    : QObject(parent), m_home(home.isEmpty() ? QDir::homePath() : home), m_realHome(home.isEmpty()) {
    // Drives get plugged in and folders come and go: a gentle poll keeps
    // the sidebar honest without any background service.
    m_systemDevice = QString::fromUtf8(QStorageInfo::root().device());
    connect(&m_mounts, &QFutureWatcherBase::finished, this, &Places::mountsRead);
    m_poll.setInterval(15000);
    // GIO's drives say at once when they come, go or mount; one unmounted
    // from anywhere (here, another app, pulled out) is left by whatever
    // shows a folder on it.
    if (m_realHome) {
        m_drives = new Drives(this);
        connect(m_drives, &Drives::changed, this, &Places::refreshDevices);
        connect(m_drives, &Drives::leaving, this, &Places::leave);
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
    auto add = [&](const QString &name, const QString &path, const QString &icon) {
        places << QVariantMap{{"name", name}, {"path", path}, {"icon", icon}};
    };
    // The XDG folders keep their own glyphs, wherever user-dirs points.
    const QHash<QString, QString> icons{
        {userDirectory(QStandardPaths::DocumentsLocation, "Documents"), "documents"},
        {userDirectory(QStandardPaths::DownloadLocation, "Downloads"), "downloads"},
        {userDirectory(QStandardPaths::PicturesLocation, "Pictures"), "pictures"},
        {userDirectory(QStandardPaths::MusicLocation, "Music"), "music"},
        {userDirectory(QStandardPaths::MoviesLocation, "Videos"), "videos"},
    };

    QStringList names = QDir(m_home).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) { return collator.compare(a, b) < 0; });
    for (const QString &name : names) {
        const QString path = m_home + "/" + name;
        add(displayName(name), path, icons.value(path, "folder"));
    }

    m_places = places;

    // Views: lists that aren't a folder of their own. The sidebar names
    // them, so a language switch renames them at once.
    m_views = {QVariantMap{{"path", FolderModel::recentPath()}, {"icon", "recent"}},
               QVariantMap{{"path", QDir::cleanPath(FileOps::homeTrash() + "/files")}, {"icon", "trash"}}};
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

QVariantList Places::physicalDrives(const QList<Mount> &mounts, const QString &systemDevice) {
    static const QStringList pseudo{"tmpfs", "devtmpfs", "squashfs", "overlay", "ramfs", "fuse.portal",
                                    "fuse.gvfsd-fuse", "efivarfs", "autofs"};
    QVariantList drives;
    QStringList seen;
    for (const Mount &m : mounts) {
        // A drive mounted where the system lives is the computer, not a
        // device to browse to.
        const bool system = Disks::isSystemMount(m.root);
        // Only block devices; loop images (snaps, AppImages) and zram aren't
        // drives. Btrfs subvolumes of the system disk share its device.
        const bool block = m.device.startsWith("/dev/") && !m.device.startsWith("/dev/loop")
                           && !m.device.startsWith("/dev/zram");
        if (system || !block || pseudo.contains(m.type) || m.device == systemDevice || seen.contains(m.device))
            continue;
        seen << m.device;
        const QString name = m.label.isEmpty() ? QFileInfo(m.root).fileName() : m.label;
        // A drive that can be ejected is one to pull out afterwards; only
        // eject makes sure it can be (the stick powered down, the disc out).
        const QString release = m.canEject ? "eject" : m.canUnmount ? "unmount" : "";
        drives << QVariantMap{{"name", name.isEmpty() ? m.root : name}, {"path", m.root}, {"icon", "drive"},
                              {"release", release}, {"free", m.free}};
    }
    return drives;
}

QVariantList Places::devicesFrom(const QList<Drives::Volume> &volumes, const QVariantList &drives) {
    QVariantList devices;
    QStringList listed;
    for (const Drives::Volume &v : volumes) {
        if (v.root.isEmpty()) {
            devices << QVariantMap{{"name", v.name}, {"path", QString()}, {"icon", "drive"}, {"volume", v.id},
                                   {"release", QString()}, {"free", -1}};
            continue;
        }
        for (const QVariant &d : drives) {
            QVariantMap drive = d.toMap();
            if (drive.value("path").toString() != v.root)
                continue;
            drive.insert("volume", v.id);
            devices << drive;
            listed << v.root;
            break;
        }
    }
    for (const QVariant &drive : drives)
        if (!listed.contains(drive.toMap().value("path").toString()))
            devices << drive;
    return devices;
}

QList<Places::Mount> Places::readMounts() {
    QList<Mount> mounts;
    auto volumes = QStorageInfo::mountedVolumes();
    // Shortest path first, so a drive mounted twice is listed at its top.
    std::sort(volumes.begin(), volumes.end(),
              [](const QStorageInfo &a, const QStorageInfo &b) { return a.rootPath().size() < b.rootPath().size(); });
    for (const QStorageInfo &v : volumes) {
        if (!v.isValid() || !v.isReady())
            continue;
        Mount mount{v.rootPath(), QString::fromUtf8(v.device()), QString::fromUtf8(v.fileSystemType()), v.name()};
        // A read-only medium (an installer image, a DVD) has nothing free to
        // speak of; it says nothing rather than "0 B free".
        mount.free = v.isReadOnly() ? -1 : v.bytesAvailable();
        mounts << mount;
    }
    return mounts;
}

void Places::refreshDevices() {
    // One reading at a time: a filesystem that hangs holds up only the
    // next reading, never a pile of them, and never the window.
    if (m_mounts.isRunning()) {
        m_rereadMounts = true;
        return;
    }
    // In a pool that's never torn down: Qt's own waits for its work on the
    // way out, and a filesystem that never answers would then keep Folio
    // from quitting.
    static QThreadPool *const pool = new QThreadPool;
    m_mounts.setFuture(QtConcurrent::run(pool, &Places::readMounts));
}

void Places::mountsRead() {
    QList<Mount> mounts = m_mounts.result();
    // What GIO can do with each drive is asked here, on the UI thread, where
    // GIO's volume monitor lives; it already knows, so asking is quick.
    for (Mount &mount : mounts)
        if (GMount *gmount = mount.device.startsWith("/dev/") ? mountAt(mount.root) : nullptr) {
            mount.canEject = g_mount_can_eject(gmount);
            mount.canUnmount = g_mount_can_unmount(gmount);
            g_object_unref(gmount);
        }
    const QVariantList physical = physicalDrives(mounts, m_systemDevice);
    const QVariantList devices = m_drives ? devicesFrom(m_drives->volumes(), physical) : physical;
    if (devices != m_devices) {
        m_devices = devices;
        emit changed();
    }
    if (std::exchange(m_rereadMounts, false))
        refreshDevices();
}

void Places::leave(const QString &root) {
    emit releasing(root);
    // The process's own working directory follows the folder shown, and
    // would hold the drive too.
    if (const QString here = QDir::currentPath(); here == root || here.startsWith(root + "/"))
        QDir::setCurrent(m_home);
}

void Places::eject(const QString &path) {
    QVariantMap drive;
    for (const QVariant &d : m_devices)
        if (d.toMap().value("path").toString() == path)
            drive = d.toMap();
    const QString release = drive.value("release").toString();
    GMount *mount = release.isEmpty() ? nullptr : mountAt(path);
    if (!mount)
        return;
    leave(path);
    // Without a mount operation, a drive in use is reported as busy rather
    // than put to a question nobody here would answer, which GIO takes as a
    // cancel. Qt runs on GLib's main loop, so the answer comes back on the
    // UI thread.
    auto *context = new Release{this, drive.value("name").toString(), release == "eject"};
    if (context->eject)
        g_mount_eject_with_operation(mount, G_MOUNT_UNMOUNT_NONE, nullptr, nullptr, released, context);
    else
        g_mount_unmount_with_operation(mount, G_MOUNT_UNMOUNT_NONE, nullptr, nullptr, released, context);
    g_object_unref(mount);
}
