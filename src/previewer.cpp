#include "previewer.h"

#include "fileops.h"
#include <QDeadlineTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QImageReader>
#include <QMimeDatabase>
#include <QProcess>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent>
#include <cstdio>
#include <unistd.h>

Previewer::Previewer(QObject *parent) : Previewer(FileOps::openInDefaultApp, parent) {}

Previewer::Previewer(Opener opener, QObject *parent) : QObject(parent), m_opener(std::move(opener)) {
    Q_INIT_RESOURCE(qmlcache_folio_core);
}

Previewer::~Previewer() {
    *m_listing = 0;
    *m_reading = 0;
}

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
        static const QSet<QByteArray> supported = [] {
            const auto list = QImageReader::supportedMimeTypes();
            return QSet<QByteArray>(list.cbegin(), list.cend());
        }();
        if (supported.contains(mime.toLatin1()))
            return "image";
        return "thumbnail";
    }
    if (listsArchive(mime))
        return "archive";
    if (mime == "text/markdown")
        return "markdown";
    if (FolderModel::kindForMime(mime) == "text")
        return "text";
    return "other";
}

bool Previewer::listsArchive(const QString &mime) {
    // Whatever Folio calls an archive (every compressed tar among them),
    // but a lone compressed file (notes.txt.gz), which bsdtar can't list.
    static const QStringList single{"application/gzip", "application/x-xz", "application/zstd"};
    return FolderModel::kindForMime(mime) == "archive" && !single.contains(mime);
}

// A line of `bsdtar -t` back to the name it escapes: `\\` is a backslash
// and `\303` a byte, so `caf\303\251 \\ two` reads `café \ two`.
static QString unescaped(QByteArrayView line) {
    const auto octal = [&](qsizetype at) { return at < line.size() && line[at] >= '0' && line[at] <= '7'; };
    QByteArray name;
    for (qsizetype i = 0; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size() && line[i + 1] == '\\') {
            name += '\\';
            ++i;
        } else if (line[i] == '\\' && octal(i + 1) && octal(i + 2) && octal(i + 3)) {
            name += char((line[i + 1] - '0') * 64 + (line[i + 2] - '0') * 8 + (line[i + 3] - '0'));
            i += 3;
        } else {
            name += line[i];
        }
    }
    return QString::fromUtf8(name);
}

QString Previewer::archiveNames(const QString &path, int shown) {
    QProcess bsdtar;
    // UTF-8 names print as they are, whatever Folio's language.
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("LC_ALL", "C.UTF-8");
    bsdtar.setProcessEnvironment(environment);
    // No terminal or input to ask for a passphrase on.
    bsdtar.setStandardInputFile(QProcess::nullDevice());
    // An absolute path is never read as an option, or as "-" for stdin.
    bsdtar.start("bsdtar", {"-tf", QFileInfo(path).absoluteFilePath()});
    // Read as it comes, and stopped once there's enough to show: a huge
    // listing is never held whole, nor waited out.
    const QDeadlineTimer deadline(5000);
    QStringList names;
    qint64 bytes = 0;
    bool full = false, done = false;
    const auto take = [&](const QByteArray &line) {
        QString name = unescaped(QByteArrayView(line).chopped(line.endsWith('\n')));
        // The "./" a tar made with `-C folder .` starts with, or an ISO's ".", is the archive itself.
        if (name.startsWith("./"))
            name.remove(0, 2);
        if (name.isEmpty() || name == ".")
            return;
        full = names.size() == shown || (bytes += line.size()) > 256 * 1024;
        names << (full ? QString("…") : name);
    };
    while (!done) {
        while (!full && bsdtar.canReadLine())
            take(bsdtar.readLine());
        if (full)
            break;
        if (bsdtar.state() == QProcess::NotRunning) {
            take(bsdtar.readAll());
            done = true;
        } else if (deadline.hasExpired()) {
            break;
        } else {
            bsdtar.waitForReadyRead(int(deadline.remainingTime()));
        }
    }
    const bool listed = full || (done && bsdtar.exitStatus() == QProcess::NormalExit && bsdtar.exitCode() == 0);
    bsdtar.kill();
    bsdtar.waitForFinished(1000);
    return listed && !names.isEmpty() ? names.join('\n') : tr("Couldn't list this archive.");
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
    if (const auto it = m_pathIndex.constFind(absolute); it != m_pathIndex.cend()) {
        if (*it != m_index)
            setIndex(*it, false);
        return;
    }

    // A file from another folder: show it alone, then fill in its siblings.
    m_entries = {FolderModel::entryFor(QFileInfo(absolute), QMimeDatabase())};
    m_pathIndex = {{absolute, 0}};
    setIndex(0, false);
    relist();
}

