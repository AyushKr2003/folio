#pragma once

#include <QAbstractListModel>
#include <functional>
#include <QDateTime>
#include <QFileSystemWatcher>
#include <QLocale>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>
#include <QVariantList>
#include <QSet>
#include <QVector>
#include <atomic>
#include <memory>

// One folder's contents, listed off the UI thread so a slow mount (an SFTP
// share on another machine, say) never freezes the window. Listing happens in
// two passes: the entries first, then each subfolder's item count.
class QFileInfo;
class QMimeDatabase;

class FolderModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString path READ path WRITE setPath NOTIFY pathChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(bool showHidden READ showHidden WRITE setShowHidden NOTIFY showHiddenChanged)
    Q_PROPERTY(QString sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    // Set, the folder lists what matches it here and in every folder below.
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    // The items a plugin's view lists, as Plugins::views gives them.
    Q_PROPERTY(QVariantList items READ items WRITE setItems NOTIFY itemsChanged)
    // A file chooser's filter: only files that fit it are listed (see fits()).
    Q_PROPERTY(QStringList filter READ filter WRITE setFilter NOTIFY filterChanged)
    // Whether folders here can be opened in place, as a tree (the list
    // view's): in a folder, not a search, Recents, the trash or a plugin's
    // view.
    Q_PROPERTY(bool expandable READ expandable NOTIFY expandableChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        PathRole,
        IsDirRole,
        SizeRole,
        ModifiedRole,
        KindRole,
        MimeRole,
        ExtensionRole,
        ItemCountRole,
        SelectedRole,
        IconRole,
        DepthRole,
        ExpandedRole,
    };

    struct Entry {
        QString name;
        QString path;
        bool isDir = false;
        qint64 size = 0;
        QDateTime modified;
        QString kind;
        QString mime;
        // A file's extension; a plugin item's subtitle.
        QString extension;
        // A plugin item's glyph and its rows for the details column.
        QString icon;
        QVariantList details;
        int itemCount = -1;
        // The first pictures in a folder, for the preview window.
        QStringList peek;
        // How deep in the tree: 0 for the folder's own items, 1 for those of
        // a folder opened in place under them, and so on.
        int depth = 0;
    };

    explicit FolderModel(QObject *parent = nullptr);
    ~FolderModel() override;

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString path() const { return m_path; }
    int count() const { return m_entries.size(); }
    bool loading() const { return m_loading; }
    QString error() const { return m_error; }
    bool showHidden() const { return m_showHidden; }
    void setShowHidden(bool show);
    QString sortMode() const { return m_sort; }
    void setSortMode(const QString &sort);
    // Goes to `path` already in `sort`, listing it once.
    void setPath(const QString &path, const QString &sort);
    QString query() const { return m_query; }
    void setQuery(const QString &query);
    QVariantList items() const { return m_items; }
    void setItems(const QVariantList &items);
    QStringList filter() const { return m_filter; }
    void setFilter(const QStringList &filter);

    void setPath(const QString &path);
    Q_INVOKABLE void reload();
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int indexOf(const QString &name) const;
    // By path, for Recents, where two files from different folders can
    // share a name.
    Q_INVOKABLE int indexOfPath(const QString &path) const;

    // The selection, kept by path so it survives a re-read or a re-sort.
    // The views keep the cursor (the current item) themselves.
    int selectedCount() const { return m_selected.size(); }
    Q_INVOKABLE void selectOnly(int row);
    Q_INVOKABLE void toggleSelected(int row);
    Q_INVOKABLE void selectRange(int from, int to, bool add);
    Q_INVOKABLE void selectRows(const QList<int> &rows);
    Q_INVOKABLE void selectAll();
    Q_INVOKABLE void clearSelection();
    Q_INVOKABLE bool isSelected(int row) const;
    Q_INVOKABLE QStringList selectedPaths() const;

    // The tree: a folder opened in place lists its items right under it, a
    // level deeper, read in the background; closed, they go, and so does
    // any folder opened inside it. Rows keep being rows, so selecting,
    // file work and the cursor treat them as any other. Going to another
    // folder closes them all.
    bool expandable() const;
    Q_INVOKABLE void setExpanded(int row, bool expanded);
    Q_INVOKABLE bool isExpanded(int row) const;
    Q_INVOKABLE void collapseAll();
    // The row of the folder `row` is listed under, or -1 at the top.
    Q_INVOKABLE int parentRow(int row) const;
    // A folder's items with the open folders' own under them, `depth` deep.
    static QVector<Entry> flatten(const QVector<Entry> &entries, const QHash<QString, QVector<Entry>> &open, int depth = 0);

    // Listings run here, not on the global pool, so folder counts and file
    // transfers can't hold up the folder just opened.
    static QThreadPool &listingPool();

    // Synchronous building blocks, used by the background passes and tests.
    // A listing stops, empty, as soon as `stop` returns true. Names sort
    // by `locale`'s alphabet; a background pass passes the one it was
    // started with, because the default locale may only be read on the
    // thread that sets it.
    static QVector<Entry> list(const QString &path, bool showHidden, const QString &sort, QString *error,
                               const std::function<bool()> &stop = {}, const QLocale &locale = QLocale());
    static Entry entryFor(const QFileInfo &info, const QMimeDatabase &mimes);
    // An entry as QML reads it, by the names of its fields.
    static QVariantMap toMap(const Entry &entry);
    // A folder's item count and first pictures, for the preview window.
    static void summarize(Entry &entry, bool showHidden);
    // Only how many items a folder holds, for the folder counts, which have
    // no use for its pictures or their order. Stopped partway, it's -1 (unknown).
    static int itemCount(const QString &folder, bool showHidden, const std::function<bool()> &stop = {});
    static QStringList sortModes();
    static void sortEntries(QVector<Entry> &entries, const QString &sort, const QLocale &locale = QLocale());
    // The Recents view: a path that isn't a folder, listing recently used files.
    static QString recentPath() { return QStringLiteral("recent:"); }
    static QString recentFile();
    static QVector<Entry> listRecent(const QString &xbel, bool showHidden, int limit = 200,
                                     const std::function<bool()> &stop = {});
    // A plugin's view: plugin:<id>, listing `items` in the plugin's order,
    // each an item of the "item" kind at plugin:<id>/<item id>. The ones
    // whose names contain `query` (ignoring case), or all of them.
    static bool isPluginView(const QString &path) { return path.startsWith("plugin:") && !path.contains('/'); }
    static QVector<Entry> listItems(const QVariantList &items, const QString &query = {});
    // The trash is the home trash's files folder, and it lists every trash's:
    // what's deleted on another drive goes to that drive's own.
    static bool isTrash(const QString &path);
    static QVector<Entry> listTrash(const QStringList &trashDirs, const QString &query, bool showHidden,
                                    const QString &sort, const std::function<bool()> &stop = {}, int limit = 1000,
                                    const QLocale &locale = QLocale(), const QStringList &filter = {});
    // Recents and plugins' views: lists of their own, not folders.
    static bool isView(const QString &path) { return path == recentPath() || isPluginView(path); }
    static QString kindForMime(const QString &mime);
    // Whether an entry fits a filter: folders always do, and a file when any
    // pattern matches, a MIME type ("image/png", "image/*", or a type it
    // inherits) when the pattern has a slash, else a glob for its name,
    // ignoring case. An empty filter fits everything.
    static bool fits(const QStringList &filter, const Entry &entry);
    // Everything under `root` whose name contains `query` (ignoring case),
    // named by its path from `root`. Stops at `limit` matches, at other
    // filesystems and at symlinks, and as soon as `stop` returns true.
    static QVector<Entry> search(const QString &root, const QString &query, bool showHidden, const QString &sort,
                                 const std::function<bool()> &stop = {}, int limit = 1000,
                                 const QLocale &locale = QLocale(), const QStringList &filter = {});

signals:
    void pathChanged();
    void countChanged();
    void loadingChanged();
    void errorChanged();
    void showHiddenChanged();
    void sortModeChanged();
    void queryChanged();
    void itemsChanged();
    void filterChanged();
    void selectionChanged();
    void loaded();
    void expandableChanged();
    // A folder opened in place couldn't be read (or read again), and was
    // closed: `message` says which, and why.
    void expandFailed(const QString &message);

private:
    void setLoading(bool loading);
    void setEntries(const QVector<Entry> &entries);
    void clearEntries();
    QString cacheKey() const;
    void remember();
    static bool sameListing(const QVector<Entry> &a, const QVector<Entry> &b);
    void setError(const QString &error);
    void countFolders(quint64 generation);

    QString m_path;
    QVector<Entry> m_entries;
    QSet<QString> m_selected;
    // The folders opened in place, by path, and what each was last read to
    // hold. One just opened is in m_expanded until its listing arrives.
    QSet<QString> m_expanded;
    QHash<QString, QVector<Entry>> m_open;
    // Each opening's own number, while it's read, so a reading from before
    // the folder was closed and opened again is dropped.
    QHash<QString, quint64> m_opening;
    quint64 m_openings = 0;
    // Closes a folder that couldn't be read, saying why.
    void failExpanded(const QString &folder, const QString &error);
    void forgetExpanded();
    void setSelected(const QSet<QString> &paths);
    bool m_loading = false;
    QString m_error;
    bool m_showHidden = false;
    QString m_sort = "name";
    QString m_query;
    QVariantList m_items;
    QStringList m_filter;
    quint64 m_generation = 0;
    // The newest listing's generation, shared with the background listing,
    // search and folder counts, so work that's been overtaken (more typing,
    // another folder, the model gone) stops early.
    std::shared_ptr<std::atomic<quint64>> m_latest = std::make_shared<std::atomic<quint64>>(0);
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QTimer m_staleClear;
};
