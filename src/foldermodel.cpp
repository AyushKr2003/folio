#include "foldermodel.h"
#include "fileops.h"

#include <QCache>
#include <QCollator>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QTimeZone>
#include <QXmlStreamReader>
#include <QUrl>
#include <QStandardPaths>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMimeDatabase>
#include <QtConcurrent>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <numeric>
#include <optional>
#include <vector>
#include <tuple>
#include <dirent.h>
#include <fcntl.h>
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
    m_pathIndexDirty = true;
    endResetModel();
    emit countChanged();
}

void FolderModel::ensurePathIndex() const {
    if (!m_pathIndexDirty)
        return;
    m_pathIndex.clear();
    m_pathIndex.reserve(m_entries.size());
    for (int row = 0; row < m_entries.size(); ++row)
        m_pathIndex.insert(m_entries[row].path, row);
    m_pathIndexDirty = false;
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

namespace {

// Pre-compiled representation of a file chooser filter so filtering a folder
// of thousands of files compiles each glob once and checks MIME inheritance
// at most once per unique MIME type.
struct CompiledFilter {
    bool empty = true;
    bool matchAll = false;
    bool needsMime = false;
    QList<QRegularExpression> nameGlobs;
    QStringList mimeGroups;
    QStringList mimeTypes;
    mutable QHash<QString, bool> mimeCache;

    explicit CompiledFilter(const QStringList &filter) : empty(filter.isEmpty()) {
        for (const QString &pattern : filter) {
            if (!pattern.contains(QLatin1Char('/'))) {
                nameGlobs.push_back(QRegularExpression::fromWildcard(pattern, Qt::CaseInsensitive));
            } else if (pattern == QLatin1String("*/*")) {
                matchAll = true;
            } else if (pattern.endsWith(QLatin1String("/*"))) {
                mimeGroups.push_back(pattern.chopped(1));
                needsMime = true;
            } else {
                mimeTypes.push_back(pattern);
                needsMime = true;
            }
        }
    }

    bool matches(const FolderModel::Entry &entry) const {
        if (empty || matchAll || entry.isDir)
            return true;
        if (!nameGlobs.isEmpty()) {
            const qsizetype slash = entry.path.lastIndexOf(QLatin1Char('/'));
            const QStringView name = slash >= 0 ? QStringView(entry.path).mid(slash + 1) : QStringView(entry.path);
            for (const QRegularExpression &re : nameGlobs)
                if (re.matchView(name).hasMatch())
                    return true;
        }
        if (!needsMime)
            return false;
        if (const auto it = mimeCache.constFind(entry.mime); it != mimeCache.cend())
            return *it;
        bool ok = false;
        for (const QString &group : mimeGroups) {
            if (entry.mime.startsWith(group)) {
                ok = true;
                break;
            }
        }
        if (!ok) {
            const QMimeType type = QMimeDatabase().mimeTypeForName(entry.mime);
            for (const QString &group : mimeGroups) {
                for (const QString &parent : type.allAncestors()) {
                    if (parent.startsWith(group)) {
                        ok = true;
                        break;
                    }
                }
                if (ok)
                    break;
            }
            if (!ok) {
                for (const QString &pattern : mimeTypes) {
                    if (type.inherits(pattern)) {
                        ok = true;
                        break;
                    }
                }
            }
        }
        mimeCache.insert(entry.mime, ok);
        return ok;
    }
};

// Reads shared-mime-info's globs2 files once so simple single-extension
// filenames (like photo.jpg or main.cpp) can be resolved from a per-thread
// extension cache without calling QMimeDatabase's glob matcher on every file,
// while literal names (CMakeLists.txt, Makefile), compound extensions
// (*.tar.gz) and special wildcard globs still fall through to QMimeDatabase.
struct SpecialGlobs {
    bool available = false;
    QSet<QString> literals;
    QStringList prefixes;
    QSet<QString> wildcardExts;
    QList<QRegularExpression> extraRegexes;

    SpecialGlobs() {
        const QStringList files = QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
                                                            QStringLiteral("mime/globs2"));
        for (const QString &filePath : files) {
            QFile file(filePath);
            if (!file.open(QIODevice::ReadOnly))
                continue;
            available = true;
            while (!file.atEnd()) {
                const QByteArray raw = file.readLine().trimmed();
                if (raw.isEmpty() || raw.startsWith('#'))
                    continue;
                const qsizetype c1 = raw.indexOf(':');
                const qsizetype c2 = c1 >= 0 ? raw.indexOf(':', c1 + 1) : -1;
                if (c2 < 0)
                    continue;
                const qsizetype c3 = raw.indexOf(':', c2 + 1);
                const QString pattern = QString::fromUtf8(
                    c3 >= 0 ? raw.mid(c2 + 1, c3 - c2 - 1) : raw.mid(c2 + 1));
                if (pattern.isEmpty())
                    continue;
                // Simple *.ext pattern (single dot, no extra wildcards).
                if (pattern.startsWith(QLatin1String("*."))) {
                    const QStringView ext = QStringView(pattern).mid(2);
                    const bool plainExt = !ext.isEmpty() && !ext.contains(QLatin1Char('.'))
                                          && !ext.contains(QLatin1Char('*')) && !ext.contains(QLatin1Char('?'))
                                          && !ext.contains(QLatin1Char('['));
                    if (plainExt)
                        continue;
                    // Compound extension like *.tar.gz is already excluded by the single-dot check.
                    if (ext.contains(QLatin1Char('.')))
                        continue;
                    extraRegexes.push_back(QRegularExpression::fromWildcard(pattern, Qt::CaseInsensitive));
                    continue;
                }
                const bool hasWildcard = pattern.contains(QLatin1Char('*')) || pattern.contains(QLatin1Char('?'))
                                         || pattern.contains(QLatin1Char('['));
                if (!hasWildcard) {
                    literals.insert(pattern.toLower());
                } else if (pattern.endsWith(QLatin1Char('*')) && pattern.indexOf(QLatin1Char('*')) == pattern.size() - 1
                           && !pattern.contains(QLatin1Char('?')) && !pattern.contains(QLatin1Char('['))) {
                    prefixes.push_back(pattern.chopped(1).toLower());
                } else {
                    const qsizetype dot = pattern.lastIndexOf(QLatin1Char('.'));
                    if (dot >= 0) {
                        const QString ext = pattern.mid(dot + 1);
                        if (!ext.isEmpty() && !ext.contains(QLatin1Char('*')) && !ext.contains(QLatin1Char('?'))
                            && !ext.contains(QLatin1Char('['))) {
                            wildcardExts.insert(ext.toLower());
                            continue;
                        }
                    }
                    // Patterns like *~, *%, *,v only match non-alphanumeric tails.
                    extraRegexes.push_back(QRegularExpression::fromWildcard(pattern, Qt::CaseInsensitive));
                }
            }
        }
    }
};

const SpecialGlobs &specialGlobs() {
    static const SpecialGlobs globs;
    return globs;
}

bool isSimpleExtCandidate(const QString &name, qsizetype dot, const QString &ext, const SpecialGlobs &globs) {
    if (!globs.available || dot <= 0 || ext.isEmpty())
        return false;
    // Any earlier dot could match a compound glob (*.tar.gz, *.d.ts).
    if (QStringView(name).left(dot).contains(QLatin1Char('.')))
        return false;
    for (const QChar c : ext)
        if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z'))
              || (c >= QLatin1Char('0') && c <= QLatin1Char('9'))))
            return false;
    const QString lowerExt = ext.toLower();
    if (globs.wildcardExts.contains(lowerExt))
        return false;
    const QString lower = name.toLower();
    if (globs.literals.contains(lower))
        return false;
    for (const QString &prefix : globs.prefixes)
        if (lower.startsWith(prefix))
            return false;
    for (const QRegularExpression &re : globs.extraRegexes)
        if (re.match(name).hasMatch())
            return false;
    return true;
}

