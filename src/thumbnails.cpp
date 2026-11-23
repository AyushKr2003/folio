#include "thumbnails.h"
#include "foldermodel.h"
#include "mounter.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QtEndian>
#include <zlib.h>
#include <QDir>
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
// group of its own, so that past `patience` milliseconds what it started
// (glycin and the office thumbnailer start their own) is stopped with it:
// a broken file must never hang a tile forever.
bool run(QProcess &process, const QString &program, const QStringList &args, int patience) {
    process.setChildProcessModifier([] { ::setpgid(0, 0); });
    process.start(program, args);
    if (!process.waitForFinished(patience)) {
        if (process.processId() > 0)
            ::kill(-pid_t(process.processId()), SIGKILL);
        process.waitForFinished(1000);
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

QImage Thumbnails::sharp(const QString &path, const QString &mime, int pixels, bool *fresh, QDateTime *read) const {
    *fresh = false;
    const QFileInfo info(path);
    if (!info.isFile())
        return {};
    const QDateTime modified = info.lastModified();
    *read = modified;
    QString stem = cacheKey(path);
    stem.chop(4);
    for (const bool alpha : {false, true}) {
        const QFileInfo cached(sharpPath(stem, pixels, alpha));
        if (cached.exists() && cached.lastModified() == modified)
            if (QImage image(cached.filePath()); !image.isNull())
                return image;
    }
    QImageReader reader(path);
    reader.setAutoTransform(true);
    if (!reader.canRead())
        return load(path, mime, 1024);
    const QSize full = reader.size();
    // Small enough already: read as it is, quickly, and not kept.
    const bool small = full.isValid() && full.width() <= pixels && full.height() <= pixels;
    if (full.isValid() && !small)
        reader.setScaledSize(full.scaled(pixels, pixels, Qt::KeepAspectRatio));
    const QImage image = reader.read();
    if (image.isNull())
        return load(path, mime, 1024);
    *fresh = !small;
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

QImage Thumbnails::loadNow(const QString &path, const QString &mime, int size, bool *fresh, bool *failed,
                           QDateTime *modifiedOut) const {
    *fresh = false;
    *failed = false;
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

    // A folder's peek asks for any picture ("image/*"); named by extension,
    // as the grid names it, it goes to the same thumbnailer as its tile.
    const QString named = mime.endsWith("/*")
        ? QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchExtension).name()
        : mime;
    // A file its extension could not name goes to the reader or thumbnailer for what it holds.
    const QString type = FolderModel::typeByContent(path, named);
    const int pixels = bucketPixels(bucketFor(size));
    bool attempted = false;
    QImage image = generate(path, type, pixels, attempted);
    if (image.isNull()) {
        // Only a reader or thumbnailer that ran and failed is remembered, so
        // one installed later still gets its turn.
        *failed = attempted;
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

QImage Thumbnails::generate(const QString &path, const QString &mime, int pixels, bool &attempted) const {
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
    const QString command = thumbnailerFor(mime);
    if (!command.isEmpty()) {
        attempted = true;
        return runThumbnailer(command, path, pixels);
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
            return pdfPage(path, pixels, m_patience);
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

QImage Thumbnails::pdfPage(const QString &path, int pixels, int patience) {
    QTemporaryDir scratch;
    if (!scratch.isValid())
        return {};
    QProcess process;
    if (!run(process, "pdftoppm", {"-f", "1", "-l", "1", "-singlefile", "-png", "-scale-to",
                                   QString::number(pixels), path, scratch.filePath("page")}, patience))
        return {};
    return QImage(scratch.filePath("page.png"));
}

QImage Thumbnails::runThumbnailer(const QString &command, const QString &path, int pixels) const {
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
    if (!run(process, program, args, m_patience))
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

class ThumbnailResponse : public QQuickImageResponse, public QRunnable {
public:
    ThumbnailResponse(const Thumbnails *thumbnails, const QString &id) : m_thumbnails(thumbnails), m_id(id) {
        setAutoDelete(false);
    }

    void run() override {
        // A decoder that throws (LibRaw on a camera RAW it can't read, #164)
        // must not take Folio down: an exception out of a pool thread ends
        // the process. The file gets no thumbnail instead; once finished()
        // has been told, the response may be gone, so it's left alone.
        bool told = false;
        try {
            work(told);
        } catch (...) {
            if (!told) {
                m_image = QImage();
                m_error = QStringLiteral("No thumbnail");
                emit finished();
            }
        }
    }

    void work(bool &told) {
        // id = <size>/<modified>/<url-encoded mime>/<url-encoded path>, or
        // page/<size>/<modified>/<paper>/<ink>/<url-encoded path> for text.
        // cached/<width>/<height>/<modified>/<url-encoded path> fits a cached preview.
        // The time is only there to tell Qt's own cache apart; load() checks
        // the file's.
        const QStringList parts = m_id.split('/');
        // A tile scrolled away before its turn came shouldn't keep the pool
        // from the ones on screen.
        if (!m_cancelled && parts.size() >= 5 && parts.at(0) == "cached") {
            const QString path = QUrl::fromPercentEncoding(parts.mid(4).join('/').toUtf8());
            const QImage thumbnail = m_thumbnails->cached(path);
            if (!thumbnail.isNull()) {
                // Read the header for geometry. The full Image still decodes the pixels.
                QImageReader reader(path);
                QSize original = reader.size();
                if (reader.transformation().testFlag(QImageIOHandler::TransformationRotate90))
                    original.transpose();
                const QSize viewport(qBound(1, parts.at(1).toInt(), 2048),
                                     qBound(1, parts.at(2).toInt(), 2048));
                if (!original.isEmpty()) {
                    if (original.width() > viewport.width() || original.height() > viewport.height())
                        original.scale(viewport, Qt::KeepAspectRatio);
                    m_image = thumbnail.scaled(original.expandedTo(QSize(1, 1)), Qt::IgnoreAspectRatio,
                                               Qt::SmoothTransformation);
                }
            }
        } else if (!m_cancelled && parts.size() >= 5 && parts.at(0) == "sharp") {
            const int pixels = qBound(256, parts.at(1).toInt(), 8192);
            const QString mime = QUrl::fromPercentEncoding(parts.at(3).toUtf8());
            const QString path = QUrl::fromPercentEncoding(parts.mid(4).join('/').toUtf8());
            bool fresh = false;
            QDateTime modified;
            m_image = m_thumbnails->sharp(path, mime, pixels, &fresh, &modified);
            // Shown first, kept after: writing it shouldn't hold it up. Qt
            // may delete the response once it's finished, so nothing of it
            // is used after.
            const QImage made = fresh ? m_image : QImage();
            const Thumbnails *thumbnails = m_thumbnails;
            if (m_image.isNull())
                m_error = QStringLiteral("No picture");
            told = true;
            emit finished();
            if (!made.isNull())
                thumbnails->keep(made, path, pixels, modified);
            return;
        } else if (!m_cancelled && parts.size() >= 6 && parts.at(0) == "page") {
            const QString path = QUrl::fromPercentEncoding(parts.mid(5).join('/').toUtf8());
            m_image = Thumbnails::textPage(path, qBound(64, parts.at(1).toInt(), 1024),
                                           QColor::fromString("#" + parts.at(3)), QColor::fromString("#" + parts.at(4)));
        } else if (!m_cancelled && parts.size() >= 4) {
            const int size = qBound(64, parts.at(0).toInt(), 1024);
            const QString mime = QUrl::fromPercentEncoding(parts.at(2).toUtf8());
            const QString path = QUrl::fromPercentEncoding(parts.mid(3).join('/').toUtf8());
            bool fresh = false, failed = false;
            QDateTime modified;
            m_image = m_thumbnails->loadNow(path, mime, size, &fresh, &failed, &modified);
            // Shown first, written to the PNG cache after so tiles never wait
            // on PNG encoding and disk sync. Nothing of `this` is touched once
            // finished() is emitted.
            const QImage made = fresh ? m_image : QImage();
            const Thumbnails *thumbnails = m_thumbnails;
            if (m_image.isNull())
                m_error = QStringLiteral("No thumbnail");
            told = true;
            emit finished();
            if (!made.isNull())
                thumbnails->remember(made, path, size, modified);
            else if (failed)
                thumbnails->rememberFailure(path, modified);
            return;
        }
        if (m_image.isNull())
            m_error = QStringLiteral("No thumbnail");
        told = true;
        emit finished();
    }

    void cancel() override { m_cancelled = true; }

    QQuickTextureFactory *textureFactory() const override {
        return QQuickTextureFactory::textureFactoryForImage(m_image);
    }
    QString errorString() const override { return m_error; }

private:
    const Thumbnails *m_thumbnails;
    QString m_id;
    QImage m_image;
    QString m_error;
    std::atomic_bool m_cancelled{false};
};

} // namespace

ThumbnailProvider::ThumbnailProvider(const Thumbnails *thumbnails) : m_thumbnails(thumbnails) {
    // Thumbnailers are separate processes; a few at once keeps a big folder
    // filling in quickly without starving the machine.
    m_pool.setMaxThreadCount(qBound(2, QThread::idealThreadCount() / 2, 6));
}

// The size comes in the id; no view sets a sourceSize to request one.
QQuickImageResponse *ThumbnailProvider::requestImageResponse(const QString &id, const QSize &) {
    auto *response = new ThumbnailResponse(m_thumbnails, id);
    // Prioritize active previews over grid/row thumbnail backlogs.
    const int priority = id.startsWith(QLatin1String("cached/")) ? 2
                       : id.startsWith(QLatin1String("sharp/")) ? 1 : 0;
    m_pool.start(response, priority);
    return response;
}
