#pragma once

#include <QCache>
#include <QObject>
#include <QProcess>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include "foldermodel.h"
#include "mounter.h"
#include "plugins.h"
#include "previewer.h"

// Where the window is and how it got there: the current folder, its
// breadcrumbs, back/forward history, and the actions that leave the folder
// (opening files and plugins' items, mounting other machines, running
// plugins' actions).
class Backend : public QObject {
    Q_OBJECT
    Q_PROPERTY(FolderModel *folder READ folder CONSTANT)
    Q_PROPERTY(Mounter *mounter READ mounter CONSTANT)
    Q_PROPERTY(QString path READ path NOTIFY pathChanged)
    // What the place is called: changes with the path, and with a plugin's
    // labels (a poll, a language switch) while its view is shown.
    Q_PROPERTY(QString title READ title NOTIFY placeChanged)
    Q_PROPERTY(QVariantList crumbs READ crumbs NOTIFY placeChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    Q_PROPERTY(QString busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool previewing READ previewing NOTIFY previewingChanged)
    // What the details column shows beyond the listing, for the item last
    // given to describe(), once they've been read in the background: a
    // picture's dimensions, a text's opening lines, an archive's names, or
    // a trashed item's origin and deletion time; and under `plugins`,
    // plugins' rows about a file or folder, [[label, value]...].
    // Changes whenever an order is chosen, for what shows other folders
    // than the one shown (the columns) to look their orders up again.
    Q_PROPERTY(int sortsRevision READ sortsRevision NOTIFY sortsChanged)
    Q_PROPERTY(QVariantMap details READ details NOTIFY detailsChanged)
    Q_PROPERTY(bool describing READ describing NOTIFY describingChanged)

public:
    explicit Backend(const QString &home = {}, QObject *parent = nullptr);
    ~Backend() override;

    // The plugins whose views, items and actions this window offers.
    void setPlugins(Plugins *plugins);

    FolderModel *folder() { return &m_folder; }
    // The order each folder was last put in, kept in Folio's settings: a
    // folder without one of its own takes its nearest parent's, so sorting
    // ~/Downloads by date sorts the folders inside it the same way.
    // Tests keep theirs in a file of their own.
    static QString sortFor(const QString &path);
    static void rememberSort(const QString &path, const QString &sort);
    static void setSettingsFile(const QString &file) { s_settingsFile = file; }
    // The order chosen for the folder shown, from the sort menu or S:
    // remembered even when it's the one already in use, which can free
    // the folder to follow its parent again.
    Q_INVOKABLE void chooseSort(const QString &sort);
    // Each column of the columns view in its own folder's order.
    Q_INVOKABLE QString sortOf(const QString &path) const { return sortFor(path); }
    Mounter *mounter() { return &m_mounter; }
    QString path() const { return m_folder.path(); }
    QString title() const;
    QVariantList crumbs() const;
    int sortsRevision() const { return m_sortsRevision; }
    bool canBack() const { return m_index > 0; }
    bool canForward() const { return m_index + 1 < m_history.size(); }
    QString message() const { return m_message; }
    QString notice() const { return m_notice; }
    // Mounting, or else the latest plugin action still running.
    QString busy() const { return m_busy.isEmpty() && !m_runs.isEmpty() ? m_runs.last().second : m_busy; }
    bool previewing() const { return m_preview.state() != QProcess::NotRunning; }

    Q_INVOKABLE void navigate(const QString &path);
    Q_INVOKABLE void open(const QString &path);
    Q_INVOKABLE void goUp();
    Q_INVOKABLE void back();
    Q_INVOKABLE void forward();
    // Runs a plugin's action on the paths, sent to `target` (an item's id)
    // for a submenu, showing its busy text meanwhile and what it says when
    // it's done.
    Q_INVOKABLE void perform(const QString &plugin, const QString &action, const QStringList &paths,
                             const QString &target = {});
    Q_INVOKABLE void dismissMessage();
    Q_INVOKABLE void showMessage(const QString &message) { setMessage(message); }
    // Good news that needs no answer ("Link copied"): shown for a few
    // seconds in the accent, unlike problems, which wait to be dismissed.
    Q_INVOKABLE void showNotice(const QString &notice);
    // Selects `name` the next time the folder loads (after file work).
    Q_INVOKABLE void expectSelection(const QString &name) {
        m_pendingSelection = name;
        m_leftBehind = false;
    }
    QVariantMap details() const;
    bool describing() const { return m_describingNow; }
    Q_INVOKABLE void describe(const QVariantMap &item, bool inTrash = false);
    // The system's own name for a type ("JPEG image"), in the language of
    // the moment.
    Q_INVOKABLE static QString typeName(const QString &mime);
    // Whether the details list an archive of this type's names.
    Q_INVOKABLE static bool listsArchive(const QString &mime) { return Previewer::listsArchive(mime); }
    // The item to select once the folder has loaded (where we came from, or
    // what "Show in folder" asked for); empty means the first item.
    Q_INVOKABLE QString takeSelection();
    // Whether that item is the folder just left, going up or back, rather
    // than one picked to be shown.
    Q_INVOKABLE bool leftBehind() const { return m_leftBehind; }
    // The path as it's shown for editing (~ for home), going to a typed
    // path (a folder, or a file to select in its folder; ~ works; or a
    // server's address, connecting to it), and completing the last part of
    // one to a folder name.
    Q_INVOKABLE QString typedPath() const;
    Q_INVOKABLE bool goTyped(const QString &text);
    Q_INVOKABLE QString completePath(const QString &text) const;
    // Space: opens the large preview of a file in its own process, or closes
    // it when it's already open.
    Q_INVOKABLE void togglePreview(const QString &path);
    // A line from the preview: the file it moved to, or "step -1"/"step 1"
    // asking for the row above or below.
    void previewSaid(const QString &line);
    // Ctrl+N: another window, on the folder this one shows.
    Q_INVOKABLE void newWindow();
    // In a file chooser, opening a file picks it (chosen) rather than opening
    // it in its app, and there are no other windows to open.
    void setChoosing(bool choosing) { m_choosing = choosing; }
    bool choosing() const { return m_choosing; }
    // Keeps an open preview on the item selected in the grid.
    Q_INVOKABLE void followSelection(const QString &path);
    // Omarchy's Super+Return opens a terminal in the focused window's working
    // directory, so the process works in the folder this pane shows, from
    // now on as it moves, until another pane asks for it. Views and servers
    // aren't local folders and leave it where it was.
    Q_INVOKABLE void keepWorkingDirectory();

    // Called for org.freedesktop.FileManager1: open a folder, or open the
    // folder holding a file and select it.
    void showFolder(const QString &path);
    Q_INVOKABLE void showItem(const QString &path);

    static QString expandTyped(const QString &text, const QString &home, const QString &current);
    static QString complete(const QString &text, const QString &home, const QString &current);
    static QString mountLabel(const QString &mountFolder);
    // A server address (sftp://, smb://, nfs://, ftp://, ftps://, dav://,
    // davs://, or \\server\share) as GIO mounts it, or empty when the text
    // isn't one. `problem` says why when it's an address that can't be
    // mounted as written.
    static QString serverAddress(const QString &text, QString *problem = nullptr);

signals:
    void sortsChanged();
    void pathChanged();
    void placeChanged();
    void messageChanged();
    void noticeChanged();
    void busyChanged();
    void previewingChanged();
    void detailsChanged();
    void describingChanged();
    void newWindowRequested(const QString &path);
    void chosen(const QString &path);
    void selectRequested(const QString &name);
    // The preview moved to `path`, in the folder shown or a folder opened in
    // place in it: the cursor goes to its row.
    void selectPathRequested(const QString &path);
    // The preview's ↑ or ↓ on a file it can't scroll: move one row in the
    // folder view, whose layout only this window knows.
    void stepRequested(int direction);
    void activateRequested();

private:
    static QString gvfsRoot();
    QString trashPath() const;
    // False when `path` isn't a folder (any more), which is said in the
    // window and leaves everything as it was.
    bool go(const QString &path, bool record);
    void setMessage(const QString &message);
    void setBusy(const QString &busy);
    // Opening a plugin's item: a folder, a mount or a command.
    void openItem(const QString &path);
    // Mounts `url` through GVfs (a plugin's machine, or a server typed in
    // the path bar) and goes where it points once it's mounted.
    void mount(const QString &url, const QString &name, bool verified);
    // Goes to `folder` on a mount: a bare server's to the folder at home's
    // path on it when there is one, a file's to its folder, selected.
    void arrive(const QString &folder);
    // A plugin action finished: its busy text goes, and what it asks for
    // happens.
    void finish(int run, const Plugins::Result &result);
    // Plugins' rows about the item described: those known, then the rest
    // as their commands answer. While `keep`, rows already shown stay until
    // there are others, so the same item asked about again doesn't flicker.
    void showPluginRows(bool keep);
    // Asks plugins about the item described; false when nothing's coming.
    bool askPlugins();
    // A GVfs mount's name on screen: its plugin item's name, or its host.
    QString mountName(const QString &mount) const;

    QString m_home;
    FolderModel m_folder;
    QStringList m_history;
    int m_index = -1;
    QString m_message;
    QString m_notice;
    QTimer m_noticeTimer;
    QString m_busy;
    QString m_pendingSelection;
    bool m_leftBehind = false;
    static QString s_settingsFile;
    int m_sortsRevision = 0;
    bool m_choosing = false;
    Plugins *m_plugins = nullptr;
    Mounter m_mounter;
    // What's being connected to, as the window names it, and whether it's
    // a bare server, which lands in the matching home folder.
    QString m_mountName;
    bool m_mountBare = false;
    // The plugin actions running, each with its busy text, oldest first.
    QList<std::pair<int, QString>> m_runs;
    int m_lastRun = 0;
    QProcess m_preview;
    // Set while Folio closes the preview itself, which isn't a crash.
    bool m_closingPreview = false;
    QString m_previewed;
    QVariantMap m_details;
    QCache<QString, QVariantMap> m_detailsCache{64};
    bool m_describingNow = false;
    void setDescribing(bool describing);
    // Whose details they are (the path and its type, as a file can be
    // replaced by a folder of the same name), and the newest describe(),
    // which a reading has to match to be shown, or even to start.
    QString m_described;
    // The real file or folder described, which plugins are asked about.
    QVariantMap m_pluginItem;
    QVariantList m_pluginRows;
    std::shared_ptr<std::atomic<quint64>> m_describing = std::make_shared<std::atomic<quint64>>(0);
    // The pane the working directory follows: the one used last.
    static inline Backend *s_working = nullptr;
    void moveWorkingDirectory();
};