std::pair<QString, QString> mimeAndKindForName(const QString &name, const QMimeDatabase &mimes,
                                               qsizetype dot, const QString &ext) {
    const SpecialGlobs &globs = specialGlobs();
    if (isSimpleExtCandidate(name, dot, ext, globs)) {
        thread_local QHash<QString, std::pair<QString, QString>> extCache;
        if (const auto it = extCache.constFind(ext); it != extCache.cend())
            return *it;
        const QString mime = mimes.mimeTypeForFile(QLatin1String("x.") + ext, QMimeDatabase::MatchExtension).name();
        const QString kind = FolderModel::kindForMime(mime);
        const std::pair<QString, QString> result{mime, kind};
        if (extCache.size() > 4096)
            extCache.clear();
        extCache.insert(ext, result);
        return result;
    }
    const QString mime = mimes.mimeTypeForFile(name, QMimeDatabase::MatchExtension).name();
    return {mime, FolderModel::kindForMime(mime)};
}

FolderModel::Entry entryFromStat(const QString &name, const QString &fullPath, const struct stat *st,
                                 const QMimeDatabase &mimes) {
    FolderModel::Entry e;
    e.name = name;
    e.path = fullPath;
    e.isDir = st && S_ISDIR(st->st_mode);
    e.size = (!st || e.isDir) ? 0 : qint64(st->st_size);
    if (st)
        e.modified = QDateTime::fromMSecsSinceEpoch(qint64(st->st_mtim.tv_sec) * 1000LL
                                                    + qint64(st->st_mtim.tv_nsec) / 1000000LL,
                                                    QTimeZone::UTC);
    if (e.isDir) {
        e.mime = QStringLiteral("inode/directory");
        e.kind = QStringLiteral("folder");
    } else {
        const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
        const QString ext = dot >= 0 ? name.mid(dot + 1) : QString();
        auto [mime, kind] = mimeAndKindForName(name, mimes, dot, ext);
        e.mime = std::move(mime);
        e.kind = std::move(kind);
        e.extension = ext.toUpper();
    }
    return e;
}

} // namespace

