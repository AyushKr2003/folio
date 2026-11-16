#include "fileops.h"
#include "disks.h"
#include "foldermodel.h"

#include <QClipboard>
#include <QDir>
#include <QEventLoopLocker>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QLocale>
#include <QMimeData>
#include <QMimeDatabase>
#include <QProcess>
#include <QSet>
#include <QStorageInfo>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QTimer>
#include <QtConcurrent>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

// GIO's headers use `signals` as a struct member, which Qt defines as a
// keyword.
#undef signals
#include <gio/gdesktopappinfo.h>
#include <gio/gio.h>
#define signals Q_SIGNALS

// What's been done and undone in this Folio, newest last, shared by every
// window's FileOps, each told when they change. Kept in memory only, and
// only the latest few.
static QList<FileOps::Change> s_undo, s_redo;
static QList<FileOps *> s_everyone;
static constexpr int historyLength = 50;

// Work under way in this Folio, whichever pane started it. Panes work side
// by side, but an undo or redo runs alone, so nothing changes the items it
// hasn't reached yet, nor the history it finds, and so does a drive's
// change, so nothing is on the drive.
static int s_working = 0;
static bool s_inHistory = false, s_onDrive = false;

static void tellEveryone() {
    for (FileOps *ops : s_everyone)
        emit ops->historyChanged();
}

FileOps::FileOps(QObject *parent) : QObject(parent) {
    if (auto *clipboard = QGuiApplication::clipboard())
        connect(clipboard, &QClipboard::dataChanged, this, &FileOps::clipboardChanged);
    s_everyone << this;
}

FileOps::~FileOps() {
    s_everyone.removeOne(this);
    release();
}

void FileOps::deleteWhenDone() {
    // Checked once each piece of work has finished, and what it went on
    // to (an extraction placing what it unpacked) has started.
    connect(this, &FileOps::busyChanged, this, &FileOps::deleteIfDone, Qt::QueuedConnection);
    deleteIfDone();
}

void FileOps::deleteIfDone() {
    if (!m_busy.isEmpty())
        return;
    // There's no one left to answer a clash.
    dropClash();
    deleteLater();
}

bool FileOps::canStart(Lane lane) {
    if (s_onDrive)
        return false;
    return lane == Lane::Files ? !s_inHistory : s_working == 0;
}

void FileOps::claim(Lane lane) {
    ++s_working;
    s_inHistory |= lane == Lane::History;
    s_onDrive |= lane == Lane::Drive;
    m_lane = lane;
}

void FileOps::release() {
    if (!m_lane)
        return;
    --s_working;
    if (*m_lane == Lane::History)
        s_inHistory = false;
    if (*m_lane == Lane::Drive)
        s_onDrive = false;
    m_lane.reset();
}

QMimeData *FileOps::mimeDataFor(const QStringList &paths, bool cut) {
    auto *data = new QMimeData;
    QList<QUrl> urls;
    QByteArray gnome = cut ? "cut" : "copy";
    for (const QString &path : paths) {
        const QUrl url = QUrl::fromLocalFile(path);
        urls << url;
        gnome += '\n' + url.toEncoded();
    }
    data->setUrls(urls);
    data->setData("x-special/gnome-copied-files", gnome);
    data->setText(paths.join('\n'));
    return data;
}

QStringList FileOps::pathsFrom(const QMimeData *data, bool *cut) {
    if (cut)
        *cut = false;
    QStringList paths;
    if (!data)
        return paths;
    if (data->hasFormat("x-special/gnome-copied-files")) {
        const QList<QByteArray> lines = data->data("x-special/gnome-copied-files").split('\n');
        if (cut && !lines.isEmpty())
            *cut = lines.first().trimmed() == "cut";
        for (int i = 1; i < lines.size(); ++i) {
            const QUrl url = QUrl::fromEncoded(lines[i].trimmed());
            if (url.isLocalFile())
                paths << url.toLocalFile();
        }
        return paths;
    }
    for (const QUrl &url : data->urls())
        if (url.isLocalFile())
            paths << url.toLocalFile();
    return paths;
}

bool FileOps::canPaste() const {
    const auto *clipboard = QGuiApplication::clipboard();
    return clipboard && !pathsFrom(clipboard->mimeData(), nullptr).isEmpty();
}

bool FileOps::shiftHeld() {
    return QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier;
}

void FileOps::copy(const QStringList &paths) {
    if (!paths.isEmpty())
        QGuiApplication::clipboard()->setMimeData(mimeDataFor(paths, false));
}

void FileOps::cut(const QStringList &paths) {
    if (!paths.isEmpty())
        QGuiApplication::clipboard()->setMimeData(mimeDataFor(paths, true));
}

void FileOps::yank(const QStringList &paths) {
    if (!paths.isEmpty())
        QGuiApplication::clipboard()->setText(paths.join('\n'));
}

int FileOps::extensionStart(const QString &name, bool isDir) {
    int dot = isDir ? -1 : name.lastIndexOf('.');
    if (dot <= 0)
        return -1;
    if (dot > 4 && name.mid(dot - 4, 4).compare(".tar", Qt::CaseInsensitive) == 0)
        dot -= 4;
    return dot;
}

// Whether something is at `path`, a link that leads nowhere included.
static bool taken(const QString &path) {
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

QString FileOps::freeName(const QString &folder, const QString &name) {
    const QDir dir(folder);
    if (!taken(dir.filePath(name)))
        return name;
    const QFileInfo info(dir.filePath(name));
    const int dot = extensionStart(name, info.isDir());
    const QString base = dot > 0 ? name.left(dot) : name;
    const QString ext = dot > 0 ? name.mid(dot) : QString();
    for (int n = 1;; ++n) {
        // One arg() for all three: a name holding "%2" would otherwise be
        // filled in again.
        const QString candidate = n == 1 ? QObject::tr("%1 (copy)%2").arg(base, ext)
                                         : QObject::tr("%1 (copy %2)%3").arg(base, QString::number(n), ext);
        if (!taken(dir.filePath(candidate)))
            return candidate;
    }
}

static bool isFolder(const QString &path) {
    const QFileInfo info(path);
    return info.isDir() && !info.isSymLink();
}

static bool writeAll(int fd, const char *data, qint64 size) {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        size -= n;
    }
    return true;
}

// Copies a file a piece at a time, so a big one can be stopped partway (and
// is then removed), counting what's copied. copy_file_range lets the kernel
// do the copying, which on btrfs shares the blocks rather than copying them;
// what it can't take (a pipe, some drives) is read and written instead. Like
// QFile::copy, it never overwrites, and the copy gets the file's permissions:
// it's made with them, so a private file is never open to others while it's
// copied.
static bool copyFile(const QString &from, const QString &to, FileOps::Job *job) {
    QFile in(from), out(to);
    if (!in.open(QIODevice::ReadOnly) || !out.open(QIODevice::WriteOnly | QIODevice::NewOnly, in.permissions()))
        return false;
    bool kernel = true, ok = true;
    QByteArray buffer;
    for (;;) {
        if (job && job->cancelled) {
            ok = false;
            break;
        }
        ssize_t n;
        if (kernel) {
            n = copy_file_range(in.handle(), nullptr, out.handle(), nullptr, 8 << 20, 0);
            // Some drives (FUSE ones, files that say they're empty) answer
            // 0 when they can't, which would end a copy early and, moving,
            // remove an original that was never copied: reading finds the
            // real end.
            if (n == 0 || (n < 0 && (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP))) {
                kernel = false;
                continue;
            }
        } else {
            buffer.resize(1 << 20);
            n = ::read(in.handle(), buffer.data(), buffer.size());
            if (n > 0 && !writeAll(out.handle(), buffer.constData(), n))
                n = -1;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ok = n == 0;
            break;
        }
        if (job)
            job->bytes += n;
    }
    // Some drives (FAT) keep no permissions; the copy is still a copy.
    if (ok)
        out.setPermissions(in.permissions());
    out.close();
    if (!ok)
        out.remove();
    return ok;
}

// Where a step's item is now, and where flipping it takes it (empty for
// into the trash, where it goes under a name found then).
static std::pair<QString, QString> ends(const FileOps::Step &step) {
    // A folder removed is nowhere, and comes back where it was.
    if (step.folder)
        return step.gone ? std::pair{QString(), step.path} : std::pair{step.path, QString()};
    if (!step.trash)
        return {step.path, step.other};
    if (step.other.isEmpty())
        return {step.path, {}};
    return {step.other, step.path};
}

// The step, knowing which item is where it leaves it, so undo can tell it
// from another put there since. Nothing's known when nothing's there.
static FileOps::Step identified(FileOps::Step step) {
    struct stat info;
    const bool there = ::lstat(QFile::encodeName(ends(step).first).constData(), &info) == 0;
    step.device = there ? info.st_dev : 0;
    step.inode = there ? info.st_ino : 0;
    step.type = there ? info.st_mode & S_IFMT : 0;
    return step;
}

