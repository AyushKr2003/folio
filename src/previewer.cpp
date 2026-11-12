#include "previewer.h"

#include "fileops.h"
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QMimeDatabase>
#include <QUrl>
#include <QtConcurrent>
#include <cstdio>
#include <unistd.h>

Previewer::Previewer(QObject *parent) : QObject(parent) {}

QString Previewer::viewerFor(const QString &mime, bool isDir) {
    if (isDir)
        return "folder";
    if (mime == "application/pdf")
        return "pdf";
    if (mime.startsWith("video/"))
        return "video";
    if (mime.startsWith("audio/"))
        return "audio";
    if (mime.startsWith("image/")) {
        // Formats that can move are drawn by AnimatedImage, which shows the
        // still ones just as well.
        if (mime == "image/gif" || mime == "image/webp" || mime == "image/apng")
            return "animated";
        // Qt decodes the common formats at full size; the rest (HEIC, JPEG
        // XL, raw camera files) go through the installed thumbnailers at the
        // largest size the cache spec has.
        if (QImageReader::supportedMimeTypes().contains(mime.toLatin1()))
            return "image";
        return "thumbnail";
    }
    if (mime == "text/markdown")
        return "markdown";
    const QMimeType type = QMimeDatabase().mimeTypeForName(mime);
    if (mime.startsWith("text/") || type.inherits("text/plain"))
        return "text";
    return "other";
}

QString Previewer::readText(const QString &path, qint64 limit) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QByteArray data = file.read(limit + 1);
    if (data.left(8192).contains('\0'))
        return {};
    // Cut where a line ends, not inside one; with no line end to cut at,
    // at least not inside a character.
    if (data.size() > limit) {
        data.truncate(limit);
        if (const qsizetype end = data.lastIndexOf('\n'); end >= 0) {
            data.truncate(end + 1);
        } else {
            // Back to the start of the last character, then keep it only
            // if all of its bytes made it in.
            qsizetype start = data.size() - 1;
            while (start > 0 && start > data.size() - 4 && (uchar(data.at(start)) & 0xC0) == 0x80)
                --start;
            const uchar lead = uchar(data.at(start));
            const int length = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
            if (data.size() - start < length)
                data.truncate(start);
        }
    }
    return QString::fromUtf8(data);
}

QString Previewer::markdownOpening(const QString &text, int cells) {
    // A table cell is counted by its | (a row has one more than its cells,
    // which errs on the safe side). Code in fences is shown as it is, never
    // laid out as a table, so its | don't count. As Markdown reads them, a
    // fence is three or more ` or ~, indented less than four spaces (more is
    // indented code), and only a run of the same mark at least as long, with
    // nothing after it, closes it.
    qsizetype at = 0;
    QChar fence;
    qsizetype fenceLength = 0;
    while (at < text.size()) {
        qsizetype end = text.indexOf('\n', at);
        end = end < 0 ? text.size() : end + 1;
        const QStringView line = QStringView(text).mid(at, end - at);
        qsizetype indent = 0;
        while (indent < 4 && indent < line.size() && line.at(indent) == ' ')
            ++indent;
        const QChar mark = indent < line.size() ? line.at(indent) : QChar();
        qsizetype run = 0;
        while (indent < 4 && indent + run < line.size() && line.at(indent + run) == mark && (mark == '`' || mark == '~'))
            ++run;
        const QStringView rest = line.mid(indent + run).trimmed();
        if (fence.isNull() && run >= 3 && !(mark == '`' && rest.contains('`'))) {
            fence = mark;
            fenceLength = run;
        } else if (!fence.isNull() && mark == fence && run >= fenceLength && rest.isEmpty()) {
            fence = QChar();
        } else if (fence.isNull()) {
            cells -= line.count('|');
        }
        // One line is a paragraph, however many | it has.
        if (cells < 0)
            return text.left(at > 0 ? at : end);
        at = end;
    }
    return text;
}

void Previewer::start(const QString &path, bool showHidden, const QString &sort) {
    m_showHidden = showHidden;
    m_sort = sort;
    show(path);
}

void Previewer::show(const QString &path) {
    const QString absolute = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < m_entries.size(); ++i) {
        if (m_entries[i].path == absolute) {
            if (i != m_index)
                setIndex(i, false);
            return;
        }
    }

    // A file from another folder: show it alone, then fill in its siblings.
    m_entries = {FolderModel::entryFor(QFileInfo(absolute), QMimeDatabase())};
    setIndex(0, false);
    relist();
}

