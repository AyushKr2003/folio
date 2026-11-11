#include "mounter.h"
#include "drives.h"

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QTimer>

// GIO's headers use `signals` as a struct member, which Qt defines as a
// keyword.
#undef signals
#include <gio/gio.h>
#define signals Q_SIGNALS

namespace {

// The encrypted volume `id` names is unlocked: the kernel shows its
// cleartext (dm-crypt) device holding it.
bool unlockedNow(const QString &id) {
    GVolume *volume = Drives::find(id);
    if (!volume)
        return false;
    char *device = g_volume_get_identifier(volume, G_VOLUME_IDENTIFIER_KIND_UNIX_DEVICE);
    const QString name = QFileInfo(QFileInfo(QString::fromUtf8(device ? device : "")).canonicalFilePath()).fileName();
    g_free(device);
    g_object_unref(volume);
    return !name.isEmpty()
        && !QDir(QStringLiteral("/sys/class/block/%1/holders").arg(name)).isEmpty(QDir::NoDotAndDotDot | QDir::AllEntries);
}

} // namespace

struct MounterCallbacks {
    // GMountOperation's own handlers run after these and answer
    // "unhandled" at once, which GVfs reports as a cancelled login; stopping
    // the emission leaves the operation waiting for the user's reply.

    // GVfs's SFTP backend asks two questions, about a host seen for the
    // first time and about a host whose key differs from the key for its IP
    // address, and both list "Log In Anyway" first. A tailnet machine is
    // reached over WireGuard and already authenticated by Tailscale, so the
    // answer is always yes. A changed key for a known machine never gets
    // here: ssh refuses it outright.
    static void question(GMountOperation *op, const char *, const char **, gpointer data) {
        // A drive's questions are left to GIO, which declines them, rather
        // than answered yes for the user.
        if (static_cast<Mounter *>(data)->m_drive)
            return;
        g_signal_stop_emission_by_name(op, "ask-question");
        g_mount_operation_set_choice(op, 0);
        g_mount_operation_reply(op, G_MOUNT_OPERATION_HANDLED);
    }

    static void password(GMountOperation *op, const char *message, const char *user, const char *,
                         GAskPasswordFlags, gpointer data) {
        g_signal_stop_emission_by_name(op, "ask-password");
        auto *self = static_cast<Mounter *>(data);
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
            self->report(ok, error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED),
                         QString::fromUtf8(path ? path : ""), error);
            g_free(path);
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
            self->report(ok, error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED), local, error);
        }
        if (error)
            g_error_free(error);
    }
};

void Mounter::report(bool ok, bool already, const QString &local, const GError *error) {
    // Given up on after the passphrase went in, or GVfs calling it
    // cancelled when nobody did: either the drive unlocked with nothing to
    // mount inside, or it's still working. Which, the kernel says.
    const bool stopped = m_drive && m_answered && !ok && !already
        && (!error || g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED));
    const bool unlocked = stopped && unlockedNow(m_volume);
    const bool timedOut = stopped && m_lost;
    finish();
    // Mounted earlier (by Folio or another app) is as good as now.
    if (ok || already) {
        if (local.isEmpty())
            emit failed(tr("GVfs mounted it but gave no local folder for it"));
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
    // An encrypted drive that unlocked and then didn't mount has nothing
    // udisks can mount inside (no filesystem, or LVM).
    if (unlocked) {
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

void Mounter::begin(bool drive) {
    m_drive = drive;
    m_operation = g_mount_operation_new();
    g_signal_connect(m_operation, "ask-question", G_CALLBACK(MounterCallbacks::question), this);
    g_signal_connect(m_operation, "ask-password", G_CALLBACK(MounterCallbacks::password), this);
    g_signal_connect(m_operation, "aborted", G_CALLBACK(MounterCallbacks::aborted), this);
}

void Mounter::mount(const QString &uri) {
    if (m_operation)
        return;
    GFile *file = g_file_new_for_uri(uri.toUtf8().constData());
    begin(false);
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
    begin(true);
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
        const int attempt = m_attempt;
        QTimer::singleShot(stalledAfterMs, this, [this, attempt] {
            if (attempt != m_attempt || !m_cancellable || m_asking)
                return;
            m_lost = true;
            g_cancellable_cancel(m_cancellable);
        });
    }
}

void Mounter::cancel() {
    if (!m_operation || !m_asking)
        return;
    setAsking(false);
    g_mount_operation_reply(m_operation, G_MOUNT_OPERATION_ABORTED);
}

void Mounter::setAsking(bool asking) {
    m_asking = asking;
    if (!asking)
        m_prompt.clear();
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