bool FileOps::copyRecursively(const QString &from, const QString &to, QString *error, bool move, Job *job,
                              bool *made, QList<Step> *moved) {
    const QFileInfo info(from);
    if (made)
        *made = false;
    // Moving, the original once it's gone is the item at `to`, made again
    // under another inode, which undo follows.
    struct stat was;
    const bool known = move && ::lstat(QFile::encodeName(from).constData(), &was) == 0;
    const auto went = [&] {
        const Step step = identified({false, to, from});
        if (moved)
            *moved << step;
        if (job && known && step.inode != 0)
            job->renumbered << Job::Renumbered{was.st_dev, was.st_ino, step.device, step.inode};
    };
    // An original that couldn't go leaves its copy as a copy, which undo
    // sends to the trash, so the folder made for it can go too.
    const auto left = [&] {
        if (moved)
            *moved << identified({true, to, {}});
    };
    if (info.isSymLink()) {
        // Links are copied as links, not followed, and as written: one
        // relative to its folder points into the copy, not at the original.
        if (!QFile::link(info.readSymLink(), to)) {
            *error = QObject::tr("Couldn't copy the link %1.").arg(info.fileName());
            return false;
        }
        if (made)
            *made = true;
        if (move)
            QFile::remove(from) ? went() : left();
        return true;
    }
    if (!info.isDir()) {
        if (!copyFile(from, to, job)) {
            // Stopped when asked: that's not a problem to report.
            if (!job || !job->cancelled)
                *error = QObject::tr("Couldn't copy %1.").arg(info.fileName());
            return false;
        }
        if (made)
            *made = true;
        if (move)
            QFile::remove(from) ? went() : left();
        return true;
    }
    // A folder that can't be read would list as empty and be copied empty.
    if (!info.isReadable()) {
        *error = QObject::tr("Couldn't read %1.").arg(info.fileName());
        return false;
    }
    // A folder keeps its permissions, as QFile::copy keeps a file's. It's
    // made with them, plus its owner's, so a private one is never open to
    // others even when the copy stops partway, and a read-only one can be
    // filled before it gets them exactly.
    if (!QDir().mkdir(to, info.permissions() | QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        *error = QObject::tr("Couldn't create %1.").arg(QFileInfo(to).fileName());
        return false;
    }
    if (made)
        *made = true;
    // Moving, what's moved in it is undone item by item when it doesn't all
    // move, and the folder made for them goes once they're back.
    QList<Step> inside;
    const auto partly = [&] {
        if (!moved)
            return;
        Step folder{false, to, {}};
        folder.folder = true;
        struct stat now;
        if (::lstat(QFile::encodeName(to).constData(), &now) == 0) {
            folder.mode = now.st_mode;
            folder.modified = now.st_mtim;
        }
        *moved << identified(folder) << inside;
    };
    const QDir source(from);
    const auto entries = source.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    // The job that counts this folder's items, if this is the folder it
    // counts in.
    Job *counter = job && !job->countIn.isEmpty() && job->countIn == QDir::cleanPath(from) ? job : nullptr;
    if (counter)
        counter->total = entries.size();
    for (const QFileInfo &entry : entries) {
        if ((job && job->cancelled)
            || !copyRecursively(entry.filePath(), QDir(to).filePath(entry.fileName()), error, move, job, nullptr,
                                moved ? &inside : nullptr)) {
            partly();
            return false;
        }
        if (counter)
            counter->done.fetch_add(1);
    }
    QFile::setPermissions(to, info.permissions());
    // Only once it's empty: what turned up in it while the rest moved was
    // never copied, so it stays where it is, and the folder with it.
    if (move) {
        if (QDir().rmdir(from))
            went();
        else
            partly();
    }
    return true;
}

// The paths that aren't inside another of them. A folder takes what's in it
// along, so anything also chosen inside it (search results list both) would
// be copied or archived twice, be gone by its turn once the folder moved, or
// be deleted twice. Each path looks for the folders above it among the
// chosen, rather than at every other path, which for a Select All of
// thousands took minutes. A link on the way up is copied as a link, not
// gone into, so what was chosen beyond it stays chosen, and goes first:
// once the link had moved or gone to the trash, it would be out of reach.
static QStringList outermost(const QStringList &paths) {
    QSet<QString> chosen;
    for (const QString &path : paths)
        chosen.insert(QDir::cleanPath(path));
    QStringList beyondLinks, outer;
    // Each once, however often it was chosen (another app's clipboard can
    // name one twice).
    QSet<QString> seen;
    for (const QString &path : paths) {
        const QString clean = QDir::cleanPath(path);
        if (seen.contains(clean))
            continue;
        seen.insert(clean);
        bool inside = false, beyondLink = false;
        for (qsizetype slash = clean.lastIndexOf('/'); slash > 0 && !inside && !beyondLink;
             slash = clean.lastIndexOf('/', slash - 1)) {
            if (!chosen.contains(clean.left(slash)))
                continue;
            // Only now, for the few that have a chosen folder above them,
            // is each step between looked at on disk.
            for (qsizetype step = clean.lastIndexOf('/'); step >= slash && !beyondLink; step = clean.lastIndexOf('/', step - 1))
                beyondLink = QFileInfo(clean.left(step)).isSymLink();
            inside = !beyondLink;
        }
        if (beyondLink)
            beyondLinks << path;
        else if (!inside)
            outer << path;
    }
    return beyondLinks + outer;
}

// Does `each` to the outermost of `paths`, counting them in `job`, until
// it's cancelled.
template <typename Each>
static void eachOutermost(const QStringList &paths, FileOps::Job &job, Each each) {
    const QStringList outer = outermost(paths);
    job.total = outer.size();
    for (const QString &path : outer) {
        if (job.cancelled)
            break;
        each(path);
        ++job.done;
    }
}

// Whether `folder` is the folder at `path` or inside it, wherever links lead,
// so a link to a folder inside it counts. A link at `path` itself isn't a
// folder here, since it's copied as a link.
static bool inside(const QString &folder, const QString &path) {
    const QFileInfo item(path);
    const QString self = item.canonicalFilePath(), target = QFileInfo(folder).canonicalFilePath();
    return item.isDir() && !item.isSymLink() && (target == self || target.startsWith(self + '/'));
}

// A rename that never replaces what's there, failing with EEXIST when the
// name is taken. On a drive that can't promise that (some network ones), a
// file or link is linked under the new name, which can't replace either,
// then let go of under the old. A folder can't be, nor anything where links
// can't be made: that fails with EXDEV, to be copied as if across drives.
static bool renameNoReplace(const QString &from, const QString &to) {
    const QByteArray source = QFile::encodeName(from), target = QFile::encodeName(to);
    if (!FileOps::drivesRefuseNoReplace
        && renameat2(AT_FDCWD, source.constData(), AT_FDCWD, target.constData(), RENAME_NOREPLACE) == 0)
        return true;
    if (!FileOps::drivesRefuseNoReplace && errno != EINVAL)
        return false;
    struct stat item;
    if (::lstat(source.constData(), &item) != 0)
        return false;
    if (S_ISDIR(item.st_mode) || ::linkat(AT_FDCWD, source.constData(), AT_FDCWD, target.constData(), 0) != 0) {
        if (S_ISDIR(item.st_mode) || errno != EEXIST)
            errno = EXDEV;
        return false;
    }
    if (::unlink(source.constData()) == 0)
        return true;
    const int why = errno;
    ::unlink(target.constData());
    errno = why;
    return false;
}

// Puts one item at `to`. A rename is instant on the same drive; across
// drives a move is a copy that removes each original once it's copied. A
// link is made again rather than renamed, which across drives would copy
// what it leads to. `made` says whether anything arrived at `to`, even when
// it didn't all, and `moved` the steps that undo what did.
static bool put(const QString &from, const QString &to, bool move, QString *error, FileOps::Job *job,
                bool *made = nullptr, QList<FileOps::Step> *moved = nullptr) {
    const QFileInfo info(from);
    if (move && !info.isSymLink() && renameNoReplace(from, to)) {
        if (made)
            *made = true;
        return true;
    }
    if (!FileOps::copyRecursively(from, to, error, move, job, made, moved)) {
        // Each original went as it was copied, so what arrived is now the
        // only copy of it: it's said to be only part.
        if (move && taken(to) && !error->isEmpty())
            *error = QObject::tr("Moved only part of %1: %2").arg(info.fileName(), *error);
        return false;
    }
    if (move && taken(from)) {
        *error = QObject::tr("Copied %1, but couldn't remove the original.").arg(info.fileName());
        return false;
    }
    return true;
}

// Whether the item at `path` is the one `step` left there, as far as it's
// known.
static bool sameAsLeft(const FileOps::Step &step, const QString &path) {
    struct stat info;
    return step.inode == 0
           || (::lstat(QFile::encodeName(path).constData(), &info) == 0 && info.st_dev == step.device
               && info.st_ino == step.inode && (info.st_mode & S_IFMT) == step.type);
}

// Puts `from` at `to`, first sending what's there to the trash when
// replacing it. When nothing arrives in its place (a file that can't be
// read, a full drive, a cancel), what was there is put back, so a replace
// that fails never leaves the place empty.
static bool putReplacing(const QString &from, const QString &to, bool move, bool replacing, QString *error,
                         FileOps::Job *job, QList<FileOps::Step> *steps, bool *made = nullptr) {
    QString trashed;
    if (replacing) {
        QString why;
        trashed = FileOps::trashOne(to, &why);
        if (trashed.isEmpty() && !why.isEmpty()) {
            *error = QObject::tr("Couldn't move %1 to the trash to replace it: %2").arg(QFileInfo(to).fileName(), why);
            return false;
        }
        // In the trash, but not found there, it can't be brought back by undo.
        if (!trashed.isEmpty() && steps)
            *steps << identified({true, to, trashed});
    }
    bool arrived = false;
    QList<FileOps::Step> moved;
    const bool finished = put(from, to, move, error, job, &arrived, &moved);
    // Noted for undo: a move that finished can be moved back, as can what
    // moved of one that didn't, and a copy that made something (even one
    // that stopped partway, or a move whose original couldn't go) can go to
    // the trash. What was at `to` already, and stayed when the copy failed,
    // was never the copy's.
    if (steps && move && finished)
        *steps << identified({false, to, from});
    else if (steps && move && !moved.isEmpty())
        *steps << moved;
    else if (steps && arrived)
        *steps << identified({true, to, {}});
    if (made)
        *made = arrived;
    if (!finished && !trashed.isEmpty() && !taken(to)) {
        QString why;
        if (!FileOps::restoreOne(trashed, &why).isEmpty() && steps)
            steps->removeLast();
    }
    return finished;
}

// Puts what's in the folder `from` into the folder `to`, which is already
// there: what isn't there yet goes in, a folder onto a folder merges in
// turn, and anything else in the way goes to the trash first. Moving, the
// folder goes once it's empty, noted with its permissions and modified time
// from before anything left it, for undo to make it again as it was.
static bool merge(const QString &from, const QString &to, bool move, QString *error, FileOps::Job *job,
                  QList<FileOps::Step> *steps) {
    if (!QFileInfo(from).isReadable()) {
        *error = QObject::tr("Couldn't read %1.").arg(QFileInfo(from).fileName());
        return false;
    }
    struct stat before;
    const bool known = ::lstat(QFile::encodeName(from).constData(), &before) == 0;
    const auto entries = QDir(from).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    for (const QFileInfo &entry : entries) {
        if (job && job->cancelled)
            return false;
        const QString there = QDir(to).filePath(entry.fileName());
        const bool occupied = taken(there);
        if (occupied && isFolder(entry.filePath()) && isFolder(there)) {
            if (!merge(entry.filePath(), there, move, error, job, steps))
                return false;
            continue;
        }
        if (!putReplacing(entry.filePath(), there, move, occupied, error, job, steps))
            return false;
    }
    if (move && QDir().rmdir(from) && steps && known) {
        FileOps::Step removed{false, from, {}};
        removed.folder = removed.gone = true;
        removed.mode = before.st_mode;
        removed.modified = before.st_mtim;
        removed.device = before.st_dev;
        removed.inode = before.st_ino;
        removed.type = before.st_mode & S_IFMT;
        *steps << removed;
    }
    return true;
}

// Whether two paths are one item on disk, however each is reached: a file
// in a folder reached through a link to that folder is the file itself.
static bool sameItem(const QString &a, const QString &b) {
    if (QDir::cleanPath(a) == QDir::cleanPath(b))
        return true;
    struct stat one, other;
    return ::lstat(QFile::encodeName(a).constData(), &one) == 0 && ::lstat(QFile::encodeName(b).constData(), &other) == 0
           && one.st_ino != 0 && one.st_dev == other.st_dev && one.st_ino == other.st_ino;
}

// Whether putting `path` at `there` clashes with something else: not when
// it's the item itself (copied onto itself, it's kept beside), nor what
// holds it, which replacing would take along. Either may be reached through
// a link, and replacing it would then send the very item being put to the
// trash.
static bool clashing(const QString &path, const QString &there) {
    return taken(there) && !sameItem(path, there) && !inside(path, there);
}

// Whether `path` is already in `folder`, wherever links lead, so a drop
// into a link to the folder it's in is no move either.
static bool alreadyIn(const QString &path, const QString &folder) {
    return QFileInfo(QFileInfo(path).absolutePath()).canonicalFilePath() == QFileInfo(folder).canonicalFilePath();
}

QStringList FileOps::clashes(const QStringList &paths, const QString &folder) {
    QStringList clash;
    const QString target = QDir::cleanPath(folder);
    for (const QString &path : outermost(paths)) {
        const QFileInfo info(path);
        if (clashing(path, QDir(target).filePath(info.fileName())))
            clash << path;
    }
    return clash;
}

QStringList FileOps::transfer(const QStringList &paths, const QString &folder, Mode mode, QString *error,
                              const QHash<QString, Choice> &choices, Job *job, QList<Step> *steps) {
    QStringList made;
    const QString target = QDir::cleanPath(folder);
    const QStringList outer = outermost(paths);
    if (job)
        job->total = outer.size();
    for (const QString &path : outer) {
        if (job && job->cancelled)
            break;
        const QFileInfo info(path);
        if (!info.exists() && !info.isSymLink()) {
            *error = QObject::tr("%1 is no longer there.").arg(info.fileName());
            break;
        }
        const bool move = mode == Move || (mode == Drop && sameDrive(path, target));
        QString name = info.fileName();
        const QString there = QDir(target).filePath(name);
        // Moving something into the folder it's already in does nothing.
        if (move && sameItem(path, there)) {
            if (job)
                ++job->done;
            continue;
        }
        if (inside(target, path)) {
            *error = QObject::tr("Can't put %1 inside itself.").arg(info.fileName());
            break;
        }
        // A name taken by the item itself, or by what holds it, is kept
        // beside without asking.
        bool replacing = false;
        if (taken(there)) {
            const Choice choice = clashing(path, there) ? choices.value(path, KeepBoth) : KeepBoth;
            if (choice == Skip) {
                if (job)
                    ++job->done;
                continue;
            }
            if (choice == Replace && isFolder(path) && isFolder(there)) {
                const bool merged = merge(path, there, move, error, job, steps);
                made << name;
                if (!merged)
                    break;
                if (job)
                    ++job->done;
                continue;
            }
            if (choice == KeepBoth)
                name = freeName(target, name);
            else
                replacing = true;
        }
        const QString destination = QDir(target).filePath(name);
        // A single folder counts what's in it, so there's more to count.
        if (job && outer.size() == 1 && isFolder(path))
            job->countIn = QDir::cleanPath(path);
        bool arrived = false;
        const bool finished = putReplacing(path, destination, move, replacing, error, job, steps, &arrived);
        if (!finished) {
            // What arrived of a move is now the only copy of it, and what was
            // copied before a cancel stays: either is shown.
            if (arrived && (move || error->isEmpty()))
                made << name;
            break;
        }
        made << name;
        if (job && job->countIn.isEmpty())
            ++job->done;
    }
    return made;
}

void FileOps::paste(const QString &folder) {
    bool cut = false;
    const QStringList paths = pathsFrom(QGuiApplication::clipboard()->mimeData(), &cut);
    if (paths.isEmpty() || !idle())
        return;
    // Only Delete puts things in the trash, with the note Restore needs.
    if (!QFileInfo(folder).isDir() || inTrash(folder)) {
        emit failed(tr("Files can't be pasted here."));
        return;
    }
    // Cut files are gone from where they were once moved; pasting them
    // again would find nothing, so the clipboard lets go of them.
    transferInBackground(paths, folder, cut ? Move : Copy, cut);
}

bool FileOps::sameDrive(const QString &path, const QString &folder) {
    // QStorageInfo follows a link, but a link is on the drive of the folder
    // it's in, wherever it leads.
    const QFileInfo item(path);
    return QStorageInfo(item.isSymLink() ? item.absolutePath() : path).device() == QStorageInfo(folder).device();
}

bool FileOps::canDrop(const QList<QUrl> &urls, const QString &folder) const {
    if (!QFileInfo(folder).isDir() || inTrash(folder))
        return false;
    const QString target = QDir::cleanPath(folder);
    bool any = false;
    for (const QUrl &url : urls) {
        if (!url.isLocalFile())
            continue;
        const QString path = QDir::cleanPath(url.toLocalFile());
        // Not onto itself or inside itself.
        if (path == target || inside(target, path))
            return false;
        // Already there: nothing would happen.
        if (!alreadyIn(path, target))
            any = true;
    }
    return any;
}

void FileOps::dropInto(const QList<QUrl> &urls, const QString &folder) {
    if (!canDrop(urls, folder) || !idle())
        return;
    transferInBackground(localPaths(urls), folder, Drop, false);
}

QString FileOps::uriList(const QStringList &paths) const {
    QStringList uris;
    for (const QString &path : paths)
        uris << QString::fromLatin1(QUrl::fromLocalFile(path).toEncoded());
    return uris.join("\r\n");
}

QStringList FileOps::localPaths(const QList<QUrl> &urls) const {
    QStringList paths;
    for (const QUrl &url : urls)
        if (url.isLocalFile())
            paths << url.toLocalFile();
    return paths;
}

template <typename Work, typename Done>
bool FileOps::inBackground(const std::function<QString(const Job &)> &label, Work work, Done done, Lane lane) {
    // Asked about first, work can find another pane's under way by now.
    if (!canStart(lane)) {
        emit failed(tr("Try again once the work under way is done."));
        return false;
    }
    claim(lane);
    auto job = std::make_shared<Job>();
    m_job = job;
    setBusy(label(*job));
    auto *watcher = new QFutureWatcher<std::invoke_result_t<Work, Job &>>(this);
    // How far it's got, a few times a second.
    auto *timer = new QTimer(watcher);
    connect(timer, &QTimer::timeout, this, [this, job, label] { setBusy(label(*job)); });
    timer->start(200);
    // Closing the last window doesn't quit Folio under the work: it runs on
    // until `done` has finished it, and whatever `done` starts holds on too.
    auto running = std::make_shared<QEventLoopLocker>();
    connect(watcher, &QFutureWatcherBase::finished, this, [this, watcher, timer, job, running, done = std::move(done)] {
        timer->stop();
        watcher->deleteLater();
        m_job.reset();
        release();
        setBusy({});
        renumber(*job);
        done(watcher->result(), *job);
    });
    watcher->setFuture(QtConcurrent::run([job, work = std::move(work)]() mutable { return work(*job); }));
    return true;
}

void FileOps::cancel() {
    if (m_job)
        m_job->cancelled = true;
    if (m_process) {
        m_killed = true;
        m_process->kill();
    }
}

// "Restoring 3 of 120 items…" once there are several, `one` before that.
static QString counting(const FileOps::Job &job, const QString &several, const QString &one) {
    return job.total > 1 ? several.arg(qMin(job.done + 1, int(job.total))) : one;
}

void FileOps::sayStopped(const Job &job) {
    emit notice(tr("Stopped after %L1 of %Ln items.", "", job.total).arg(job.done.load()));
}

void FileOps::transferInBackground(const QStringList &paths, const QString &folder, Mode mode, bool releaseClipboard) {
    // Each name that's taken is asked about before anything moves; the
    // last answer starts it.
    const QStringList clashing = clashes(paths, folder);
    if (!clashing.isEmpty()) {
        Pending pending{{}, {}, [=, this](const QHash<QString, Choice> &choices) {
                            startTransfer(paths, folder, mode, releaseClipboard, choices);
                        }};
        for (const QString &path : clashing)
            pending.clashing << std::pair{path, folder};
        m_pending = std::move(pending);
        emit clashChanged();
        return;
    }
    startTransfer(paths, folder, mode, releaseClipboard);
}

QVariantMap FileOps::clash() const {
    if (!m_pending)
        return {};
    const auto &[path, into] = m_pending->clashing.at(m_pending->choices.size());
    const QString there = QDir(into).filePath(QFileInfo(path).fileName());
    const QString folder = QFileInfo(into).fileName();
    return {{"name", QFileInfo(path).fileName()},
            {"folder", folder.isEmpty() ? into : folder},
            {"merge", isFolder(path) && isFolder(there)},
            {"others", m_pending->clashing.size() - m_pending->choices.size() - 1}};
}

void FileOps::answerClash(const QString &choice, bool forAll) {
    if (!m_pending)
        return;
    Pending &pending = *m_pending;
    const Choice answer = choice == "replace" ? Replace : choice == "skip" ? Skip : KeepBoth;
    do
        pending.choices.insert(pending.clashing.at(pending.choices.size()).first, answer);
    while (forAll && pending.choices.size() < pending.clashing.size());
    if (pending.choices.size() < pending.clashing.size()) {
        emit clashChanged();
        return;
    }
    const Pending ready = std::move(pending);
    m_pending.reset();
    emit clashChanged();
    ready.then(ready.choices);
}

void FileOps::dropClash() {
    m_pending.reset();
    emit clashChanged();
}

namespace {
// What work hands back from the worker: what it made, what went wrong, and
// the changes it made, for undo.
struct Outcome {
    QStringList made;
    QString error;
    QStringList failures;
    QList<FileOps::Step> steps;
    // Undoing, how many changes are left that weren't undone.
    qsizetype left = 0;
};
}

void FileOps::startTransfer(const QStringList &paths, const QString &folder, Mode mode, bool releaseClipboard,
                            const QHash<QString, Choice> &choices, bool duplicating) {
    // A drop says what its first item does; each is moved or copied by its
    // own drive.
    const bool moving = mode == Move || (mode == Drop && sameDrive(paths.first(), folder));
    const QFileInfo first(paths.first());
    // A single file shows how much of it is copied, its size known now.
    const qint64 size = paths.size() == 1 && first.isFile() && !first.isSymLink() ? first.size() : 0;
    const QString name = first.fileName();
    const int count = paths.size();
    inBackground(
        [=](const Job &job) {
            const auto bytes = [](qint64 n) { return QLocale().formattedDataSize(n, 1, QLocale::DataSizeSIFormat); };
            const QString one = size > 0 && job.bytes > 0
                                    ? (duplicating ? tr("Duplicating %1: %2 of %3…").arg(name, bytes(job.bytes), bytes(size))
                                       : moving    ? tr("Moving %1: %2 of %3…").arg(name, bytes(job.bytes), bytes(size))
                                                   : tr("Copying %1: %2 of %3…").arg(name, bytes(job.bytes), bytes(size)))
                                : duplicating ? tr("Duplicating %1…").arg(name)
                                : moving      ? tr("Moving %Ln items…", "", count)
                                              : tr("Copying %Ln items…", "", count);
            return moving ? counting(job, tr("Moving %L1 of %Ln items…", "", job.total), one)
                          : counting(job, tr("Copying %L1 of %Ln items…", "", job.total), one);
        },
        [paths, folder, mode, choices](Job &job) {
            Outcome result;
            result.made = transfer(paths, folder, mode, &result.error, choices, &job, &result.steps);
            return result;
        },
        [this, paths, folder, releaseClipboard, duplicating, moving, name](const Outcome &result, const Job &job) {
            // Only the cut that was pasted is let go of; something copied
            // while it moved stays on the clipboard.
            bool cut = false;
            if (releaseClipboard && result.error.isEmpty() && !job.cancelled
                && pathsFrom(QGuiApplication::clipboard()->mimeData(), &cut) == paths && cut)
                QGuiApplication::clipboard()->clear();
            if (!result.steps.isEmpty())
                remember({duplicating ? Duplicating : moving ? Moving : Copying, name, int(result.made.size()),
                          result.steps});
            if (!result.error.isEmpty())
                emit failed(result.error);
            else if (job.cancelled)
                sayStopped(job);
            if (!result.made.isEmpty())
                emit done(folder, result.made.first());
        });
}

void FileOps::duplicate(const QString &path) {
    if (!idle())
        return;
    const QFileInfo info(path);
    // A copy beside itself never clashes, so there's nothing to ask.
    startTransfer({path}, info.absolutePath(), Copy, false, {}, true);
}

QString FileOps::archiveName(const QStringList &paths, const QString &folder) {
    const QString base = paths.size() == 1 ? QFileInfo(paths.first()).fileName() : tr("Archive");
    return freeName(folder, base + ".zip");
}

void FileOps::compress(const QStringList &selected) {
    const QStringList paths = outermost(selected);
    if (paths.isEmpty() || !idle())
        return;
    // Search results and Recents can span folders, so the archive goes in
    // the deepest folder holding them all, with their paths from there.
    const auto holds = [](const QString &folder, const QString &path) {
        const QString relative = QDir(folder).relativeFilePath(QFileInfo(path).absolutePath());
        return relative != ".." && !relative.startsWith("../");
    };
    QString folder = QFileInfo(paths.first()).absolutePath();
    for (const QString &path : paths)
        while (!holds(folder, path))
            folder = QFileInfo(folder).absolutePath();
    // bsdtar writes into a hidden folder of its own beside the archive, so a
    // half-written archive stays out of sight, and another compression into
    // this folder (from another window, or another Folio) can't write into
    // the same file or remove it when that one fails. The folder goes away
    // with what's left in it once bsdtar is done.
    auto temporary = std::make_shared<QTemporaryDir>(QDir(folder).filePath(".compressing-XXXXXX"));
    if (!temporary->isValid()) {
        emit failed(tr("Couldn't compress: %1").arg(temporary->errorString()));
        return;
    }
    const QString archive = archiveName(paths, folder);
    QStringList args{"-a", "-v", "-cf", temporary->filePath(archive), "-C", folder, "--"};
    // -v names each item as bsdtar starts on it, which counts them.
    QHash<QString, qsizetype> order;
    for (const QString &path : paths) {
        args << QDir(folder).relativeFilePath(path);
        order.insert(args.last(), order.size());
    }
    // What bsdtar says that isn't an item is a problem, kept for saying why
    // it failed.
    auto errors = std::make_shared<QString>();
    const auto read = [this, order, archive, errors](QProcess *process) {
        while (process->canReadLine()) {
            const QString line = QString::fromUtf8(process->readLine()).chopped(1);
            if (!line.startsWith("a "))
                *errors += line + '\n';
            else if (const auto at = order.constFind(line.mid(2)); at != order.cend() && order.size() > 1)
                setBusy(tr("Compressing %L1 of %Ln items into %2…", "", order.size()).arg(*at + 1).arg(archive));
        }
    };
    QProcess *process = runTool(
        "bsdtar", args, tr("Compressing into %1…").arg(archive),
        tr("Compressing needs bsdtar, from the libarchive package."), tr("Stopped compressing; no archive was made."),
        [this, paths, folder, temporary, archive, errors, read](QProcess *process, int code) {
            if (code != 0) {
                read(process);
                emit failed(tr("Couldn't compress: %1").arg((*errors + QString::fromUtf8(process->readAll())).trimmed()));
                return;
            }
            // Only a finished archive is named, with a name that's free
            // now. A rename never overwrites, so if another took that
            // name first, it takes the next.
            QFile made(temporary->filePath(archive));
            QString name = archiveName(paths, folder);
            while (!made.rename(QDir(folder).filePath(name))) {
                if (!QFileInfo::exists(QDir(folder).filePath(name))) {
                    emit failed(tr("Couldn't compress: %1").arg(made.errorString()));
                    return;
                }
                name = archiveName(paths, folder);
            }
            remember({Compressing, name, 1, {identified({true, QDir(folder).filePath(name), {}})}});
            emit done(folder, name);
        });
    if (!process)
        return;
    process->setReadChannel(QProcess::StandardError);
    connect(process, &QProcess::readyReadStandardError, this, [process, read] { read(process); });
}

bool FileOps::canExtract(const QStringList &paths) const {
    // A lone compressed file (notes.txt.gz) is no archive to bsdtar, and a
    // disk image is for burning, not unpacking.
    static const QStringList single{"application/gzip", "application/x-xz", "application/zstd",
                                    "application/x-iso9660-image", "application/x-cd-image"};
    // A folder is inode/directory, never an archive, whatever its name.
    const QMimeDatabase mimes;
    for (const QString &path : paths) {
        const QString mime = mimes.mimeTypeForFile(QFileInfo(path)).name();
        if (FolderModel::kindForMime(mime) != "archive" || single.contains(mime))
            return false;
    }
    return !paths.isEmpty();
}

// Why bsdtar couldn't extract `name`, from what it said.
static QString whyNot(const QString &name, const QString &said) {
    if (said.contains("passphrase", Qt::CaseInsensitive) || said.contains("encrypt", Qt::CaseInsensitive))
        return FileOps::tr("Couldn't extract %1: it's protected by a password.").arg(name);
    if (said.contains("Path contains '..'") || said.contains("through symlink"))
        return FileOps::tr("Couldn't extract %1: it would put files outside its folder.").arg(name);
    return FileOps::tr("Couldn't extract %1: it's damaged, or isn't an archive Folio can read.").arg(name);
}

// Unpacks `archive` into a folder named after it in `into`. bsdtar runs as a
// program of its own, so an archive made to crash it can't take Folio along,
// and a cancel can stop it. Returns what goes beside the archive: the one
// item it held, or the folder holding its several. Empty when stopped, or
// with `error` saying why it couldn't.
static QString unpack(const QString &archive, const QString &into, FileOps::Job &job, bool countEntries,
                      QString *error) {
    const QString name = QFileInfo(archive).fileName();
    const int dot = FileOps::extensionStart(name, false);
    // "...tar" would leave "..", the folder beside the archive.
    QString base = dot > 0 ? name.left(dot) : name;
    if (base == "." || base == "..")
        base = FileOps::tr("%1 contents").arg(name);
    const QString folder = QDir(into).filePath(base);
    QDir().mkdir(folder);
    QProcess bsdtar;
    bsdtar.setProcessChannelMode(QProcess::MergedChannels);
    // bsdtar refuses absolute paths, "..", and writing through a link, unless
    // told otherwise (-P), which it never is here. Owners and the setuid,
    // setgid and sticky bits aren't restored, as for any user but root; that
    // holds even when Folio runs as root. A password is given, so an
    // encrypted archive is refused as one, rather than having bsdtar ask on
    // a terminal there isn't, forever. Mac archives' __MACOSX folder holds
    // nothing but Finder's notes, and would make one item look like two.
    bsdtar.start("bsdtar", {"-x", "-v", "--no-same-owner", "--no-same-permissions", "--passphrase", "folio",
                            "--exclude", "__MACOSX", "-f", archive, "-C", folder});
    if (!bsdtar.waitForStarted()) {
        *error = FileOps::tr("Extracting needs bsdtar, from the libarchive package.");
        return {};
    }
    // -v names each item as it's made ("x name"), which counts them. What
    // went wrong with one follows its name ("x name: why"); only the why is
    // kept, so a name can't be taken for a reason.
    QByteArray said;
    const auto read = [&] {
        while (bsdtar.canReadLine()) {
            const QByteArray line = bsdtar.readLine();
            if (!line.startsWith("x "))
                said += line;
            else if (const qsizetype why = line.indexOf(": "); why >= 0)
                said += line.mid(why);
            if (countEntries && line.startsWith("x "))
                ++job.done;
        }
    };
    while (!bsdtar.waitForFinished(100) && bsdtar.state() != QProcess::NotRunning) {
        read();
        if (job.cancelled) {
            bsdtar.kill();
            bsdtar.waitForFinished();
            return {};
        }
    }
    read();
    if (bsdtar.exitStatus() != QProcess::NormalExit || bsdtar.exitCode() != 0) {
        *error = whyNot(name, QString::fromUtf8(said + bsdtar.readAll()));
        return {};
    }
    const QStringList items = QDir(folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System);
    if (items.isEmpty()) {
        *error = FileOps::tr("There's nothing in %1 to extract.").arg(name);
        return {};
    }
    return items.size() == 1 ? QDir(folder).filePath(items.first()) : folder;
}

void FileOps::extract(const QStringList &archives) {
    if (archives.isEmpty() || !idle())
        return;
    // Each archive is unpacked into a hidden folder of its own beside it, so
    // what's half made stays out of sight and goes with that folder when
    // it's stopped or fails. Only what's finished is moved out, which on the
    // same drive is a rename.
    QList<std::shared_ptr<QTemporaryDir>> hidden;
    QStringList into;
    for (const QString &archive : archives) {
        const QFileInfo info(archive);
        auto temporary = std::make_shared<QTemporaryDir>(info.absoluteDir().filePath(".extracting-XXXXXX"));
        if (!temporary->isValid()) {
            emit failed(tr("Couldn't extract %1: %2").arg(info.fileName(), temporary->errorString()));
            return;
        }
        hidden << temporary;
        into << temporary->path();
    }
    const QString name = QFileInfo(archives.first()).fileName();
    const int count = archives.size();
    inBackground(
        [name, count](const Job &job) {
            if (count > 1)
                return counting(job, tr("Extracting %L1 of %Ln archives…", "", job.total),
                                tr("Extracting %Ln archives…", "", count));
            return job.done > 0 ? tr("Extracting %1: %Ln items…", "", job.done).arg(name) : tr("Extracting %1…").arg(name);
        },
        [archives, into](Job &job) {
            // Several archives count themselves; one counts what's in it.
            Outcome result;
            const bool one = archives.size() == 1;
            job.total = one ? 0 : archives.size();
            for (qsizetype i = 0; i < archives.size() && !job.cancelled; ++i) {
                QString error;
                result.made << unpack(archives[i], into[i], job, one, &error);
                if (!error.isEmpty())
                    result.failures << error;
                if (!one)
                    ++job.done;
            }
            return result;
        },
        [this, archives, hidden, name, count](const Outcome &result, const Job &job) {
            if (job.cancelled) {
                for (const auto &folder : hidden)
                    folder->remove();
                emit notice(count == 1 ? tr("Stopped extracting %1; nothing was extracted.").arg(name)
                                       : tr("Stopped extracting; nothing was extracted."));
                return;
            }
            if (!result.failures.isEmpty())
                emit failed(result.failures.join(' '));
            // What each archive gave goes in beside it, a name that's taken
            // asked about first, as a paste's is: taken already, or by what
            // an archive before it gives.
            QList<std::pair<QString, QString>> items, clashing;
            QStringList from;
            QSet<QString> placed;
            for (qsizetype i = 0; i < result.made.size(); ++i) {
                if (result.made[i].isEmpty())
                    continue;
                const QFileInfo archive(archives[i]);
                items << std::pair{result.made[i], archive.absolutePath()};
                from << archive.fileName();
                const QString there = archive.absoluteDir().filePath(QFileInfo(result.made[i]).fileName());
                if (taken(there) || placed.contains(there))
                    clashing << items.last();
                placed.insert(there);
            }
            // Placed off the UI thread too, since merging into a folder
            // that's there goes through all of it, and it can be stopped.
            // The hidden folders go once what's in them is placed, with
            // whatever was skipped or stopped, or once the clash is dropped.
            const auto place = [this, items, from, hidden, name, count](const QHash<QString, Choice> &choices) {
                inBackground(
                    [name, count](const Job &) {
                        return count > 1 ? tr("Extracting %Ln archives…", "", count) : tr("Extracting %1…").arg(name);
                    },
                    [items, choices](Job &job) {
                        Outcome result;
                        for (const auto &[item, into] : items) {
                            const QStringList names = transfer({item}, into, Move, &result.error, choices, &job, &result.steps);
                            if (result.made.isEmpty() && !names.isEmpty())
                                result.made = {into, names.first()};
                            if (!result.error.isEmpty() || job.cancelled)
                                break;
                        }
                        return result;
                    },
                    [this, from, hidden, name, count](const Outcome &result, const Job &job) {
                        // The folders a merge emptied were the hidden ones'
                        // own, and went with them: there's nothing to make
                        // again. Moved out of its hidden folder, what was
                        // extracted is new where it is now: undo sends it to
                        // the trash, as it does a copy.
                        QList<Step> steps = result.steps;
                        steps.removeIf([](const Step &step) { return step.folder; });
                        for (Step &step : steps)
                            if (!step.trash) {
                                step.trash = true;
                                step.other.clear();
                            }
                        for (const auto &folder : hidden)
                            folder->remove();
                        if (!steps.isEmpty())
                            remember({Extracting, from.first(), int(from.size()), steps});
                        if (!result.error.isEmpty())
                            emit failed(result.error);
                        else if (job.cancelled && steps.isEmpty())
                            emit notice(count == 1 ? tr("Stopped extracting %1; nothing was extracted.").arg(name)
                                                   : tr("Stopped extracting; nothing was extracted."));
                        else if (job.cancelled)
                            emit notice(count == 1 ? tr("Stopped extracting %1; only part of it was extracted.").arg(name)
                                                   : tr("Stopped extracting; only part was extracted."));
                        if (!result.made.isEmpty())
                            emit done(result.made.first(), result.made.last());
                    });
            };
            if (clashing.isEmpty()) {
                place({});
                return;
            }
            m_pending = Pending{clashing, {}, place};
            emit clashChanged();
        });
}

void FileOps::trash(const QStringList &paths) {
    if (paths.isEmpty() || !idle())
        return;
    const int count = paths.size();
    inBackground(
        [count](const Job &job) {
            return counting(job, tr("Moving %L1 of %Ln items to the trash…", "", job.total),
                            tr("Moving %Ln items to the trash…", "", count));
        },
        [paths](Job &job) {
            Outcome result;
            eachOutermost(paths, job, [&](const QString &path) {
                QString why;
                // In the trash, but not found there, it can't be undone.
                if (const QString trashed = trashOne(path, &why); !trashed.isEmpty())
                    result.steps << identified({true, path, trashed});
                else if (!why.isEmpty())
                    result.failures << tr("%1 (%2)").arg(QFileInfo(path).fileName(), why);
            });
            return result;
        },
        [this, name = QFileInfo(paths.first()).fileName()](const Outcome &result, const Job &job) {
            if (!result.steps.isEmpty())
                remember({Trashing, name, int(result.steps.size()), result.steps});
            if (!result.failures.isEmpty())
                emit failed(tr("Couldn't move to the trash: %1").arg(QLocale().createSeparatedList(result.failures)));
            else if (job.cancelled)
                sayStopped(job);
        });
}

QStringList FileOps::trashedFrom(const QString &original) {
    const QString name = QFileInfo(original).fileName();
    QStringList found;
    for (const QString &dir : trashDirs())
        for (const QString &note : QDir(dir + "/info").entryList(QDir::Files | QDir::Hidden))
            if (note.startsWith(name) && note.endsWith(".trashinfo")
                && originalPath(dir + "/files/" + note.chopped(10)) == original)
                found << dir + "/files/" + note.chopped(10);
    return found;
}

// Deletion times count only seconds, so two trashed in one can tie: what
// was there before isn't it, whatever its time.
QString FileOps::trashedSince(const QString &original, const QStringList &before) {
    QString found;
    QDateTime newest;
    for (const QString &item : trashedFrom(original)) {
        const QDateTime when = deletedAt(item);
        if (!before.contains(item) && (found.isEmpty() || when > newest)) {
            found = item;
            newest = when;
        }
    }
    return found;
}

// The home trash is filled here rather than by GIO, which doesn't say where
// in the trash an item went, and undo needs to know that to bring it back.
// Each item gets its note first, under a name no other item can take at
// the same time, then goes in, as the freedesktop Trash spec has it. Other
// drives' trash folders, with their own rules, are GIO's to fill, and the
// item is looked for there once it's gone.
QString FileOps::trashOne(const QString &path, QString *error) {
    const QFileInfo item(path);
    const QString trash = homeTrash();
    if (!taken(path)) {
        *error = QString::fromLocal8Bit(strerror(ENOENT));
        return {};
    }
    const auto owner = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    if (QDir().mkpath(trash + "/files", owner) && QDir().mkpath(trash + "/info", owner)
        && sameDrive(path, trash)) {
        const QByteArray note = "[Trash Info]\nPath=" + QUrl::toPercentEncoding(item.absoluteFilePath(), "/")
                                + "\nDeletionDate=" + QDateTime::currentDateTime().toString("yyyy-MM-ddTHH:mm:ss").toUtf8() + "\n";
        // A name taken in either folder goes on to the next; a drive that
        // turns out to be another (a btrfs subvolume) goes to GIO.
        for (int n = 1, why = EEXIST; why == EEXIST; ++n) {
            const QString name = n == 1 ? item.fileName() : item.fileName() + '.' + QString::number(n);
            QFile info(trash + "/info/" + name + ".trashinfo");
            if (!info.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
                if (info.exists())
                    continue;
                *error = info.errorString();
                return {};
            }
            const bool written = info.write(note) == note.size() && info.flush();
            info.close();
            const QString trashed = trash + "/files/" + name;
            if (written && renameNoReplace(path, trashed))
                return trashed;
            why = errno;
            info.remove();
            if (!written || (why != EEXIST && why != EXDEV)) {
                *error = written ? QString::fromLocal8Bit(strerror(why)) : info.errorString();
                return {};
            }
        }
    }
    const QStringList before = trashedFrom(item.absoluteFilePath());
    GFile *file = g_file_new_for_path(QFile::encodeName(path).constData());
    GError *problem = nullptr;
    const bool trashed = g_file_trash(file, nullptr, &problem);
    g_object_unref(file);
    if (!trashed) {
        *error = problem ? QString::fromUtf8(problem->message) : QString();
        if (problem)
            g_error_free(problem);
        return {};
    }
    return trashedSince(item.absoluteFilePath(), before);
}

QString FileOps::homeTrash() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Trash";
}