bool FolderModel::fits(const QStringList &filter, const Entry &entry) {
    if (filter.isEmpty() || entry.isDir)
        return true;
    thread_local QStringList lastFilter;
    thread_local std::optional<CompiledFilter> compiled;
    if (!compiled || lastFilter != filter) {
        lastFilter = filter;
        compiled.emplace(filter);
    }
    return compiled->matches(entry);
}

QString FolderModel::kindForMime(const QString &mime) {
    if (mime == "inode/directory") return QStringLiteral("folder");
    if (mime == "application/pdf") return QStringLiteral("pdf");
    if (mime.startsWith("image/")) return QStringLiteral("image");
    if (mime.startsWith("video/")) return QStringLiteral("video");
    if (mime.startsWith("audio/")) return QStringLiteral("audio");
    if (mime.startsWith("text/")) return QStringLiteral("text");
    // Per-thread cache avoids taking QMimeDatabase's global lock on every file
    // in a large folder across concurrent listing threads.
    thread_local QHash<QString, QString> cache;
    if (const auto it = cache.constFind(mime); it != cache.cend())
        return *it;
    QString kind = QStringLiteral("other");
    // Anything readable as text (source code, config, JSON, Ruby, ...) is
    // text, whatever its own type is called.
    static const QMimeDatabase types;
    if (types.mimeTypeForName(mime).inherits("text/plain")) {
        kind = QStringLiteral("text");
    } else {
        static const QStringList archives{"application/zip", "application/x-tar", "application/gzip",
                                          "application/x-xz", "application/zstd", "application/x-7z-compressed",
                                          "application/x-compressed-tar", "application/x-zstd-compressed-tar",
                                          "application/vnd.rar", "application/x-iso9660-image"};
        // Every compressed tar too: .tar.xz, .tar.bz2, .tar.lz4 and the rest.
        if (archives.contains(mime) || mime.endsWith("-compressed-tar"))
            kind = QStringLiteral("archive");
    }
    cache.insert(mime, kind);
    return kind;
}