void Previewer::relist() {
    if (m_index < 0 || m_index >= m_entries.size())
        return;
    const quint64 generation = ++m_generation;
    *m_listing = generation;
    const QString folder = QFileInfo(m_entries.at(m_index).path).absolutePath();
    const bool showHidden = m_showHidden;
    const QString sort = m_sort;
    const QLocale locale;
    const auto latest = m_listing;
    auto *watcher = new QFutureWatcher<QVector<FolderModel::Entry>>(this);
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation] {
        watcher->deleteLater();
        if (generation != m_generation)
            return;
        const auto entries = watcher->result();
        const QString current = m_entries.at(m_index).path;
        QHash<QString, int> indexByPath;
        indexByPath.reserve(entries.size());
        int found = -1;
        for (int i = 0; i < entries.size(); ++i) {
            indexByPath.insert(entries[i].path, i);
            if (entries[i].path == current)
                found = i;
        }
        if (found >= 0) {
            // Keep the entry already shown: it has the folder summary.
            const FolderModel::Entry shown = m_entries.at(m_index);
            m_entries = entries;
            m_entries[found] = shown;
            m_pathIndex = std::move(indexByPath);
            m_index = found;
            emit listed();
        }
    });
    watcher->setFuture(QtConcurrent::run(&FolderModel::listingPool(), [folder, showHidden, sort, locale, latest, generation] {
        return FolderModel::list(folder, showHidden, sort, nullptr, [&] { return *latest != generation; }, locale);
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
    if (!m_readType.isEmpty()) {
        map.insert("mime", m_readType);
        map.insert("kind", FolderModel::kindForMime(m_readType));
    }
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
    return QMimeDatabase().mimeTypeForName(m_readType.isEmpty() ? m_entries.at(m_index).mime : m_readType).comment();
}

void Previewer::next() { move(Move::Next); }
void Previewer::previous() { move(Move::Previous); }
void Previewer::stepRow(int direction) { move(direction < 0 ? Move::Up : Move::Down); }

void Previewer::move(Move move) {
    // Folio answers within milliseconds, sooner than a key repeats.
    if (m_awaitingStep)
        return;
    // On its own, the preview has no rows: ↑ and ↓ go to the previous and
    // next file.
    if (!m_reporting && (move == Move::Up || move == Move::Down))
        move = move == Move::Up ? Move::Previous : Move::Next;
    switch (move) {
    case Move::Previous:
        if (m_index > 0)
            setIndex(m_index - 1, true);
        break;
    case Move::Next:
        if (m_index + 1 < m_entries.size())
            setIndex(m_index + 1, true);
        break;
    case Move::Up:
    case Move::Down: {
        m_awaitingStep = true;
        // A reply that never comes (a Folio too old to send it, or stuck)
        // stills the arrows for a second, not for good.
        const quint64 step = ++m_step;
        QTimer::singleShot(1000, this, [this, step] {
            if (step == m_step)
                stepped();
        });
        report(move == Move::Up ? "step -1" : "step 1");
        break;
    }
    }
}

void Previewer::stepped() {
    m_awaitingStep = false;
    ++m_step;
}

void Previewer::receive(const QString &line) {
    if (line == "stepped")
        stepped();
    else if (!line.isEmpty())
        show(line);
}

void Previewer::report(const QString &line) {
    emit reported(line);
    if (m_overStdio) {
        std::fprintf(stdout, "%s\n", line.toUtf8().constData());
        std::fflush(stdout);
    }
}

void Previewer::open() {
    if (m_index >= 0) {
        // The same lookup as opening from the folder: the type's default
        // app, or its parent type's.
        if (const QString problem = m_opener(m_entries.at(m_index).path); !problem.isEmpty())
            qWarning("%s", qPrintable(problem));
        else
            emit opened();
    }
}

void Previewer::setIndex(int index, bool report) {
    m_index = index;
    const FolderModel::Entry e = m_entries.at(index);
    m_viewer = viewerFor(e.mime, e.isDir);
    m_text.clear();
    m_readType.clear();
    m_readingText = false;

    // A folder's count and first pictures, a text's opening part and an
    // archive's names are read off the UI thread: a big folder, or any file
    // on a slow mount, would hold up the window until they were in.
    const quint64 generation = ++*m_reading;
    const bool count = e.isDir && e.itemCount < 0;
    const bool text = m_viewer == "text" || m_viewer == "markdown" || m_viewer == "archive";
    // A file its extension could not name is named by what it holds, read with the rest and kept apart from the listing.
    const bool unnamed = e.mime == "application/octet-stream";
    if (count || text || unnamed) {
        // A file its name gave no type may turn out to be a long text.
        m_readingText = text || unnamed;
        auto *watcher = new QFutureWatcher<QPair<FolderModel::Entry, QString>>(this);
        connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, generation, unnamed] {
            watcher->deleteLater();
            if (generation != *m_reading)
                return; // Stepped on to another item while this was read.
            const auto [entry, read] = watcher->result();
            if (unnamed)
                m_readType = entry.mime;
            else
                m_entries[m_index] = entry;
            m_text = read;
            m_viewer = viewerFor(entry.mime, entry.isDir);
            m_readingText = false;
            // A ".txt" that turns out to be binary is shown as a plain file.
            if ((m_viewer == "text" || m_viewer == "markdown") && m_text.isEmpty() && entry.size > 0)
                m_viewer = "other";
            emit changed();
        });
        const bool showHidden = m_showHidden;
        watcher->setFuture(QtConcurrent::run([entry = e, showHidden, count,
                                              latest = m_reading, generation]() mutable {
            if (*latest != generation)
                return qMakePair(entry, QString());
            if (count)
                FolderModel::summarize(entry, showHidden);
            entry.mime = FolderModel::typeByContent(entry.path, entry.mime);
            const QString viewer = viewerFor(entry.mime, entry.isDir);
            const QString read = viewer == "archive" ? archiveNames(entry.path)
                               : viewer == "text" || viewer == "markdown" ? readText(entry.path) : QString();
            return qMakePair(entry, viewer == "markdown" ? markdownOpening(read) : read);
        }));
    }
    emit changed();
    emit listed();

    if (report && m_reporting)
        this->report(e.path);
}

void Previewer::connectToFolio(bool overStdio) {
    m_reporting = true;
    m_overStdio = overStdio;
    if (!overStdio)
        return;
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
            const QString line = QString::fromUtf8(m_pending.left(newline));
            m_pending.remove(0, newline + 1);
            receive(line);
        }
    });
}