QStringList FileOps::trashDirs() {
    QStringList dirs{homeTrash()};
    // Drives keep their own trash at the top, so deleting never copies
    // across disks: .Trash-<uid> (or .Trash/<uid>).
    const QString uid = QString::number(getuid());
    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        for (const QString &dir : {volume.rootPath() + "/.Trash-" + uid, volume.rootPath() + "/.Trash/" + uid})
            if (QFileInfo(dir + "/files").isDir() && !dirs.contains(dir))
                dirs << dir;
    }
    return dirs;
}

bool FileOps::inTrash(const QString &path) const {
    const QString clean = QDir::cleanPath(path);
    for (const QString &dir : trashDirs())
        if (clean == dir + "/files" || clean.startsWith(dir + "/files/"))
            return true;
    return false;
}

QString FileOps::infoFileFor(const QString &trashedPath) {
    // Only what's directly in a trash's files folder was trashed on its own,
    // with a note. What's inside a trashed folder came along with it and has
    // none; looking one level up would find a file in a trashed folder that
    // happens to be called "info".
    const QFileInfo item(trashedPath);
    const QString folder = QDir::cleanPath(item.absolutePath());
    for (const QString &dir : trashDirs())
        if (folder == QDir::cleanPath(dir + "/files"))
            return dir + "/info/" + item.fileName() + ".trashinfo";
    return {};
}

