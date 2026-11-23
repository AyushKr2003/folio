#pragma once

#include <QHash>
#include <QDateTime>
#include <QImage>
#include <QQuickAsyncImageProvider>
#include <QStringList>
#include <QThreadPool>
#include <mutex>

// Thumbnails per the freedesktop thumbnail spec: one shared cache under
// ~/.cache/thumbnails, keyed by the MD5 of the file's URI and invalidated by
// its modification time, so thumbnails made by any other app are reused and
// ours are useful to them. Images are scaled and text is set on a page
// in-process; everything else goes through the thumbnailers installed in
// /usr/share/thumbnailers (videos via ffmpegthumbnailer, and so on), with
// Poppler's pdftoppm drawing PDFs when no thumbnailer does. Nothing is generated until a
// tile asks for it: there is no background crawl.
class Thumbnails {
public:
    // `remoteRoot` is where GVfs mounts other machines (Mounter::root());
    // tests give their own. `patience` is how long a thumbnailer may take,
    // in milliseconds; tests shorten it.
    explicit Thumbnails(const QString &cacheDirectory = {}, const QStringList &thumbnailerDirectories = {},
                        const QString &remoteRoot = {}, int patience = 15000);

    // Returns a thumbnail no larger than `size` on its longest side, from the
    // cache when it is fresh, or a null image when none can be made.
    QImage load(const QString &path, const QString &mime, int size) const;
    // Like load(), but defers writing the cache or failure marker so the
    // provider can hand the pixels to QML before encoding PNG to disk.
    QImage loadNow(const QString &path, const QString &mime, int size, bool *fresh, bool *failed,
                   QDateTime *modified) const;
    void remember(QImage image, const QString &path, int size, const QDateTime &modified) const;
    void rememberFailure(const QString &path, const QDateTime &modified) const;

    // Read a fresh thumbnail from any standard bucket, smallest first.
    QImage cached(const QString &path) const;

    // A picture at `pixels` on its longest side, as sharp as the details
    // column draws it: read from the file when it's that small already,
    // otherwise from Folio's own cache (~/.cache/folio/sharp) when one's been
    // made, and made from the file when not. A picture Qt can't read gets
    // the largest thumbnail instead. `fresh` says the picture was made just
    // now, for keep() to cache, dated `modified`: the picture's time as it
    // was before its pixels were read, so one saved over meanwhile is made
    // again rather than kept old under its new time.
    QImage sharp(const QString &path, const QString &mime, int pixels, bool *fresh, QDateTime *modified) const;
    void keep(const QImage &image, const QString &path, int pixels, const QDateTime &modified) const;

    static QString cacheKey(const QString &path);
    static QString bucketFor(int size);
    // A text file's first lines set on a page in the given colors. Made
    // fresh each time rather than cached, since the colors follow the theme.
    static QImage textPage(const QString &path, int pixels, const QColor &paper, const QColor &ink);
    QString thumbnailerFor(const QString &mime) const;

private:
    void ensureThumbnailers() const;
    QString cachePath(const QString &key, int size) const;
    QString failPath(const QString &key) const;
    QString sharpPath(const QString &stem, int pixels, bool alpha) const;
    // Sets `attempted` when Qt's reader or a thumbnailer could take the file.
    QImage generate(const QString &path, const QString &mime, int pixels, bool &attempted) const;
    QImage runThumbnailer(const QString &command, const QString &path, int pixels) const;
    static QImage pdfPage(const QString &path, int pixels, int patience);
    bool failedBefore(const QString &key, const QDateTime &modified) const;
    void save(QImage image, const QString &target, const QString &path, const QDateTime &modified) const;
    static bool madeFor(const QImage &image, const QDateTime &modified);

    QString m_cache;
    QString m_sharpRoot;
    QString m_remoteRoot;
    int m_patience;
    QStringList m_thumbnailerDirs;
    mutable std::once_flag m_thumbnailersOnce;
    mutable QHash<QString, QString> m_thumbnailers;
};

// Serves image://thumb/<size>/<modified>/<mime>/<path> to QML on a thread
// pool, and sharp/<pixels>/<modified>/<mime>/<path> for the details.
class ThumbnailProvider : public QQuickAsyncImageProvider {
public:
    explicit ThumbnailProvider(const Thumbnails *thumbnails);
    QQuickImageResponse *requestImageResponse(const QString &id, const QSize &requestedSize) override;

private:
    const Thumbnails *m_thumbnails;
    QThreadPool m_pool;
};
