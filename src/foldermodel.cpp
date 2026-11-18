#include "foldermodel.h"
#include "fileops.h"

#include <QCache>
#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QXmlStreamReader>
#include <QUrl>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMimeDatabase>
#include <QtConcurrent>
#include <algorithm>
#include <tuple>
#include <sys/stat.h>

FolderModel::FolderModel(QObject *parent) : QAbstractListModel(parent) {
    // Copies, saves and downloads change a folder in bursts; wait for the
    // burst to settle instead of relisting on every event.
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(250);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    // Recents' file is rewritten (replaced) by GTK, so it's watched again
    // after each change.
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this](const QString &file) {
        m_debounce.start();
        if (!m_watcher.files().contains(file) && QFileInfo::exists(file))
            m_watcher.addPath(file);
    });
    connect(&m_debounce, &QTimer::timeout, this, &FolderModel::reload);
    m_staleClear.setSingleShot(true);
    m_staleClear.setInterval(150);
    connect(&m_staleClear, &QTimer::timeout, this, &FolderModel::clearEntries);
    connect(this, &FolderModel::pathChanged, this, &FolderModel::expandableChanged);
    connect(this, &FolderModel::queryChanged, this, &FolderModel::expandableChanged);
}

// Generations start at 1, so 0 stops any search or folder count still
// running for this model.
FolderModel::~FolderModel() {
    *m_latest = 0;
}

// Never deleted, like Places', so a dead mount can't stop Folio quitting: a listing
// gets a second to finish, and one stuck longer may still meet destroyed statics.
QThreadPool &FolderModel::listingPool() {
    static QThreadPool *const pool = [] {
        qAddPostRoutine([] { listingPool().waitForDone(1000); });
        auto *p = new QThreadPool;
        // Eight, not the CPU count, so a few folders stuck on dead mounts leave others listing.
        p->setMaxThreadCount(8);
        return p;
    }();
    return *pool;
}

// Recent listings, shared by every model (the window's folder and each
// column), so stepping back and forth or opening a column for a folder just
// seen shows it in the same frame instead of an old or empty one. Each is
// re-read in the background and only replaced when something changed.
static QCache<QString, QVector<FolderModel::Entry>> &recentListings() {
    static QCache<QString, QVector<FolderModel::Entry>> cache(64);
    return cache;
}

// The language is part of it: names sort by its alphabet.
QString FolderModel::cacheKey() const {
    return m_path + '\n' + (m_showHidden ? "h" : "") + '\n' + m_sort + '\n' + QLocale().name() + '\n'
           + m_filter.join('\n');
}

void FolderModel::remember() {
    // Search results are a moment's question, not a folder to come back to.
    if (!m_query.isEmpty())
        return;
    // The folder's own items: what's opened in place is closed on coming
    // back.
    auto *own = new QVector<Entry>;
    for (const Entry &e : m_entries)
        if (e.depth == 0)
            own->push_back(e);
    recentListings().insert(cacheKey(), own);
}

bool FolderModel::sameListing(const QVector<Entry> &a, const QVector<Entry> &b) {
    if (a.size() != b.size())
        return false;
    // By path, not name: a folder copied with cp -a or rsync lists the same
    // names, sizes and times as the one it came from.
    for (int i = 0; i < a.size(); ++i)
        // Name, subtitle, icon and details too: a plugin's item keeps its
        // path (its id) when they change.
        if (a[i].path != b[i].path || a[i].size != b[i].size || a[i].modified != b[i].modified
            || a[i].isDir != b[i].isDir || a[i].name != b[i].name || a[i].extension != b[i].extension
            || a[i].icon != b[i].icon || a[i].details != b[i].details || a[i].depth != b[i].depth)
            return false;
    return true;
}

void FolderModel::setEntries(const QVector<Entry> &entries) {
    beginResetModel();
    m_entries = entries;
    endResetModel();
    emit countChanged();
}

void FolderModel::clearEntries() {
    m_staleClear.stop();
    if (!m_entries.isEmpty())
        setEntries({});
}

int FolderModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_entries.size();
}

