#pragma once

#include <QHash>
#include <QObject>
#include <QString>

typedef struct _GMountOperation GMountOperation;
typedef struct _GError GError;
typedef struct _GCancellable GCancellable;

// Mounts other machines (sftp://, smb://, anything GVfs mounts) through GIO,
// the way Nautilus does, so they land under GVfs where every app can see
// them. GVfs asks questions while mounting. Host-key questions are answered
// here with yes when the mount is verified (a plugin such as Tailscale has
// already made sure who's on the other end). Everything else (a password,
// when keys aren't set up; a server's unknown key or certificate) goes to
// the window to put to the user, instead of dying silently as it does under
// `gio mount` with no terminal to ask in. Drives from the sidebar mount
// here too, their passphrase (an encrypted drive) put to the user the same
// way; anything udisks needs authorized goes to the session's polkit agent.
class Mounter : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool asking READ asking NOTIFY askingChanged)
    Q_PROPERTY(QString prompt READ prompt NOTIFY askingChanged)
    Q_PROPERTY(QString user READ user NOTIFY askingChanged)
    // A question's answers, in GVfs's words; empty for a password request.
    Q_PROPERTY(QStringList choices READ choices NOTIFY askingChanged)
    // The request is for a drive's passphrase, not a sign-in.
    Q_PROPERTY(bool drive READ drive NOTIFY askingChanged)

public:
    explicit Mounter(QObject *parent = nullptr);
    ~Mounter() override;

    bool busy() const { return m_operation != nullptr; }
    bool asking() const { return m_asking; }
    QString prompt() const { return m_prompt; }
    QString user() const { return m_user; }
    QStringList choices() const { return m_choices; }
    bool drive() const { return m_drive; }

    // Mounts the location; `mounted` carries its local GVfs path. A
    // verified location's questions are answered here, with their first
    // choice.
    void mount(const QString &uri, bool verified);
    // Mounts the volume Drives knows by `id`; `mounted` carries its folder.
    void mountVolume(const QString &id);
    // How long a drive's mount may run once its passphrase is in.
    static constexpr int stalledAfterMs = 60000;

    // Where GVfs keeps its mounts, and the mount `url` is in (its root,
    // where its row is lit): worked out for SFTP, whose mount is named by
    // the URL alone, and known for the rest once mounted; empty otherwise.
    static QString root();
    static QString folderFor(const QString &url);
    // The folder `url` itself opens, inside its mount; empty when unknown.
    static QString destinationFor(const QString &url);
    // Each address mounted this session and the folder GVfs opened for it.
    static QHash<QString, QString> &known();

    // The user's answer to a password request; remembered for this session
    // only, never written to a keyring without asking.
    Q_INVOKABLE void providePassword(const QString &user, const QString &password);
    // The user's answer to a question, by its place among the choices.
    Q_INVOKABLE void choose(int choice);
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
    void begin(bool drive, bool verified);
    void report(bool ok, bool already, const QString &local, const GError *error);

    GMountOperation *m_operation = nullptr;
    bool m_verified = false;
    bool m_drive = false;
    // A drive's mount, cancelled when it stalls after unlocking (see
    // providePassword); each mount, passphrase request and reply counts as
    // a new attempt, so only the latest reply's timer can cancel it.
    QString m_volume;
    GCancellable *m_cancellable = nullptr;
    bool m_lost = false;
    // Its passphrase was given.
    bool m_answered = false;
    int m_attempt = 0;
    QString m_url;
    bool m_asking = false;
    QString m_prompt;
    QString m_user;
    QStringList m_choices;
};
