#include "chooser.h"

#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

#include "backend.h"
#include "windows.h"

bool operator==(const ChooserRule &a, const ChooserRule &b) {
    return a.type == b.type && a.pattern == b.pattern;
}

bool operator==(const ChooserFilter &a, const ChooserFilter &b) {
    return a.name == b.name && a.rules == b.rules;
}

QDBusArgument &operator<<(QDBusArgument &arg, const ChooserRule &rule) {
    arg.beginStructure();
    arg << rule.type << rule.pattern;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, ChooserRule &rule) {
    arg.beginStructure();
    arg >> rule.type >> rule.pattern;
    arg.endStructure();
    return arg;
}

QDBusArgument &operator<<(QDBusArgument &arg, const ChooserFilter &filter) {
    arg.beginStructure();
    arg << filter.name << filter.rules;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, ChooserFilter &filter) {
    arg.beginStructure();
    arg >> filter.name >> filter.rules;
    arg.endStructure();
    return arg;
}

void ChooserFilter::registerTypes() {
    qDBusRegisterMetaType<ChooserRule>();
    qDBusRegisterMetaType<QList<ChooserRule>>();
    qDBusRegisterMetaType<ChooserFilter>();
    qDBusRegisterMetaType<QList<ChooserFilter>>();
}

// An option as it arrives: simple types already converted, compound ones
// still marshalled, so their signature is checked before they're read.
template <typename T>
static bool take(const QVariant &value, const char *signature, T *out) {
    if (value.metaType() == QMetaType::fromType<QDBusArgument>()) {
        const QDBusArgument arg = value.value<QDBusArgument>();
        if (arg.currentSignature() != QLatin1String(signature))
            return false;
        arg >> *out;
        return true;
    }
    if (value.metaType() != QMetaType::fromType<T>())
        return false;
    *out = value.value<T>();
    return true;
}

// Paths come as bytes in the filesystem's encoding, ending in a nul.
static QString fromBytes(const QByteArray &bytes) {
    return QFile::decodeName(bytes.constData());
}

static bool isBareName(const QString &name) {
    return !name.isEmpty() && !name.contains('/') && name != "." && name != "..";
}

ChooserRequest ChooserRequest::read(Mode mode, const QString &title, const QVariantMap &options, QString *error) {
    ChooserRequest request;
    request.mode = mode;
    request.title = title;
    request.folder = QDir::homePath();
    error->clear();
    const auto get = [&](const char *key, const char *signature, auto *out) {
        const QVariant value = options.value(QLatin1String(key));
        if (!value.isValid() || !error->isEmpty())
            return false;
        if (take(value, signature, out))
            return true;
        *error = QStringLiteral("%1 must be of type %2").arg(QLatin1String(key), QLatin1String(signature));
        return false;
    };

    QString label;
    if (get("accept_label", "s", &label))
        request.acceptLabel = withoutMnemonics(label);
    if (mode == Open) {
        get("multiple", "b", &request.multiple);
        get("directory", "b", &request.directory);
    }
    QByteArray folder;
    if (get("current_folder", "ay", &folder)) {
        const QString path = fromBytes(folder);
        if (QDir::isAbsolutePath(path) && QFileInfo(path).isDir())
            request.folder = QDir::cleanPath(path);
    }
    // Choosing a folder lists only folders, so the app's filters don't apply.
    if (mode != SaveFiles && !request.directory) {
        get("filters", "a(sa(us))", &request.filters);
        ChooserFilter current;
        if (get("current_filter", "(sa(us))", &current)) {
            request.filter = request.filters.indexOf(current);
            // Without a list to pick from, it's the one filter there is.
            if (request.filters.isEmpty()) {
                request.filters << current;
                request.filter = 0;
            }
        }
        if (request.filter < 0 && !request.filters.isEmpty())
            request.filter = 0;
    }
    // The app only suggests a name: the last part of what it sends, never
    // a path out of the folder the user picks.
    if (mode == Save) {
        QString name;
        if (get("current_name", "s", &name)) {
            name = name.section('/', -1);
            request.name = isBareName(name) ? name : QString();
        }
        QByteArray file;
        if (get("current_file", "ay", &file)) {
            const QFileInfo info(fromBytes(file));
            if (info.isAbsolute() && isBareName(info.fileName()) && QFileInfo(info.absolutePath()).isDir()) {
                request.folder = QDir::cleanPath(info.absolutePath());
                request.name = info.fileName();
            }
        }
    }
    if (mode == SaveFiles) {
        QByteArrayList files;
        if (get("files", "aay", &files)) {
            for (const QByteArray &bytes : std::as_const(files)) {
                const QString name = fromBytes(bytes);
                if (!isBareName(name)) {
                    *error = QStringLiteral("files must be bare file names: %1").arg(name);
                    break;
                }
                request.files << name;
            }
        }
        if (request.files.isEmpty() && error->isEmpty())
            *error = QStringLiteral("files names no files to save");
    }
    return request;
}