QVariant FolderModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(index.row());
    switch (role) {
    case NameRole: return e.name;
    case PathRole: return e.path;
    case IsDirRole: return e.isDir;
    case SizeRole: return e.size;
    case ModifiedRole: return e.modified;
    case KindRole: return e.kind;
    case MimeRole: return e.mime;
    case ExtensionRole: return e.extension;
    case ItemCountRole: return e.itemCount;
    case SelectedRole: return m_selected.contains(e.path);
    case IconRole: return e.icon;
    case DepthRole: return e.depth;
    case ExpandedRole: return m_expanded.contains(e.path);
    }
    return {};
}

QHash<int, QByteArray> FolderModel::roleNames() const {
    return {
        {NameRole, "name"}, {PathRole, "path"}, {IsDirRole, "isDir"}, {SizeRole, "size"},
        {ModifiedRole, "modified"}, {KindRole, "kind"}, {MimeRole, "mime"},
        {ExtensionRole, "extension"}, {ItemCountRole, "itemCount"}, {SelectedRole, "selected"},
        {IconRole, "icon"}, {DepthRole, "depth"}, {ExpandedRole, "expanded"},
    };
}

void FolderModel::setShowHidden(bool show) {
    if (show == m_showHidden)
        return;
    m_showHidden = show;
    emit showHiddenChanged();
    reload();
}

void FolderModel::setSortMode(const QString &sort) {
    if (sort == m_sort || !sortModes().contains(sort))
        return;
    m_sort = sort;
    emit sortModeChanged();
    reload();
}

void FolderModel::setPath(const QString &path, const QString &sort) {
    const bool resort = sort != m_sort && sortModes().contains(sort);
    if (resort) {
        m_sort = sort;
        emit sortModeChanged();
    }
    const QString clean = path.isEmpty() || isView(path) ? path : QDir::cleanPath(path);
    if (clean != m_path)
        setPath(clean);
    else if (resort)
        reload();
}

void FolderModel::setQuery(const QString &query) {
    if (query == m_query)
        return;
    m_query = query;
    // Search results are a flat list.
    if (!m_query.isEmpty())
        forgetExpanded();
    emit queryChanged();
    setSelected({});
    // Back to the folder: its remembered listing shows at once.
    if (m_query.isEmpty()) {
        if (const auto *cached = recentListings().object(cacheKey()))
            setEntries(*cached);
    }
    reload();
}

void FolderModel::setItems(const QVariantList &items) {
    if (items == m_items)
        return;
    m_items = items;
    emit itemsChanged();
    // A plugin's view follows its polls.
    if (isPluginView(m_path))
        reload();
}

void FolderModel::setFilter(const QStringList &filter) {
    if (filter == m_filter)
        return;
    m_filter = filter;
    emit filterChanged();
    reload();
}

void FolderModel::setPath(const QString &path) {
    const QString clean = path.isEmpty() || isView(path) ? path : QDir::cleanPath(path);
    if (clean == m_path)
        return;
    if (!m_watcher.directories().isEmpty())
        m_watcher.removePaths(m_watcher.directories());
    if (!m_watcher.files().isEmpty())
        m_watcher.removePaths(m_watcher.files());
    m_path = clean;
    m_expanded.clear();
    m_open.clear();
    emit pathChanged();
    // A search is of one folder; going elsewhere ends it.
    if (!m_query.isEmpty()) {
        m_query.clear();
        emit queryChanged();
    }
    setSelected({});

    // The old folder stays up while the new one is read, which locally takes
    // a few milliseconds: swapping straight from one to the other avoids an
    // empty frame between them. A slow listing (another machine's) clears it
    // after a moment, so the old folder never lingers under the new path.
    // An empty path is a column with nothing to show yet.
    if (m_path.isEmpty()) {
        // What's still being read is for the folder left behind.
        ++m_generation;
        *m_latest = m_generation;
        setLoading(false);
        clearEntries();
        return;
    }
    if (const auto *cached = recentListings().object(cacheKey())) {
        m_staleClear.stop();
        setEntries(*cached);
        setError({});
        emit loaded();
    } else {
        m_staleClear.start();
    }
    // Plugins' views have nothing to watch: their plugins are polled.
    if (!isPluginView(m_path))
        m_watcher.addPath(m_path == recentPath() ? recentFile() : m_path);
    reload();
}