QVector<FolderModel::Entry> FolderModel::list(const QString &path, bool showHidden, const QString &sort,
                                              QString *error, const std::function<bool()> &stop,
                                              const QLocale &locale) {
    if (path == recentPath()) {
        // Recents are always most recently used first; that's what they're for.
        return listRecent(recentFile(), showHidden, 200, stop);
    }
    QVector<Entry> entries;
    const QString absPath = QFileInfo(path).absoluteFilePath();
    DIR *dir = ::opendir(QFile::encodeName(absPath).constData());
    if (!dir) {
        if (error) {
            if (errno == EACCES || errno == EPERM)
                *error = QObject::tr("You don't have permission to open this folder.");
            else
                *error = QObject::tr("This folder no longer exists.");
        }
        return entries;
    }

    const int dfd = ::dirfd(dir);
    const QString prefix = absPath.endsWith(QLatin1Char('/')) ? absPath : absPath + QLatin1Char('/');
    // Extension-only matching: reading every file's header would make large
    // folders and network mounts crawl.
    QMimeDatabase mimes;
    while (dirent *ent = ::readdir(dir)) {
        const char *raw = ent->d_name;
        if (raw[0] == '.') {
            if (raw[1] == '\0' || (raw[1] == '.' && raw[2] == '\0'))
                continue;
            if (!showHidden)
                continue;
        }
        if (stop && stop()) {
            ::closedir(dir);
            return {};
        }
        struct stat st;
        const bool ok = ::fstatat(dfd, raw, &st, 0) == 0
                        || ::fstatat(dfd, raw, &st, AT_SYMLINK_NOFOLLOW) == 0;
        const QString name = QFile::decodeName(raw);
        entries.push_back(entryFromStat(name, prefix + name, ok ? &st : nullptr, mimes));
    }
    ::closedir(dir);
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

QString FolderModel::typeByContent(const QString &path, const QString &mime) {
    if (mime != "application/octet-stream")
        return mime;
    return QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchContent).name();
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

// What a search query asks for: a name containing some text, with one of
// some extensions if it names them ("beach ext:jpg,png", "ext:pdf").
struct SearchFilter {
    QString text;
    QStringList extensions;

    explicit SearchFilter(const QString &query) {
        const QString q = query.trimmed();
        // Split keeping the empty words between repeated spaces, so the text
        // left once the ext: words are taken out is as it was typed.
        QStringList words;
        for (const QString &word : q.split(QLatin1Char(' '))) {
            if (word.startsWith(QLatin1String("ext:"), Qt::CaseInsensitive)) {
                for (QString ext : word.mid(4).split(QLatin1Char(','), Qt::SkipEmptyParts)) {
                    if (ext.startsWith(QLatin1Char('.')))
                        ext.remove(0, 1);
                    if (!ext.isEmpty())
                        extensions << ext.toLower();
                }
            } else {
                words << word;
            }
        }
        text = words.join(QLatin1Char(' ')).trimmed();
    }

    bool matchesName(const QString &name, bool isDir) const {
        if (!extensions.isEmpty()) {
            if (isDir)
                return false;
            const qsizetype dot = name.lastIndexOf(QLatin1Char('.'));
            if (dot < 0 || !extensions.contains(name.mid(dot + 1).toLower()))
                return false;
        }
        return name.contains(text, Qt::CaseInsensitive);
    }
};

QVector<FolderModel::Entry> FolderModel::search(const QString &root, const QString &query, bool showHidden,
                                                const QString &sort, const std::function<bool()> &stop,
                                                int limit, const QLocale &locale, const QStringList &filter,
                                                const std::function<void(const QVector<Entry> &)> &found) {
    QVector<Entry> entries;
    // What's been found since `found` was last told.
    qsizetype told = 0;
    QElapsedTimer since;
    since.start();
    const auto tell = [&](bool all) {
        if (found && entries.size() > told && (all || since.elapsed() >= 100)) {
            found(entries.mid(told));
            told = entries.size();
            since.restart();
        }
    };
    const QString absRoot = QFileInfo(root).absoluteFilePath();
    struct stat top;
    if (::stat(QFile::encodeName(absRoot).constData(), &top) != 0)
        return entries;
    const SearchFilter wanted(query);
    const CompiledFilter compiledFilter(filter);

    // Breadth first, so what's near comes before what's deep when there are
    // more matches than the limit.
    QMimeDatabase mimes;
    const QString rootPrefix = absRoot.endsWith(QLatin1Char('/')) ? absRoot : absRoot + QLatin1Char('/');
    std::deque<QString> folders{absRoot};
    while (!folders.empty() && entries.size() < limit) {
        if (stop && stop())
            return {};
        const QString currentDir = std::move(folders.front());
        folders.pop_front();
        DIR *dir = ::opendir(QFile::encodeName(currentDir).constData());
        if (!dir)
            continue;
        const int dfd = ::dirfd(dir);
        const QString dirPrefix = currentDir.endsWith(QLatin1Char('/')) ? currentDir : currentDir + QLatin1Char('/');
        while (dirent *ent = ::readdir(dir)) {
            const char *raw = ent->d_name;
            if (raw[0] == '.') {
                if (raw[1] == '\0' || (raw[1] == '.' && raw[2] == '\0'))
                    continue;
                if (!showHidden)
                    continue;
            }
            if (stop && stop()) {
                ::closedir(dir);
                return {};
            }
            // Fast path: use d_type to skip stat() on every non-matching file.
            const unsigned char dtype = ent->d_type;
            const bool definitelyDir = dtype == DT_DIR;
            const bool definitelyNonDir = dtype != DT_DIR && dtype != DT_LNK && dtype != DT_UNKNOWN;
            const QString name = QFile::decodeName(raw);

            // Check name match before stating, when we know it's not a dir (or no ext: filter).
            bool statLoaded = false;
            bool lstatLoaded = false;
            struct stat st{};
            struct stat lst{};
            const auto loadStat = [&] {
                if (!statLoaded) {
                    statLoaded = ::fstatat(dfd, raw, &st, 0) == 0
                                 || ::fstatat(dfd, raw, &st, AT_SYMLINK_NOFOLLOW) == 0;
                }
                return statLoaded;
            };
            const auto loadLstat = [&] {
                if (!lstatLoaded)
                    lstatLoaded = ::fstatat(dfd, raw, &lst, AT_SYMLINK_NOFOLLOW) == 0;
                return lstatLoaded;
            };

            if (entries.size() < limit) {
                bool candidate = false;
                if (definitelyNonDir) {
                    candidate = wanted.matchesName(name, false);
                } else if (definitelyDir) {
                    candidate = wanted.matchesName(name, true);
                } else {
                    const bool isDir = loadStat() && S_ISDIR(st.st_mode);
                    candidate = wanted.matchesName(name, isDir);
                }
                if (candidate) {
                    const QString fullPath = dirPrefix + name;
                    const bool ok = loadStat();
                    Entry e = entryFromStat(name, fullPath, ok ? &st : nullptr, mimes);
                    e.name = fullPath.startsWith(rootPrefix) ? fullPath.mid(rootPrefix.size()) : fullPath;
                    // Only what fits a file chooser's filter uses up the limit.
                    if (compiledFilter.matches(e))
                        entries.push_back(std::move(e));
                }
            }
            // At every entry, not just at matches: a match found early isn't
            // held back by a long stretch of folders with none.
            if (entries.size() > told)
                tell(false);
            // Other filesystems (a mounted drive, another machine, /proc
            // from /) and symlinked folders aren't walked into.
            if (definitelyDir || dtype == DT_UNKNOWN) {
                if (loadLstat() && S_ISDIR(lst.st_mode) && !S_ISLNK(lst.st_mode) && lst.st_dev == top.st_dev)
                    folders.push_back(dirPrefix + name);
            }
        }
        ::closedir(dir);
    }
    tell(true);
    sortEntries(entries, sort, locale);
    return entries;
}

QStringList FolderModel::sortModes() {
    // "modified" is newest first; it keeps its name so saved settings
    // from before "oldest" existed still read right.
    return {"name", "kind", "modified", "oldest", "size"};
}

static QCollator collatorFor(const QLocale &locale) {
    QCollator collator(locale);
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    return collator;
}

// The order a sort mode names, read once rather than on every compare.
struct Order {
    explicit Order(const QString &sort)
        : newest(sort == "modified"), oldest(sort == "oldest"), size(sort == "size"), kind(sort == "kind") {}
    bool newest, oldest, size, kind;

    // Whether `a` comes before `b`, `collate` comparing their names (or,
    // given true, their extensions) as a collator does.
    template <typename Collate>
    bool operator()(const FolderModel::Entry &a, const FolderModel::Entry &b, const Collate &collate) const {
        // Folders first in every order, as everywhere else on the desktop.
        if (a.isDir != b.isDir)
            return a.isDir;
        if (newest && a.modified != b.modified)
            return a.modified > b.modified;
        if (oldest && a.modified != b.modified)
            return a.modified < b.modified;
        // Biggest first; folders have no size of their own and stay by name.
        if (size && !a.isDir && a.size != b.size)
            return a.size > b.size;
        if (kind && !a.isDir) {
            if (a.kind != b.kind)
                return a.kind < b.kind;
            if (a.extension != b.extension)
                return collate(true) < 0;
        }
        return collate(false) < 0;
    }
};

void FolderModel::sortEntries(QVector<Entry> &entries, const QString &sort, const QLocale &locale) {
    if (entries.size() <= 1)
        return;
    // The C locale's keys keep the case its compare ignores.
    if (locale.language() == QLocale::C) {
        std::stable_sort(entries.begin(), entries.end(), before(sort, locale));
        return;
    }
    const QCollator collator = collatorFor(locale);
    const Order comesBefore(sort);
    const bool byNameOnly = !comesBefore.newest && !comesBefore.oldest && !comesBefore.size && !comesBefore.kind;
    // By name, every entry's key is needed up front. By date, size or kind,
    // compute name keys lazily on ties so sorting a large folder by date or
    // size skips generating thousands of unused ICU sort keys, while still
    // computing each key at most once when many entries tie.
    std::vector<std::optional<QCollatorSortKey>> names(entries.size());
    if (byNameOnly) {
        for (qsizetype i = 0; i < entries.size(); ++i)
            names[i].emplace(collator.sortKey(entries.at(i).name));
    }
    QHash<QString, QCollatorSortKey> extKeys;
    if (comesBefore.kind) {
        for (const Entry &e : std::as_const(entries))
            if (!e.isDir && !extKeys.contains(e.extension))
                extKeys.insert(e.extension, collator.sortKey(e.extension));
    }
    const auto nameKey = [&](qsizetype idx) -> const QCollatorSortKey & {
        if (!names[idx].has_value())
            names[idx].emplace(collator.sortKey(entries.at(idx).name));
        return *names[idx];
    };
    std::vector<qsizetype> order(entries.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](qsizetype i, qsizetype j) {
        return comesBefore(entries.at(i), entries.at(j), [&](bool extension) {
            return extension ? extKeys.find(entries.at(i).extension)->compare(*extKeys.find(entries.at(j).extension))
                             : nameKey(i).compare(nameKey(j));
        });
    });
    QVector<Entry> sorted;
    sorted.reserve(entries.size());
    for (qsizetype i : order)
        sorted.push_back(std::move(entries[i]));
    entries = std::move(sorted);
}