QString FileOps::originalPath(const QString &trashedPath) {
    QFile info(infoFileFor(trashedPath));
    if (info.fileName().isEmpty() || !info.open(QIODevice::ReadOnly))
        return {};
    for (const QByteArray &line : info.readAll().split('\n')) {
        if (!line.startsWith("Path="))
            continue;
        const QString path = QUrl::fromPercentEncoding(line.mid(5).trimmed());
        if (path.startsWith('/'))
            return path;
        // A drive's own trash records paths from the top of that drive.
        const QString trashDir = QFileInfo(QFileInfo(trashedPath).absolutePath()).absolutePath();
        const QString top = trashDir.contains("/.Trash/") ? trashDir.section("/.Trash/", 0, 0)
                                                          : QFileInfo(trashDir).absolutePath();
        return QDir(top).filePath(path);
    }
    return {};
}

QDateTime FileOps::deletedAt(const QString &trashedPath) {
    QFile info(infoFileFor(trashedPath));
    if (info.fileName().isEmpty() || !info.open(QIODevice::ReadOnly))
        return {};
    for (const QByteArray &line : info.readAll().split('\n'))
        if (line.startsWith("DeletionDate="))
            // Local time, without a zone, as the spec writes it.
            return QDateTime::fromString(QString::fromUtf8(line.mid(13).trimmed()), Qt::ISODate);
    return {};
}

