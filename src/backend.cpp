#include "backend.h"
#include "fileops.h"
#include "previewer.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QThreadPool>
#include <QGuiApplication>
#include <QImageReader>
#include <QMimeDatabase>
#include <QLockFile>
#include <QPointer>
#include <QSettings>
#include <QStorageInfo>
#include <QUrl>
#include <QtConcurrent>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

Backend::Backend(const QString &home, QObject *parent)
    : QObject(parent), m_home(home.isEmpty() ? QDir::homePath() : home) {
    m_noticeTimer.setSingleShot(true);
    m_noticeTimer.setInterval(4000);
    connect(&m_noticeTimer, &QTimer::timeout, this, [this] {
        m_notice.clear();
        emit noticeChanged();
    });
    connect(&m_mounter, &Mounter::mounted, this, [this](const QString &folder) {
        setBusy({});
        // The mount's folder is known now, so the plugin's row can light up.
        if (m_plugins)
            m_plugins->mountsChanged();
        arrive(folder);
    });
    connect(&m_mounter, &Mounter::failed, this, [this](const QString &error) {
        setBusy({});
        // An empty error is the user cancelling the sign-in: nothing to report.
        if (!error.isEmpty())
            setMessage(tr("Couldn't connect to %1: %2").arg(m_mountName, error));
    });
    // The preview reports each file it steps to, and the grid follows.
    connect(&m_preview, &QProcess::readyReadStandardOutput, this, [this] {
        while (m_preview.canReadLine())
            previewSaid(QString::fromUtf8(m_preview.readLine()).trimmed());
    });
    connect(&m_preview, &QProcess::stateChanged, this, &Backend::previewingChanged);
    connect(&m_preview, &QProcess::finished, this, [this](int, QProcess::ExitStatus status) {
        // A decoder that crashed took down the preview, not Folio; say so
        // rather than letting the window just vanish. Closing it from here
        // ends it with SIGTERM, which QProcess also reports as a crash.
        if (status == QProcess::CrashExit && !m_closingPreview)
            setMessage(tr("The preview closed unexpectedly on %1.").arg(QFileInfo(m_previewed).fileName()));
    });
}

QString Backend::gvfsRoot() {
    return Mounter::root();
}

// What a gvfs mount is called on screen: the host or server from its
// folder's name ("sftp:host=nas,user=me", "smb-share:server=nas,...").
QString Backend::mountLabel(const QString &mountFolder) {
    QString host = mountFolder;
    for (const QString &field : mountFolder.section(':', 1).split(','))
        if (field.startsWith("host=") || field.startsWith("server="))
            host = field.section('=', 1);
    return host;
}

QString Backend::mountName(const QString &mount) const {
    const QString name = m_plugins ? m_plugins->nameForMount(mount) : QString();
    return name.isEmpty() ? mountLabel(QFileInfo(mount).fileName()) : name;
}

// Where the removable or extra drive holding `path` is mounted, when it's one
// (under /run/media, /media or /mnt), so its path starts at its name rather
// than at /run/media/<user>/.
static QString driveRoot(const QString &path) {
    static const QStringList places{"/run/media/", "/media/", "/mnt/"};
    for (const QString &place : places) {
        if (!path.startsWith(place))
            continue;
        const QString root = QStorageInfo(path).rootPath();
        if (root.startsWith(place) && (path == root || path.startsWith(root + "/")))
            return root;
    }
    return {};
}

QString Backend::title() const {
    const QString p = path();
    if (p == FolderModel::recentPath())
        return tr("Recents");
    if (FolderModel::isPluginView(p))
        return m_plugins ? m_plugins->views().value(p).toMap().value("title").toString() : p;
    if (p == trashPath())
        return tr("Trash");
    if (p == m_home)
        return tr("Home");
    if (p == "/")
        return tr("Computer");
    if (QFileInfo(p).absolutePath() == gvfsRoot())
        return mountName(p);
    if (driveRoot(p) == p)
        return driveName(p);
    return QFileInfo(p).fileName();
}