bool FolderModel::fits(const QStringList &filter, const Entry &entry) {
    if (filter.isEmpty() || entry.isDir)
        return true;
    // From the path: in search results the name is the path from where the
    // search started.
    const QString name = entry.path.section('/', -1);
    const QMimeType type = QMimeDatabase().mimeTypeForName(entry.mime);
    for (const QString &pattern : filter) {
        if (!pattern.contains('/')) {
            if (QDir::match(QStringList{pattern}, name))
                return true;
        } else if (pattern == "*/*") {
            return true;
        } else if (pattern.endsWith("/*")) {
            const QString group = pattern.chopped(1);
            if (entry.mime.startsWith(group))
                return true;
            for (const QString &parent : type.allAncestors())
                if (parent.startsWith(group))
                    return true;
        } else if (type.inherits(pattern)) {
            return true;
        }
    }
    return false;
}

QString FolderModel::kindForMime(const QString &mime) {
    if (mime == "inode/directory") return "folder";
    if (mime == "application/pdf") return "pdf";
    if (mime.startsWith("image/")) return "image";
    if (mime.startsWith("video/")) return "video";
    if (mime.startsWith("audio/")) return "audio";
    // Anything readable as text (source code, config, JSON, Ruby, ...) is
    // text, whatever its own type is called.
    static const QMimeDatabase types;
    if (mime.startsWith("text/") || types.mimeTypeForName(mime).inherits("text/plain"))
        return "text";
    static const QStringList archives{"application/zip", "application/x-tar", "application/gzip",
                                      "application/x-xz", "application/zstd", "application/x-7z-compressed",
                                      "application/x-compressed-tar", "application/x-zstd-compressed-tar",
                                      "application/vnd.rar", "application/x-iso9660-image"};
    // Every compressed tar too: .tar.xz, .tar.bz2, .tar.lz4 and the rest.
    if (archives.contains(mime) || mime.endsWith("-compressed-tar")) return "archive";
    return "other";
}

// Everything in a folder (sockets and broken links too), the hidden only
// when they're shown.
static QDir::Filters entryFilters(bool showHidden) {
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::System;
    if (showHidden)
        filters |= QDir::Hidden;
    return filters;
}

QVector<FolderModel::Entry> FolderModel::list(const QString &path, bool showHidden, const QString &sort,
                                              QString *error, const std::function<bool()> &stop,
                                              const QLocale &locale) {
    if (path == recentPath()) {
        // Recents are always most recently used first; that's what they're for.
        return listRecent(recentFile(), showHidden, 200, stop);
    }
    QVector<Entry> entries;
    QDir dir(path);
    if (!dir.exists()) {
        if (error) *error = QObject::tr("This folder no longer exists.");
        return entries;
    }
    if (!QFileInfo(path).isReadable()) {
        if (error) *error = QObject::tr("You don't have permission to open this folder.");
        return entries;
    }

    // Extension-only matching: reading every file's header would make large
    // folders and network mounts crawl.
    QMimeDatabase mimes;
    QDirIterator it(path, entryFilters(showHidden));
    while (it.hasNext()) {
        if (stop && stop())
            return {};
        entries.push_back(entryFor(it.nextFileInfo(), mimes));
    }
    sortEntries(entries, sort, locale);
    return entries;
}

bool FolderModel::isTrash(const QString &path) {
    return path == QDir::cleanPath(FileOps::homeTrash() + "/files");
}

QVector<FolderModel::Entry> FolderModel::listTrash(const QStringList &trashDirs, const QString &query, bool showHidden,
                                                   const QString &sort, const std::function<bool()> &stop,
                                                   int limit, const QLocale &locale, const QStringList &filter) {
    // A search's limit is for every trash together, and a listing left
    // behind stops before it reads another drive.
    QVector<Entry> entries;
    for (const QString &dir : trashDirs) {
        if (stop && stop())
            return {};
        if (!query.isEmpty() && entries.size() >= limit)
            break;
        entries += query.isEmpty() ? list(dir + "/files", showHidden, sort, nullptr, stop, locale)
                                   : search(dir + "/files", query, showHidden, sort, stop, limit - entries.size(), locale, filter);
    }
    sortEntries(entries, sort, locale);
    return entries;
}

