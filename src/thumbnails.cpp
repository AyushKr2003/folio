#include "thumbnails.h"
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
    QByteArray out = png.left(8);
    int at = 8;
    while (at + 12 <= png.size()) {
        const quint32 length = qFromBigEndian<quint32>(png.constData() + at);
        if (at + 12 + qsizetype(length) > png.size())
            return png;
        const QByteArray type = png.mid(at + 4, 4);
        const QByteArray body = png.mid(at + 8, length);
        if (type == "zTXt") {
            const int nul = body.indexOf('\0');
            QByteArray compressed = body.mid(nul + 2);
            // qUncompress wants the expected length up front; the 4 bytes are a
            // hint, so give it a generous one.
            compressed.prepend(QByteArray("\x00\x01\x00\x00", 4));
            const QByteArray text = qUncompress(compressed);
            if (nul > 0 && !text.isNull()) {
                const QByteArray plain = body.left(nul) + '\0' + text;
                QByteArray chunk(4, 0);
                qToBigEndian<quint32>(plain.size(), chunk.data());
                const QByteArray typed = "tEXt" + plain;
                chunk += typed;
                QByteArray crc(4, 0);
                qToBigEndian<quint32>(crc32(0, reinterpret_cast<const Bytef *>(typed.constData()), typed.size()), crc.data());
                out += chunk + crc;
                at += 12 + length;
                continue;
            }
        }
        out += png.mid(at, 12 + length);
        at += 12 + length;
    }
    return out;
}

// The spec's Exec field: %i input path, %u input URI, %o output, %s size.
QStringList expandCommand(const QString &command, const QString &input, const QString &output, int size) {
    QStringList args = QProcess::splitCommand(command);
    for (QString &arg : args) {
        arg.replace("%i", input).replace("%u", fileUri(input)).replace("%o", output)
           .replace("%s", QString::number(size)).replace("%%", "%");
    }
    return args;
}

} // namespace

Thumbnails::Thumbnails(const QString &cacheDirectory, const QStringList &thumbnailerDirectories,
                       const QString &remoteRoot)
    : m_cache(cacheDirectory.isEmpty()
                  ? QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + "/thumbnails"
                  : cacheDirectory),
      m_remoteRoot(remoteRoot.isEmpty() ? Mounter::root() : remoteRoot) {
    QStringList directories = thumbnailerDirectories;
    if (directories.isEmpty()) {
        // Later directories win, so the user's own thumbnailers override the
        // system's.
        QStringList data = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
        std::reverse(data.begin(), data.end());
        for (const QString &base : data)
            directories << base + "/thumbnailers";
    }
    for (const QString &directory : directories) {
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
            const QString tryExec = fields.value("TryExec");
            if (exec.isEmpty() || (!tryExec.isEmpty() && QStandardPaths::findExecutable(tryExec).isEmpty()))
                continue;
            for (const QString &mime : fields.value("MimeType").split(';', Qt::SkipEmptyParts))
                m_thumbnailers.insert(mime.trimmed(), exec);
        }
    }
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

QString Thumbnails::cachePath(const QString &path, int size) const {
    return m_cache + "/" + bucketFor(size) + "/" + cacheKey(path);
}

QString Thumbnails::failPath(const QString &path) const {
    return m_cache + "/fail/folio/" + cacheKey(path);
}

// Kept apart from the shared thumbnails, whose sizes stop at 1024: in
// ~/.cache/folio/sharp/<pixels>/, as a JPEG (a PNG when it's transparent),
// which reads back in a few milliseconds where the picture itself, a large
// PNG, takes half a second. Each has the picture's own modification time,
// so a changed picture is made again.
QString Thumbnails::sharpPath(const QString &path, int pixels, bool alpha) const {
    QString key = cacheKey(path);
    key.chop(4);
    return QFileInfo(m_cache).absolutePath() + "/folio/sharp/" + QString::number(pixels) + "/" + key + (alpha ? ".png" : ".jpg");
}

