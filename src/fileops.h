#pragma once

#include <QDateTime>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantList>
#include <atomic>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>

class QMimeData;
class QProcess;

// What can be done to files: the clipboard (copy, cut, paste, and yanking
// paths as text), trash, rename, new folder, and opening with another app.
// Long work (copying a folder of videos) runs off the UI thread, reports how
// far it's got through `busy`, and can be cancelled. Nothing is ever
// overwritten: a name that's taken is asked about first (`clash`), and what's
// replaced goes to the trash. What's done can be undone, and redone.
class FileOps : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(bool canPaste READ canPaste NOTIFY clipboardChanged)
    Q_PROPERTY(QVariantMap clash READ clash NOTIFY clashChanged)
    Q_PROPERTY(QString undoName READ undoName NOTIFY historyChanged)

public:
    explicit FileOps(QObject *parent = nullptr);
    ~FileOps() override;

    QString busy() const { return m_busy; }
    bool canPaste() const;

    // How far a piece of work has got, and whether it's been asked to stop:
    // the worker counts and checks, the UI thread shows and asks.
    struct Job {
        std::atomic<bool> cancelled = false;
        std::atomic<int> done = 0, total = 0;
        // Bytes copied so far, for showing progress through one big file.
        std::atomic<qint64> bytes = 0;
        // A single folder counts what's in it as its items, set by the
        // worker once it has listed it.
        QString countIn;
        // Items moved across drives, made again there under another inode:
        // the drive and inode each had, and has, for undo to follow them.
        struct Renumbered {
            quint64 device, inode, nowDevice, nowInode;
        };
        QList<Renumbered> renumbered;
    };
    // One change work made, which undo makes backwards and redo forwards
    // again. A move: the item is at `path` and came from `other`. Into or
    // out of the trash (`trash`): the item belongs at `path`, and is in the
    // trash at `other`, which is empty while it's at `path`.
    struct Step {
        bool trash = false;
        QString path, other;
        // Which item it is, where the step leaves it: its drive, inode and
        // type, so undo never takes another put there since for it. Zero
        // when it isn't known.
        quint64 device = 0, inode = 0;
        uint type = 0;
        // A folder a merge emptied and removed (`folder`): undo makes it
        // again at `path` with the permissions and modified time it had,
        // and redo removes it. It's `gone` while removed, and keeps the
        // drive and inode it had. One a move stopped partway made, which
        // undo removes once what arrived in it is back, starts there.
        bool folder = false, gone = false;
        uint mode = 0;
        timespec modified{};
        // What an undo or redo stopped partway had moved of an item, put
        // back (`back`) before the item goes again, and never redone.
        bool back = false;
    };
    // What undo names: the kind of change, and the item or how many.
    enum Kind { Renaming, Moving, Copying, Duplicating, MakingFolder, Compressing, Extracting, Trashing };
    struct Change {
        Kind kind;
        QString name;
        int count = 0;
        QList<Step> steps;
    };

    // The clipboard holds files the way GNOME and KDE apps expect
    // (x-special/gnome-copied-files plus text/uri-list), and their paths as
    // plain text, so pasting into a terminal or editor gives the path.
    // Whether Shift is held right now, for menus that change with it.
    Q_INVOKABLE static bool shiftHeld();
    Q_INVOKABLE void copy(const QStringList &paths);
    Q_INVOKABLE void cut(const QStringList &paths);
    Q_INVOKABLE void yank(const QStringList &paths);
    Q_INVOKABLE void paste(const QString &folder);
    // Files dropped on a folder: moved when they're on the same drive,
    // copied from another drive. Dropping something where it already is
    // does nothing.
    Q_INVOKABLE void dropInto(const QList<QUrl> &urls, const QString &folder);
    // Whether these can be dropped on `folder`: not onto themselves, and
    // not into the trash, which only Delete fills.
    Q_INVOKABLE bool canDrop(const QList<QUrl> &urls, const QString &folder) const;
    // Files as a text/uri-list, for dragging them somewhere, and back from
    // dropped URLs to the paths of the local ones. QUrl escapes what a
    // path may hold but a URI can't (a "#" or "?" in a name).
    Q_INVOKABLE QString uriList(const QStringList &paths) const;
    Q_INVOKABLE QStringList localPaths(const QList<QUrl> &urls) const;

    // A copy beside the original, named "(copy)".
    Q_INVOKABLE void duplicate(const QString &path);
    // Packs the items into a .zip beside them (bsdtar, from libarchive).
    Q_INVOKABLE void compress(const QStringList &paths);
    // Whether these are all archives bsdtar can unpack (zip, tar and its
    // compressed forms, 7z, rar), never a disk image, which is for burning.
    Q_INVOKABLE bool canExtract(const QStringList &paths) const;
    // Unpacks each archive beside itself: an archive of one item gives
    // that item, one of several a folder named after the archive.
    Q_INVOKABLE void extract(const QStringList &paths);

    Q_INVOKABLE void trash(const QStringList &paths);

    // The trash, per the freedesktop Trash spec: each item in Trash/files
    // has a note in Trash/info saying where it came from.
    Q_INVOKABLE bool inTrash(const QString &path) const;
    // Where a trashed item came from, or empty if its note is missing.
    Q_INVOKABLE static QString originalPath(const QString &trashedPath);
    // When it was trashed, from its note.
    Q_INVOKABLE static QDateTime deletedAt(const QString &trashedPath);
    // Puts items back where they came from (recreating the folder if it's
    // gone, and never overwriting what's there now).
    Q_INVOKABLE void restore(const QStringList &paths);
    // Deletes trashed items for good, and empties the whole trash.
    Q_INVOKABLE void deleteForever(const QStringList &paths);
    Q_INVOKABLE void emptyTrash();
    Q_INVOKABLE int trashCount() const;
    // Empty string on success, or why not.
    Q_INVOKABLE QString rename(const QString &path, const QString &name);
    // Where a name's extension begins, for renaming and "(copy)" names:
    // ".pdf" in "Report v1.2.pdf", ".tar.gz" kept whole. -1 for folders and
    // dotfiles, which have none.
    Q_INVOKABLE static int extensionStart(const QString &name, bool isDir);
    // Makes "New Folder" (or "New Folder 2"...); done() names it.
    Q_INVOKABLE void newFolder(const QString &folder);

    // The apps that can open a file, from the desktop's MIME database:
    // {id, name, isDefault}, the default first.
    Q_INVOKABLE QVariantList appsFor(const QString &path) const;
    Q_INVOKABLE void openWith(const QString &path, const QString &appId);
    // Makes the app what the file's type opens with from now on.
    Q_INVOKABLE void setDefaultApp(const QString &path, const QString &appId);
    Q_INVOKABLE void openTerminal(const QString &folder);

    // Drives, through udisks (see Disks). The disk a drive mounted at
    // `root` is on, for its menu and the format dialog: {id, title, label},
    // or empty unless it's a drive plugged in that may be erased (see
    // Disks::fit).
    Q_INVOKABLE QVariantMap driveAt(const QString &root) const;
    // What a drive can be formatted as on this machine: {type, name,
    // detail, labelLimit}, exFAT first.
    Q_INVOKABLE QVariantList formatChoices() const;
    // Erases the disk to one partition of `type` (exFAT by default, as
    // Omarchy's format-drive makes) named `label`, and mounts it again.
    Q_INVOKABLE void format(const QString &id, const QString &label, const QString &type = QStringLiteral("exfat"));
    // Whether `path` is a disk image to burn onto a drive: an .iso, or a
    // raw .img such as a Raspberry Pi's.
    Q_INVOKABLE bool canBurn(const QString &path) const;
    // The drives it can be burned onto: {id, title, text, enabled}, where
    // those too small for it say so and aren't enabled.
    Q_INVOKABLE QVariantList burnTargets(const QString &image) const;
    // Writes the image over the whole disk, then ejects it, as Omarchy's
    // iso2sd does.
    Q_INVOKABLE void burn(const QString &image, const QString &id);

    // Stops the work under way at the next file; what's done stays done.
    Q_INVOKABLE void cancel();
    // Deletes this once the work under way is done, for a pane that's
    // closed: what the work changes still reaches the history.
    void deleteWhenDone();

    // A name taken where things are going, asked about one at a time before
    // anything moves: {name, folder (its name), merge (a folder onto a
    // folder), others (how many more clash after this one)}, or empty.
    QVariantMap clash() const;
    // "replace" (or merge, for folders), "skip" or "both"; `forAll` gives
    // the clashes still to come the same answer.
    Q_INVOKABLE void answerClash(const QString &choice, bool forAll);
    // Cancels the whole operation the clash was asked for.
    Q_INVOKABLE void dropClash();

    // Undo and redo the file changes made in this Folio, whichever window
    // made them. `undoName` names the next undo ("Undo Rename"), or is
    // empty when there's nothing to undo.
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    QString undoName() const;
    // Forgets every change, for tests.
    static void forgetHistory();

    // Building blocks, kept static for tests.
    // Has every rename go as on a drive that can't rename without
    // replacing (some network ones), for tests.
    static inline bool drivesRefuseNoReplace = false;
    static QMimeData *mimeDataFor(const QStringList &paths, bool cut);
    // The paths on the clipboard and whether they were cut.
    static QStringList pathsFrom(const QMimeData *data, bool *cut);
    // A name in `folder` that isn't taken, not even by a link that leads
    // nowhere: the name itself, or "name (copy).ext", "name (copy 2).ext"...
    static QString freeName(const QString &folder, const QString &name);
    // Copies `from` to `to`, a folder with all it holds. Moving, each file
    // is removed once it's copied and each folder once it's empty; what
    // can't be removed is left where it is.
    // With a job, it counts the bytes it copies and stops when asked; a file
    // stopped partway is removed. `made` says whether it made anything at
    // `to`, even when it failed after. Moving, `moved` gets the steps that
    // undo what did move when not all of it did.
    static bool copyRecursively(const QString &from, const QString &to, QString *error, bool move = false,
                                Job *job = nullptr, bool *made = nullptr, QList<Step> *moved = nullptr);
    // The archive name for compressing these into `folder`: "name.zip" for
    // one item, "Archive.zip" for several; never one that exists.
    static QString archiveName(const QStringList &paths, const QString &folder);
    // The app a file opens with by default; GIO also looks at the type's
    // parents, so a Ruby file finds the plain-text app. Empty for none.
    static QString defaultAppFor(const QString &path);
    // The name of the app a type opens with by default ("Plain"), for
    // "Open with Plain"; empty when none is set up. By the type the listing
    // already knows, so a slow drive is never read for it.
    Q_INVOKABLE static QString defaultAppName(const QString &mime);
    // Opens the file in that app; a sentence saying why not when it can't.
    static QString openInDefaultApp(const QString &path);
    // Whether the item at `path` is on the drive `folder` is on.
    static bool sameDrive(const QString &path, const QString &folder);
    // The home trash, where g_file_trash puts things: $XDG_DATA_HOME/Trash.
    static QString homeTrash();
    // The trash folders: the home one, then each mounted drive's own.
    static QStringList trashDirs();
    // For an item directly in a trash's files folder: its note. Empty for
    // anything else, such as what's inside a trashed folder, which has none.
    static QString infoFileFor(const QString &trashedPath);
    // Restores one item; returns where it went, or empty with `error` set.
    static QString restoreOne(const QString &trashedPath, QString *error, Job *job = nullptr);
    // Moves one item to the trash; returns where it is there, or empty with
    // `error` saying why, or with no error when it went but can't be found.
    static QString trashOne(const QString &path, QString *error);
    // The items in the trash that came from `original`, and of them, which
    // newest isn't among `before`: where GIO just put it.
    static QStringList trashedFrom(const QString &original);
    static QString trashedSince(const QString &original, const QStringList &before);
    static bool deleteTrashed(const QString &trashedPath);
    static int emptyTrashDir(const QString &trashDir, Job *job = nullptr);
    // How transfer() takes the paths: copied, moved, or as a drop takes
    // them, each moved from the folder's drive and copied from any other.
    enum Mode { Copy, Move, Drop };
    // What to do when a name is taken: put it beside under a "(copy)" name,
    // leave it, or replace what's there (sending that to the trash), which
    // for a folder onto a folder merges them, each clash inside replaced.
    enum Choice { KeepBoth, Skip, Replace };
    // The paths whose names are taken in `folder`, which would clash.
    static QStringList clashes(const QStringList &paths, const QString &folder);
    // Copies or moves each path into `folder`, a clash as `choices` says
    // (keeping both when it doesn't); returns the new names. With `steps`,
    // adds each change made, for undo.
    static QStringList transfer(const QStringList &paths, const QString &folder, Mode mode, QString *error,
                                const QHash<QString, Choice> &choices = {}, Job *job = nullptr,
                                QList<Step> *steps = nullptr);