FolderModel::Entry FolderModel::entryFor(const QFileInfo &info, const QMimeDatabase &mimes) {
    Entry e;
    e.name = info.fileName();
    e.path = info.absoluteFilePath();
    e.isDir = info.isDir();
    e.size = e.isDir ? 0 : info.size();
    e.modified = info.lastModified();
    e.mime = e.isDir ? QStringLiteral("inode/directory")
                     : mimes.mimeTypeForFile(info, QMimeDatabase::MatchExtension).name();
    e.kind = kindForMime(e.mime);
    e.extension = e.isDir ? QString() : info.suffix().toUpper();
    return e;
}

QVector<FolderModel::Entry> FolderModel::search(const QString &root, const QString &query, bool showHidden,
                                                const QString &sort, const std::function<bool()> &stop,
                                                int limit, const QLocale &locale, const QStringList &filter) {
    QVector<Entry> entries;
    struct stat top;
    if (::stat(QFile::encodeName(root).constData(), &top) != 0)
        return entries;
    const QDir::Filters filters = entryFilters(showHidden);

    // Breadth first, so what's near comes before what's deep when there are
    // more matches than the limit.
    QMimeDatabase mimes;
    const QDir base(root);
    QStringList folders{root};
    while (!folders.isEmpty() && entries.size() < limit) {
        if (stop && stop())
            return {};
        QDirIterator it(folders.takeFirst(), filters);
        while (it.hasNext()) {
            if (stop && stop())
                return {};
            const QFileInfo info = it.nextFileInfo();
            if (info.fileName().contains(query, Qt::CaseInsensitive) && entries.size() < limit) {
                Entry e = entryFor(info, mimes);
                e.name = base.relativeFilePath(e.path);
                // Only what fits a file chooser's filter uses up the limit.
                if (fits(filter, e))
                    entries.push_back(e);
            }
            // Other filesystems (a mounted drive, another machine, /proc
            // from /) and symlinked folders aren't walked into.
            struct stat st;
            if (info.isDir() && !info.isSymLink()
                && ::lstat(QFile::encodeName(info.absoluteFilePath()).constData(), &st) == 0
                && st.st_dev == top.st_dev)
                folders << info.absoluteFilePath();
        }
    }
    sortEntries(entries, sort, locale);
    return entries;
}

QStringList FolderModel::sortModes() {
    // "modified" is newest first; it keeps its name so saved settings
    // from before "oldest" existed still read right.
    return {"name", "kind", "modified", "oldest", "size"};
}

void FolderModel::sortEntries(QVector<Entry> &entries, const QString &sort, const QLocale &locale) {
    QCollator collator(locale);
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::stable_sort(entries.begin(), entries.end(), [&](const Entry &a, const Entry &b) {
        // Folders first in every order, as everywhere else on the desktop.
        if (a.isDir != b.isDir)
            return a.isDir;
        if (sort == "modified" && a.modified != b.modified)
            return a.modified > b.modified;
        if (sort == "oldest" && a.modified != b.modified)
            return a.modified < b.modified;
        // Biggest first; folders have no size of their own and stay by name.
        if (sort == "size" && !a.isDir && a.size != b.size)
            return a.size > b.size;
        if (sort == "kind" && !a.isDir) {
            if (a.kind != b.kind)
                return a.kind < b.kind;
            if (a.extension != b.extension)
                return collator.compare(a.extension, b.extension) < 0;
        }
        return collator.compare(a.name, b.name) < 0;
    });
}

// Recents are GTK's recently used files (~/.local/share/recently-used.xbel),
// which every GTK and many other apps add to: the files still there, the
// most recently used first.
QVector<FolderModel::Entry> FolderModel::listRecent(const QString &xbel, bool showHidden, int limit,
                                                    const std::function<bool()> &stop) {
    struct Used { QString path; QDateTime when; };
    QVector<Used> used;
    QFile file(xbel);
    if (file.open(QIODevice::ReadOnly)) {
        QXmlStreamReader xml(&file);
        while (!xml.atEnd()) {
            if (xml.readNext() == QXmlStreamReader::StartElement && xml.name() == QLatin1String("bookmark")) {
                const auto attrs = xml.attributes();
                const QUrl url(attrs.value("href").toString());
                QDateTime when = QDateTime::fromString(attrs.value("visited").toString(), Qt::ISODate);
                const QDateTime modified = QDateTime::fromString(attrs.value("modified").toString(), Qt::ISODate);
                if (!when.isValid() || (modified.isValid() && modified > when))
                    when = modified;
                if (url.isLocalFile())
                    used.push_back({url.toLocalFile(), when});
            }
        }
    }
    std::stable_sort(used.begin(), used.end(), [](const Used &a, const Used &b) { return a.when > b.when; });

    QVector<Entry> entries;
    QStringList seen;
    QMimeDatabase mimes;
    for (const Used &u : used) {
        // Each check can wait on a slow drive, so leaving Recents stops it.
        if (stop && stop())
            break;
        const QFileInfo info(u.path);
        if (seen.contains(u.path) || !info.exists() || (!showHidden && info.fileName().startsWith('.')))
            continue;
        seen << u.path;
        entries.push_back(entryFor(info, mimes));
        if (entries.size() == limit)
            break;
    }
    return entries;
}