QImage Thumbnails::sharp(const QString &path, const QString &mime, int pixels, bool *fresh, QDateTime *read) const {
    *fresh = false;
    const QFileInfo info(path);
    if (!info.isFile())
        return {};
    const QDateTime modified = info.lastModified();
    *read = modified;
    for (const bool alpha : {false, true}) {
        const QFileInfo cached(sharpPath(path, pixels, alpha));
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
    const QString target = sharpPath(path, pixels, image.hasAlphaChannel());
    // Copies of private pictures: only their owner may read them, as with
    // the thumbnails (see save()).
    const QString folder = QFileInfo(target).absolutePath();
    QDir().mkpath(folder);
    const auto ownerOnly = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    for (const QString &dir : {QFileInfo(folder).absolutePath(), folder})
        QFile::setPermissions(dir, ownerOnly);
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

QImage Thumbnails::load(const QString &path, const QString &mime, int size) const {
    const QFileInfo info(path);
    if (!info.isFile())
        return {};

    // Thumbnails of the cache itself would recurse forever.
    if (info.absoluteFilePath().startsWith(m_cache + "/"))
        return {};

    const QDateTime modified = info.lastModified();
    if (const QImage image(cachePath(path, size)); madeFor(image, modified))
        return image;
    if (failedBefore(path, modified))
        return {};

    // A folder's peek asks for any picture ("image/*"); named by extension,
    // as the grid names it, it goes to the same thumbnailer as its tile.
    const QString type = mime.endsWith("/*")
        ? QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchExtension).name()
        : mime;
    const int pixels = bucketPixels(bucketFor(size));
    bool attempted = false;
    QImage image = generate(path, type, pixels, attempted);
    if (image.isNull()) {
        // Only a reader or thumbnailer that ran and failed is remembered, so
        // one installed later still gets its turn.
        if (attempted)
            rememberFailure(path, modified);
        return {};
    }
    if (image.width() > pixels || image.height() > pixels)
        image = image.scaled(pixels, pixels, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    remember(image, path, size, modified);
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
    const QString command = m_thumbnailers.value(mime);
    if (!command.isEmpty()) {
        attempted = true;
        return runThumbnailer(command, path, pixels);
    }
    // Without a PDF thumbnailer installed, Poppler's pdftoppm (there with
    // CUPS) draws the first page.
    if (mime == "application/pdf" && !QStandardPaths::findExecutable("pdftoppm").isEmpty()) {
        attempted = true;
        return pdfPage(path, pixels);
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
    QString text = decode(head);
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
    for (QString row : text.replace("\r\n", "\n").split('\n')) {
        if (y > size.height() - margin)
            break;
        row.replace('\t', "    ");
        painter.drawText(QPointF(margin, y), row);
        y += line;
    }
    painter.end();
    return page;
}

QImage Thumbnails::pdfPage(const QString &path, int pixels) {
    QTemporaryDir scratch;
    if (!scratch.isValid())
        return {};
    QProcess process;
    process.start("pdftoppm", {"-f", "1", "-l", "1", "-singlefile", "-png", "-scale-to", QString::number(pixels),
                               path, scratch.filePath("page")});
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
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
    process.start(program, args);
    // A broken file must never hang a tile forever.
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {};
    return QImage(output);
}

void Thumbnails::remember(QImage image, const QString &path, int size, const QDateTime &modified) const {
    image.setText("Software", "folio");
    save(std::move(image), cachePath(path, size), path, modified);
}

// Markers say which round of thumbnailing failed: ones from before a fix
// (round 2: files on other machines) are tried again once.
static const QString failedRound = QStringLiteral("2");

void Thumbnails::rememberFailure(const QString &path, const QDateTime &modified) const {
    QImage marker(1, 1, QImage::Format_ARGB32);
    marker.fill(Qt::transparent);
    marker.setText("X-Folio::Round", failedRound);
    save(std::move(marker), failPath(path), path, modified);
}

bool Thumbnails::failedBefore(const QString &path, const QDateTime &modified) const {
    const QImage marker(failPath(path));
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
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile::setPermissions(m_cache, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

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
        // The time is only there to tell Qt's own cache apart; load() checks
        // the file's.
        const QStringList parts = m_id.split('/');
        // A tile scrolled away before its turn came shouldn't keep the pool
        // from the ones on screen.
        if (!m_cancelled && parts.size() >= 5 && parts.at(0) == "sharp") {
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
        }
        if (!m_cancelled && parts.size() >= 6 && parts.at(0) == "page") {
            const QString path = QUrl::fromPercentEncoding(parts.mid(5).join('/').toUtf8());
            m_image = Thumbnails::textPage(path, qBound(64, parts.at(1).toInt(), 1024),
                                           QColor::fromString("#" + parts.at(3)), QColor::fromString("#" + parts.at(4)));
        } else if (!m_cancelled && parts.size() >= 4) {
            const int size = parts.at(0).toInt();
            const QString mime = QUrl::fromPercentEncoding(parts.at(2).toUtf8());
            const QString path = QUrl::fromPercentEncoding(parts.mid(3).join('/').toUtf8());
            m_image = m_thumbnails->load(path, mime, qBound(64, size, 1024));
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
    m_pool.start(response);
    return response;
}