QVariantList Backend::crumbs() const {
    // Breadcrumbs start at the nearest root that means something to a person:
    // ~ for home, another machine's name for its mount, a drive's name for
    // a drive, / otherwise.
    const QString p = path();
    if (p == FolderModel::recentPath())
        return {QVariantMap{{"name", tr("Recents")}, {"path", p}}};
    if (FolderModel::isPluginView(p))
        return {QVariantMap{{"name", title()}, {"path", p}}};
    // The trash's files folder is an implementation detail; it's "Trash".
    if (p == trashPath())
        return {QVariantMap{{"name", tr("Trash")}, {"path", p}}};
    QString base = "/";
    QString label = "/";
    if (p == m_home || p.startsWith(m_home + "/")) {
        base = m_home;
        label = "~";
    } else if (p.startsWith(gvfsRoot() + "/")) {
        const QString mount = p.section('/', 0, 5);
        base = mount;
        label = mountName(mount);
    } else if (const QString drive = driveRoot(p); !drive.isEmpty()) {
        base = drive;
        label = driveName(drive);
    }

    QVariantList crumbs{QVariantMap{{"name", label}, {"path", base}}};
    const QString rest = p.mid(base.length());
    QString walk = base;
    for (const QString &part : rest.split('/', Qt::SkipEmptyParts)) {
        walk = walk.endsWith('/') ? walk + part : walk + "/" + part;
        crumbs << QVariantMap{{"name", part}, {"path", walk}};
    }
    return crumbs;
}

QString Backend::s_settingsFile;

namespace {
QSettings sortSettings(const QString &file) {
    return file.isEmpty() ? QSettings() : QSettings(file, QSettings::IniFormat);
}

// The folders with an order of their own, by path.
QHash<QString, QString> readSorts(QSettings &settings) {
    QHash<QString, QString> sorts;
    const int count = settings.beginReadArray("sorts");
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        sorts.insert(settings.value("path").toString(), settings.value("sort").toString());
    }
    settings.endArray();
    return sorts;
}

// What `path` takes from the folders above it. Views (Recents, plugins')
// have nothing above them.
QString inherited(const QHash<QString, QString> &sorts, QString path) {
    if (FolderModel::isView(path) || !path.startsWith('/'))
        return "name";
    while (path != "/") {
        path = QFileInfo(path).path();
        if (sorts.contains(path))
            return sorts.value(path);
    }
    return "name";
}
// Cached per settings file and its on-disk stat, so sortFor() doesn't
// reopen and parse folio.conf on every column or navigation while still
// picking up changes made by another Folio process (such as the chooser).
struct SortsCache {
    QString file;
    bool existed = false;
    qint64 mtimeSec = 0;
    qint64 mtimeNsec = 0;
    qint64 size = 0;
    QHash<QString, QString> sorts;
};

SortsCache &sortsCache() {
    static SortsCache cache;
    return cache;
}

const QHash<QString, QString> &cachedSorts(const QString &file) {
    static const QString defaultPath = QSettings().fileName();
    const QString path = file.isEmpty() ? defaultPath : file;
    struct stat st{};
    const bool exists = ::stat(QFile::encodeName(path).constData(), &st) == 0;
    auto &cache = sortsCache();
    if (cache.file == path && cache.existed == exists
        && (!exists || (cache.mtimeSec == st.st_mtim.tv_sec && cache.mtimeNsec == st.st_mtim.tv_nsec
                        && cache.size == st.st_size)))
        return cache.sorts;
    cache.file = path;
    cache.existed = exists;
    cache.mtimeSec = exists ? st.st_mtim.tv_sec : 0;
    cache.mtimeNsec = exists ? st.st_mtim.tv_nsec : 0;
    cache.size = exists ? st.st_size : 0;
    if (exists) {
        QSettings settings = sortSettings(file);
        cache.sorts = readSorts(settings);
    } else {
        cache.sorts.clear();
    }
    return cache.sorts;
}
}

QString Backend::sortFor(const QString &path) {
    const auto &sorts = cachedSorts(s_settingsFile);
    return sorts.contains(path) ? sorts.value(path) : inherited(sorts, path);
}