QVector<FolderModel::Entry> FolderModel::listItems(const QVariantList &items, const QString &query) {
    QVector<Entry> entries;
    for (const QVariant &item : items) {
        const QVariantMap map = item.toMap();
        Entry e;
        e.name = map.value("name").toString();
        if (!e.name.contains(query, Qt::CaseInsensitive))
            continue;
        e.path = map.value("path").toString();
        e.kind = QStringLiteral("item");
        e.icon = map.value("icon").toString();
        e.extension = map.value("subtitle").toString();
        e.details = map.value("details").toList();
        entries.push_back(e);
    }
    return entries;
}

QString FolderModel::recentFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/recently-used.xbel";
}

void FolderModel::summarize(Entry &entry, bool showHidden) {
    QDir dir(entry.path);
    const QStringList names = dir.entryList(entryFilters(showHidden), QDir::Name);
    entry.itemCount = names.size();
    entry.peek.clear();

    static const QStringList pictures{"jpg", "jpeg", "png", "webp", "gif", "heic", "avif", "jxl"};
    for (const QString &name : names) {
        if (pictures.contains(QFileInfo(name).suffix().toLower()))
            entry.peek << dir.filePath(name);
        if (entry.peek.size() == 2)
            break;
    }
}

int FolderModel::itemCount(const QString &folder, bool showHidden, const std::function<bool()> &stop) {
    int count = 0;
    for (QDirIterator it(folder, entryFilters(showHidden)); it.hasNext(); it.next()) {
        if (stop && stop())
            return -1;
        ++count;
    }
    return count;
}

void FolderModel::reload() {
    if (m_path.isEmpty())
        return;
    const quint64 generation = ++m_generation;
    setLoading(true);

    const QString path = m_path;
    const bool showHidden = m_showHidden;
    const QString sort = m_sort;
    const QString query = m_query;
    const QLocale locale;
    const QVariantList items = m_items;
    const QStringList filter = m_filter;
    const QStringList expanded = expandable() ? QStringList(m_expanded.begin(), m_expanded.end()) : QStringList();
    *m_latest = generation;
    const auto latest = m_latest;
    auto *watcher = new QFutureWatcher<std::tuple<QVector<Entry>, QString, QStringList, QHash<QString, QVector<Entry>>, QHash<QString, QString>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_generation)
            return; // A newer listing replaced this one while it ran.
        m_staleClear.stop();
        const auto [listed, error, trashDirs, open, unread] = watcher->result();
        // The folders opened in place, each with its items under it, and
        // those that can no longer be read closed.
        for (auto it = open.begin(); it != open.end(); ++it)
            if (m_expanded.contains(it.key()))
                m_open.insert(it.key(), it.value());
        for (auto it = unread.begin(); it != unread.end(); ++it)
            if (m_expanded.contains(it.key()))
                failExpanded(it.key(), it.value());
        const QVector<Entry> entries = m_open.isEmpty() ? listed : flatten(listed, m_open);
        // Unchanged (the usual case after showing a remembered listing):
        // leave the items alone, and only fill in folder counts still missing.
        const bool same = error == m_error && !m_entries.isEmpty() && sameListing(m_entries, entries);
        if (!same)
            setEntries(entries);
        if (!m_selected.isEmpty()) {
            QSet<QString> still;
            for (const Entry &e : m_entries)
                if (m_selected.contains(e.path))
                    still.insert(e.path);
            if (still != m_selected) {
                m_selected = still;
                emit selectionChanged();
            }
        }
        // Every drive's trash is watched while the trash is shown, as the home one is.
        for (const QString &dir : trashDirs.mid(1))
            if (!m_watcher.directories().contains(dir + "/files"))
                m_watcher.addPath(dir + "/files");
        setError(error);
        setLoading(false);
        emit loaded();
        if (error.isEmpty())
            remember();
        countFolders(generation);
    });
    watcher->setFuture(QtConcurrent::run(&FolderModel::listingPool(), [path, showHidden, sort, query, items, filter, expanded, latest, generation, locale] {
        QString error;
        const auto overtaken = [&] { return *latest != generation; };
        // Finding the drives' trash looks over every mount, so it's done here,
        // off the window's thread.
        const QStringList trashDirs = isTrash(path) ? FileOps::trashDirs() : QStringList();
        auto entries = isPluginView(path) ? listItems(items, query)
                     : isTrash(path) ? listTrash(trashDirs, query, showHidden, sort, overtaken, 1000, locale, filter)
                     : query.isEmpty() || path == recentPath() ? list(path, showHidden, sort, &error, overtaken, locale)
                     : search(path, query, showHidden, sort, overtaken, 1000, locale, filter);
        if (!isPluginView(path))
            entries.removeIf([&](const Entry &entry) { return !fits(filter, entry); });
        // Every folder opened in place, read again with it; one that can't
        // be is told by its error.
        QHash<QString, QVector<Entry>> open;
        QHash<QString, QString> unread;
        for (const QString &folder : expanded) {
            if (overtaken())
                break;
            QString why;
            auto items = list(folder, showHidden, sort, &why, overtaken, locale);
            if (!why.isEmpty()) {
                unread.insert(folder, why);
                continue;
            }
            items.removeIf([&](const Entry &entry) { return !fits(filter, entry); });
            open.insert(folder, items);
        }
        return std::make_tuple(entries, error, trashDirs, open, unread);
    }));
}