QString FileOps::restoreOne(const QString &trashedPath, QString *error, Job *job) {
    const QString original = originalPath(trashedPath);
    if (original.isEmpty()) {
        *error = tr("%1 has no record of where it came from.").arg(QFileInfo(trashedPath).fileName());
        return {};
    }
    const QString folder = QFileInfo(original).absolutePath();
    if (!QDir().mkpath(folder)) {
        *error = tr("Couldn't recreate %1.").arg(folder);
        return {};
    }
    const QString destination = QDir(folder).filePath(freeName(folder, QFileInfo(original).fileName()));
    // A link is made again rather than renamed, which across drives would
    // copy what it leads to.
    if (!QFileInfo(trashedPath).isSymLink() && QFile::rename(trashedPath, destination)) {
        QFile::remove(infoFileFor(trashedPath));
        return destination;
    }
    // Across drives (rare: trash lives on the item's own drive), it moves
    // the way a move across drives does, each original removed once it's
    // copied. The note stays while anything of it is left in the trash.
    if (!copyRecursively(trashedPath, destination, error, true, job)) {
        // What arrived is now the only copy of it, so where it went is said,
        // stopped when asked or not.
        const QString name = QFileInfo(trashedPath).fileName();
        if (QFileInfo::exists(destination))
            *error = error->isEmpty() ? tr("Stopped partway: part of %1 is restored, to %2.").arg(name, destination)
                                      : tr("Restored only part of %1, to %2: %3").arg(name, destination, *error);
        return {};
    }
    if (QFileInfo::exists(trashedPath) || QFileInfo(trashedPath).isSymLink()) {
        *error = tr("Restored %1, but couldn't remove it from the trash.").arg(QFileInfo(trashedPath).fileName());
        return {};
    }
    QFile::remove(infoFileFor(trashedPath));
    return destination;
}

bool FileOps::deleteTrashed(const QString &trashedPath) {
    const QFileInfo item(trashedPath);
    const bool removed = item.isDir() && !item.isSymLink() ? QDir(trashedPath).removeRecursively()
                                                           : QFile::remove(trashedPath);
    // The note goes once its item is gone, so what's left can be restored.
    if (const QString note = infoFileFor(trashedPath); removed && !note.isEmpty())
        QFile::remove(note);
    return removed;
}

