#include "mounter.h"
#include "drives.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <unistd.h>

// GIO's headers use `signals` as a struct member, which Qt defines as a
// keyword.
#undef signals
#include <gio/gio.h>
#define signals Q_SIGNALS

namespace {

// A block device holds a filesystem, by what udev found on it.
bool holdsFilesystem(const QString &name) {
    QFile dev(QStringLiteral("/sys/class/block/%1/dev").arg(name));
    if (!dev.open(QIODevice::ReadOnly))
        return false;
    QFile data(QStringLiteral("/run/udev/data/b%1").arg(QString::fromLatin1(dev.readAll()).trimmed()));
    return data.open(QIODevice::ReadOnly) && data.readAll().contains("\nE:ID_FS_USAGE=filesystem\n");
}

// The encrypted volume `id` names unlocked (the kernel shows a cleartext,
// dm-crypt, device holding it) and nothing inside is a filesystem: no
// filesystem at all, or LVM. A filesystem inside means it's only slow.
bool unlockedEmpty(const QString &id) {
    GVolume *volume = Drives::find(id);
    if (!volume)
        return false;
    char *device = g_volume_get_identifier(volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
    const QString name = QFileInfo(QFileInfo(QString::fromUtf8(device ? device : "")).canonicalFilePath()).fileName();
    g_free(device);
    g_object_unref(volume);
    if (name.isEmpty())
        return false;
    const QStringList holders = QDir(QStringLiteral("/sys/class/block/%1/holders").arg(name))
                                    .entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
    if (holders.isEmpty())
        return false;
    for (const QString &holder : holders)
        if (holdsFilesystem(holder))
            return false;
    return true;
}

} // namespace

struct MounterCallbacks {
    // GMountOperation's own handlers run after these and answer
    // "unhandled" at once, which GVfs reports as a cancelled login; stopping
    // the emission leaves the operation waiting for the user's reply.

    // GVfs's SFTP backend asks two questions, about a host seen for the
    // first time and about a host whose key differs from the key for its IP
    // address, and both list "Log In Anyway" first. A verified machine (one
    // on a tailnet, reached over WireGuard and already authenticated by
    // Tailscale) gets yes. A changed key for a known machine never gets
    // here: ssh refuses it outright. Any other server's questions (a WebDAV
    // server's certificate that can't be verified, too) are the user's.
    static void question(GMountOperation *op, const char *message, const char **choices, gpointer data) {
        g_signal_stop_emission_by_name(op, "ask-question");
        auto *self = static_cast<Mounter *>(data);
        if (self->m_verified) {
            g_mount_operation_set_choice(op, 0);
            g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
            return;
        }
        self->m_prompt = QString::fromUtf8(message).trimmed();
        for (const char **choice = choices; choice && *choice; ++choice)
            self->m_choices << QString::fromUtf8(*choice);
        self->setAsking(true);
    }

    static void password(GMountOperation *op, const char *message, const char *user, const char *,
                         GAskPasswordFlags, gpointer data) {
        g_signal_stop_emission_by_name(op, "ask-password");
        auto *self = static_cast<Mounter *>(data);
        // Asked again (a wrong passphrase): the last reply's clock stops.
        ++self->m_attempt;
        self->m_prompt = QString::fromUtf8(message).trimmed();
        self->m_user = QString::fromUtf8(user ? user : "");
        self->setAsking(true);
    }

    // GVfs gave up waiting for an answer.
    static void aborted(GMountOperation *, gpointer data) {
        static_cast<Mounter *>(data)->setAsking(false);
    }

    static void mounted(GObject *source, GAsyncResult *result, gpointer data) {
        auto *guard = static_cast<QPointer<Mounter> *>(data);
        Mounter *self = guard->data();
        delete guard;
        GError *error = nullptr;
        const bool ok = g_file_mount_enclosing_volume_finish(G_FILE(source), result, &error);
        if (self) {
            char *path = g_file_get_path(G_FILE(source));
            const QString local = QString::fromUtf8(path ? path : "");
            g_free(path);
            // Where GVfs put it, for the address it was asked for: only
            // SFTP's folder can be worked out ahead of time.
            if ((ok || alreadyMounted(error)) && local.startsWith(Mounter::root() + "/"))
                Mounter::known()[self->m_url] = local;
            self->report(ok, alreadyMounted(error), local, error);
        }
        if (error)
            g_error_free(error);
    }