void FolderModel::countFolders(quint64 generation) {
    // Only the folders not counted yet: a listing just read, or one whose
    // counting was overtaken by a re-read (and so remembered without them).
    QVector<QPair<int, Entry>> folders;
    for (int row = 0; row < m_entries.size(); ++row)
        if (m_entries[row].isDir && m_entries[row].itemCount < 0)
            folders.push_back({row, m_entries[row]});
    if (folders.isEmpty())
        return;

    const bool showHidden = m_showHidden;
    auto *watcher = new QFutureWatcher<QVector<QPair<int, Entry>>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        bool updated = false;
        for (const auto &[row, summary] : watcher->result()) {
            if (row >= m_entries.size() || m_entries[row].path != summary.path)
                continue;
            m_entries[row].itemCount = summary.itemCount;
            const QModelIndex at = index(row);
            emit dataChanged(at, at, {ItemCountRole});
            updated = true;
        }
        // QML re-reads its get() snapshots (the current item, which the
        // details column shows) on countChanged, and the counts they show
        // just arrived.
        if (updated)
            emit countChanged();
        remember();
    });
    const auto latest = m_latest;
    watcher->setFuture(QtConcurrent::run([folders, showHidden, latest, generation]() mutable {
        // Overtaken (another folder, or a re-read), the counts would be
        // thrown away, so it stops instead of listing every subfolder.
        const auto overtaken = [&] { return *latest != generation; };
        for (auto &folder : folders) {
            if (overtaken())
                break;
            folder.second.itemCount = itemCount(folder.second.path, showHidden, overtaken);
        }
        return folders;
    }));
}

QVariantMap FolderModel::get(int row) const {
    if (row < 0 || row >= m_entries.size())
        return {};
    const Entry &e = m_entries.at(row);
    QVariantMap map = toMap(e);
    map.insert("selected", m_selected.contains(e.path));
    map.insert("expanded", m_expanded.contains(e.path));
    return map;
}

QVariantMap FolderModel::toMap(const Entry &entry) {
    return {
        {"name", entry.name}, {"path", entry.path}, {"isDir", entry.isDir}, {"size", entry.size},
        {"modified", entry.modified}, {"kind", entry.kind}, {"mime", entry.mime},
        {"extension", entry.extension}, {"itemCount", entry.itemCount}, {"icon", entry.icon},
        {"details", entry.details}, {"depth", entry.depth},
    };
}