int FileOps::emptyTrashDir(const QString &trashDir, Job *job) {
    int count = 0;
    const QDir files(trashDir + "/files");
    for (const QFileInfo &item : files.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        if (job && job->cancelled)
            break;
        if (deleteTrashed(item.absoluteFilePath()))
            ++count;
        if (job)
            ++job->done;
    }
    // The size cache goes too. A note without its item stays: something
    // being trashed gets its note first, so it may be on its way.
    QFile::remove(trashDir + "/directorysizes");
    return count;
}

int FileOps::trashCount() const {
    int count = 0;
    for (const QString &dir : trashDirs())
        count += QDir(dir + "/files").entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).size();
    return count;
}

void FileOps::restore(const QStringList &paths) {
    if (paths.isEmpty() || !idle())
        return;
    // Restoring across drives copies, which for a folder can take a while.
    const int count = paths.size();
    inBackground(
        [count](const Job &job) {
            return counting(job, tr("Restoring %L1 of %Ln items…", "", job.total), tr("Restoring %Ln items…", "", count));
        },
        [paths](Job &job) {
            Outcome result;
            eachOutermost(paths, job, [&](const QString &path) {
                QString error;
                const QString to = restoreOne(path, &error, &job);
                if (!to.isEmpty())
                    result.made = {to};
                else if (!error.isEmpty())
                    result.failures << error;
            });
            return result;
        },
        [this, count](const Outcome &result, const Job &job) {
            if (!result.failures.isEmpty())
                emit failed(result.failures.join(' '));
            else if (job.cancelled)
                sayStopped(job);
            else if (!result.made.isEmpty()) {
                const QFileInfo last(result.made.first());
                emit notice(count == 1 ? tr("Restored %1 to %2.").arg(last.fileName(), last.absolutePath())
                                       : tr("Restored %Ln items.", "", count));
            }
        });
}

// Deleting for good can't be undone, and what's deleted can't be redone
// either, so the redos go. The undos stay: those that need what's gone say
// so when they're tried.
static void forgetRedos() {
    s_redo.clear();
    tellEveryone();
}

void FileOps::deleteForever(const QStringList &paths) {
    if (paths.isEmpty() || !idle())
        return;
    forgetRedos();
    const int count = paths.size();
    inBackground(
        [count](const Job &job) {
            return counting(job, tr("Deleting %L1 of %Ln items…", "", job.total), tr("Deleting %Ln items…", "", count));
        },
        [paths](Job &job) {
            Outcome result;
            eachOutermost(paths, job, [&](const QString &path) {
                if (!deleteTrashed(path))
                    result.failures << path;
            });
            return result;
        },
        [this](const Outcome &result, const Job &job) {
            if (!result.failures.isEmpty())
                emit failed(tr("Couldn't delete %Ln items.", "", result.failures.size()));
            else if (job.cancelled)
                sayStopped(job);
        });
}

void FileOps::emptyTrash() {
    if (!idle())
        return;
    forgetRedos();
    inBackground(
        [](const Job &job) {
            return counting(job, tr("Emptying the trash: %L1 of %Ln items…", "", job.total), tr("Emptying the trash…"));
        },
        [](Job &job) {
            const QStringList dirs = trashDirs();
            int total = 0;
            for (const QString &dir : dirs)
                total += QDir(dir + "/files").entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System).size();
            job.total = total;
            int count = 0;
            for (const QString &dir : dirs)
                count += emptyTrashDir(dir, &job);
            return count;
        },
        [this](int count, const Job &job) {
            if (job.cancelled)
                sayStopped(job);
            else
                emit notice(tr("Emptied the trash: %Ln items deleted.", "", count));
        });
}

QString FileOps::rename(const QString &path, const QString &name) {
    const QString clean = name.trimmed();
    const QFileInfo info(path);
    if (clean.isEmpty() || clean == info.fileName())
        return {};
    if (clean.contains('/'))
        return tr("A name can't contain \"/\".");
    if (!canStart(Lane::Files))
        return tr("Try again once the work under way is done.");
    const QString destination = QDir(info.absolutePath()).filePath(clean);
    // QFile::rename never overwrites, and lets a name change only its case
    // on drives that ignore case, where the new name already "exists".
    if (!QFile::rename(path, destination))
        return QFileInfo::exists(destination) ? tr("%1 already exists here.").arg(clean)
                                              : tr("Couldn't rename %1.").arg(info.fileName());
    remember({Renaming, info.fileName(), 1, {identified({false, destination, path})}});
    emit done(info.absolutePath(), clean);
    return {};
}

void FileOps::newFolder(const QString &folder) {
    if (!canStart(Lane::Files)) {
        emit failed(tr("Try again once the work under way is done."));
        return;
    }
    if (!QFileInfo(folder).isDir()) {
        emit failed(tr("Folders can't be made here."));
        return;
    }
    // Not "New Folder", the menu's command: many languages word a command
    // as a verb and a name as a noun.
    QString name = tr("Untitled Folder");
    for (int n = 2; QFileInfo::exists(QDir(folder).filePath(name)); ++n)
        name = tr("Untitled Folder %1").arg(n);
    if (!QDir(folder).mkdir(name)) {
        emit failed(tr("Couldn't create a folder here."));
        return;
    }
    remember({MakingFolder, name, 1, {identified({true, QDir(folder).filePath(name), {}})}});
    emit done(folder, name);
}

// Undo and redo.

// Adds a change to the undos or redos, letting go of the oldest past the
// latest few.
static void keep(QList<FileOps::Change> &list, const FileOps::Change &change) {
    list << change;
    if (list.size() > historyLength)
        list.removeFirst();
}

void FileOps::remember(const Change &change) {
    keep(s_undo, change);
    s_redo.clear();
    tellEveryone();
}

void FileOps::renumber(const Job &job) {
    if (job.renumbered.isEmpty())
        return;
    // An item made again more than once goes from what it was before the
    // job to what it is last, followed through each change in order.
    using Id = std::pair<quint64, quint64>;
    QHash<Id, Id> now, before;
    for (const Job::Renumbered &item : job.renumbered) {
        const Id was{item.device, item.inode}, is{item.nowDevice, item.nowInode};
        const Id first = before.contains(was) ? before.take(was) : was;
        now.insert(first, is);
        before.insert(is, first);
    }
    for (QList<Change> *history : {&s_undo, &s_redo})
        for (Change &change : *history)
            for (Step &step : change.steps)
                if (const auto it = now.constFind({step.device, step.inode}); it != now.cend()) {
                    step.device = it->first;
                    step.inode = it->second;
                }
}

void FileOps::forgetHistory() {
    s_undo.clear();
    s_redo.clear();
    tellEveryone();
}

QString FileOps::undoName() const {
    if (s_undo.isEmpty())
        return {};
    switch (s_undo.last().kind) {
    case Renaming: return tr("Undo Rename");
    case Moving: return tr("Undo Move");
    case Copying: return tr("Undo Copy");
    case Duplicating: return tr("Undo Duplicate");
    case MakingFolder: return tr("Undo New Folder");
    case Compressing: return tr("Undo Compress");
    case Extracting: return tr("Undo Extract");
    case Trashing: return tr("Undo Delete");
    }
    return {};
}

// What undoing or redoing `change` did, in a sentence.
static QString didMessage(const FileOps::Change &change, bool undoing) {
    const QString &name = change.name;
    const int n = change.count;
    switch (change.kind) {
    case FileOps::Renaming:
        return undoing ? FileOps::tr("Undid renaming %1.").arg(name) : FileOps::tr("Redid renaming %1.").arg(name);
    case FileOps::Moving:
        return undoing ? FileOps::tr("Undid moving %Ln items.", "", n) : FileOps::tr("Redid moving %Ln items.", "", n);
    case FileOps::Copying:
        return undoing ? FileOps::tr("Undid copying %Ln items.", "", n) : FileOps::tr("Redid copying %Ln items.", "", n);
    case FileOps::Duplicating:
        return undoing ? FileOps::tr("Undid duplicating %1.").arg(name) : FileOps::tr("Redid duplicating %1.").arg(name);
    case FileOps::MakingFolder:
        return undoing ? FileOps::tr("Undid making %1.").arg(name) : FileOps::tr("Redid making %1.").arg(name);
    case FileOps::Compressing:
        return undoing ? FileOps::tr("Undid compressing into %1.").arg(name)
                       : FileOps::tr("Redid compressing into %1.").arg(name);
    case FileOps::Extracting:
        if (n > 1)
            return undoing ? FileOps::tr("Undid extracting %Ln archives.", "", n)
                           : FileOps::tr("Redid extracting %Ln archives.", "", n);
        return undoing ? FileOps::tr("Undid extracting %1.").arg(name) : FileOps::tr("Redid extracting %1.").arg(name);
    case FileOps::Trashing:
        return undoing ? FileOps::tr("Undid moving %Ln items to the trash.", "", n)
                       : FileOps::tr("Redid moving %Ln items to the trash.", "", n);
    }
    return {};
}

// Whether everything is where `steps` left it, so flipping them (the last
// first) does what it says: each item where it was put, and nothing in the
// place it goes back to. A sentence saying what's changed when not.
static QString unchanged(const QList<FileOps::Step> &steps, bool undoing) {
    // What the steps before have moved, which isn't on disk yet.
    QHash<QString, bool> after;
    const auto there = [&](const QString &path) {
        const auto it = after.constFind(path);
        return it != after.cend() ? *it : taken(path);
    };
    for (auto step = steps.crbegin(); step != steps.crend(); ++step) {
        const auto [now, next] = ends(*step);
        // An item in the trash is the one trashed only while its note still
        // says it came from here: once it was deleted for good, the name
        // there can have gone to another.
        const bool trashed = step->trash && !step->other.isEmpty();
        // Nor is anything else put where it was.
        if (!now.isEmpty()
            && (!there(now) || (trashed && FileOps::originalPath(now) != step->path)
                || (!after.contains(now) && !sameAsLeft(*step, now)))) {
            const QString name = QFileInfo(step->path).fileName();
            return undoing ? FileOps::tr("Can't undo that: %1 isn't where it was left.").arg(name)
                           : FileOps::tr("Can't redo that: %1 isn't where it was left.").arg(name);
        }
        if (!next.isEmpty() && there(next)) {
            const QFileInfo blocked(next);
            const QString folder = QFileInfo(blocked.absolutePath()).fileName();
            return undoing ? FileOps::tr("Can't undo that: %1 already exists in %2.").arg(blocked.fileName(), folder)
                           : FileOps::tr("Can't redo that: %1 already exists in %2.").arg(blocked.fileName(), folder);
        }
        if (!now.isEmpty())
            after[now] = false;
        if (!next.isEmpty())
            after[next] = true;
    }
    return {};
}

// Gives a folder made again its permissions and modified time.
static void settle(const FileOps::Step &step) {
    const QByteArray path = QFile::encodeName(step.path);
    ::chmod(path.constData(), step.mode & 07777);
    const timespec times[2] = {{0, UTIME_OMIT}, step.modified};
    ::utimensat(AT_FDCWD, path.constData(), times, AT_SYMLINK_NOFOLLOW);
}