std::function<bool(const FolderModel::Entry &, const FolderModel::Entry &)> FolderModel::before(const QString &sort, const QLocale &locale) {
    auto collator = std::make_shared<QCollator>(collatorFor(locale));
    return [collator, order = Order(sort)](const Entry &a, const Entry &b) {
        return order(a, b, [&](bool extension) {
            return extension ? collator->compare(a.extension, b.extension) : collator->compare(a.name, b.name);
        });
    };
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
    entry.itemCount = 0;
    entry.peek.clear();
    DIR *dir = ::opendir(QFile::encodeName(entry.path).constData());
    if (!dir)
        return;

    static const QSet<QString> pictures{QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("png"),
                                        QStringLiteral("webp"), QStringLiteral("gif"), QStringLiteral("heic"),
                                        QStringLiteral("avif"), QStringLiteral("jxl")};
    // Single-pass count and top-2 lexicographically smallest picture names,
    // matching QDir::Name order without allocating or sorting every filename.
    QString best[2];
    int found = 0;
    while (dirent *ent = ::readdir(dir)) {
        const char *raw = ent->d_name;
        if (raw[0] == '.') {
            if (raw[1] == '\0' || (raw[1] == '.' && raw[2] == '\0'))
                continue;
            if (!showHidden)
                continue;
        }
        ++entry.itemCount;
        const char *dot = std::strrchr(raw, '.');
        if (!dot || dot[1] == '\0')
            continue;
        const QString ext = QString::fromLatin1(dot + 1).toLower();
        if (!pictures.contains(ext))
            continue;
        const QString name = QFile::decodeName(raw);
        if (found == 0) {
            best[0] = name;
            found = 1;
        } else if (found == 1) {
            if (name < best[0]) {
                best[1] = std::move(best[0]);
                best[0] = name;
            } else {
                best[1] = name;
            }
            found = 2;
        } else if (name < best[1]) {
            if (name < best[0]) {
                best[1] = std::move(best[0]);
                best[0] = name;
            } else {
                best[1] = name;
            }
        }
    }
    ::closedir(dir);
    const QString prefix = entry.path.endsWith(QLatin1Char('/')) ? entry.path : entry.path + QLatin1Char('/');
    for (int i = 0; i < found; ++i)
        entry.peek << prefix + best[i];
}

