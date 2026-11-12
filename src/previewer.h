#pragma once

#include <QObject>
#include <QSocketNotifier>
#include <QVariantMap>
#include <QVector>

#include "foldermodel.h"

// The state behind `folio preview`: the file being looked at, its siblings
// to step through, and which viewer shows it. It runs in its own process, as
// Sushi does for Nautilus, so a video or PDF that crashes a decoder takes the
// preview window down, never the file manager.
//
// Talks to the Folio window that started it over stdin and stdout, one path
// per line: Folio writes the item selected in its grid, the preview writes
// the item it steps to, and each side follows the other.
class Previewer : public QObject {
    Q_OBJECT
    // Listing the folder only changes where the item sits among its
    // siblings, so it has a signal of its own: re-reading the item would set
    // a playing video's source again, which stops it.
    Q_PROPERTY(int count READ count NOTIFY listed)
    Q_PROPERTY(int index READ index NOTIFY listed)
    Q_PROPERTY(QVariantMap item READ item NOTIFY changed)
    Q_PROPERTY(QString viewer READ viewer NOTIFY changed)
    Q_PROPERTY(QString url READ url NOTIFY changed)
    Q_PROPERTY(QString text READ text NOTIFY changed)
    Q_PROPERTY(QString type READ type NOTIFY changed)

public:
    explicit Previewer(QObject *parent = nullptr);

    // Shows the file right away, then lists its folder in the background so
    // the arrow keys can step to its neighbours.
    void start(const QString &path, bool showHidden, const QString &sort);
    // Reads paths from stdin and reports steps on stdout.
    void connectToFolio();
    // Shows another file; one in the same folder keeps the list.
    void show(const QString &path);
    // Lists the folder again, keeping the file shown: after a language
    // switch, names sort by the new alphabet.
    void relist();

    int count() const { return m_entries.size(); }
    int index() const { return m_index; }
    QVariantMap item() const;
    QString viewer() const { return m_viewer; }
    QString url() const;
    QString text() const { return m_text; }
    QString type() const;

    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void open();

    // Which view shows a type: image, animated, thumbnail, video, audio,
    // pdf, markdown, text, folder or other.
    static QString viewerFor(const QString &mime, bool isDir);
    // The opening part of a text file, ending at a line when there's more,
    // or nothing when it looks binary. Laying out text takes the UI thread
    // about a third of a millisecond per KB, so the preview reads 128 KB.
    static QString readText(const QString &path, qint64 limit = 128 * 1024);
    // The top of a Markdown text up to the line where its tables pass
    // `cells` cells: laid out, each costs the UI thread about as much as a
    // whole line of prose, and a long table froze the preview for seconds.
    static QString markdownOpening(const QString &text, int cells = 600);

signals:
    void changed();
    void listed();

private:
    void setIndex(int index, bool report);

    QVector<FolderModel::Entry> m_entries;
    int m_index = -1;
    QString m_viewer;
    QString m_text;
    bool m_showHidden = false;
    QString m_sort = "name";
    quint64 m_generation = 0;
    // The newest item read in the background; older readings are dropped,
    // or skipped when they haven't started.
    std::shared_ptr<std::atomic<quint64>> m_reading = std::make_shared<std::atomic<quint64>>(0);
    bool m_reporting = false;
    QSocketNotifier *m_stdin = nullptr;
    QByteArray m_pending;
};
