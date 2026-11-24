#pragma once

#include <QDBusAbstractAdaptor>
#include <QDBusArgument>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QEventLoopLocker>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <functional>
#include <memory>

class Windows;

// A filter an app offers: its name, and rules that are each a glob for the
// name (type 0, "*.png") or a MIME type (type 1, "image/png", "image/*").
struct ChooserRule {
    uint type = 0;
    QString pattern;
};
struct ChooserFilter {
    QString name;
    QList<ChooserRule> rules;

    // Both, and lists of them, for QtDBus to send back and to test with.
    static void registerTypes();
};
bool operator==(const ChooserRule &a, const ChooserRule &b);
bool operator==(const ChooserFilter &a, const ChooserFilter &b);
QDBusArgument &operator<<(QDBusArgument &arg, const ChooserRule &rule);
const QDBusArgument &operator>>(const QDBusArgument &arg, ChooserRule &rule);
QDBusArgument &operator<<(QDBusArgument &arg, const ChooserFilter &filter);
const QDBusArgument &operator>>(const QDBusArgument &arg, ChooserFilter &filter);
Q_DECLARE_METATYPE(ChooserRule)
Q_DECLARE_METATYPE(ChooserFilter)

// What an app asks the file chooser for, read from the options of its
// OpenFile, SaveFile or SaveFiles call.
struct ChooserRequest {
    enum Mode { Open, Save, SaveFiles };
    Mode mode = Open;
    QString title;
    // Without GTK's mnemonic underscores; empty for the mode's own word.
    QString acceptLabel;
    bool multiple = false;
    bool directory = false;
    // Where the window opens, and the name Save suggests.
    QString folder;
    QString name;
    // SaveFiles' names, each a bare file name.
    QStringList files;
    QList<ChooserFilter> filters;
    // The filter in use, into `filters`; -1 with none.
    int filter = -1;

    // Reads a call's options. A known option of the wrong type, or a name
    // to save under that isn't a bare file name, sets `error` instead.
    static ChooserRequest read(Mode mode, const QString &title, const QVariantMap &options, QString *error);
    static QString withoutMnemonics(const QString &label);
    // The filter in use as FolderModel's patterns; directory mode lists
    // folders only.
    QStringList patterns() const;
    // The paths the answer names, given the folder shown, what's picked
    // there and the name typed; empty when there's nothing to answer with.
    QStringList answer(const QString &folder, const QStringList &picked, const QString &typed) const;
};

// One dialog, from the call that opened it to its one answer. It's the
// window's `chooser`, and the org.freedesktop.impl.portal.Request object at
// the call's handle, which the caller can close it through.
class ChooserDialog : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.Request")
    // "open", "directory", "save" or "saveFiles".
    Q_PROPERTY(QString mode READ mode CONSTANT)
    Q_PROPERTY(QString title READ title CONSTANT)
    Q_PROPERTY(QString acceptLabel READ acceptLabel CONSTANT)
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(QStringList filters READ filters CONSTANT)
    Q_PROPERTY(int filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(QStringList patterns READ patterns NOTIFY filterChanged)

public:
    // Sends the call's reply: 0 with the results, 1 cancelled, 2 failed.
    using Reply = std::function<void(uint response, const QVariantMap &results)>;

    ChooserDialog(const ChooserRequest &request, const QString &caller, Reply reply, QObject *parent = nullptr);
    // A dialog that goes unanswered (its window closed) answers cancelled.
    ~ChooserDialog() override;

    QString mode() const;
    QString title() const { return m_request.title; }
    QString acceptLabel() const { return m_request.acceptLabel; }
    QString name() const { return m_request.name; }
    QStringList filters() const;
    int filter() const { return m_request.filter; }
    void setFilter(int filter);
    QStringList patterns() const { return m_request.patterns(); }

    Q_INVOKABLE QStringList answer(const QString &folder, const QStringList &picked, const QString &typed) const {
        return m_request.answer(folder, picked, typed);
    }
    // Those of the paths that something is already at.
    Q_INVOKABLE static QStringList existing(const QStringList &paths);
    // Those of the paths that are folders, which no file can replace.
    Q_INVOKABLE static QStringList folders(const QStringList &paths);
    Q_INVOKABLE void accept(const QStringList &paths);
    Q_INVOKABLE void cancel();
    // Only the first answer is sent; later ones do nothing.
    void respond(uint response, const QVariantMap &results);
    // Request.Close: false, and nothing closes, when it's not the caller.
    bool closeFor(const QString &sender);

public slots:
    Q_SCRIPTABLE void Close(const QDBusMessage &message);

signals:
    void filterChanged();
    // Answered: the window closes.
    void done();

private:
    ChooserRequest m_request;
    QString m_caller;
    Reply m_reply;
    bool m_answered = false;
};

// org.freedesktop.impl.portal.FileChooser, which xdg-desktop-portal calls
// for other apps' Open and Save dialogs. `folio chooser` runs it, opening
// a Folio window for each call.
class Chooser : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.impl.portal.FileChooser")

public:
    using WindowsFactory = std::function<Windows *()>;

    explicit Chooser(QObject *parent, WindowsFactory windows);

    // Exports the object, then claims the bus name; false when another
    // chooser holds it. The factory builds the UI on the first dialog call,
    // so portal activation at login doesn't pay for windows never opened.
    static bool registerOn(QObject *object, WindowsFactory windows);

public slots:
    void OpenFile(const QDBusObjectPath &handle, const QString &appId, const QString &parentWindow,
                  const QString &title, const QVariantMap &options, const QDBusMessage &message);
    void SaveFile(const QDBusObjectPath &handle, const QString &appId, const QString &parentWindow,
                  const QString &title, const QVariantMap &options, const QDBusMessage &message);
    void SaveFiles(const QDBusObjectPath &handle, const QString &appId, const QString &parentWindow,
                   const QString &title, const QVariantMap &options, const QDBusMessage &message);

private:
    void open(ChooserRequest::Mode mode, const QDBusObjectPath &handle, const QString &parentWindow,
              const QString &title, const QVariantMap &options, const QDBusMessage &message);

    WindowsFactory m_windows;
    int m_open = 0;
    // D-Bus starts the chooser again for the next dialog, so it stays only
    // a minute after its last one, and longer only for file work started in
    // one.
    std::unique_ptr<QEventLoopLocker> m_linger;
    QTimer m_idle;
};