void Backend::rememberSort(const QString &path, const QString &sort) {
    QSettings settings = sortSettings(s_settingsFile);
    // The list is read and rewritten whole: another Folio (a file chooser,
    // say) doing the same at once would otherwise undo this one's choice.
    QLockFile lock(settings.fileName() + ".sorts.lock");
    if (!lock.lock())
        return;
    settings.sync();
    auto sorts = readSorts(settings);
    // An order the folder would take anyway is left to its parent, so
    // changing the parent's later changes it too.
    if (sort == inherited(sorts, path))
        sorts.remove(path);
    else
        sorts.insert(path, sort);
    QStringList paths = sorts.keys();
    paths.sort();
    // Rewritten whole, so a shorter list leaves none of the old entries.
    settings.remove("sorts");
    settings.beginWriteArray("sorts", paths.size());
    for (int i = 0; i < paths.size(); ++i) {
        settings.setArrayIndex(i);
        settings.setValue("path", paths[i]);
        settings.setValue("sort", sorts.value(paths[i]));
    }
    settings.endArray();
    settings.sync();
    // Update the in-memory cache to match what we just wrote.
    const QString filePath = settings.fileName();
    struct stat st{};
    const bool exists = ::stat(QFile::encodeName(filePath).constData(), &st) == 0;
    auto &cache = sortsCache();
    cache.file = filePath;
    cache.existed = exists;
    cache.mtimeSec = exists ? st.st_mtim.tv_sec : 0;
    cache.mtimeNsec = exists ? st.st_mtim.tv_nsec : 0;
    cache.size = exists ? st.st_size : 0;
    cache.sorts = std::move(sorts);
}

void Backend::chooseSort(const QString &sort) {
    if (!FolderModel::sortModes().contains(sort))
        return;
    m_folder.setSortMode(sort);
    if (!path().isEmpty())
        rememberSort(path(), sort);
    ++m_sortsRevision;
    emit sortsChanged();
}

void Backend::navigate(const QString &path) {
    go(path, true);
}

bool Backend::go(const QString &path, bool record) {
    const QString clean = FolderModel::isView(path) ? path : QDir::cleanPath(path);
    // The trash folder only exists once something has been trashed; the
    // spec lets anyone create it, so Trash always opens.
    if (clean == trashPath())
        QDir().mkpath(clean);
    // A view is there only while its plugin is.
    if (FolderModel::isPluginView(clean) && !(m_plugins && m_plugins->hasView(clean))) {
        setMessage(tr("%1 doesn't exist.").arg(clean));
        return false;
    }
    // A relative path (from a plugin) would be taken from wherever Folio
    // was started, which means nothing here.
    if (!FolderModel::isView(clean) && (QDir::isRelativePath(clean) || !QFileInfo(clean).isDir())) {
        setMessage(tr("%1 isn't a folder.").arg(clean));
        return false;
    }
    if (record) {
        if (m_index >= 0 && m_history.value(m_index) == clean)
            return true;
        m_history = m_history.mid(0, m_index + 1);
        m_history << clean;
        m_index = m_history.size() - 1;
    }
    m_folder.setPath(clean, sortFor(clean));
    if (s_working == this)
        moveWorkingDirectory();
    emit pathChanged();
    emit placeChanged();
    return true;
}

void Backend::keepWorkingDirectory() {
    s_working = this;
    moveWorkingDirectory();
}

void Backend::moveWorkingDirectory() {
    const QString p = path();
    if (!FolderModel::isView(p) && !p.startsWith(gvfsRoot() + "/"))
        QDir::setCurrent(p);
}

void Backend::open(const QString &path) {
    if (path.startsWith("plugin:") && !FolderModel::isView(path)) {
        openItem(path);
        return;
    }
    const QFileInfo info(path);
    if (FolderModel::isView(path) || info.isDir()) {
        navigate(path);
        return;
    }
    if (m_choosing) {
        emit chosen(path);
        return;
    }
    // The desktop's default app for the type, or for the nearest type it
    // belongs to.
    if (const QString problem = FileOps::openInDefaultApp(path); !problem.isEmpty())
        setMessage(problem);
}

void Backend::goUp() {
    // Recents, plugins' views and the trash are places of their own:
    // nothing above them.
    if (FolderModel::isView(path()) || path() == trashPath())
        return;
    const QString parent = QFileInfo(path()).absolutePath();
    if (parent == path())
        return;
    // Arriving back in the parent keeps the folder we came from selected, so
    // going up and down again is one keystroke each way.
    m_pendingSelection = QFileInfo(path()).fileName();
    m_leftBehind = true;
    navigate(parent);
}