QString ChooserRequest::withoutMnemonics(const QString &label) {
    // GTK marks a mnemonic with _ and writes a real underscore as __.
    QString plain;
    for (qsizetype i = 0; i < label.size(); ++i) {
        if (label.at(i) == '_' && i + 1 < label.size())
            ++i;
        plain += label.at(i);
    }
    return plain;
}

QStringList ChooserRequest::patterns() const {
    // No file's type is a folder's, so only folders fit.
    if (directory)
        return {QStringLiteral("inode/directory")};
    QStringList patterns;
    if (filter < 0 || filter >= filters.size())
        return patterns;
    // FolderModel tells a MIME type by its slash, which no glob for a name
    // could use.
    for (const ChooserRule &rule : filters.at(filter).rules)
        if ((rule.type == 0 && !rule.pattern.contains('/')) || (rule.type == 1 && rule.pattern.contains('/')))
            patterns << rule.pattern;
    return patterns;
}

QStringList ChooserRequest::answer(const QString &folder, const QStringList &picked, const QString &typed) const {
    QStringList paths;
    if (mode == Open) {
        for (const QString &path : picked) {
            const QFileInfo info(path);
            if (directory ? info.isDir() : info.isFile())
                paths << path;
        }
        // The cursor always rests on some folder, so one under it isn't a
        // choice: the answer is the folder shown, unless several are picked.
        if (directory && !(multiple && paths.size() > 1))
            return QFileInfo(folder).isDir() ? QStringList{folder} : QStringList{};
        return multiple ? paths : paths.mid(0, 1);
    }
    // Saving goes into the folder shown, which Recents and plugins' views
    // aren't.
    if (!QFileInfo(folder).isDir())
        return {};
    if (mode == SaveFiles) {
        for (const QString &name : files)
            paths << QDir(folder).filePath(name);
        return paths;
    }
    // Typed by the user, the name may be a path; one that ends in a folder
    // is somewhere to go, not a file to save.
    if (typed.trimmed().isEmpty() || typed.trimmed().endsWith('/'))
        return {};
    // A name is kept as typed, spaces and all; only a path is read the way
    // the path bar reads one.
    const QString path = typed.contains('/') || typed.startsWith('~')
                             ? QDir::cleanPath(Backend::expandTyped(typed, QDir::homePath(), folder))
                             : QDir(folder).filePath(typed);
    const QFileInfo info(path);
    if (info.isDir() || !QFileInfo(info.absolutePath()).isDir())
        return {};
    return {path};
}

ChooserDialog::ChooserDialog(const ChooserRequest &request, const QString &caller, Reply reply, QObject *parent)
    : QObject(parent), m_request(request), m_caller(caller), m_reply(std::move(reply)) {}

ChooserDialog::~ChooserDialog() {
    respond(1, {});
}

QString ChooserDialog::mode() const {
    switch (m_request.mode) {
    case ChooserRequest::Save: return QStringLiteral("save");
    case ChooserRequest::SaveFiles: return QStringLiteral("saveFiles");
    case ChooserRequest::Open: break;
    }
    return m_request.directory ? QStringLiteral("directory") : QStringLiteral("open");
}

QStringList ChooserDialog::filters() const {
    QStringList names;
    for (const ChooserFilter &filter : m_request.filters)
        names << filter.name;
    return names;
}

void ChooserDialog::setFilter(int filter) {
    if (filter == m_request.filter || filter < 0 || filter >= m_request.filters.size())
        return;
    m_request.filter = filter;
    emit filterChanged();
}

QStringList ChooserDialog::existing(const QStringList &paths) {
    QStringList there;
    // A link that leads nowhere too: saving would write where it leads.
    for (const QString &path : paths)
        if (const QFileInfo info(path); info.exists() || info.isSymLink())
            there << path;
    return there;
}