static int itemCountBounded(const QString &folder, bool showHidden, const std::function<bool()> &stop, int limit) {
    DIR *dir = ::opendir(QFile::encodeName(folder).constData());
    if (!dir)
        return 0;
    int count = 0;
    while (dirent *ent = ::readdir(dir)) {
        const char *raw = ent->d_name;
        if (raw[0] == '.') {
            if (raw[1] == '\0' || (raw[1] == '.' && raw[2] == '\0'))
                continue;
            if (!showHidden)
                continue;
        }
        if (stop && stop()) {
            ::closedir(dir);
            return -1;
        }
        ++count;
        if (limit > 0 && count > limit) {
            ::closedir(dir);
            return -2;
        }
    }
    ::closedir(dir);
    return count;
}

int FolderModel::itemCount(const QString &folder, bool showHidden, const std::function<bool()> &stop) {
    return itemCountBounded(folder, showHidden, stop, 0);
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
    // A search of a folder shows its matches as they're found.
    if (!query.isEmpty() && path != recentPath() && !isPluginView(path) && !isTrash(path)) {
        searchHere(generation);
        return;
    }
    m_shownQuery.clear();
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
        const CompiledFilter compiledFilter(filter);
        if (!isPluginView(path) && !compiledFilter.empty)
            entries.removeIf([&](const Entry &entry) { return !compiledFilter.matches(entry); });
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
            if (!compiledFilter.empty)
                items.removeIf([&](const Entry &entry) { return !compiledFilter.matches(entry); });
            open.insert(folder, items);
        }
        return std::make_tuple(entries, error, trashDirs, open, unread);
    }));
}