void FolderModel::setSelected(const QSet<QString> &paths) {
    if (paths == m_selected)
        return;
    m_selected = paths;
    if (!m_entries.isEmpty())
        emit dataChanged(index(0), index(m_entries.size() - 1), {SelectedRole});
    emit selectionChanged();
}

void FolderModel::selectOnly(int row) {
    setSelected(row >= 0 && row < m_entries.size() ? QSet<QString>{m_entries[row].path} : QSet<QString>{});
}

void FolderModel::toggleSelected(int row) {
    if (row < 0 || row >= m_entries.size())
        return;
    QSet<QString> paths = m_selected;
    const QString path = m_entries[row].path;
    if (!paths.remove(path))
        paths.insert(path);
    setSelected(paths);
}

void FolderModel::selectRange(int from, int to, bool add) {
    if (m_entries.isEmpty())
        return;
    from = qBound(0, from, int(m_entries.size()) - 1);
    to = qBound(0, to, int(m_entries.size()) - 1);
    QSet<QString> paths = add ? m_selected : QSet<QString>{};
    for (int row = qMin(from, to); row <= qMax(from, to); ++row)
        paths.insert(m_entries[row].path);
    setSelected(paths);
}

void FolderModel::selectRows(const QList<int> &rows) {
    QSet<QString> paths;
    for (int row : rows)
        if (row >= 0 && row < m_entries.size())
            paths.insert(m_entries[row].path);
    setSelected(paths);
}

void FolderModel::selectAll() {
    QSet<QString> paths;
    for (const Entry &e : m_entries)
        paths.insert(e.path);
    setSelected(paths);
}

void FolderModel::clearSelection() {
    setSelected({});
}

bool FolderModel::isSelected(int row) const {
    return row >= 0 && row < m_entries.size() && m_selected.contains(m_entries[row].path);
}

QStringList FolderModel::selectedPaths() const {
    // In the folder's order, not the order they were picked in.
    QStringList paths;
    for (const Entry &e : m_entries)
        if (m_selected.contains(e.path))
            paths << e.path;
    return paths;
}

// Among the folder's own items, not those of a folder opened in place.
int FolderModel::indexOf(const QString &name) const {
    for (int row = 0; row < m_entries.size(); ++row)
        if (m_entries[row].depth == 0 && m_entries[row].name == name)
            return row;
    return -1;
}

bool FolderModel::expandable() const {
    return !m_path.isEmpty() && !isView(m_path) && !isTrash(m_path) && m_query.isEmpty();
}

QVector<FolderModel::Entry> FolderModel::flatten(const QVector<Entry> &entries, const QHash<QString, QVector<Entry>> &open, int depth) {
    QVector<Entry> rows;
    for (Entry e : entries) {
        e.depth = depth;
        rows.push_back(e);
        if (e.isDir)
            if (const auto inside = open.constFind(e.path); inside != open.constEnd())
                rows += flatten(*inside, open, depth + 1);
    }
    return rows;
}

bool FolderModel::isExpanded(int row) const {
    return row >= 0 && row < m_entries.size() && m_expanded.contains(m_entries[row].path);
}

int FolderModel::parentRow(int row) const {
    if (row < 0 || row >= m_entries.size())
        return -1;
    for (int above = row - 1; above >= 0; --above)
        if (m_entries[above].depth < m_entries[row].depth)
            return above;
    return -1;
}