// A folder in the history that's gone (deleted, or on a machine that went
// away) is said in the window, and the history stays on the folder shown.
void Backend::back() {
    if (!canBack())
        return;
    const QString pending = m_pendingSelection;
    const bool leftBehind = m_leftBehind;
    m_pendingSelection = QFileInfo(path()).fileName();
    m_leftBehind = true;
    --m_index;
    if (!go(m_history.at(m_index), false)) {
        ++m_index;
        m_pendingSelection = pending;
        m_leftBehind = leftBehind;
    }
}

void Backend::forward() {
    if (!canForward())
        return;
    ++m_index;
    if (!go(m_history.at(m_index), false))
        --m_index;
}

QString Backend::driveName(const QString &root) const {
    if (m_places)
        for (const QVariant &device : m_places->devices())
            if (const QVariantMap drive = device.toMap(); drive.value("path").toString() == root)
                return drive.value("name").toString();
    return QFileInfo(root).fileName();
}

void Backend::setPlaces(Places *places) {
    m_places = places;
    // A drive renamed or newly listed renames the title and first crumb.
    connect(places, &Places::changed, this, &Backend::placeChanged);
}

void Backend::setPlugins(Plugins *plugins) {
    m_plugins = plugins;
    // A view's title and a mount's name come from the plugins.
    connect(plugins, &Plugins::changed, this, &Backend::placeChanged);
    connect(plugins, &Plugins::detailsChanged, this, [this] { showPluginRows(askPlugins()); });
}

void Backend::openItem(const QString &path) {
    const auto item = m_plugins ? m_plugins->item(path) : std::nullopt;
    if (!item || m_mounter.busy())
        return;
    // Remembered as it's asked for, not once it's open: a machine that
    // didn't open (asleep) is still the one to try again, and the sidebar
    // only shows it while its plugin lists it.
    m_plugins->remember(path);
    const QJsonObject open = item->open;
    if (open.contains("path")) {
        navigate(Plugins::expand(open.value("path").toString()));
    } else if (open.contains("mount")) {
        mount(open.value("mount").toString(), item->name, open.value("verified").toBool());
    } else if (open.contains("run")) {
        const int run = ++m_lastRun;
        QPointer<Backend> self(this);
        m_plugins->run(path, [self, run](const Plugins::Result &result) {
            if (self)
                self->finish(run, result);
        });
        // It never finishes before this returns, so its busy text is up first.
        m_runs.append({run, tr("Opening %1…").arg(item->name)});
        emit busyChanged();
    }
}

void Backend::mount(const QString &url, const QString &name, bool verified) {
    m_mountBare = QUrl(url).path().isEmpty() || QUrl(url).path() == "/";
    m_mountName = name;
    // Mounted already (by Folio or another app): straight there.
    const QString folder = Mounter::destinationFor(url);
    if (!folder.isEmpty() && QFileInfo::exists(folder)) {
        arrive(folder);
        return;
    }
    setBusy(tr("Connecting to %1…").arg(name));
    m_mounter.mount(url, verified);
}

void Backend::arrive(const QString &folder) {
    // A bare server lands in the matching home folder when the other
    // machine has one, which among one person's machines it usually does.
    const QString remoteHome = folder + QDir::homePath();
    const QFileInfo target(m_mountBare && QFileInfo(remoteHome).isDir() ? remoteHome : folder);
    // An address can name a file, which opens its folder with it selected.
    if (target.isDir() || !target.exists())
        navigate(target.filePath());
    else
        showItem(target.filePath());
}