void FolderModel::countFolders(quint64 generation) {
    struct FolderRef {
        int row;
        QString path;
        int count = -1;
    };
    // Only the folders not counted yet: a listing just read, or one whose
    // counting was overtaken by a re-read (and so remembered without them).
    QVector<FolderRef> folders;
    for (int row = 0; row < m_entries.size(); ++row)
        if (m_entries[row].isDir && m_entries[row].itemCount < 0)
            folders.push_back({row, m_entries[row].path, -1});
    if (folders.isEmpty())
        return;

    const bool showHidden = m_showHidden;
    auto *watcher = new QFutureWatcher<QVector<FolderRef>>(this);
    connect(watcher, &QFutureWatcherBase::resultsReadyAt, this, [this, watcher, generation](int from, int to) {
        if (generation != m_generation)
            return;
        QVector<int> updated;
        for (int i = from; i < to; ++i) {
            for (const auto &summary : watcher->resultAt(i)) {
                int row = summary.row;
                if (row < 0 || row >= m_entries.size() || m_entries[row].path != summary.path)
                    row = indexOfPath(summary.path);
                if (row < 0)
                    continue;
                m_entries[row].itemCount = summary.count;
                updated.push_back(row);
            }
        }
        if (updated.isEmpty())
            return;
        std::sort(updated.begin(), updated.end());
        int i = 0;
        while (i < updated.size()) {
            int j = i;
            while (j + 1 < updated.size() && updated[j + 1] <= updated[j] + 1)
                ++j;
            emit dataChanged(index(updated[i]), index(updated[j]), {ItemCountRole});
            i = j + 1;
        }
        // QML re-reads its get() snapshots (the current item, which the
        // details column shows) on countChanged, and the counts they show
        // just arrived.
        emit countChanged();
    });
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        remember();
    });
    const auto latest = m_latest;
    watcher->setFuture(QtConcurrent::run([folders, showHidden, latest, generation](QPromise<QVector<FolderRef>> &promise) mutable {
        // Overtaken (another folder, or a re-read), the counts would be
        // thrown away, so it stops instead of listing every subfolder.
        const auto overtaken = [&] { return *latest != generation; };
        constexpr int fastPassLimit = 1024;
        QVector<FolderRef> batch;
        QVector<FolderRef> large;
        QElapsedTimer since;
        since.start();
        // Pass 1: count ordinary folders first and stream them in small
        // batches so one huge folder (e.g. node_modules) never holds back its
        // siblings' counts.
        for (auto &folder : folders) {
            if (overtaken())
                return;
            const int c = itemCountBounded(folder.path, showHidden, overtaken, fastPassLimit);
            if (c == -1)
                return;
            if (c == -2) {
                large.push_back(folder);
                continue;
            }
            folder.count = c;
            batch.push_back(folder);
            if (batch.size() >= 32 || since.elapsed() >= 40) {
                promise.addResult(std::move(batch));
                batch.clear();
                since.restart();
            }
        }
        if (!batch.isEmpty())
            promise.addResult(std::move(batch));
        // Pass 2: finish the huge folders and emit each as soon as it lands.
        for (auto &folder : large) {
            if (overtaken())
                return;
            const int c = itemCount(folder.path, showHidden, overtaken);
            if (c < 0)
                return;
            folder.count = c;
            promise.addResult(QVector<FolderRef>{folder});
        }
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
    const QSet<QString> previous = std::move(m_selected);
    m_selected = paths;
    if (!m_entries.isEmpty()) {
        // Emit dataChanged only for rows whose selection actually changed when
        // moving the cursor or toggling individual items in a large folder.
        if (previous.size() + m_selected.size() <= 16) {
            ensurePathIndex();
            QVector<int> changed;
            for (const QString &p : previous)
                if (!m_selected.contains(p))
                    if (const auto it = m_pathIndex.constFind(p); it != m_pathIndex.cend())
                        changed.push_back(*it);
            for (const QString &p : m_selected)
                if (!previous.contains(p))
                    if (const auto it = m_pathIndex.constFind(p); it != m_pathIndex.cend())
                        changed.push_back(*it);
            std::sort(changed.begin(), changed.end());
            int i = 0;
            while (i < changed.size()) {
                int j = i;
                while (j + 1 < changed.size() && changed[j + 1] == changed[j] + 1)
                    ++j;
                emit dataChanged(index(changed[i]), index(changed[j]), {SelectedRole});
                i = j + 1;
            }
        } else {
            emit dataChanged(index(0), index(m_entries.size() - 1), {SelectedRole});
        }
    }
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

void FolderModel::searchHere(quint64 generation) {
    const QString path = m_path, query = m_query, sort = m_sort;
    const bool showHidden = m_showHidden;
    const QStringList filter = m_filter;
    const QLocale locale;
    const auto latest = m_latest;
    // A new search starts empty and fills in as matches are found. The same
    // one again (the folder changed under it) leaves its matches up, and
    // replaces them only if they've changed once it's done.
    const bool fresh = query != m_shownQuery;
    m_shownQuery = query;
    m_staleClear.stop();
    if (fresh)
        clearEntries();
    setError({});
    auto *watcher = new QFutureWatcher<QVector<Entry>>(this);
    // Told once, at its first matches, so the cursor goes to the first.
    auto announced = std::make_shared<bool>(false);
    connect(watcher, &QFutureWatcherBase::resultsReadyAt, this, [this, watcher, generation, fresh, announced](int from, int to) {
        if (generation != m_generation || !fresh)
            return;
        for (int i = from; i < to; ++i)
            insertFound(watcher->resultAt(i));
        if (!*announced && !m_entries.isEmpty()) {
            *announced = true;
            emit loaded();
        }
    });
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, fresh, announced] {
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        if (!fresh) {
            QVector<Entry> all;
            for (const QVector<Entry> &found : watcher->future().results())
                all += found;
            sortEntries(all, m_sort);
            if (!sameListing(m_entries, all))
                setEntries(all);
            // What was selected and is no longer found isn't selected.
            QSet<QString> still;
            for (const Entry &e : std::as_const(m_entries))
                if (m_selected.contains(e.path))
                    still.insert(e.path);
            setSelected(still);
        }
        setLoading(false);
        emit loaded();
        countFolders(generation);
    });
    watcher->setFuture(QtConcurrent::run(&FolderModel::listingPool(), [path, query, showHidden, sort, filter, locale, latest, generation](QPromise<QVector<Entry>> &promise) {
        search(path, query, showHidden, sort, [&] { return *latest != generation; }, 1000, locale, filter,
               [&](const QVector<Entry> &found) { promise.addResult(found); });
    }));
}

void FolderModel::insertFound(const QVector<Entry> &found) {
    if (found.isEmpty())
        return;
    if (m_entries.isEmpty()) {
        QVector<Entry> sorted = found;
        sortEntries(sorted, m_sort);
        beginInsertRows({}, 0, int(sorted.size()) - 1);
        m_entries = std::move(sorted);
        m_pathIndexDirty = true;
        endInsertRows();
        emit countChanged();
        return;
    }
    const auto order = before(m_sort);
    for (const Entry &e : found) {
        const int row = int(std::upper_bound(m_entries.begin(), m_entries.end(), e, order) - m_entries.begin());
        beginInsertRows({}, row, row);
        m_entries.insert(row, e);
        m_pathIndexDirty = true;
        endInsertRows();
    }
    emit countChanged();
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
            m_pathIndexDirty = true;
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
            m_pathIndexDirty = true;
            endRemoveRows();
        }
        const QVector<Entry> rows = flatten(items, m_open, m_entries[row].depth + 1);
        if (rows.isEmpty())
            return;
        beginInsertRows({}, row + 1, row + int(rows.size()));
        const qsizetype oldSize = m_entries.size();
        const qsizetype insertAt = row + 1;
        const qsizetype add = rows.size();
        m_entries.resize(oldSize + add);
        std::move_backward(m_entries.begin() + insertAt, m_entries.begin() + oldSize, m_entries.end());
        std::copy(rows.cbegin(), rows.cend(), m_entries.begin() + insertAt);
        m_pathIndexDirty = true;
        endInsertRows();
        emit countChanged();
        countFolders(generation);
    });
    watcher->setFuture(QtConcurrent::run(&FolderModel::listingPool(), [folder, showHidden, sort, filter, latest, generation, locale] {
        QString error;
        auto items = list(folder, showHidden, sort, &error, [&] { return *latest != generation; }, locale);
        const CompiledFilter compiledFilter(filter);
        if (!compiledFilter.empty)
            items.removeIf([&](const Entry &entry) { return !compiledFilter.matches(entry); });
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
    ensurePathIndex();
    return m_pathIndex.value(path, -1);
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