void FolderModel::setExpanded(int row, bool expanded) {
    if (row < 0 || row >= m_entries.size() || !m_entries[row].isDir || !expandable()
        || expanded == m_expanded.contains(m_entries[row].path))
        return;
    const QString folder = m_entries[row].path;
    const QModelIndex at = index(row);
    if (!expanded) {
        // It, and every folder opened inside it, close.
        const QString inside = folder + '/';
        for (auto it = m_expanded.begin(); it != m_expanded.end();) {
            if (*it == folder || it->startsWith(inside)) {
                m_watcher.removePath(*it);
                m_open.remove(*it);
                m_opening.remove(*it);
                it = m_expanded.erase(it);
            } else {
                ++it;
            }
        }
        int end = row + 1;
        while (end < m_entries.size() && m_entries[end].depth > m_entries[row].depth)
            ++end;
        if (end > row + 1) {
            QSet<QString> selected = m_selected;
            for (int r = row + 1; r < end; ++r)
                selected.remove(m_entries[r].path);
            beginRemoveRows({}, row + 1, end - 1);
            m_entries.remove(row + 1, end - row - 1);
            endRemoveRows();
            emit countChanged();
            // What was selected inside goes with it.
            setSelected(selected);
        }
        emit dataChanged(at, at, {ExpandedRole});
        return;
    }

    m_expanded.insert(folder);
    m_watcher.addPath(folder);
    emit dataChanged(at, at, {ExpandedRole});
    const quint64 opening = ++m_openings;
    m_opening.insert(folder, opening);
    const quint64 generation = m_generation;
    const bool showHidden = m_showHidden;
    const QString sort = m_sort;
    const QStringList filter = m_filter;
    const QLocale locale;
    const auto latest = m_latest;
    auto *watcher = new QFutureWatcher<std::pair<QVector<Entry>, QString>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, folder, opening] {
        watcher->deleteLater();
        // A re-read since (which reads it too), or closed meanwhile, and
        // perhaps opened again: only this opening's reading counts.
        if (generation != m_generation || m_opening.value(folder) != opening)
            return;
        m_opening.remove(folder);
        const auto &[items, error] = watcher->result();
        if (!error.isEmpty()) {
            failExpanded(folder, error);
            return;
        }
        const int row = indexOfPath(folder);
        if (row < 0)
            return;
        m_open.insert(folder, items);
        // A re-read that finished first may have listed it already: its rows
        // are replaced, never listed twice.
        int end = row + 1;
        while (end < m_entries.size() && m_entries[end].depth > m_entries[row].depth)
            ++end;
        if (end > row + 1) {
            beginRemoveRows({}, row + 1, end - 1);
            m_entries.remove(row + 1, end - row - 1);
            endRemoveRows();
        }
        const QVector<Entry> rows = flatten(items, m_open, m_entries[row].depth + 1);
        if (rows.isEmpty())
            return;
        beginInsertRows({}, row + 1, row + int(rows.size()));
        for (int i = 0; i < rows.size(); ++i)
            m_entries.insert(row + 1 + i, rows[i]);
        endInsertRows();
        emit countChanged();
        countFolders(generation);
    });
    watcher->setFuture(QtConcurrent::run(&FolderModel::listingPool(), [folder, showHidden, sort, filter, latest, generation, locale] {
        QString error;
        auto items = list(folder, showHidden, sort, &error, [&] { return *latest != generation; }, locale);
        items.removeIf([&](const Entry &entry) { return !fits(filter, entry); });
        return std::make_pair(items, error);
    }));
}

void FolderModel::failExpanded(const QString &folder, const QString &error) {
    // Closed as a click on its arrow would, its items (if any were shown)
    // going with it.
    if (const int row = indexOfPath(folder); row >= 0 && m_expanded.contains(folder)) {
        setExpanded(row, false);
    } else {
        m_expanded.remove(folder);
        m_open.remove(folder);
        m_opening.remove(folder);
        m_watcher.removePath(folder);
    }
    //: %1 is a folder's name, %2 why it couldn't be opened.
    emit expandFailed(tr("%1: %2").arg(QFileInfo(folder).fileName(), error));
}

void FolderModel::forgetExpanded() {
    for (const QString &folder : std::as_const(m_expanded))
        if (folder != m_path)
            m_watcher.removePath(folder);
    m_expanded.clear();
    m_open.clear();
    m_opening.clear();
}

void FolderModel::collapseAll() {
    if (m_expanded.isEmpty())
        return;
    forgetExpanded();
    QVector<Entry> own;
    for (const Entry &e : std::as_const(m_entries))
        if (e.depth == 0)
            own.push_back(e);
    QSet<QString> selected;
    for (const Entry &e : std::as_const(own))
        if (m_selected.contains(e.path))
            selected.insert(e.path);
    setEntries(own);
    setSelected(selected);
}

int FolderModel::indexOfPath(const QString &path) const {
    for (int row = 0; row < m_entries.size(); ++row)
        if (m_entries[row].path == path)
            return row;
    return -1;
}

void FolderModel::setLoading(bool loading) {
    if (loading == m_loading)
        return;
    m_loading = loading;
    emit loadingChanged();
}

void FolderModel::setError(const QString &error) {
    if (error == m_error)
        return;
    m_error = error;
    emit errorChanged();
}