QString Backend::serverAddress(const QString &text, QString *problem) {
    if (problem)
        problem->clear();
    const QString raw = text.trimmed();
    static const QStringList schemes{"sftp", "smb", "nfs", "ftp", "ftps", "dav", "davs"};
    // Tolerant, so a path typed with spaces in it is escaped rather than
    // refused; QUrl puts the scheme in lower case. A Windows UNC path has no
    // query or fragment, so its folder names are set decoded.
    QUrl url;
    if (raw.startsWith("\\\\")) {
        const QString unc = raw.mid(2).replace('\\', '/');
        url.setScheme("smb");
        url.setHost(unc.section('/', 0, 0));
        const QString rest = unc.section('/', 1);
        url.setPath(rest.isEmpty() ? "/" : "/" + rest, QUrl::DecodedMode);
    } else {
        url = QUrl(raw, QUrl::TolerantMode);
    }
    if (url.scheme() == "ssh")
        url.setScheme("sftp");
    else if (url.scheme() == "cifs")
        url.setScheme("smb");
    else if (url.scheme() == "webdav")
        url.setScheme("dav");
    else if (url.scheme() == "webdavs")
        url.setScheme("davs");
    if (!url.isValid() || !schemes.contains(url.scheme()) || url.host().isEmpty())
        return {};
    if (!url.password().isEmpty()) {
        if (problem)
            *problem = tr("Leave the password out of the address; enter it when asked to sign in.");
        return {};
    }
    // A Windows host on its own is fine (its shares are offered to pick
    // from), but an NFS server has no such list.
    if (url.scheme() == "nfs" && url.path().section('/', 0, 0, QString::SectionSkipEmpty).isEmpty()) {
        if (problem)
            *problem = tr("Add the exported folder, as in nfs://%1/export.").arg(url.host());
        return {};
    }
    if (url.path().isEmpty())
        url.setPath("/");
    return url.toString(QUrl::FullyEncoded);
}

void Backend::perform(const QString &plugin, const QString &action, const QStringList &paths, const QString &target) {
    if (!m_plugins)
        return;
    const int run = ++m_lastRun;
    QPointer<Backend> self(this);
    const QString busy = m_plugins->perform(plugin, action, paths, target, [self, run](const Plugins::Result &result) {
        if (self)
            self->finish(run, result);
    });
    // It never finishes before this returns, so its busy text is up first.
    if (!busy.isEmpty()) {
        m_runs.append({run, busy});
        emit busyChanged();
    }
}

void Backend::finish(int run, const Plugins::Result &result) {
    if (m_runs.removeIf([run](const auto &r) { return r.first == run; }) > 0)
        emit busyChanged();
    if (!result.error.isEmpty())
        setMessage(result.error);
    if (!result.copy.isEmpty())
        QGuiApplication::clipboard()->setText(result.copy);
    if (!result.notice.isEmpty())
        showNotice(result.notice);
    if (!result.path.isEmpty())
        navigate(Plugins::expand(result.path));
}

void Backend::showNotice(const QString &notice) {
    m_notice = notice;
    emit noticeChanged();
    m_noticeTimer.start();
}

void Backend::dismissMessage() {
    setMessage({});
}

QString Backend::takeSelection() {
    const QString name = m_pendingSelection;
    m_pendingSelection.clear();
    return name;
}

// As ls writes them: read, write and run for the owner, the group and
// everyone, with the setuid, setgid and sticky bits in the run places.
static QString permissionsText(mode_t mode) {
    const auto run = [mode](mode_t x, mode_t special, char set, char setOnly) {
        return mode & special ? (mode & x ? set : setOnly) : (mode & x ? 'x' : '-');
    };
    const char text[] = {mode & S_IRUSR ? 'r' : '-', mode & S_IWUSR ? 'w' : '-', run(S_IXUSR, S_ISUID, 's', 'S'),
                         mode & S_IRGRP ? 'r' : '-', mode & S_IWGRP ? 'w' : '-', run(S_IXGRP, S_ISGID, 's', 'S'),
                         mode & S_IROTH ? 'r' : '-', mode & S_IWOTH ? 'w' : '-', run(S_IXOTH, S_ISVTX, 't', 'T'), 0};
    return QString::fromLatin1(text);
}

void Backend::setDescribing(bool describing) {
    if (describing == m_describingNow)
        return;
    m_describingNow = describing;
    emit describingChanged();
}