    static void volumeMounted(GObject *source, GAsyncResult *result, gpointer data) {
        auto *guard = static_cast<QPointer<Mounter> *>(data);
        Mounter *self = guard->data();
        delete guard;
        GError *error = nullptr;
        const bool ok = g_volume_mount_finish(G_VOLUME(source), result, &error);
        if (self) {
            QString local;
            if (GMount *mount = g_volume_get_mount(G_VOLUME(source))) {
                GFile *root = g_mount_get_root(mount);
                char *path = g_file_get_path(root);
                local = QString::fromUtf8(path ? path : "");
                g_free(path);
                g_object_unref(root);
                g_object_unref(mount);
            }
            self->report(ok, alreadyMounted(error), local, error);
        }
        if (error)
            g_error_free(error);
    }

    // Mounted earlier (by Folio or another app) is as good as now.
    static bool alreadyMounted(const GError *error) {
        return error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED);
    }
};

void Mounter::report(bool ok, bool already, const QString &local, const GError *error) {
    // Given up on after the passphrase went in, or GVfs calling it
    // cancelled when nobody did: either the drive unlocked with nothing to
    // mount inside, or it's still working (unlocking, or mounting a slow
    // filesystem). Which, the kernel and udev say.
    const bool stopped = m_drive && m_answered && !ok && !already
        && (!error || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED));
    const bool empty = stopped && unlockedEmpty(m_volume);
    const bool timedOut = stopped && m_lost;
    finish();
    if (ok || already) {
        if (local.isEmpty())
            emit failed(tr("GVfs gave no local folder for the mount"));
        else
            emit mounted(local);
        return;
    }
    // The user turning down the password, or polkit's prompt for a drive,
    // is nothing to report.
    if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) {
        emit failed(QString());
        return;
    }
    // An encrypted drive that unlocked with nothing udisks can mount inside.
    if (empty) {
        emit failed(tr("it unlocked, but there's nothing on it that can be mounted"));
        return;
    }
    if (timedOut) {
        emit failed(tr("it didn't finish mounting within a minute"));
        return;
    }
    emit failed(QString::fromUtf8(error ? error->message : ""));
}

Mounter::Mounter(QObject *parent) : QObject(parent) {}

Mounter::~Mounter() {
    // A prompt nobody will answer any more would keep GVfs waiting.
    cancel();
    if (m_operation)
        g_signal_handlers_disconnect_by_data(m_operation, this);
    finish();
}

QString Mounter::root() {
    return QStringLiteral("/run/user/%1/gvfs").arg(getuid());
}

QHash<QString, QString> &Mounter::known() {
    static QHash<QString, QString> roots;
    return roots;
}

QString Mounter::folderFor(const QString &url) {
    const QUrl parsed(url);
    if (parsed.scheme() != "sftp" || parsed.host().isEmpty())
        return known().value(url).section('/', 0, 5);
    // GVfs keeps a mount's parts sorted by key: host, port, user.
    return root() + "/sftp:host=" + parsed.host() + (parsed.port() < 0 ? QString() : ",port=" + QString::number(parsed.port()))
           + (parsed.userName().isEmpty() ? QString() : ",user=" + parsed.userName());
}

QString Mounter::destinationFor(const QString &url) {
    if (known().contains(url))
        return known().value(url);
    const QString mount = folderFor(url);
    // The address's path is read from the server's top, where ".." goes no
    // higher, as GVfs reads it. Joined first and cleaned after, it would
    // climb out of the mount to a local folder ("sftp://nas/../../tmp");
    // QDir::cleanPath keeps a leading "..", so the steps are taken here.
    if (mount.isEmpty())
        return {};
    QStringList parts;
    for (const QString &part : QUrl(url).path().split('/', Qt::SkipEmptyParts)) {
        if (part == "..") {
            if (!parts.isEmpty())
                parts.removeLast();
        } else if (part != ".") {
            parts << part;
        }
    }
    return parts.isEmpty() ? mount : mount + '/' + parts.join('/');
}

