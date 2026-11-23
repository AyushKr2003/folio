#include "thumbnails.h"
#include "foldermodel.h"
#include "mounter.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QtEndian>
#include <zlib.h>
#include <QDir>
#include <QElapsedTimer>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QStringDecoder>
#include <QDirIterator>
#include <QFileInfo>
#include <QImageReader>
#include <QMimeDatabase>
#include <QProcess>
#include <QQuickTextureFactory>
#include <QRunnable>
#include <QSaveFile>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <atomic>
#include <csignal>
#include <unistd.h>

namespace {

QString fileUri(const QString &path) {
    return QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
}

// Qt writes longer PNG text values as compressed zTXt chunks. The thumbnail
// spec's keys belong in plain tEXt, which is all some thumbnail readers look
// at, so re-encode them after Qt writes the image.
QByteArray withPlainText(const QByteArray &png) {
    if (!png.startsWith("\x89PNG\r\n\x1a\n"))
        return png;
    QByteArray out;
    out.reserve(png.size() + 256);
    out.append(png.constData(), 8);
    qsizetype at = 8;
    while (at + 12 <= png.size()) {
        const quint32 length = qFromBigEndian<quint32>(png.constData() + at);
        if (at + 12 + qsizetype(length) > png.size())
            return png;
        const QByteArrayView type(png.constData() + at + 4, 4);
        if (type == QByteArrayView("zTXt")) {
            const QByteArrayView body(png.constData() + at + 8, length);
            const qsizetype nul = body.indexOf('\0');
            if (nul > 0 && nul + 2 <= body.size()) {
                QByteArray compressed;
                compressed.reserve(4 + body.size() - (nul + 2));
                // qUncompress wants the expected length up front; the 4 bytes are a
                // hint, so give it a generous one.
                compressed.append("\x00\x01\x00\x00", 4);
                compressed.append(body.constData() + nul + 2, body.size() - (nul + 2));
                const QByteArray text = qUncompress(compressed);
                if (!text.isNull()) {
                    const qsizetype plainSize = nul + 1 + text.size();
                    QByteArray typed;
                    typed.reserve(4 + plainSize);
                    typed.append("tEXt", 4);
                    typed.append(body.constData(), nul);
                    typed.append('\0');
                    typed.append(text);
                    char lenBuf[4], crcBuf[4];
                    qToBigEndian<quint32>(quint32(plainSize), lenBuf);
                    qToBigEndian<quint32>(crc32(0, reinterpret_cast<const Bytef *>(typed.constData()), typed.size()), crcBuf);
                    out.append(lenBuf, 4);
                    out.append(typed);
                    out.append(crcBuf, 4);
                    at += 12 + qsizetype(length);
                    continue;
                }
            }
        }
        out.append(png.constData() + at, 12 + qsizetype(length));
        at += 12 + qsizetype(length);
    }
    return out;
}

// The spec's Exec field, such as "ffmpegthumbnailer -i %i -o %o -s %s -f": %i input path, %u input URI, %o output, %s size, %% a percent sign.
// Each code is read once, so a name that holds one arrives as it is.
QStringList expandCommand(const QString &command, const QString &input, const QString &output, int size) {
    QStringList args = QProcess::splitCommand(command);
    for (QString &arg : args) {
        QString expanded;
        for (qsizetype at = 0; at < arg.size(); ++at) {
            const QChar code = arg.at(at) == '%' && at + 1 < arg.size() ? arg.at(at + 1) : QChar();
            if (code == 'i')
                expanded += input;
            else if (code == 'u')
                expanded += fileUri(input);
            else if (code == 'o')
                expanded += output;
            else if (code == 's')
                expanded += QString::number(size);
            else if (code == '%')
                expanded += '%';
            else {
                expanded += arg.at(at);
                continue;
            }
            ++at;
        }
        arg = expanded;
    }
    return args;
}

// Runs a helper to its end, true when it ended well. It runs in a process
// group of its own, so that past `patience` milliseconds — or as soon as
// `stop` returns true because the tile scrolled away — what it started
// (glycin and the office thumbnailer start their own) is stopped with it:
// a broken or discarded file never hangs a worker.
bool run(QProcess &process, const QString &program, const QStringList &args, int patience,
         const std::function<bool()> &stop = {}) {
    if (stop && stop())
        return false;
    process.setChildProcessModifier([] { ::setpgid(0, 0); });
    process.start(program, args);
    QElapsedTimer elapsed;
    elapsed.start();
    while (true) {
        if (stop && stop()) {
            if (process.processId() > 0)
                ::kill(-pid_t(process.processId()), SIGKILL);
            process.waitForFinished(1000);
            return false;
        }
        const qint64 left = qint64(patience) - elapsed.elapsed();
        if (left <= 0) {
            if (process.processId() > 0)
                ::kill(-pid_t(process.processId()), SIGKILL);
            process.waitForFinished(1000);
            return false;
        }
        if (process.waitForFinished(int(qMin<qint64>(50, left))))
            break;
        if (process.state() == QProcess::NotRunning)
            return false;
    }
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

} // namespace

Thumbnails::Thumbnails(const QString &cacheDirectory, const QStringList &thumbnailerDirectories,
                       const QString &remoteRoot, int patience)
    : m_cache(cacheDirectory.isEmpty()
                  ? QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/thumbnails"
                  : cacheDirectory),
      m_sharpRoot(QFileInfo(m_cache).absolutePath() + "/folio/sharp/"),
      m_remoteRoot(remoteRoot.isEmpty() ? Mounter::root() : remoteRoot),
      m_patience(patience),
      m_thumbnailerDirs(thumbnailerDirectories) {}

void Thumbnails::ensureThumbnailers() const {
    std::call_once(m_thumbnailersOnce, [this] {
        QStringList directories = m_thumbnailerDirs;
        if (directories.isEmpty()) {
            // Later directories win, so the user's own thumbnailers override the
            // system's.
            QStringList data = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
            std::reverse(data.begin(), data.end());
            for (const QString &base : data)
                directories << base + "/thumbnailers";
        }
        QMimeDatabase mimes;
        for (const QString &directory : directories) {
            // A .thumbnailer file may name a type by an alias, as glycin names DDS pictures image/x-dds.
            QHash<QString, QString> named, aliased;
            QDirIterator it(directory, {"*.thumbnailer"}, QDir::Files);
            while (it.hasNext()) {
                // A tiny desktop-entry reader: QSettings would treat the
                // semicolons in MimeType and the %-codes in Exec as its own syntax.
                QFile file(it.next());
                if (!file.open(QIODevice::ReadOnly))
                    continue;
                QHash<QString, QString> fields;
                bool inEntry = false;
                for (const QByteArray &raw : file.readAll().split('\n')) {
                    const QString line = QString::fromUtf8(raw).trimmed();
                    if (line.startsWith('['))
                        inEntry = line == "[Thumbnailer Entry]";
                    else if (inEntry && line.contains('=') && !line.startsWith('#'))
                        fields.insert(line.section('=', 0, 0).trimmed(), line.section('=', 1).trimmed());
                }
                const QString exec = fields.value("Exec");
                // The program Exec runs has to be installed, or a .thumbnailer
                // file left behind would mark every file it was asked about failed.
                const QStringList command = expandCommand(exec, {}, {}, 0);
                const QString tryExec = fields.value("TryExec");
                const auto missing = [](const QString &program) { return QStandardPaths::findExecutable(program).isEmpty(); };
                if (command.isEmpty() || missing(command.first()) || (!tryExec.isEmpty() && missing(tryExec)))
                    continue;
                for (const QString &declared : fields.value("MimeType").split(';', Qt::SkipEmptyParts)) {
                    const QString mime = declared.trimmed();
                    named.insert(mime, exec);
                    if (const QString type = mimes.mimeTypeForName(mime).name(); !type.isEmpty())
                        aliased.insert(type, exec);
                }
            }
            // Within one directory the file that names a type itself wins over one that names an alias of it.
            m_thumbnailers.insert(aliased);
            m_thumbnailers.insert(named);
        }
    });
}

QString Thumbnails::thumbnailerFor(const QString &mime) const {
    ensureThumbnailers();
    return m_thumbnailers.value(mime);
}

QString Thumbnails::cacheKey(const QString &path) {
    return QCryptographicHash::hash(fileUri(path).toUtf8(), QCryptographicHash::Md5).toHex() + ".png";
}

QString Thumbnails::bucketFor(int size) {
    if (size <= 128) return "normal";
    if (size <= 256) return "large";
    if (size <= 512) return "x-large";
    return "xx-large";
}

static int bucketPixels(const QString &bucket) {
    if (bucket == "normal") return 128;
    if (bucket == "large") return 256;
    if (bucket == "x-large") return 512;
    return 1024;
}

QString Thumbnails::cachePath(const QString &key, int size) const {
    return m_cache + "/" + bucketFor(size) + "/" + key;
}

QString Thumbnails::failPath(const QString &key) const {
    return m_cache + "/fail/folio/" + key;
}

// Kept apart from the shared thumbnails, whose sizes stop at 1024: in
// ~/.cache/folio/sharp/<pixels>/, as a JPEG (a PNG when it's transparent),
// which reads back in a few milliseconds where the picture itself, a large
// PNG, takes half a second. Each has the picture's own modification time,
// so a changed picture is made again.
QString Thumbnails::sharpPath(const QString &stem, int pixels, bool alpha) const {
    return m_sharpRoot + QString::number(pixels) + "/" + stem + (alpha ? ".png" : ".jpg");
}

QImage Thumbnails::sharpCached(const QString &path, int pixels) const {
    const QString memKey = path + '\n' + QString::number(pixels);
    SharpEntry cachedEntry;
    bool foundMem = false;
    {
        std::lock_guard lock(m_sharpMemoryMutex);
        if (const SharpEntry *entry = m_sharpMemory.object(memKey)) {
            cachedEntry = *entry;
            foundMem = true;
        }
    }
    if (foundMem) {
        const QFileInfo info(path);
        if (info.isFile() && info.lastModified() == cachedEntry.modified)
            return cachedEntry.image;
        std::lock_guard lock(m_sharpMemoryMutex);
        if (const SharpEntry *entry = m_sharpMemory.object(memKey); entry && entry->modified == cachedEntry.modified)
            m_sharpMemory.remove(memKey);
    }
    QString stem = cacheKey(path);
    stem.chop(4);
    const QString jpg = sharpPath(stem, pixels, false);
    const QString png = sharpPath(stem, pixels, true);
    const bool hasJpg = QFileInfo::exists(jpg);
    const bool hasPng = QFileInfo::exists(png);
    if (!hasJpg && !hasPng)
        return {};
    const QFileInfo info(path);
    if (!info.isFile())
        return {};
    const QDateTime modified = info.lastModified();
    for (const QString &candidate : {jpg, png}) {
        const QFileInfo cached(candidate);
        if (cached.exists() && cached.lastModified() == modified) {
            if (QImage image(candidate); !image.isNull()) {
                const qsizetype bytes = image.sizeInBytes();
                if (bytes > 0 && bytes <= m_sharpMemory.maxCost()) {
                    std::lock_guard lock(m_sharpMemoryMutex);
                    m_sharpMemory.insert(memKey, new SharpEntry{modified, image}, int(bytes));
                }
                return image;
            }
        }
    }
    return {};
}

QImage Thumbnails::sharp(const QString &path, const QString &mime, int pixels, bool *fresh, QDateTime *read,
                         const std::function<bool()> &stop) const {
    *fresh = false;
    if (stop && stop())
        return {};
    const QFileInfo info(path);
    if (!info.isFile())
        return {};
    const QDateTime modified = info.lastModified();
    *read = modified;
    const QString memKey = path + '\n' + QString::number(pixels);
    const auto rememberMem = [&](const QImage &image) {
        const qsizetype bytes = image.sizeInBytes();
        if (!image.isNull() && bytes > 0 && bytes <= m_sharpMemory.maxCost()) {
            std::lock_guard lock(m_sharpMemoryMutex);
            m_sharpMemory.insert(memKey, new SharpEntry{modified, image}, int(bytes));
        }
    };
    {
        std::lock_guard lock(m_sharpMemoryMutex);
        if (const SharpEntry *entry = m_sharpMemory.object(memKey); entry && entry->modified == modified)
            return entry->image;
    }
    QString stem = cacheKey(path);
    stem.chop(4);
    for (const bool alpha : {false, true}) {
        const QFileInfo cached(sharpPath(stem, pixels, alpha));
        if (cached.exists() && cached.lastModified() == modified) {
            if (QImage image(cached.filePath()); !image.isNull()) {
                rememberMem(image);
                return image;
            }
        }
    }
    if (stop && stop())
        return {};
    const auto fallback = [&] {
        bool thumbFresh = false, thumbFailed = false;
        QDateTime thumbModified;
        const QImage img = loadNow(path, mime, 1024, &thumbFresh, &thumbFailed, &thumbModified, stop);
        if (thumbFresh)
            remember(img, path, 1024, thumbModified);
        else if (thumbFailed)
            rememberFailure(path, thumbModified);
        return img;
    };
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (!reader.canRead())
        return fallback();
    const QSize full = reader.size();
    // Small enough already: read as it is, quickly, and not kept.
    const bool small = full.isValid() && full.width() <= pixels && full.height() <= pixels;
    if (full.isValid() && !small)
        reader.setScaledSize(full.scaled(pixels, pixels, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull())
        return fallback();
    *fresh = !small;
    rememberMem(image);
    return image;
}

void Thumbnails::keep(const QImage &image, const QString &path, int pixels, const QDateTime &modified) const {
    QString stem = cacheKey(path);
    stem.chop(4);
    const QString target = sharpPath(stem, pixels, image.hasAlphaChannel());
    // Copies of private pictures: only their owner may read them, as with
    // the thumbnails (see save()).
    const QString folder = QFileInfo(target).absolutePath();
    if (!QFileInfo::exists(folder)) {
        QDir().mkpath(folder);
        const auto ownerOnly = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
        for (const QString &dir : {QFileInfo(folder).absolutePath(), folder})
            QFile::setPermissions(dir, ownerOnly);
    }
    QSaveFile file(target);
    if (!file.open(QIODevice::WriteOnly) || !image.save(&file, image.hasAlphaChannel() ? "PNG" : "JPG", image.hasAlphaChannel() ? 10 : 90))
        return;
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (file.commit()) {
        QFile written(target);
        if (written.open(QIODevice::ReadWrite))
            written.setFileTime(modified, QFileDevice::FileModificationTime);
    }
}

QImage Thumbnails::cached(const QString &path) const {
    const QFileInfo info(path);
    if (!info.isFile())
        return {};
    const QString key = cacheKey(path);
    const QDateTime modified = info.lastModified();
    const QString uri = fileUri(path);

    for (const int size : {128, 256, 512, 1024}) {
        QImageReader reader(cachePath(key, size), "png");
        const QSize pixels = reader.size();
        if (!pixels.isValid() || pixels.width() > size || pixels.height() > size)
            continue;
        const QImage image = reader.read();
        if (madeFor(image, modified) && image.text("Thumb::URI") == uri)
            return image;
    }
    return {};
}

Thumbnails::CacheLookup Thumbnails::loadCached(const QString &path, int size, QImage *imageOut) const {
    const QString key = cacheKey(path);
    const QString cachedFile = cachePath(key, size);
    const QString failFile = failPath(key);
    const bool hasCached = QFileInfo::exists(cachedFile);
    const bool hasFail = !hasCached && QFileInfo::exists(failFile);
    if (!hasCached && !hasFail)
        return CacheLookup::Miss;

    const QFileInfo info(path);
    if (!info.isFile() || info.absoluteFilePath().startsWith(m_cache + "/"))
        return CacheLookup::Failed;

    const QDateTime modified = info.lastModified();
    if (hasCached) {
        if (const QImage image(cachedFile); madeFor(image, modified)) {
            if (imageOut)
                *imageOut = image;
            return CacheLookup::Hit;
        }
        return failedBefore(key, modified) ? CacheLookup::Failed : CacheLookup::Miss;
    }
    return failedBefore(key, modified) ? CacheLookup::Failed : CacheLookup::Miss;
}

QImage Thumbnails::loadNow(const QString &path, const QString &mime, int size, bool *fresh, bool *failed,
                           QDateTime *modifiedOut, const std::function<bool()> &stop) const {
    *fresh = false;
    *failed = false;
    if (stop && stop())
        return {};
    const QFileInfo info(path);
    if (!info.isFile())
        return {};

    // Thumbnails of the cache itself would recurse forever.
    if (info.absoluteFilePath().startsWith(m_cache + "/"))
        return {};

    const QDateTime modified = info.lastModified();
    *modifiedOut = modified;
    const QString key = cacheKey(path);
    if (const QImage image(cachePath(key, size)); madeFor(image, modified))
        return image;
    if (failedBefore(key, modified))
        return {};
    if (stop && stop())
        return {};

    // A folder's peek asks for any picture ("image/*"); named by extension,
    // as the grid names it, it goes to the same thumbnailer as its tile.
    const QString named = mime.endsWith("/*")
        ? QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchExtension).name()
        : mime;
    // A file its extension could not name goes to the reader or thumbnailer for what it holds.
    const QString type = FolderModel::typeByContent(path, named);
    const int pixels = bucketPixels(bucketFor(size));
    bool attempted = false;
    QImage image = generate(path, type, pixels, attempted, stop);
    if (image.isNull()) {
        // Only a reader or thumbnailer that ran to completion and failed is
        // remembered, never one stopped early because the tile scrolled away.
        *failed = attempted && !(stop && stop());
        return {};
    }
    if (image.width() > pixels || image.height() > pixels)
        image = image.scaled(pixels, pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    *fresh = true;
    return image;
}

QImage Thumbnails::load(const QString &path, const QString &mime, int size) const {
    bool fresh = false, failed = false;
    QDateTime modified;
    const QImage image = loadNow(path, mime, size, &fresh, &failed, &modified);
    if (fresh)
        remember(image, path, size, modified);
    else if (failed)
        rememberFailure(path, modified);
    return image;
}

QImage Thumbnails::generate(const QString &path, const QString &mime, int pixels, bool &attempted,
                            const std::function<bool()> &stop) const {
    if (stop && stop())
        return {};
    // Common photo formats decode fastest in-process; anything Qt can't read
    // (HEIC, JPEG XL) falls through to the installed thumbnailers.
    if (mime.startsWith("image/")) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        if (reader.canRead()) {
            attempted = true;
            const QSize full = reader.size();
            if (full.isValid())
                reader.setScaledSize(full.scaled(pixels, pixels, Qt::KeepAspectRatio));
            const QImage image = reader.read();
            if (!image.isNull())
                return image;
        }
    }
    if (stop && stop())
        return {};
    const QString command = thumbnailerFor(mime);
    if (!command.isEmpty()) {
        attempted = true;
        return runThumbnailer(command, path, pixels, stop);
    }
    // Without a PDF thumbnailer installed, Poppler's pdftoppm (there with
    // CUPS) draws the first page. Cache a found path (verifying it still
    // exists), never caching a miss so installing Poppler later works at once.
    if (mime == "application/pdf") {
        static std::mutex pdftoppmMutex;
        static QString pdftoppm;
        QString executable;
        {
            std::lock_guard lock(pdftoppmMutex);
            if (pdftoppm.isEmpty() || !QFileInfo::exists(pdftoppm))
                pdftoppm = QStandardPaths::findExecutable("pdftoppm");
            executable = pdftoppm;
        }
        if (!executable.isEmpty()) {
            attempted = true;
            return pdfPage(path, pixels, m_patience, stop);
        }
    }
    return {};
}

QImage Thumbnails::textPage(const QString &path, int pixels, const QColor &paper, const QColor &ink) {
    // Only a regular file: a FIFO or device named like text would block the
    // read, and the worker with it, until something writes to it.
    if (!QFileInfo(path).isFile())
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QByteArray head = file.read(4096);
    // Binary that merely claims to be text (or a cut through a multibyte
    // character at the very end, which is fine) gets the stand-in instead.
    if (head.contains('\0'))
        return {};
    QStringDecoder decode(QStringDecoder::Utf8);
    const QString text = decode(head);
    if (decode.hasError() && head.size() < 4096)
        return {};

    // A sheet a little narrower than it is tall, so it still reads as a page
    // rather than a folder, in the theme's colors: the monospace font at a
    // size that stays readable on the tile, as many lines as fit, no wrapping.
    const QSize size(qRound(pixels * 0.85), pixels);
    QImage page(size, QImage::Format_ARGB32_Premultiplied);
    page.fill(paper);
    QPainter painter(&page);
    painter.setRenderHint(QPainter::TextAntialiasing);
    QFont font("monospace");
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(qMax(6, pixels / 17));
    painter.setFont(font);
    painter.setPen(ink);
    const QFontMetricsF metrics(font);
    const qreal margin = size.width() * 0.08, line = metrics.lineSpacing();
    // Long lines stop at the right margin rather than the page's edge.
    painter.setClipRect(QRectF(margin, 0, size.width() - 2 * margin, size.height()));
    qreal y = margin + metrics.ascent();
    const qreal limit = size.height() - margin;
    qsizetype at = 0;
    while (at <= text.size() && y <= limit) {
        qsizetype end = text.indexOf(QLatin1Char('\n'), at);
        const qsizetype next = end < 0 ? text.size() + 1 : end + 1;
        if (end < 0)
            end = text.size();
        if (end > at && text.at(end - 1) == QLatin1Char('\r'))
            --end;
        const QStringView slice = QStringView(text).mid(at, end - at);
        if (slice.contains(QLatin1Char('\t'))) {
            QString expanded = slice.toString();
            expanded.replace(QLatin1Char('\t'), QLatin1String("    "));
            painter.drawText(QPointF(margin, y), expanded);
        } else {
            painter.drawText(QPointF(margin, y), slice.toString());
        }
        y += line;
        at = next;
    }
    painter.end();
    return page;
}

QImage Thumbnails::pdfPage(const QString &path, int pixels, int patience, const std::function<bool()> &stop) {
    if (stop && stop())
        return {};
    QTemporaryDir scratch;
    if (!scratch.isValid())
        return {};
    QProcess process;
    if (!run(process, "pdftoppm", {"-f", "1", "-l", "1", "-singlefile", "-png", "-scale-to",
                                   QString::number(pixels), path, scratch.filePath("page")}, patience, stop))
        return {};
    return QImage(scratch.filePath("page.png"));
}

QImage Thumbnails::runThumbnailer(const QString &command, const QString &path, int pixels,
                                  const std::function<bool()> &stop) const {
    if (stop && stop())
        return {};
    QTemporaryDir scratch;
    if (!scratch.isValid())
        return {};
    const QString output = scratch.filePath("thumbnail.png");
    // A thumbnailer given a URI asks GIO for the file, and GIO turns one on
    // a GVfs mount (another machine's) back into its network address,
    // sftp://…, which most can't open, Papers' among them. A link here reads
    // through the mount the same, and GIO leaves it a file.
    QString input = path;
    if (path.startsWith(m_remoteRoot + "/")) {
        const QString suffix = QFileInfo(path).suffix();
        const QString link = scratch.filePath(suffix.isEmpty() ? "input" : "input." + suffix);
        if (QFile::link(path, link))
            input = link;
    }
    QStringList args = expandCommand(command, input, output, pixels);
    if (args.isEmpty())
        return {};
    const QString program = args.takeFirst();
    // ffmpegthumbnailer's -f frames the picture as a film strip; the tile
    // marks a video with its own play badge instead.
    if (QFileInfo(program).fileName() == "ffmpegthumbnailer")
        args.removeAll("-f");

    QProcess process;
    process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    if (!run(process, program, args, m_patience, stop))
        return {};
    return QImage(output);
}

void Thumbnails::remember(QImage image, const QString &path, int size, const QDateTime &modified) const {
    image.setText("Software", "folio");
    save(std::move(image), cachePath(cacheKey(path), size), path, modified);
}

// Markers say which round of thumbnailing failed: ones from before a fix
// (round 2: files on other machines; round 3: names holding a %, helpers
// that never started, camera RAW) are tried again once.
static const QString failedRound = QStringLiteral("3");

void Thumbnails::rememberFailure(const QString &path, const QDateTime &modified) const {
    QImage marker(1, 1, QImage::Format_ARGB32);
    marker.fill(Qt::transparent);
    marker.setText("X-Folio::Round", failedRound);
    save(std::move(marker), failPath(cacheKey(path)), path, modified);
}

bool Thumbnails::failedBefore(const QString &key, const QDateTime &modified) const {
    const QImage marker(failPath(key));
    return madeFor(marker, modified) && marker.text("X-Folio::Round") == failedRound;
}

// Whether a cached thumbnail (or failure marker) was made from the file as it
// was last modified. The spec's Thumb::MTime is in whole seconds, which a
// picture saved over within the same second matches; Folio's own also keep
// the milliseconds. One another app made goes by the seconds alone.
// QImageReader::text() splits keys at ':', so these are only readable from
// the decoded image.
bool Thumbnails::madeFor(const QImage &image, const QDateTime &modified) {
    const QString exact = image.text("X-Folio::MTime");
    return !image.isNull() && image.text("Thumb::MTime") == QString::number(modified.toSecsSinceEpoch())
        && (exact.isEmpty() || exact == QString::number(modified.toMSecsSinceEpoch()));
}

void Thumbnails::save(QImage image, const QString &target, const QString &path, const QDateTime &modified) const {
    const QString dir = QFileInfo(target).absolutePath();
    if (!QFileInfo::exists(dir)) {
        QDir().mkpath(dir);
        QFile::setPermissions(m_cache, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }

    image.setText("Thumb::URI", fileUri(path));
    image.setText("Thumb::MTime", QString::number(modified.toSecsSinceEpoch()));
    image.setText("X-Folio::MTime", QString::number(modified.toMSecsSinceEpoch()));

    // Written beside the target and renamed, so another app never reads half
    // a thumbnail.
    QByteArray png;
    QBuffer buffer(&png);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return;
    QSaveFile file(target);
    if (file.open(QIODevice::WriteOnly) && file.write(withPlainText(png)) > 0) {
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        file.commit();
    }
}

namespace {

class ThumbnailResponse : public QQuickImageResponse {
public:
    ThumbnailResponse() : m_cancelled(std::make_shared<std::atomic_bool>(false)) {}

    void cancel() override { m_cancelled->store(true, std::memory_order_relaxed); }
    std::shared_ptr<std::atomic_bool> cancelled() const { return m_cancelled; }

    void finish(QImage image, const QString &error = {}) {
        m_image = std::move(image);
        if (m_image.isNull())
            m_error = error.isEmpty() ? QStringLiteral("No thumbnail") : error;
        emit finished();
    }

    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    QString errorString() const override { return m_error; }

private:
    std::shared_ptr<std::atomic_bool> m_cancelled;
    QImage m_image;
    QString m_error;
};

class LambdaTask : public QRunnable {
public:
    explicit LambdaTask(std::function<void()> fn) : m_fn(std::move(fn)) {
        setAutoDelete(true);
    }
    void run() override { m_fn(); }

private:
    std::function<void()> m_fn;
};

} // namespace

ThumbnailProvider::ThumbnailProvider(const Thumbnails *thumbnails) : m_thumbnails(thumbnails) {
    // Two dedicated cache readers so already-cached thumbnails and cached
    // previews finish immediately even when all generation slots are busy.
    m_cachePool.setMaxThreadCount(2);
    // Thumbnailers are separate processes; a few at once keeps a big folder
    // filling in quickly without starving the machine.
    m_pool.setMaxThreadCount(qBound(2, QThread::idealThreadCount() / 2, 6));
}

ThumbnailProvider::~ThumbnailProvider() {
    m_stopping->store(true, std::memory_order_relaxed);
    // Drain cache probes first because a cache miss can enqueue onto m_pool.
    m_cachePool.waitForDone();
    m_pool.waitForDone();
}

// The size comes in the id; no view sets a sourceSize to request one.
QQuickImageResponse *ThumbnailProvider::requestImageResponse(const QString &id, const QSize &) {
    auto *response = new ThumbnailResponse();
    const auto cancelled = response->cancelled();
    const auto stopping = m_stopping;
    const Thumbnails *thumbnails = m_thumbnails;
    const auto stopped = [cancelled, stopping] {
        return cancelled->load(std::memory_order_relaxed) || stopping->load(std::memory_order_relaxed);
    };
    const auto runSafe = [response](const std::function<void(bool &)> &fn) {
        // A decoder that throws (LibRaw on a camera RAW it can't read, #164)
        // must not take Folio down: an exception out of a pool thread ends
        // the process. Once finished() has been told, the response may be
        // gone, so it's left alone.
        bool told = false;
        try {
            fn(told);
        } catch (...) {
            if (!told)
                response->finish({});
        }
    };

    const QStringList parts = id.split('/');

    // cached/<width>/<height>/<modified>/<url-encoded path> only reads existing
    // disk thumbnails and image headers; run directly on the cache pool.
    if (parts.size() >= 5 && parts.at(0) == QLatin1String("cached")) {
        const int w = qBound(1, parts.at(1).toInt(), 2048);
        const int h = qBound(1, parts.at(2).toInt(), 2048);
        const QString path = QUrl::fromPercentEncoding(parts.mid(4).join('/').toUtf8());
        m_cachePool.start(new LambdaTask([=]() {
            runSafe([&](bool &told) {
                QImage out;
                if (!stopped()) {
                    const QImage thumbnail = thumbnails->cached(path);
                    if (!thumbnail.isNull()) {
                        QImageReader reader(path);
                        QSize original = reader.size();
                        if (reader.transformation().testFlag(QImageIOHandler::TransformationRotate90))
                            original.transpose();
                        const QSize viewport(w, h);
                        if (!original.isEmpty()) {
                            if (original.width() > viewport.width() || original.height() > viewport.height())
                                original.scale(viewport, Qt::KeepAspectRatio);
                            out = thumbnail.scaled(original.expandedTo(QSize(1, 1)), Qt::IgnoreAspectRatio,
                                                   Qt::SmoothTransformation);
                        }
                    }
                }
                told = true;
                response->finish(std::move(out));
            });
        }), 2);
        return response;
    }

    // page/<size>/<modified>/<paper>/<ink>/<url-encoded path>: never disk-cached.
    if (parts.size() >= 6 && parts.at(0) == QLatin1String("page")) {
        const int size = qBound(64, parts.at(1).toInt(), 1024);
        const QColor paper = QColor::fromString("#" + parts.at(3));
        const QColor ink = QColor::fromString("#" + parts.at(4));
        const QString path = QUrl::fromPercentEncoding(parts.mid(5).join('/').toUtf8());
        m_pool.start(new LambdaTask([=]() {
            runSafe([&](bool &told) {
                QImage out = stopped() ? QImage() : Thumbnails::textPage(path, size, paper, ink);
                told = true;
                response->finish(std::move(out));
            });
        }), 0);
        return response;
    }

    // sharp/<pixels>/<modified>/<mime>/<path>: probe disk cache first, fall
    // back to high-priority generation on miss.
    if (parts.size() >= 5 && parts.at(0) == QLatin1String("sharp")) {
        const int pixels = qBound(256, parts.at(1).toInt(), 8192);
        const QString mime = QUrl::fromPercentEncoding(parts.at(3).toUtf8());
        const QString path = QUrl::fromPercentEncoding(parts.mid(4).join('/').toUtf8());
        QThreadPool *genPool = &m_pool;
        m_cachePool.start(new LambdaTask([=]() {
            runSafe([&](bool &told) {
                if (stopped()) {
                    told = true;
                    response->finish({}, QStringLiteral("No picture"));
                    return;
                }
                if (QImage cached = thumbnails->sharpCached(path, pixels); !cached.isNull()) {
                    told = true;
                    response->finish(std::move(cached), QStringLiteral("No picture"));
                    return;
                }
                if (stopped()) {
                    told = true;
                    response->finish({}, QStringLiteral("No picture"));
                    return;
                }
                told = true;
                genPool->start(new LambdaTask([=]() {
                    runSafe([&](bool &genTold) {
                        bool fresh = false;
                        QDateTime modified;
                        QImage img = stopped() ? QImage()
                                               : thumbnails->sharp(path, mime, pixels, &fresh, &modified, stopped);
                        const QImage made = fresh ? img : QImage();
                        genTold = true;
                        response->finish(std::move(img), QStringLiteral("No picture"));
                        if (!made.isNull())
                            thumbnails->keep(made, path, pixels, modified);
                    });
                }), 1);
            });
        }), 1);
        return response;
    }

    // <size>/<modified>/<mime>/<path>: probe disk cache on m_cachePool first;
    // send cold/stale misses to m_pool.
    if (parts.size() >= 4) {
        const int size = qBound(64, parts.at(0).toInt(), 1024);
        const QString mime = QUrl::fromPercentEncoding(parts.at(2).toUtf8());
        const QString path = QUrl::fromPercentEncoding(parts.mid(3).join('/').toUtf8());
        QThreadPool *genPool = &m_pool;
        m_cachePool.start(new LambdaTask([=]() {
            runSafe([&](bool &told) {
                if (stopped()) {
                    told = true;
                    response->finish({});
                    return;
                }
                QImage cached;
                const auto hit = thumbnails->loadCached(path, size, &cached);
                if (hit == Thumbnails::CacheLookup::Hit) {
                    told = true;
                    response->finish(std::move(cached));
                    return;
                }
                if (hit == Thumbnails::CacheLookup::Failed || stopped()) {
                    told = true;
                    response->finish({});
                    return;
                }
                told = true;
                genPool->start(new LambdaTask([=]() {
                    runSafe([&](bool &genTold) {
                        bool fresh = false, failed = false;
                        QDateTime modified;
                        QImage img = stopped() ? QImage()
                                               : thumbnails->loadNow(path, mime, size, &fresh, &failed, &modified, stopped);
                        const QImage made = fresh ? img : QImage();
                        genTold = true;
                        response->finish(std::move(img));
                        if (!made.isNull())
                            thumbnails->remember(made, path, size, modified);
                        else if (failed)
                            thumbnails->rememberFailure(path, modified);
                    });
                }), 0);
            });
        }), 0);
        return response;
    }

    // Unrecognized request id.
    m_cachePool.start(new LambdaTask([response]() { response->finish({}); }), 0);
    return response;
}