void Backend::describe(const QVariantMap &item, bool inTrash) {
    const QString path = item.value("path").toString();
    const QString kind = item.value("kind").toString();
    const QString mime = item.value("mime").toString();
    const QDateTime modified = item.value("modified").toDateTime();
    const qint64 size = item.value("size").toLongLong();
    const quint64 generation = ++*m_describing;
    const QString cacheKey = path.startsWith('/') && modified.isValid()
                                 ? path + '\n' + mime + '\n' + (inTrash ? QLatin1Char('1') : QLatin1Char('0')) + '\n'
                                       + QString::number(modified.toMSecsSinceEpoch()) + '\n' + QString::number(size)
                                 : QString();
    const QVariantMap *cached = !cacheKey.isEmpty() ? m_detailsCache.object(cacheKey) : nullptr;
    // The same item again (its folder re-read) keeps what it shows until
    // the new reading is in, rather than flickering. A recently described
    // item shows its cached metadata immediately while refreshing quietly.
    QVariantMap details;
    if (const QString described = path + '\n' + mime; described != m_described) {
        m_described = described;
        m_pluginRows.clear();
        if (cached) {
            m_details = *cached;
            setDescribing(false);
        } else {
            m_details = details;
            setDescribing(path.startsWith('/'));
        }
        emit detailsChanged();
    } else if (!path.startsWith('/')) {
        setDescribing(false);
    }
    // Plugins say things about real files and folders, not trashed ones or
    // a plugin's own items.
    m_pluginItem = path.startsWith('/') && !inTrash ? item : QVariantMap();
    showPluginRows(askPlugins());
    // Every real file and folder has a creation time to read; a plugin's
    // item has none.
    if (!path.startsWith('/'))
        return;

    // Reading the file waits on its disk, which for a slow mount would hold
    // up moving through files, so it happens off the UI thread.
    auto *watcher = new QFutureWatcher<QVariantMap>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, details, path, mime, cacheKey]() mutable {
        watcher->deleteLater();
        if (generation != *m_describing)
            return; // The selection moved on while this was read.
        details.insert(watcher->result());
        const bool changed = details != m_details;
        m_details = details;
        if (!cacheKey.isEmpty())
            m_detailsCache.insert(cacheKey, new QVariantMap(m_details));
        setDescribing(false);
        if (changed)
            emit detailsChanged();
        // An archive's names, by the type read from what it holds when its
        // name gave none. bsdtar can take seconds, so listings take a
        // thread of their own, and the rest of what's read never waits on one.
        if (!Previewer::listsArchive(details.value("mime", mime).toString()))
            return;
        static QThreadPool *const listing = [] {
            auto *pool = new QThreadPool;
            pool->setMaxThreadCount(1);
            return pool;
        }();
        auto *names = new QFutureWatcher<QString>(this);
        connect(names, &QFutureWatcherBase::finished, this, [this, names, generation, cacheKey] {
            names->deleteLater();
            if (generation != *m_describing)
                return;
            m_details.insert("text", names->result());
            if (!cacheKey.isEmpty())
                m_detailsCache.insert(cacheKey, new QVariantMap(m_details));
            emit detailsChanged();
        });
        names->setFuture(QtConcurrent::run(listing, [path, latest = m_describing, generation] {
            return *latest == generation ? Previewer::archiveNames(path) : QString();
        }));
    });
    // Stepping through files faster than they're read queues readings; the
    // ones overtaken before they start are skipped rather than left to tie
    // up the threads the next folder is listed on.
    // Every local item is read now (its permissions at least), so the
    // readings get a few threads of their own: moving across folders on a
    // mount that stopped answering leaves reads stuck, and in the shared
    // pool they'd keep any folder from being listed. Never torn down, so a
    // stuck one can't keep Folio from quitting either. A file its extension
    // could not name has its type read from what it holds.
    static QThreadPool *const reading = [] {
        auto *pool = new QThreadPool;
        pool->setMaxThreadCount(4);
        return pool;
    }();
    const bool unnamed = mime == "application/octet-stream";
    watcher->setFuture(QtConcurrent::run(reading, [path, kind, mime, unnamed, inTrash, latest = m_describing, generation] {
        QVariantMap read;
        if (*latest != generation)
            return read;
        const QByteArray encoded = QFile::encodeName(path);
        struct statx stx{};
        if (::statx(AT_FDCWD, encoded.constData(), 0, STATX_MODE | (inTrash ? 0u : STATX_BTIME), &stx) == 0) {
            if (stx.stx_mask & STATX_MODE)
                read.insert("permissions", permissionsText(stx.stx_mode));
            if (!inTrash && (stx.stx_mask & STATX_BTIME))
                read.insert("created", QDateTime::fromMSecsSinceEpoch(stx.stx_btime.tv_sec * 1000LL
                                                                      + stx.stx_btime.tv_nsec / 1000000LL));
        }
        if (inTrash) {
            read.insert("origin", FileOps::originalPath(path));
            read.insert("deleted", FileOps::deletedAt(path));
        }
        // The type read is this file's alone: the pane believes it only for the path it was read from.
        if (unnamed) {
            read.insert("path", path);
            read.insert("mime", FolderModel::typeByContent(path, mime));
        }
        if (kind == "image") {
            QImageReader reader(path);
            reader.setAutoTransform(true);
            // A decoder that throws (LibRaw, #164) leaves the size unknown,
            // rather than throwing again where the result is read, on the
            // window's thread.
            QSize size;
            try {
                size = reader.size();
            } catch (...) {
            }
            // Numbers, not text: the pane writes them in the language's
            // digits, and again when the language changes.
            if (size.isValid()) {
                read.insert("width", size.width());
                read.insert("height", size.height());
            }
        } else if (kind == "text") {
            // Enough of the top of the file to recognize it, never the whole
            // thing: a log can be gigabytes.
            read.insert("text", Previewer::readText(path, 6000).section('\n', 0, 40));
        }
        return read;
    }));
}