void Mounter::begin(bool drive, bool verified) {
    m_drive = drive;
    m_verified = verified;
    m_operation = g_mount_operation_new();
    g_signal_connect(m_operation, "ask-question", G_CALLBACK(MounterCallbacks::question), this);
    g_signal_connect(m_operation, "ask-password", G_CALLBACK(MounterCallbacks::password), this);
    g_signal_connect(m_operation, "aborted", G_CALLBACK(MounterCallbacks::aborted), this);
}

void Mounter::mount(const QString &uri, bool verified) {
    if (m_operation)
        return;
    begin(false, verified);
    m_url = uri;
    GFile *file = g_file_new_for_uri(uri.toUtf8().constData());
    // Qt runs on GLib's main loop on Linux, so the reply arrives on the UI
    // thread like any other event. The window can close before GVfs answers,
    // and GIO calls back regardless, so the callback gets a guard, not `this`.
    g_file_mount_enclosing_volume(file, G_MOUNT_MOUNT_NONE, m_operation, nullptr,
                                  MounterCallbacks::mounted, new QPointer<Mounter>(this));
    // The pending mount holds its own reference to the file.
    g_object_unref(file);
}

void Mounter::mountVolume(const QString &id) {
    if (m_operation)
        return;
    GVolume *volume = Drives::find(id);
    if (!volume) {
        emit failed(tr("It's no longer connected"));
        return;
    }
    // A drive's questions (another app has files open on it) are the
    // user's, as an unverified server's are.
    begin(true, false);
    m_volume = id;
    m_lost = false;
    m_cancellable = g_cancellable_new();
    g_volume_mount(volume, G_MOUNT_MOUNT_NONE, m_operation, m_cancellable, MounterCallbacks::volumeMounted,
                   new QPointer<Mounter>(this));
    g_object_unref(volume);
}

void Mounter::providePassword(const QString &user, const QString &password) {
    if (!m_operation || !m_asking)
        return;
    setAsking(false);
    if (!user.isEmpty())
        g_mount_operation_set_username(m_operation, user.toUtf8().constData());
    g_mount_operation_set_password(m_operation, password.toUtf8().constData());
    // A drive's passphrase isn't kept at all: once unlocked it stays so.
    g_mount_operation_set_password_save(m_operation, m_drive ? G_PASSWORD_SAVE_NEVER : G_PASSWORD_SAVE_FOR_SESSION);
    g_mount_operation_reply(m_operation, G_MOUNT_OPERATION_HANDLED);
    // An encrypted drive that unlocks but holds nothing udisks can mount
    // (no filesystem, or LVM inside) leaves GVfs's mount pending for good;
    // past this, it's given up on. Asked only once the passphrase is in, so
    // a polkit prompt before it isn't cut short.
    if (m_drive && m_cancellable) {
        m_answered = true;
        // Each reply gets its own minute, from when it went in.
        const int attempt = ++m_attempt;
        QTimer::singleShot(stalledAfterMs, this, [this, attempt] {
            if (attempt != m_attempt || !m_cancellable || m_asking)
                return;
            m_lost = true;
            g_cancellable_cancel(m_cancellable);
        });
    }
}

void Mounter::choose(int choice) {
    if (!m_operation || !m_asking || choice < 0 || choice >= m_choices.size())
        return;
    setAsking(false);
    g_mount_operation_set_choice(m_operation, choice);
    g_mount_operation_reply(m_operation, G_MOUNT_OPERATION_HANDLED);
}

void Mounter::cancel() {
    if (!m_operation || !m_asking)
        return;
    setAsking(false);
    g_mount_operation_reply(m_operation, G_MOUNT_OPERATION_ABORTED);
}

void Mounter::setAsking(bool asking) {
    m_asking = asking;
    if (!asking) {
        m_prompt.clear();
        m_choices.clear();
    }
    emit askingChanged();
}

void Mounter::finish() {
    if (m_asking)
        setAsking(false);
    if (m_cancellable) {
        g_object_unref(m_cancellable);
        m_cancellable = nullptr;
    }
    m_lost = false;
    m_answered = false;
    ++m_attempt;
    if (m_operation) {
        g_object_unref(m_operation);
        m_operation = nullptr;
    }
}
