#pragma once

#include <QObject>
#include <QString>

typedef struct _GMountOperation GMountOperation;
typedef struct _GError GError;
typedef struct _GCancellable GCancellable;

// Mounts tailnet machines over SFTP, and drives from the sidebar, through
// GIO, the way Nautilus does, so they land where every app can see them.
// GVfs asks questions while mounting. Host-key questions are answered here for the user, since
// Tailscale has already authenticated the machine; only a password request
// (when keys aren't set up) goes to the window to put to the user, instead
// of dying silently as it does under `gio mount` with no terminal to ask in.
// A drive's questions aren't answered for the user, and its passphrase
// request (an encrypted drive) goes to the window the same way; anything
// udisks needs authorized goes to the session's polkit agent.
class Mounter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool asking READ asking NOTIFY askingChanged)
    Q_PROPERTY(QString prompt READ prompt NOTIFY askingChanged)
    Q_PROPERTY(QString user READ user NOTIFY askingChanged)
    // The request is for a drive's passphrase, not a sign-in.
    Q_PROPERTY(bool drive READ drive NOTIFY askingChanged)

public:
    explicit Mounter(QObject *parent = nullptr);
    ~Mounter() override;

    bool busy() const { return m_operation != nullptr; }
    bool asking() const { return m_asking; }
    QString prompt() const { return m_prompt; }
    QString user() const { return m_user; }
    bool drive() const { return m_drive; }

    // Mounts the location; `mounted` carries its local GVfs path.
    void mount(const QString &uri);
    // Mounts the volume Drives knows by `id`; `mounted` carries its folder.
    void mountVolume(const QString &id);
    // How long a drive's mount may run once its passphrase is in.
    static constexpr int stalledAfterMs = 60000;

    // The user's answer to a password request; remembered for this session
    // only, never written to a keyring without asking.
    Q_INVOKABLE void providePassword(const QString &user, const QString &password);
    // Closes the password request and abandons the mount.
    Q_INVOKABLE void cancel();

signals:
    void askingChanged();
    void mounted(const QString &path);
    void failed(const QString &error);

private:
    // GIO's C callbacks, in mounter.cpp.
    friend struct MounterCallbacks;
    void setAsking(bool asking);
    void finish();
    void begin(bool drive);
    void report(bool ok, bool already, const QString &local, const GError *error);

    GMountOperation *m_operation = nullptr;
    bool m_asking = false;
    bool m_drive = false;
    // A drive's mount, cancelled when it stalls after unlocking (see
    // providePassword); each mount counts as a new attempt.
    QString m_volume;
    GCancellable *m_cancellable = nullptr;
    bool m_lost = false;
    // Its passphrase was given.
    bool m_answered = false;
    int m_attempt = 0;
    QString m_prompt;
    QString m_user;
};