signals:
    void busyChanged();
    void clipboardChanged();
    void failed(const QString &message);
    void notice(const QString &notice);
    void clashChanged();
    void historyChanged();
    // A drive mounted at `root` is about to be changed: whatever shows a
    // folder on it leaves.
    void releasing(const QString &root);
    // Drives were formatted, mounted or ejected.
    void drivesChanged();
    // Work finished in `folder`, which now holds `name` (to select it).
    void done(const QString &folder, const QString &name);

private:
    void setBusy(const QString &busy);
    void deleteIfDone();
    // What a piece of work takes part in beside other panes' (see
    // canStart()): file work, an undo or redo, or changing a drive.
    enum class Lane { Files, History, Drive };
    // Whether work in `lane` may start beside what every pane has under
    // way: an undo or redo, and a drive's change, alone.
    static bool canStart(Lane lane);
    // Counts this pane's work as under way, and stops counting it.
    void claim(Lane lane);
    void release();
    // One piece of work runs at a time: whether nothing else is running
    // (or waiting on a clash) here, nor anything work in `lane` must wait
    // for in another pane, saying why when something is, so what's
    // refused doesn't just do nothing.
    bool idle(Lane lane = Lane::Files);
    // Runs `work` off the UI thread with a job it counts in and checks,
    // showing what `label` makes of the job meanwhile, then hands what it
    // returns to `done`, back on this one. Refused (saying so) when other
    // panes' work came first.
    template <typename Work, typename Done>
    bool inBackground(const std::function<QString(const Job &)> &label, Work work, Done done, Lane lane = Lane::Files);
    // Copies or moves, first asking about each name that's taken, then off
    // the UI thread, showing how far it's got meanwhile.
    void transferInBackground(const QStringList &paths, const QString &folder, Mode mode, bool releaseClipboard);
    void startTransfer(const QStringList &paths, const QString &folder, Mode mode, bool releaseClipboard,
                       const QHash<QString, Choice> &choices = {}, bool duplicating = false);
    // Remembers a change for undo, forgetting what was undone before it.
    static void remember(const Change &change);
    // Has every change remembered follow the items `job` made again under
    // another inode.
    static void renumber(const Job &job);
    // Undoes or redoes the latest change, once it's checked that
    // everything is still where the change left it.
    void step(bool undoing);
    // Says the work was stopped, and how far it got.
    void sayStopped(const Job &job);
    // Runs a helper program (bsdtar) while showing `busy`, one piece of
    // work at a time like the rest. `missing` says what to install when it
    // isn't there, and `stopped` what a cancel left; `finished` gets its
    // exit code, or -1 if it crashed. Returns the program, or null when
    // other work is under way.
    QProcess *runTool(const QString &program, const QStringList &args, const QString &busy, const QString &missing,
                      const QString &stopped, const std::function<void(QProcess *, int)> &finished);

    QString m_busy;
    std::shared_ptr<Job> m_job;
    // The lane this pane's work under way is counted in.
    std::optional<Lane> m_lane;
    QProcess *m_process = nullptr;
    bool m_killed = false;
    // Work waiting on answers about the names it would take: each item
    // whose name is taken, with the folder it's going into, and what
    // carries on once the last answer is in.
    struct Pending {
        QList<std::pair<QString, QString>> clashing;
        QHash<QString, Choice> choices;
        std::function<void(const QHash<QString, Choice> &)> then;
    };
    std::optional<Pending> m_pending;
};