QStringList ChooserDialog::folders(const QStringList &paths) {
    QStringList folders;
    for (const QString &path : paths)
        if (QFileInfo(path).isDir())
            folders << path;
    return folders;
}

void ChooserDialog::accept(const QStringList &paths) {
    if (paths.isEmpty())
        return;
    QStringList uris;
    for (const QString &path : paths)
        uris << QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded);
    QVariantMap results{{"uris", uris}};
    if (m_request.filter >= 0)
        results.insert("current_filter", QVariant::fromValue(m_request.filters.at(m_request.filter)));
    respond(0, results);
    emit done();
}

void ChooserDialog::cancel() {
    respond(1, {});
    emit done();
}

void ChooserDialog::respond(uint response, const QVariantMap &results) {
    if (m_answered)
        return;
    m_answered = true;
    if (m_reply)
        m_reply(response, results);
}

bool ChooserDialog::closeFor(const QString &sender) {
    if (sender != m_caller)
        return false;
    cancel();
    return true;
}

void ChooserDialog::Close(const QDBusMessage &message) {
    if (closeFor(message.service()))
        return;
    message.setDelayedReply(true);
    QDBusConnection::sessionBus().send(
        message.createErrorReply(QDBusError::AccessDenied, QStringLiteral("Only the caller can close its request")));
}

Chooser::Chooser(Windows *windows)
    : QDBusAbstractAdaptor(windows), m_windows(windows), m_linger(std::make_unique<QEventLoopLocker>()) {
    m_idle.setSingleShot(true);
    m_idle.setInterval(60 * 1000);
    connect(&m_idle, &QTimer::timeout, this, [this] { m_linger.reset(); });
    m_idle.start();
}

bool Chooser::registerOn(Windows *windows) {
    ChooserFilter::registerTypes();
    QDBusConnection bus = QDBusConnection::sessionBus();
    new Chooser(windows);
    // The portal asks for the object's properties as soon as the name is
    // there, so the object comes first.
    return bus.registerObject("/org/freedesktop/portal/desktop", windows)
           && bus.registerService("org.freedesktop.impl.portal.desktop.folio");
}

void Chooser::OpenFile(const QDBusObjectPath &handle, const QString &, const QString &, const QString &title,
                       const QVariantMap &options, const QDBusMessage &message) {
    open(ChooserRequest::Open, handle, title, options, message);
}

void Chooser::SaveFile(const QDBusObjectPath &handle, const QString &, const QString &, const QString &title,
                       const QVariantMap &options, const QDBusMessage &message) {
    open(ChooserRequest::Save, handle, title, options, message);
}

void Chooser::SaveFiles(const QDBusObjectPath &handle, const QString &, const QString &, const QString &title,
                        const QVariantMap &options, const QDBusMessage &message) {
    open(ChooserRequest::SaveFiles, handle, title, options, message);
}

void Chooser::open(ChooserRequest::Mode mode, const QDBusObjectPath &handle, const QString &title,
                   const QVariantMap &options, const QDBusMessage &message) {
    // Answered once the dialog is, however that comes about.
    message.setDelayedReply(true);
    QDBusConnection bus = QDBusConnection::sessionBus();
    QString error;
    const ChooserRequest request = ChooserRequest::read(mode, title, options, &error);
    // Anywhere else, the request could take another object's path.
    if (error.isEmpty() && !handle.path().startsWith("/org/freedesktop/portal/desktop/request/"))
        error = QStringLiteral("handle must be a request's path");
    if (!error.isEmpty()) {
        bus.send(message.createErrorReply(QDBusError::InvalidArgs, error));
        return;
    }

    auto *dialog = new ChooserDialog(request, message.service(), [bus, message](uint response, const QVariantMap &results) mutable {
        bus.send(message.createReply(QVariantList{response, results}));
    });
    Backend *backend = bus.registerObject(handle.path(), dialog, QDBusConnection::ExportScriptableSlots)
                           ? m_windows->open(request.folder, {}, dialog)
                           : nullptr;
    if (!backend) {
        dialog->respond(2, {});
        delete dialog;
        return;
    }
    // It goes with its window, answering if nothing else has.
    dialog->setParent(backend);
    ++m_open;
    m_idle.stop();
    if (!m_linger)
        m_linger = std::make_unique<QEventLoopLocker>();
    connect(dialog, &QObject::destroyed, this, [this] {
        if (--m_open == 0)
            m_idle.start();
    });
}