// Makes the change in `step` the other way (back, or forward again),
// turning `step` around to match. A move across drives that doesn't all
// go leaves `moved`, the steps that put back what did.
static bool flip(FileOps::Step &step, QString *error, FileOps::Job *job, QList<FileOps::Step> *moved) {
    if (step.folder) {
        const QByteArray path = QFile::encodeName(step.path);
        const QString name = QFileInfo(step.path).fileName();
        // Made open to its owner, so what goes back in can; it gets its own
        // permissions once that has (settle()).
        if (step.gone && ::mkdir(path.constData(), (step.mode & 07777) | S_IRWXU) != 0) {
            *error = FileOps::tr("Couldn't create %1.").arg(name);
            return false;
        }
        struct stat now;
        if (!step.gone && ::lstat(path.constData(), &now) == 0)
            step.mode = now.st_mode;
        if (!step.gone && ::rmdir(path.constData()) != 0) {
            *error = FileOps::tr("Couldn't remove %1.").arg(name);
            return false;
        }
        // Removed, it keeps the inode it had; made again, it has another.
        step.gone = !step.gone;
        if (!step.gone) {
            const FileOps::Step was = step;
            step = identified(step);
            if (job && was.inode != 0 && step.inode != 0)
                job->renumbered << FileOps::Job::Renumbered{was.device, was.inode, step.device, step.inode};
        }
        return true;
    }
    if (!step.trash) {
        // The folder it came from may have gone with it, as a merge's does.
        QDir().mkpath(QFileInfo(step.other).absolutePath());
        QList<FileOps::Step> went;
        if (!put(step.path, step.other, true, error, job, nullptr, &went)) {
            *moved = went;
            return false;
        }
        std::swap(step.path, step.other);
        step = identified(step);
        return true;
    }
    if (step.other.isEmpty()) {
        // A copy in a folder copied read-only, as its original was, can go
        // only while its folder lets it, which then gets its permissions
        // back.
        const QByteArray folder = QFile::encodeName(QFileInfo(step.path).absolutePath());
        struct stat holder;
        const bool opened = ::access(folder.constData(), W_OK) != 0 && ::stat(folder.constData(), &holder) == 0
                            && ::chmod(folder.constData(), (holder.st_mode & 07777) | S_IWUSR) == 0;
        const auto close = qScopeGuard([&] {
            if (opened)
                ::chmod(folder.constData(), holder.st_mode & 07777);
        });
        QString why;
        step.other = FileOps::trashOne(step.path, &why);
        if (step.other.isEmpty())
            *error = FileOps::tr("Couldn't move %1 to the trash: %2").arg(QFileInfo(step.path).fileName(), why);
        step = identified(step);
        return !step.other.isEmpty();
    }
    const QString back = FileOps::restoreOne(step.other, error, job);
    if (back.isEmpty())
        return false;
    step.path = back;
    step.other.clear();
    step = identified(step);
    return true;
}

void FileOps::undo() {
    step(true);
}

void FileOps::redo() {
    step(false);
}

void FileOps::step(bool undoing) {
    QList<Change> &from = undoing ? s_undo : s_redo;
    if (from.isEmpty()) {
        emit notice(undoing ? tr("There's nothing to undo.") : tr("There's nothing to redo."));
        return;
    }
    if (!idle(Lane::History))
        return;
    if (const QString problem = unchanged(from.last().steps, undoing); !problem.isEmpty()) {
        emit failed(problem);
        return;
    }
    // Taken off while it runs, nothing else changing the history meanwhile.
    const Change change = from.takeLast();
    tellEveryone();
    inBackground(
        [undoing](const Job &job) {
            return undoing ? counting(job, tr("Undoing %L1 of %Ln items…", "", job.total), tr("Undoing…"))
                           : counting(job, tr("Redoing %L1 of %Ln items…", "", job.total), tr("Redoing…"));
        },
        [steps = change.steps](Job &job) mutable {
            Outcome result;
            job.total = steps.size();
            result.left = steps.size();
            QList<Step> moved;
            while (result.left > 0 && !job.cancelled && flip(steps[result.left - 1], &result.error, &job, &moved)) {
                --result.left;
                ++job.done;
            }
            // What went back into a folder made again changed it.
            for (qsizetype i = result.left; i < steps.size(); ++i)
                if (steps[i].folder && !steps[i].gone)
                    settle(steps[i]);
            // What a step stopped partway had moved is put back before it
            // goes again; what's been put back is done with, never redone.
            result.steps = steps.mid(0, result.left);
            for (Step &step : moved) {
                step.back = true;
                result.steps << step;
            }
            for (const Step &step : steps.mid(result.left))
                if (!step.back)
                    result.steps << step;
            result.left += moved.size();
            return result;
        },
        [this, change, undoing](const Outcome &result, const Job &job) {
            // What was flipped goes over to the other list, turned around so
            // it flips back the last first too; what wasn't stays.
            if (result.left > 0)
                (undoing ? s_undo : s_redo) << Change{change.kind, change.name, change.count, result.steps.mid(0, result.left)};
            if (result.left < result.steps.size()) {
                QList<Step> flipped = result.steps.mid(result.left);
                std::reverse(flipped.begin(), flipped.end());
                keep(undoing ? s_redo : s_undo, Change{change.kind, change.name, change.count, flipped});
            }
            tellEveryone();
            if (!result.error.isEmpty())
                emit failed(result.error);
            else if (job.cancelled && result.left > 0)
                sayStopped(job);
            else
                emit notice(didMessage(change, undoing));
            // What's back in a folder is selected there, as after a paste.
            for (const Step &step : result.steps.mid(result.left))
                if (!step.folder && (!step.trash || step.other.isEmpty())) {
                    const QFileInfo item(step.path);
                    emit done(item.absolutePath(), item.fileName());
                    break;
                }
        },
        Lane::History);
}

// The type apps are registered for: a folder's, or the file's by its name
// and contents.
static QString mimeFor(const QString &path) {
    return QFileInfo(path).isDir() ? QStringLiteral("inode/directory") : QMimeDatabase().mimeTypeForFile(path).name();
}

QString FileOps::defaultAppName(const QString &mime) {
    if (mime.isEmpty() || mime == "inode/directory")
        return {};
    char *type = g_content_type_from_mime_type(mime.toUtf8().constData());
    if (!type)
        return {};
    GAppInfo *app = g_app_info_get_default_for_type(type, FALSE);
    g_free(type);
    if (!app)
        return {};
    const QString name = QString::fromUtf8(g_app_info_get_name(app));
    g_object_unref(app);
    return name;
}

QString FileOps::defaultAppFor(const QString &path) {
    const QString mime = mimeFor(path);
    char *type = g_content_type_from_mime_type(mime.toUtf8().constData());
    if (!type)
        return {};
    GAppInfo *app = g_app_info_get_default_for_type(type, FALSE);
    g_free(type);
    if (!app)
        return {};
    const QString id = QString::fromUtf8(g_app_info_get_id(app));
    g_object_unref(app);
    return id;
}

QString FileOps::openInDefaultApp(const QString &path) {
    // xdg-open asks only for the file's exact type, so a type with no
    // default of its own (Ruby, C++, shell scripts) opened nothing at all.
    const QString id = defaultAppFor(path);
    if (id.isEmpty())
        return tr("No app is set up to open %1.").arg(QFileInfo(path).fileName());
    GDesktopAppInfo *app = g_desktop_app_info_new(id.toUtf8().constData());
    if (!app)
        return tr("No app is set up to open %1.").arg(QFileInfo(path).fileName());
    GList *files = g_list_append(nullptr, g_file_new_for_path(path.toUtf8().constData()));
    GError *error = nullptr;
    QString problem;
    if (!g_app_info_launch(G_APP_INFO(app), files, nullptr, &error))
        problem = tr("Couldn't open %1 with %2: %3")
                      .arg(QFileInfo(path).fileName(), QString::fromUtf8(g_app_info_get_name(G_APP_INFO(app))),
                           QString::fromUtf8(error ? error->message : ""));
    if (error)
        g_error_free(error);
    g_list_free_full(files, g_object_unref);
    g_object_unref(app);
    return problem;
}

QVariantList FileOps::appsFor(const QString &path) const {
    QVariantList apps;
    const QString mime = mimeFor(path);
    char *type = g_content_type_from_mime_type(mime.toUtf8().constData());
    if (!type)
        return apps;
    GAppInfo *fallback = g_app_info_get_default_for_type(type, FALSE);
    const QString defaultId = fallback ? QString::fromUtf8(g_app_info_get_id(fallback)) : QString();
    if (fallback)
        g_object_unref(fallback);
    GList *all = g_app_info_get_all_for_type(type);
    for (GList *l = all; l; l = l->next) {
        auto *info = G_APP_INFO(l->data);
        const QString id = QString::fromUtf8(g_app_info_get_id(info));
        // Hidden apps aren't offered, but the default is always listed,
        // even a hidden one (Omarchy's editor handler), so it can be named.
        if (!g_app_info_should_show(info) && id != defaultId)
            continue;
        const QVariantMap app{{"id", id}, {"name", QString::fromUtf8(g_app_info_get_name(info))},
                              {"isDefault", id == defaultId}};
        if (id == defaultId)
            apps.prepend(app);
        else
            apps << app;
    }
    g_list_free_full(all, g_object_unref);
    g_free(type);
    return apps;
}

void FileOps::openWith(const QString &path, const QString &appId) {
    GDesktopAppInfo *app = g_desktop_app_info_new(appId.toUtf8().constData());
    if (!app)
        return;
    GList *files = g_list_append(nullptr, g_file_new_for_path(path.toUtf8().constData()));
    GError *error = nullptr;
    if (!g_app_info_launch(G_APP_INFO(app), files, nullptr, &error))
        emit failed(tr("Couldn't open %1 with %2: %3")
                        .arg(QFileInfo(path).fileName(), QString::fromUtf8(g_app_info_get_name(G_APP_INFO(app))),
                             QString::fromUtf8(error ? error->message : "")));
    if (error)
        g_error_free(error);
    g_list_free_full(files, g_object_unref);
    g_object_unref(app);
}

void FileOps::setDefaultApp(const QString &path, const QString &appId) {
    GDesktopAppInfo *app = g_desktop_app_info_new(appId.toUtf8().constData());
    if (!app)
        return;
    // Set for the file's exact type, so a Ruby file's choice doesn't change
    // what every plain-text file opens with.
    const QString mime = mimeFor(path);
    char *type = g_content_type_from_mime_type(mime.toUtf8().constData());
    GError *error = nullptr;
    if (!type || !g_app_info_set_as_default_for_type(G_APP_INFO(app), type, &error))
        emit failed(tr("Couldn't make %1 the default app for %2: %3")
                        .arg(QString::fromUtf8(g_app_info_get_name(G_APP_INFO(app))), QMimeDatabase().mimeTypeForName(mime).comment(),
                             QString::fromUtf8(error ? error->message : "")));
    if (error)
        g_error_free(error);
    g_free(type);
    g_object_unref(app);
}

void FileOps::openTerminal(const QString &folder) {
    // xdg-terminal-exec starts the terminal Omarchy is set up with.
    if (!QProcess::startDetached("xdg-terminal-exec", {"--dir=" + folder}, folder))
        emit failed(tr("Couldn't start a terminal: xdg-terminal-exec isn't installed."));
}