QVariantMap Backend::details() const {
    QVariantMap details = m_details;
    if (!m_pluginRows.isEmpty())
        details.insert("plugins", m_pluginRows);
    return details;
}

void Backend::showPluginRows(bool keep) {
    const QVariantList rows = m_plugins && !m_pluginItem.isEmpty()
                                  ? m_plugins->details(m_pluginItem.value("path").toString(),
                                                       m_pluginItem.value("modified").toDateTime())
                                  : QVariantList();
    if (rows == m_pluginRows || (keep && rows.isEmpty()))
        return;
    m_pluginRows = rows;
    emit detailsChanged();
}

bool Backend::askPlugins() {
    if (!m_plugins || m_pluginItem.isEmpty())
        return false;
    // Answers for an item the selection has since moved on from are
    // dropped, and its commands not yet started aren't run.
    const quint64 generation = *m_describing;
    QPointer<Backend> self(this);
    return m_plugins->describe(
        m_pluginItem.value("path").toString(), m_pluginItem.value("mime").toString(),
        m_pluginItem.value("modified").toDateTime(), [latest = m_describing, generation] { return *latest == generation; },
        [self, generation] {
            if (self && *self->m_describing == generation)
                self->showPluginRows(false);
        });
}

QString Backend::typeName(const QString &mime) {
    static QString cachedLocale;
    static QHash<QString, QString> cache;
    const QString locale = QLocale().name();
    if (locale != cachedLocale) {
        cachedLocale = locale;
        cache.clear();
    }
    if (const auto it = cache.constFind(mime); it != cache.cend())
        return *it;
    const QString comment = QMimeDatabase().mimeTypeForName(mime).comment();
    cache.insert(mime, comment);
    return comment;
}

Backend::~Backend() {
    if (s_working == this)
        s_working = nullptr;
    // The preview follows this window; it closes with it.
    if (previewing()) {
        m_preview.terminate();
        m_preview.waitForFinished(1000);
    }
}

QString Backend::trashPath() const {
    return QDir::cleanPath(FileOps::homeTrash() + "/files");
}

QString Backend::typedPath() const {
    const QString p = path();
    if (p == m_home)
        return "~/";
    if (p.startsWith(m_home + "/"))
        return "~" + p.mid(m_home.size()) + "/";
    return FolderModel::isView(p) || p == "/" ? p : p + "/";
}

QString Backend::expandTyped(const QString &text, const QString &home, const QString &current) {
    QString t = text.trimmed();
    if (t.startsWith("file://"))
        t = QUrl(t).toLocalFile();
    if (t == "~")
        return home;
    if (t.startsWith("~/"))
        return home + t.mid(1);
    // A relative path is taken from the folder being shown, or from home in
    // Recents, which isn't a folder.
    if (!t.startsWith('/'))
        return QDir(QFileInfo(current).isDir() ? current : home).filePath(t);
    return t;
}