void Previewer::relist() {
    if (m_index < 0 || m_index >= m_entries.size())
        return;
    const quint64 generation = ++m_generation;
    const QString folder = QFileInfo(m_entries.at(m_index).path).absolutePath();
    const bool showHidden = m_showHidden;
    const QString sort = m_sort;
    const QLocale locale;
    auto *watcher = new QFutureWatcher<QVector<FolderModel::Entry>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        const auto entries = watcher->result();
        const QString current = m_entries.at(m_index).path;
        for (int i = 0; i < entries.size(); ++i) {
            if (entries[i].path == current) {
                // Keep the entry already shown: it has the folder summary.
                const FolderModel::Entry shown = m_entries.at(m_index);
                m_entries = entries;
                m_entries[i] = shown;
                m_index = i;
                emit listed();
                return;
            }
        }
    });
    watcher->setFuture(QtConcurrent::run([folder, showHidden, sort, locale] {
        return FolderModel::list(folder, showHidden, sort, nullptr, {}, locale);
    }));
}

QVariantMap Previewer::item() const {
    if (m_index < 0 || m_index >= m_entries.size())
        return {};
    // Only the preview fills in a folder's first pictures (in setIndex), so
    // only its items carry them; the folder model's would always be empty.
    const FolderModel::Entry &e = m_entries.at(m_index);
    QVariantMap map = FolderModel::toMap(e);
    map.insert("peek", e.peek);
    return map;
}

QString Previewer::url() const {
    if (m_index < 0 || m_index >= m_entries.size())
        return {};
    return QUrl::fromLocalFile(m_entries.at(m_index).path).toString();
}

QString Previewer::type() const {
    if (m_index < 0 || m_index >= m_entries.size())
        return {};
    return QMimeDatabase().mimeTypeForName(m_entries.at(m_index).mime).comment();
}

void Previewer::next() {
    if (m_index + 1 < m_entries.size())
        setIndex(m_index + 1, true);
}

void Previewer::previous() {
    if (m_index > 0)
        setIndex(m_index - 1, true);
}

void Previewer::open() {
    if (m_index >= 0)
        // The same lookup as opening from the folder: the type's default
        // app, or its parent type's.
        if (const QString problem = FileOps::openInDefaultApp(m_entries.at(m_index).path); !problem.isEmpty())
            qWarning("%s", qPrintable(problem));
}

void Previewer::setIndex(int index, bool report) {
    m_index = index;
    const FolderModel::Entry e = m_entries.at(index);
    m_viewer = viewerFor(e.mime, e.isDir);
    m_text.clear();

    // A folder's count and first pictures, and a text's opening part, are
    // read off the UI thread: a big folder, or any file on a slow mount,
    // would hold up the window until they were in.
    const quint64 generation = ++*m_reading;
    const bool count = e.isDir && e.itemCount < 0;
    const bool text = m_viewer == "text" || m_viewer == "markdown";
    if (count || text) {
        auto *watcher = new QFutureWatcher<QPair<FolderModel::Entry, QString>>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, text] {
            watcher->deleteLater();
            if (generation != *m_reading)
                return; // Stepped on to another item while this was read.
            const auto [entry, read] = watcher->result();
            m_entries[m_index] = entry;
            m_text = read;
            // A ".txt" that turns out to be binary is shown as a plain file.
            if (text && m_text.isEmpty() && entry.size > 0)
                m_viewer = "other";
            emit changed();
        });
        const bool showHidden = m_showHidden;
        const bool markdown = m_viewer == "markdown";
        watcher->setFuture(QtConcurrent::run([entry = e, showHidden, count, text, markdown,
                                              latest = m_reading, generation]() mutable {
            if (*latest != generation)
                return qMakePair(entry, QString());
            if (count)
                FolderModel::summarize(entry, showHidden);
            const QString read = text ? readText(entry.path) : QString();
            return qMakePair(entry, markdown ? markdownOpening(read) : read);
        }));
    }
    emit changed();
    emit listed();

    if (report && m_reporting) {
        std::fprintf(stdout, "%s\n", e.path.toUtf8().constData());
        std::fflush(stdout);
    }
}

void Previewer::connectToFolio() {
    m_reporting = true;
    m_stdin = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    connect(m_stdin, &QSocketNotifier::activated, this, [this] {
        char buffer[4096];
        const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof buffer);
        if (n <= 0) {
            // Folio went away; there's nothing left to follow.
            m_stdin->setEnabled(false);
            return;
        }
        m_pending.append(buffer, n);
        int newline;
        while ((newline = m_pending.indexOf('\n')) >= 0) {
            const QString path = QString::fromUtf8(m_pending.left(newline));
            m_pending.remove(0, newline + 1);
            if (!path.isEmpty())
                show(path);
        }
    });
}