QProcess *FileOps::runTool(const QString &program, const QStringList &args, const QString &busy, const QString &missing,
                          const QString &stopped, const std::function<void(QProcess *, int)> &finished) {
    if (!idle())
        return nullptr;
    auto *process = new QProcess(this);
    m_process = process;
    m_killed = false;
    claim(Lane::Files);
    setBusy(busy);
    // As for work in the background, closing the last window waits for it.
    auto running = std::make_shared<QEventLoopLocker>();
    connect(process, &QProcess::finished, this, [this, process, finished, stopped, running](int code, QProcess::ExitStatus status) {
        process->deleteLater();
        m_process = nullptr;
        release();
        setBusy({});
        if (m_killed)
            emit notice(stopped);
        else
            finished(process, status == QProcess::NormalExit ? code : -1);
    });
    // A program that isn't installed never finishes, so this is the only
    // word from it.
    connect(process, &QProcess::errorOccurred, this, [this, process, missing, running](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process->deleteLater();
        m_process = nullptr;
        release();
        setBusy({});
        emit failed(missing);
    });
    process->start(program, args);
    return process;
}

// The disk udisks lists as `identity` now, if it's still there and still
// the same disk.
static std::optional<Disk> diskWithId(Disks &disks, const QString &identity) {
    for (const Disk &disk : disks.list())
        if (disk.identity == identity)
            return disk;
    return std::nullopt;
}

// Whether `disk` is still the one udisks lists, checked again after any
// wait (a password asked for) and before it's changed: a disk swapped
// meanwhile takes the same object path.
static QString unchanged(Disks &disks, const Disk &disk) {
    return diskWithId(disks, disk.identity) ? QString() : FileOps::tr("The drive was changed or removed.");
}

// Whether the file at `path` is on the disk, which burning it would erase
// half way through reading it.
static bool holds(const Disk &disk, const QString &path) {
    const QFileInfo info(path);
    const QString absolute = info.absoluteFilePath(), canonical = info.canonicalFilePath();
    return std::any_of(disk.mounts.begin(), disk.mounts.end(), [&](const QString &root) {
        const QString under = root.endsWith('/') ? root : root + '/';
        return absolute.startsWith(under) || canonical.startsWith(under);
    });
}

QVariantMap FileOps::driveAt(const QString &root) const {
    for (const Disk &disk : Disks::instance()->list())
        if (disk.mounts.contains(root) && Disks::fit(disk, 0) == Disks::Fits)
            return {{"id", disk.identity}, {"title", Disks::title(disk)}, {"label", disk.label}};
    return {};
}

QVariantList FileOps::formatChoices() const {
    // Only what this machine can make; exFAT, the first, is the default.
    QVariantList choices;
    const auto disks = Disks::instance();
    for (const Disks::FileSystem &fs : Disks::fileSystems())
        if (disks->canFormat(QString::fromLatin1(fs.type)))
            choices << QVariantMap{{"type", QString::fromLatin1(fs.type)}, {"name", QString::fromLatin1(fs.name)},
                                   {"detail", QCoreApplication::translate("Disks", fs.detail)}, {"labelLimit", fs.labelLimit}};
    return choices;
}

void FileOps::format(const QString &id, const QString &label, const QString &type) {
    const QString name = label.trimmed();
    const Disks::FileSystem *fs = Disks::fileSystem(type);
    if (!fs) {
        emit failed(tr("Folio can't make that kind of drive."));
        return;
    }
    // Checked before anything's erased, as udisks only checks once it is:
    // ext4 counts a name in bytes, and FAT keeps plain ASCII without what
    // DOS kept for paths.
    if ((type == "ext4" ? name.toUtf8().size() : name.size()) > fs->labelLimit) {
        emit failed(tr("%1 keeps at most %2 characters of a drive's name.").arg(QString::fromLatin1(fs->name)).arg(fs->labelLimit));
        return;
    }
    const auto odd = std::find_if(name.begin(), name.end(), [](QChar c) {
        return c.unicode() < 32 || c.unicode() > 126 || QStringView(u"\"*/:<>?\\|").contains(c);
    });
    if (type == "vfat" && odd != name.end()) {
        emit failed(tr("FAT32 can't have %1 in a drive's name.").arg(*odd));
        return;
    }
    // Asked before anything's erased: without its mkfs, the drive would be
    // left with no partition at all.
    const auto disks = Disks::instance();
    if (!disks->canFormat(type)) {
        emit failed(tr("This computer can't make %1 drives.").arg(QString::fromLatin1(fs->name)));
        return;
    }
    if (!idle(Lane::Drive))
        return;
    const std::optional<Disk> disk = diskWithId(*disks, id);
    if (!disk || Disks::fit(*disk, 0) != Disks::Fits) {
        emit failed(tr("That drive is gone."));
        return;
    }
    for (const QString &root : disk->mounts)
        emit releasing(root);
    const QString drive = Disks::name(*disk);
    inBackground(
        [drive](const Job &) { return tr("Formatting %1…").arg(drive); },
        [disks, disk = *disk, name, type](Job &job) {
            Outcome result;
            for (const QString &filesystem : disk.filesystems)
                if (result.error = disks->unmount(filesystem); !result.error.isEmpty())
                    return result;
            // Cancelled while it was let go of, nothing's erased yet. Once
            // udisks is formatting, it can't be stopped.
            if (job.cancelled)
                return result;
            if (result.error = unchanged(*disks, disk); !result.error.isEmpty())
                return result;
            QString partition;
            if (result.error = disks->makeTable(disk); !result.error.isEmpty())
                return result;
            if (result.error = disks->addPartition(disk, type, name, &partition); !result.error.isEmpty())
                return result;
            result.made << partition;
            // Mounted again, it's back in the sidebar, ready to fill.
            if (const QString error = disks->mount(partition); !error.isEmpty())
                result.failures << error;
            return result;
        },
        [this, drive](const Outcome &result, const Job &) {
            emit drivesChanged();
            if (!result.error.isEmpty())
                emit failed(tr("Couldn't format %1. %2").arg(drive, result.error));
            else if (result.made.isEmpty())
                emit notice(tr("Stopped before formatting %1. Nothing on it was erased.").arg(drive));
            else if (!result.failures.isEmpty())
                emit failed(tr("Formatted %1, but couldn't mount it. %2").arg(drive, result.failures.first()));
            else
                emit notice(tr("Formatted %1.").arg(drive));
        },
        Lane::Drive);
}

bool FileOps::canBurn(const QString &path) const {
    const QFileInfo info(path);
    if (!info.isFile())
        return false;
    // Older MIME databases don't make an .iso a kind of raw disk image, so
    // each is asked for by name.
    const QMimeType type = QMimeDatabase().mimeTypeForFile(info);
    return type.inherits("application/x-cd-image") || type.inherits("application/x-raw-disk-image");
}

QVariantList FileOps::burnTargets(const QString &image) const {
    const qint64 size = QFileInfo(image).size();
    QVariantList targets;
    for (const Disk &disk : Disks::instance()->list()) {
        const Disks::Fit fit = Disks::fit(disk, size);
        if (fit == Disks::Unfit || holds(disk, image))
            continue;
        const QString title = Disks::title(disk);
        targets << QVariantMap{{"id", disk.identity},
                               {"title", title},
                               {"text", fit == Disks::Fits ? title : tr("%1 (too small)").arg(title)},
                               {"enabled", fit == Disks::Fits}};
    }
    return targets;
}

// Copies the image to the open device `fd` in chunks, counting the bytes in
// the job and stopping when it's cancelled; empty, or why it failed.
static QString writeImage(const QString &image, int fd, FileOps::Job &job) {
    QFile in(image);
    if (!in.open(QIODevice::ReadOnly))
        return FileOps::tr("%1 couldn't be read.").arg(QFileInfo(image).fileName());
    // 4 MiB at a time, as iso2sd's dd does.
    QByteArray buffer(4 << 20, Qt::Uninitialized);
    while (!job.cancelled) {
        const qint64 read = in.read(buffer.data(), buffer.size());
        if (read < 0)
            return FileOps::tr("%1 couldn't be read.").arg(QFileInfo(image).fileName());
        if (read == 0)
            break;
        for (qint64 at = 0; at < read;) {
            const ssize_t written = ::write(fd, buffer.constData() + at, read - at);
            if (written < 0 && errno == EINTR)
                continue;
            // A device that takes nothing more is full, or gone.
            if (written <= 0)
                return FileOps::tr("The drive stopped taking data (%1).").arg(qt_error_string(written < 0 ? errno : ENOSPC));
            at += written;
            job.bytes += written;
        }
    }
    // What can't be synced (EINVAL) has nothing waiting to be.
    if (::fsync(fd) != 0 && errno != EINVAL)
        return FileOps::tr("The drive stopped taking data (%1).").arg(qt_error_string(errno));
    return {};
}

void FileOps::burn(const QString &image, const QString &id) {
    if (!idle(Lane::Drive))
        return;
    const QFileInfo info(image);
    const auto disks = Disks::instance();
    const std::optional<Disk> disk = diskWithId(*disks, id);
    if (!disk || Disks::fit(*disk, info.size()) != Disks::Fits || holds(*disk, image)) {
        emit failed(tr("%1 can't be burned onto that drive.").arg(info.fileName()));
        return;
    }
    for (const QString &root : disk->mounts)
        emit releasing(root);
    const QString file = info.fileName();
    const QString drive = Disks::name(*disk);
    const qint64 size = info.size();
    inBackground(
        [file, drive, size](const Job &job) {
            return job.bytes > 0 ? tr("Burning %1 to %2: %3 of %4…").arg(file, drive, Disks::size(job.bytes), Disks::size(size))
                                 : tr("Burning %1 to %2…").arg(file, drive);
        },
        [disks, disk = *disk, image](Job &job) {
            Outcome result;
            for (const QString &filesystem : disk.filesystems)
                if (result.error = disks->unmount(filesystem); !result.error.isEmpty())
                    return result;
            // An unlocked volume keeps the disk from being opened whole.
            for (const QString &encrypted : disk.unlocked)
                if (result.error = disks->lock(encrypted); !result.error.isEmpty())
                    return result;
            // Cancelled while it was let go of, nothing's erased yet.
            if (job.cancelled)
                return result;
            int fd = -1;
            if (result.error = disks->open(disk, &fd); !result.error.isEmpty())
                return result;
            // Opening it may have waited on a password, long enough to
            // swap one stick for another.
            if (result.error = unchanged(*disks, disk); result.error.isEmpty())
                result.error = writeImage(image, fd, job);
            ::close(fd);
            // Only a whole image is let go of; a partial one stays, to be
            // burned again.
            if (result.error.isEmpty() && !job.cancelled)
                if (const QString error = disks->eject(disk); !error.isEmpty())
                    result.failures << error;
            return result;
        },
        [this, file, drive](const Outcome &result, const Job &job) {
            emit drivesChanged();
            if (!result.error.isEmpty())
                emit failed(tr("Couldn't burn %1 to %2. %3").arg(file, drive, result.error));
            else if (job.cancelled && job.bytes == 0)
                emit notice(tr("Stopped before burning %1. Nothing on %2 was erased.").arg(file, drive));
            else if (job.cancelled)
                emit notice(tr("Stopped burning. %1 now holds only part of %2.").arg(drive, file));
            else if (!result.failures.isEmpty())
                emit failed(tr("Burned %1 to %2, but couldn't eject it. %3").arg(file, drive, result.failures.first()));
            else
                emit notice(tr("Burned %1 to %2. It's safe to remove now.").arg(file, drive));
        },
        Lane::Drive);
}

bool FileOps::idle(Lane lane) {
    if (m_busy.isEmpty() && !m_pending && canStart(lane))
        return true;
    emit failed(tr("Try again once the work under way is done."));
    return false;
}

void FileOps::setBusy(const QString &busy) {
    if (busy == m_busy)
        return;
    m_busy = busy;
    emit busyChanged();
}