bool Backend::goTyped(const QString &text) {
    // A server is mounted through GVfs, which also says where on the mount
    // the address's path is (a WebDAV address's path can be part of the
    // mount itself), and connecting goes there.
    QString problem;
    if (const QString address = serverAddress(text, &problem); !address.isEmpty()) {
        if (!m_mounter.busy())
            mount(address, QUrl(address).host(), false);
        return true;
    }
    if (!problem.isEmpty()) {
        setMessage(problem);
        return false;
    }
    // A view is typed as the path bar shows it, by its own name.
    if (const QString view = text.trimmed(); FolderModel::isView(view))
        return go(view, true);
    const QString target = QDir::cleanPath(expandTyped(text, m_home, path()));
    const QFileInfo info(target);
    if (info.isDir()) {
        navigate(target);
        return true;
    }
    if (info.exists()) {
        showItem(target);
        return true;
    }
    setMessage(tr("%1 doesn't exist.").arg(text.trimmed()));
    return false;
}

QString Backend::complete(const QString &text, const QString &home, const QString &current) {
    // Completes the last part to the folder names it starts, as far as
    // they agree, keeping how the user typed the front (~, relative).
    const int slash = text.lastIndexOf('/');
    const QString head = slash >= 0 ? text.left(slash + 1) : QString();
    const QString part = text.mid(slash + 1);
    const QString folder = expandTyped(head.isEmpty() ? "." : head, home, current);
    QStringList matches;
    const auto names = QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);
    for (const QString &name : names)
        if (name.startsWith(part) && (part.startsWith('.') || !name.startsWith('.')))
            matches << name;
    if (matches.isEmpty())
        return text;
    if (matches.size() == 1)
        return head + matches.first() + "/";
    QString common = matches.first();
    for (const QString &m : matches)
        while (!m.startsWith(common))
            common.chop(1);
    return head + common;
}

QString Backend::completePath(const QString &text) const {
    return complete(text, m_home, path());
}

void Backend::newWindow() {
    if (m_choosing)
        return;
    emit newWindowRequested(FolderModel::isView(path()) ? m_home : path());
}

void Backend::togglePreview(const QString &path) {
    if (previewing()) {
        m_closingPreview = true;
        m_preview.terminate();
        return;
    }
    if (path.isEmpty())
        return;
    QStringList args{"preview", "--follow", "--sort", m_folder.sortMode()};
    if (m_folder.showHidden())
        args << "--hidden";
    args << path;
    m_previewed = path;
    m_preview.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    m_closingPreview = false;
    m_preview.start(QCoreApplication::applicationFilePath(), args);
}

void Backend::followSelection(const QString &path) {
    if (!previewing() || path.isEmpty() || path == m_previewed)
        return;
    m_previewed = path;
    m_preview.write(path.toUtf8() + '\n');
}

void Backend::showFolder(const QString &path) {
    navigate(path);
    emit activateRequested();
}

void Backend::showItem(const QString &path) {
    const QFileInfo info(path);
    m_pendingSelection = info.fileName();
    m_leftBehind = false;
    const QString parent = info.absolutePath();
    if (parent == this->path()) {
        if (!m_folder.loading() && m_folder.indexOf(m_pendingSelection) >= 0) {
            emit selectRequested(m_pendingSelection);
            m_pendingSelection.clear();
        } else if (!m_folder.loading()) {
            m_folder.reload();
        }
    } else {
        navigate(parent);
    }
    emit activateRequested();
}

void Backend::setMessage(const QString &message) {
    if (message == m_message)
        return;
    m_message = message;
    emit messageChanged();
}

void Backend::setBusy(const QString &busy) {
    if (busy == m_busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void Backend::previewSaid(const QString &shown) {
    if (shown.startsWith("step ")) {
        // The view moves and sends its new item right away (or nothing, at
        // the first or last row); "stepped" after it lets the preview take
        // the arrows that came meanwhile.
        emit stepRequested(shown.mid(5).toInt() < 0 ? -1 : 1);
        if (previewing())
            m_preview.write("stepped\n");
        return;
    }
    m_previewed = shown;
    // Any file in the folder shown or below it: one in a folder opened in
    // place (the list's tree) is a row too, and the cursor goes to it, so
    // ↑ and ↓ step from there.
    const QString here = path().endsWith('/') ? path() : path() + '/';
    if (shown.startsWith(here))
        emit selectPathRequested(shown);
}
