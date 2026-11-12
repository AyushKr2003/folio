#include <QClipboard>
#include <QDir>
#include <QMimeData>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPointer>
#include <memory>
#include <functional>
#include <thread>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QtConcurrent>
#include <QtTest>
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backend.h"
#include "disks.h"
#include "fileops.h"
#include "foldermodel.h"
#include "mounter.h"
#include "places.h"
#include "plugins.h"
#include "previewer.h"
#include "theme.h"
#include "thumbnails.h"
#include "translator.h"

// Puts XDG_DATA_HOME, where the trash and recent files live, in `path` for
// as long as the returned guard does.
[[nodiscard]] static auto dataHomeAt(const QString &path)
{
    const QByteArray old = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", QFile::encodeName(path));
    return qScopeGuard([old] {
        if (old.isNull())
            qunsetenv("XDG_DATA_HOME");
        else
            qputenv("XDG_DATA_HOME", old);
    });
}

// A temporary folder on another drive than QTemporaryDir's, for what has to
// cross drives: beside the tests, when they're on another drive. Null when
// they aren't.
static std::unique_ptr<QTemporaryDir> elsewhere()
{
    auto dir = std::make_unique<QTemporaryDir>(QDir::current().filePath("drive-XXXXXX"));
    if (!dir->isValid() || FileOps::sameDrive(dir->path(), QDir::tempPath()))
        return {};
    return dir;
}

// `tailscale status --json`, trimmed to what the Tailscale plugin reads, for
// a made-up tailnet: three desktops online, one offline, and a phone.
static QByteArray tailnet()
{
    return R"({"Peer":{
        "a":{"HostName":"studio","DNSName":"studio.example.ts.net.","OS":"linux","Online":true},
        "b":{"HostName":"nas","DNSName":"nas.example.ts.net.","OS":"linux","Online":false},
        "c":{"HostName":"Conference-Room","DNSName":"conference-room.example.ts.net.","OS":"macOS","Online":true},
        "d":{"HostName":"phone","DNSName":"phone.example.ts.net.","OS":"android","Online":true},
        "e":{"HostName":"build","DNSName":"build.example.ts.net.","OS":"windows","Online":true}}})";
}

// udisks stood in for: the disks it lists, and each call made to it, in
// order. Opening a disk opens `device`, a file standing in for it; `refuse`
// makes a call fail with that sentence, as udisks would when polkit says no.
class FakeDisks : public Disks {
public:
    QList<Disk> disks;
    QStringList calls;
    QHash<QString, QString> refuse;
    QString device;
    // A descriptor to hand out instead of opening `device`.
    int fd = -1;
    // Runs as each call is made (with its method), to change the world
    // under it: a stick swapped while polkit asks, say.
    std::function<void(const QString &)> during;

    QList<Disk> list() override { return disks; }
    QString unmount(const QString &filesystem) override { return made("unmount " + filesystem, "unmount"); }
    QString lock(const QString &encrypted) override { return made("lock " + encrypted, "lock"); }
    QString makeTable(const Disk &disk) override { return made("makeTable " + disk.block, "makeTable"); }
    QString addPartition(const Disk &disk, const QString &type, const QString &label, QString *partition) override {
        *partition = disk.block + "1";
        return made("addPartition " + disk.block + " " + type + " " + label, "addPartition");
    }
    // Every filesystem but those named in `missing` can be made.
    QStringList missing;
    bool canFormat(const QString &type) override { return !missing.contains(type); }
    QString mount(const QString &filesystem) override { return made("mount " + filesystem, "mount"); }
    QString open(const Disk &disk, int *descriptor) override {
        if (const QString error = made("open " + disk.block, "open"); !error.isEmpty())
            return error;
        *descriptor = fd >= 0 ? fd : ::open(QFile::encodeName(device).constData(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        return {};
    }
    QString eject(const Disk &disk) override { return made("eject " + disk.block, "eject"); }

private:
    QString made(const QString &call, const QString &method) {
        calls << call;
        if (during)
            during(method);
        return refuse.value(method);
    }
};

// A 32 GB SanDisk stick with one filesystem mounted, as udisks lists one.
static Disk sanDisk()
{
    Disk disk;
    disk.block = "/org/freedesktop/UDisks2/block_devices/sdb";
    disk.drive = "/org/freedesktop/UDisks2/drives/SanDisk_Ultra";
    disk.device = "/dev/sdb";
    disk.identity = disk.block + " SanDisk-Ultra-4C530001";
    disk.vendor = "SanDisk";
    disk.model = "Ultra";
    disk.label = "STICK";
    disk.size = 31'914'983'424;
    disk.bus = "usb";
    disk.removable = disk.canPowerOff = disk.ejectable = true;
    disk.mounts = {"/run/media/me/STICK"};
    disk.filesystems = {"/org/freedesktop/UDisks2/block_devices/sdb1"};
    return disk;
}

// Another stick, in the same place the SanDisk one was.
static Disk otherStick()
{
    Disk disk = sanDisk();
    disk.identity = disk.block + " Kingston-DataTraveler-0019E06B";
    disk.vendor = "Kingston";
    disk.model = "DataTraveler";
    return disk;
}

// Puts a fake in udisks' place for the test, and for the tests after it.
static std::shared_ptr<FakeDisks> fakeDisks(const QList<Disk> &disks = {sanDisk()})
{
    auto fake = std::make_shared<FakeDisks>();
    fake->disks = disks;
    Disks::replace(fake);
    return fake;
}

// `bytes` of made-up content at `path`.
static QByteArray writeImageFile(const QString &path, qint64 bytes)
{
    QByteArray data(bytes, Qt::Uninitialized);
    for (qint64 i = 0; i < bytes; ++i)
        data[i] = char((i * 31 + i / 4096) & 0xff);
    QFile file(path);
    if (file.open(QIODevice::WriteOnly))
        file.write(data);
    return data;
}

// Folio's sources, beside the tests.
static QString sourceFolder()
{
    return QFileInfo(QFINDTESTDATA("../src/Format.js")).absolutePath();
}

// A source file with its comments left out (their examples aren't calls)
// and its strings kept whole, so neither hides the other.
static QString withoutComments(const QString &text, bool cpp)
{
    QString out;
    for (qsizetype i = 0; i < text.size();) {
        const QChar c = text.at(i);
        if (text.mid(i, 2) == "//") {
            const qsizetype end = text.indexOf('\n', i);
            i = end < 0 ? text.size() : end;
        } else if (text.mid(i, 2) == "/*") {
            const qsizetype end = text.indexOf("*/", i + 2);
            i = end < 0 ? text.size() : end + 2;
        } else if (cpp && c == '"' && i > 0 && text.at(i - 1) == 'R') {
            // A raw string (a pattern) holds quotes of its own; it's no
            // translation, so it's left out whole.
            const qsizetype open = text.indexOf('(', i);
            const QString close = ')' + text.mid(i + 1, open - i - 1) + '"';
            out += "\"\"";
            i = text.indexOf(close, open) + close.size();
        } else if (c == '"' || c == '\'' || c == '`') {
            qsizetype end = i + 1;
            while (end < text.size() && text.at(end) != c)
                end += text.at(end) == '\\' ? 2 : 1;
            out += text.mid(i, end + 1 - i);
            i = end + 1;
        } else {
            out += c;
            ++i;
        }
    }
    return out;
}

// A literal's text as the language reads it.
static QString unescaped(const QString &literal)
{
    QString out;
    for (qsizetype i = 0; i < literal.size(); ++i) {
        QChar c = literal.at(i);
        if (c == '\\' && i + 1 < literal.size()) {
            c = literal.at(++i);
            c = c == 'n' ? QChar('\n') : c == 't' ? QChar('\t') : c;
        }
        out += c;
    }
    return out;
}

// Every text Folio's sources hand to qsTr(), tr() or QT_TR_NOOP(), and
// whether any call also passes it a count (its third argument). C++'s
// adjacent literals are one text. A call whose text isn't a literal can't
// be in a catalog, and is reported in `problems`.
static QMap<QString, bool> translatables(const QString &folder, QStringList *problems)
{
    static const QRegularExpression call(R"(\b(qsTr|tr|QT_TR_NOOP)\s*\()");
    static const QRegularExpression literal(R"(^\s*(["'])((?:\\.|(?!\1).)*)\1)",
                                            QRegularExpression::DotMatchesEverythingOption);
    QMap<QString, bool> found;
    for (const QFileInfo &info : QDir(folder).entryInfoList({"*.qml", "*.js", "*.cpp", "*.h"}, QDir::Files)) {
        QFile file(info.filePath());
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const bool cpp = info.suffix() == "cpp" || info.suffix() == "h";
        const QString code = withoutComments(QString::fromUtf8(file.readAll()), cpp);
        for (auto calls = call.globalMatch(code); calls.hasNext();) {
            const auto match = calls.next();
            qsizetype at = match.capturedEnd();
            QString text;
            bool isLiteral = false;
            for (auto part = literal.match(code.mid(at)); part.hasMatch(); part = literal.match(code.mid(at))) {
                text += unescaped(part.captured(2));
                at += part.capturedEnd();
                isLiteral = true;
                if (!cpp)
                    break;
            }
            if (!isLiteral) {
                *problems << info.fileName() + ": not a literal: " + code.mid(match.capturedStart(), 60);
                continue;
            }
            // The arguments after the text, up to the call's closing parenthesis.
            int depth = 0, commas = 0;
            for (; at < code.size(); ++at) {
                const QChar c = code.at(at);
                if (c == '"' || c == '\'') {
                    for (++at; at < code.size() && code.at(at) != c; at += code.at(at) == '\\' ? 2 : 1) { }
                } else if (c == '(' || c == '[' || c == '{') {
                    ++depth;
                } else if (c == ')' || c == ']' || c == '}') {
                    if (depth-- == 0)
                        break;
                } else if (c == ',' && depth == 0) {
                    ++commas;
                }
            }
            found[text] = found.value(text) || commas >= 2;
        }
    }
    return found;
}

static QJsonObject readCatalog(const QString &path, QStringList *problems)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *problems << path + ": can't be read";
        return {};
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (!document.isObject())
        *problems << path + ": not a JSON object: " + error.errorString();
    return document.object();
}

// The placeholders in a text, in order: %1 to %9, and the count (%n, %Ln).
static QStringList placeholders(const QString &text)
{
    static const QRegularExpression placeholder("%L?[1-9n]");
    QStringList found;
    for (auto it = placeholder.globalMatch(text); it.hasNext();)
        found << it.next().captured();
    found.sort();
    return found;
}

// What's wrong with the translations in `folder`, measured against its
// en.json: an unknown language, empty or broken translations, and plural
// forms that don't fit the language's rule. A string a catalog lacks, or
// one en.json no longer has, is work for the translation sync
// (scripts/i18n-sync.py), not a problem: the app shows the English
// meanwhile, so a change to the English never waits on 49 languages.
static QStringList catalogProblems(const QString &folder)
{
    static const QStringList codes{"ar", "bg", "bn", "ca", "cs", "da", "de", "el", "es-MX", "et", "fa", "fi", "fil",
                                   "fr", "ga", "he", "hi", "hr", "hu", "id", "is", "it", "ja", "ko", "lt", "lv", "ms",
                                   "nl", "no", "pl", "pt", "pt-BR", "ro", "ru", "si", "sk", "sl", "sr", "sv", "sw",
                                   "ta", "th", "tr", "uk", "ur", "uz", "vi", "zh-CN", "zh-TW"};
    QStringList problems;
    const QJsonObject english = readCatalog(folder + "/en.json", &problems);
    for (const QFileInfo &file : QDir(folder).entryInfoList({"*.json"}, QDir::Files)) {
        const QString code = file.completeBaseName();
        if (code == "en")
            continue;
        if (!codes.contains(code)) {
            problems << file.fileName() + ": not one of Omarchy's language codes";
            continue;
        }
        const QJsonObject catalog = readCatalog(file.filePath(), &problems);

        // The categories the language's rule reaches, and "other" always.
        QSet<QString> categories{"other", Translator::pluralCategory(code, 1000000)};
        for (long long n = 0; n <= 1000; ++n)
            categories << Translator::pluralCategory(code, n);

        for (auto it = english.begin(); it != english.end(); ++it) {
            const QJsonValue translated = catalog.value(it.key());
            const QString where = code + ": " + it.key() + ": ";
            if (translated.isUndefined())
                continue;
            if (it.value().isString()) {
                if (!translated.isString() || translated.toString().isEmpty())
                    problems << where + "needs a translation";
                else if (placeholders(translated.toString()) != placeholders(it.key()))
                    problems << where + "has other placeholders than the English";
                continue;
            }
            const QJsonObject forms = translated.toObject();
            const QStringList given = forms.keys();
            if (!translated.isObject() || QSet<QString>(given.begin(), given.end()) != categories) {
                QStringList wanted(categories.begin(), categories.end());
                wanted.sort();
                problems << where + "needs exactly the plural forms " + wanted.join(", ");
                continue;
            }
            // Every form says every placeholder; one that isn't "other" may
            // say the number in words instead of the count.
            const QStringList all = placeholders(it.value().toObject().value("other").toString());
            const QSet<QString> needed(all.begin(), all.end());
            QSet<QString> fixed = needed;
            fixed.remove("%n");
            fixed.remove("%Ln");
            for (auto form = forms.begin(); form != forms.end(); ++form) {
                const QStringList has = placeholders(form.value().toString());
                const QSet<QString> present(has.begin(), has.end());
                if (form.value().toString().isEmpty())
                    problems << where + form.key() + " needs a translation";
                else if (!(form.key() == "other" ? present == needed : present.contains(fixed) && needed.contains(present)))
                    problems << where + form.key() + " has other placeholders than the English";
            }
        }
    }
    return problems;
}

// Writes a plugin into `folder`: its manifest and, by name, its files,
// the ones starting with #! executable.
static void writePlugin(const QString &folder, const QString &id, const QByteArray &manifest,
                        const QHash<QString, QByteArray> &files = {})
{
    QDir(folder).mkpath(id);
    const QString dir = folder + "/" + id;
    QHash<QString, QByteArray> all = files;
    all.insert("manifest.json", manifest);
    for (auto it = all.begin(); it != all.end(); ++it) {
        QDir().mkpath(QFileInfo(dir + "/" + it.key()).absolutePath());
        QFile file(dir + "/" + it.key());
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(it.value());
        file.close();
        if (it.value().startsWith("#!"))
            file.setPermissions(file.permissions() | QFileDevice::ExeOwner);
    }
}

// The names of a list of maps, for comparing.
static QStringList namesOf(const QVariantList &list)
{
    QStringList names;
    for (const QVariant &v : list)
        names << v.toMap().value("name").toString();
    return names;
}

// A folder of programs for PATH: these scripts, beside the few real tools the
// bundled plugins use, so a test never runs the real tailscale or Dropbox.
static QString binWith(const QString &folder, const QHash<QString, QByteArray> &scripts)
{
    QDir().mkpath(folder);
    for (const char *tool : {"jq", "tr", "sed", "tail", "basename", "cat"})
        QFile::link(QStandardPaths::findExecutable(tool), folder + "/" + tool);
    for (auto it = scripts.begin(); it != scripts.end(); ++it) {
        QFile file(folder + "/" + it.key());
        if (file.open(QIODevice::WriteOnly))
            file.write(it.value());
        file.close();
        file.setPermissions(file.permissions() | QFileDevice::ExeOwner);
    }
    return folder;
}

// Puts PATH at `path` for as long as the returned guard does.
[[nodiscard]] static auto pathAt(const QString &path)
{
    const QByteArray old = qgetenv("PATH");
    qputenv("PATH", QFile::encodeName(path));
    return qScopeGuard([old] { qputenv("PATH", old); });
}

// Whether a process has ended. Killed, it can stay a zombie until whoever
// adopted it reaps it, which in a container's bare PID 1 is never.
static bool ended(pid_t pid)
{
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (::kill(pid, 0) != 0 || !stat.open(QIODevice::ReadOnly))
        return true;
    const QByteArray line = stat.readAll();
    return line.mid(line.lastIndexOf(')') + 2, 1) == "Z";
}

// The plugins that come with Folio, as their checkout has them.
static QString bundled()
{
    return QStringLiteral(FOLIO_PLUGINS);
}

// One of them, read with PATH as it is.
static Plugins::Manifest bundledPlugin(const QString &id)
{
    for (const auto &m : Plugins::read({bundled()}, "en"))
        if (m.id == id)
            return m;
    return {};
}

// Runs a bundled plugin's command with PATH at `bin` and HOME at `home`.
static Plugins::Run runBundled(const Plugins::Manifest &plugin, const QStringList &command, const QString &bin,
                               const QString &home, const QHash<QString, QString> &more = {})
{
    QProcessEnvironment env;
    env.insert("PATH", bin);
    env.insert("HOME", home);
    env.insert("FOLIO_PLUGIN_DIR", plugin.dir);
    env.insert("FOLIO_LANGUAGE", "en");
    for (auto it = more.begin(); it != more.end(); ++it)
        env.insert(it.key(), it.value());
    return Plugins::execute(plugin, command, env, 5000);
}

class FolioTests : public QObject {
    Q_OBJECT

private slots:
    void initTestCase()
    {
        // No test ever reaches the real udisks, which would change real disks.
        fakeDisks({});
    }

    void folderListsDirectoriesFirstWithoutHidden()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("zeta");
        write(dir.filePath("alpha.md"), "# hi");
        write(dir.filePath("photo10.jpg"), "x");
        write(dir.filePath("photo9.jpg"), "x");
        write(dir.filePath(".secret"), "x");

        QString error;
        const auto entries = FolderModel::list(dir.path(), false, "name", &error);
        QVERIFY(error.isEmpty());
        QStringList names;
        for (const auto &e : entries) names << e.name;
        // Folders first, then natural order, so photo9 comes before photo10.
        QCOMPARE(names, QStringList({"zeta", "alpha.md", "photo9.jpg", "photo10.jpg"}));
        QCOMPARE(entries.at(0).kind, QString("folder"));
        QCOMPARE(entries.at(1).kind, QString("text"));
        QCOMPARE(entries.at(2).kind, QString("image"));
        QCOMPARE(entries.at(2).extension, QString("JPG"));

        QCOMPARE(FolderModel::list(dir.path(), true, "name", &error).size(), 5);
    }

    void folderSortsByTheGivenLanguagesAlphabet()
    {
        // Swedish puts å after z; English sorts it with a.
        QTemporaryDir dir;
        write(dir.filePath("zeta.txt"), "x");
        write(dir.filePath("åsa.txt"), "x");
        auto names = [&](const QLocale &locale) {
            QStringList out;
            for (const auto &e : FolderModel::list(dir.path(), false, "name", nullptr, {}, locale))
                out << e.name;
            return out;
        };
        QCOMPARE(names(QLocale("sv_SE")), QStringList({"zeta.txt", "åsa.txt"}));
        QCOMPARE(names(QLocale("en_US")), QStringList({"åsa.txt", "zeta.txt"}));
    }

    void folderReportsMissingFolders()
    {
        QString error;
        QVERIFY(FolderModel::list("/definitely/not/here", false, "name", &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }

    void folderSummaryCountsAndPeeks()
    {
        QTemporaryDir dir;
        write(dir.filePath("a.jpg"), "x");
        write(dir.filePath("b.png"), "x");
        write(dir.filePath("c.png"), "x");
        write(dir.filePath("notes.txt"), "x");
        write(dir.filePath(".hidden"), "x");
        FolderModel::Entry entry;
        entry.path = dir.path();
        FolderModel::summarize(entry, false);
        QCOMPARE(entry.itemCount, 4);
        QCOMPARE(entry.peek.size(), 2);
        // The folder counts just count, the hidden only when they show.
        QCOMPARE(FolderModel::itemCount(dir.path(), false), 4);
        QCOMPARE(FolderModel::itemCount(dir.path(), true), 5);
    }

    void folderModelLoadsInTheBackground()
    {
        QTemporaryDir dir;
        write(dir.filePath("one.txt"), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 1);
        QCOMPARE(model.get(0).value("name").toString(), QString("one.txt"));
        QCOMPARE(model.indexOf("one.txt"), 0);
    }

    void folderSearchesEveryFolderBelow()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("Photos/2024");
        QDir(dir.path()).mkpath(".cache");
        write(dir.filePath("beach notes.txt"), "x");
        write(dir.filePath("Photos/2024/Beach.jpg"), "x");
        write(dir.filePath("Photos/city.jpg"), "x");
        write(dir.filePath(".cache/beach.tmp"), "x");

        auto names = [&](bool hidden) {
            QStringList out;
            for (const auto &e : FolderModel::search(dir.path(), "beach", hidden, "name"))
                out << e.name;
            return out.join(", ");
        };
        // Named by where they are, ignoring case, hidden ones only when shown.
        QCOMPARE(names(false), QString("beach notes.txt, Photos/2024/Beach.jpg"));
        QCOMPARE(names(true), QString(".cache/beach.tmp, beach notes.txt, Photos/2024/Beach.jpg"));
        // Stopped, it gives nothing rather than half an answer.
        QVERIFY(FolderModel::search(dir.path(), "beach", false, "name", [] { return true; }).isEmpty());

        // In the model, a query swaps the folder for its matches; clearing
        // it, or going elsewhere, brings the folder back.
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        model.setQuery("BEACH");
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        QCOMPARE(model.get(1).value("path").toString(), dir.filePath("Photos/2024/Beach.jpg"));
        model.setQuery("");
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.indexOf("Photos"), 0);
        model.setQuery("city");
        QVERIFY(loaded.wait(3000));
        model.setPath(dir.filePath("Photos"));
        QVERIFY(model.query().isEmpty());
    }

    void folderSearchSkipsSymlinkedFolders()
    {
        // A link back up the tree would otherwise be walked round and round.
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("real");
        write(dir.filePath("real/match.txt"), "x");
        QVERIFY(QFile::link(dir.path(), dir.filePath("loop")));
        const auto found = FolderModel::search(dir.path(), "match", false, "name");
        QCOMPARE(found.size(), 1);
        QCOMPARE(found.first().name, QString("real/match.txt"));
    }

    void folderModelSwapsFoldersWithoutAnEmptyFrame()
    {
        QTemporaryDir a, b;
        write(a.filePath("one.txt"), "x");
        write(b.filePath("two.txt"), "x");
        write(b.filePath("three.txt"), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(a.path());
        QVERIFY(loaded.wait(3000));

        // Moving on keeps the old entries until the new ones are in.
        QSignalSpy counts(&model, &FolderModel::countChanged);
        model.setPath(b.path());
        QCOMPARE(model.count(), 1);
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        QCOMPARE(counts.count(), 1); // straight from one to two, never zero

        // A column with no folder clears at once.
        model.setPath(QString());
        QCOMPARE(model.count(), 0);

        // Going back to a folder just seen shows it in the same step, and
        // the background re-read leaves it alone when nothing changed.
        loaded.clear();
        model.setPath(a.path());
        QCOMPARE(model.count(), 1);
        QCOMPARE(loaded.count(), 1);
        QSignalSpy resets(&model, &QAbstractItemModel::modelReset);
        QVERIFY(loaded.wait(3000));
        QCOMPARE(resets.count(), 0);
        QCOMPARE(model.count(), 1);
    }

    void folderModelCountsFoldersAfterAQuickReread()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("Photos");
        QDir(dir.path()).mkpath("Music");
        write(dir.filePath("Photos/a.jpg"), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(loaded.wait(3000));
        // A re-read before the counts came in still ends with them.
        model.reload();
        QVERIFY(loaded.wait(3000));
        QTRY_COMPARE(model.get(0).value("itemCount").toInt(), 0);
        QTRY_COMPARE(model.get(1).value("itemCount").toInt(), 1);
    }

    void folderModelForgetsAFolderLeftWhileLoading()
    {
        QTemporaryDir dir;
        write(dir.filePath("one.txt"), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(model.loading());
        // Emptied before the listing lands, the column stays empty.
        model.setPath(QString());
        QVERIFY(!model.loading());
        QVERIFY(!loaded.wait(200));
        QCOMPARE(model.count(), 0);
    }

    void folderModelStopsReadingWhatsBeenOvertaken()
    {
        // A big folder left before it's been read, or a search of it, stops
        // being read instead of holding on to a thread of the pool that the
        // next listing, the folder counts and file transfers all wait for.
        // With a pool of one thread, the next folder waits for the overtaken
        // read to stop, so it shows in a fraction of the time the read takes.
        QTemporaryDir big, small, other;
        for (int i = 0; i < 20000; ++i)
            write(big.filePath(QString("%1.txt").arg(i)), "x");
        write(small.filePath("a.txt"), "x");
        write(other.filePath("b.txt"), "x");
        QElapsedTimer timer;
        timer.start();
        FolderModel::list(big.path(), false, "name", nullptr);
        const qint64 listing = timer.restart();
        FolderModel::search(big.path(), "none", false, "name");
        const qint64 searching = timer.elapsed();

        QThreadPool *pool = QThreadPool::globalInstance();
        pool->waitForDone();
        const int threads = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        const auto restore = qScopeGuard([&] { pool->setMaxThreadCount(threads); });
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);

        model.setPath(big.path());
        timer.restart();
        model.setPath(small.path());
        QVERIFY(loaded.wait(5000));
        QVERIFY(timer.elapsed() < listing / 2);

        model.setPath(big.path());
        QVERIFY(loaded.wait(5000));
        model.setQuery("none");
        timer.restart();
        model.setPath(other.path());
        QVERIFY(loaded.wait(5000));
        QVERIFY(timer.elapsed() < searching / 2);
    }

    void folderModelTellsCopiedFoldersApart()
    {
        // A copy (cp -a, rsync -a) lists exactly like its original, but its
        // items are its own.
        QTemporaryDir a, b;
        const QDateTime when = QDateTime::fromSecsSinceEpoch(1700000000);
        for (const QTemporaryDir *dir : {&a, &b}) {
            QFile file(dir->filePath("report.txt"));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("same");
            file.flush();
            QVERIFY(file.setFileTime(when, QFileDevice::FileModificationTime));
        }
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(a.path());
        QVERIFY(loaded.wait(3000));
        model.setPath(b.path());
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.get(0).value("path").toString(), b.filePath("report.txt"));
    }

    void fileOpsDeleteForeverOutsideTheTrash()
    {
        // Shift+Delete deletes for good anywhere: the folder and what's in
        // it are gone, a link goes without what it points at, and nothing
        // lands in the trash.
        QTemporaryDir dir, data;
        const auto restore = dataHomeAt(data.path());
        QDir(dir.path()).mkpath("album/inner");
        write(dir.filePath("album/inner/a.jpg"), "picture");
        write(dir.filePath("kept.txt"), "kept");
        QVERIFY(QFile::link(dir.filePath("kept.txt"), dir.filePath("link")));
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.deleteForever({dir.filePath("album"), dir.filePath("link")});
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QVERIFY(!QFileInfo::exists(dir.filePath("album")));
        QVERIFY(!QFileInfo(dir.filePath("link")).isSymLink());
        QVERIFY(QFile::exists(dir.filePath("kept.txt")));
        QVERIFY(!QFileInfo::exists(data.filePath("Trash/files/album")));
    }

    void folderPaneKeysAreNotBoundTwice()
    {
        // Two shortcuts on one key, both on, make Qt call neither: the key
        // does nothing. A standard key can carry more than its usual one
        // (Cut is Shift+Delete too on Linux), so they count as theirs.
        QFile qml(QFINDTESTDATA("../src/FolderPane.qml"));
        QVERIFY(qml.open(QIODevice::ReadOnly));
        const QRegularExpression shortcut("^\\s*Shortcut \\{ sequences?: \\[?(.*?)\\]?; enabled: ");
        const QRegularExpression item("\"([^\"]+)\"|StandardKey\\.(\\w+)");
        const QMetaEnum standard = QMetaEnum::fromType<QKeySequence::StandardKey>();
        QHash<QKeySequence, QString> bound;
        int count = 0;
        for (const QString &line : QString::fromUtf8(qml.readAll()).split('\n')) {
            const auto found = shortcut.match(line);
            if (!found.hasMatch())
                continue;
            for (auto it = item.globalMatch(found.captured(1)); it.hasNext();) {
                const auto key = it.next();
                const QList<QKeySequence> sequences = key.captured(1).isEmpty()
                    ? QKeySequence::keyBindings(QKeySequence::StandardKey(standard.keyToValue(key.captured(2).toLatin1())))
                    : QList<QKeySequence>{QKeySequence(key.captured(1))};
                for (const QKeySequence &sequence : sequences) {
                    QVERIFY2(!bound.contains(sequence),
                             qPrintable(sequence.toString() + " is bound by both " + bound.value(sequence) + " and " + line.trimmed()));
                    bound.insert(sequence, line.trimmed());
                    ++count;
                }
            }
        }
        QVERIFY(count > 30);
        QVERIFY(bound.contains(QKeySequence("Shift+Del")));
    }

    void fileOpsNeverOverwrite()
    {
        QTemporaryDir dir;
        write(dir.filePath("notes.tar.gz"), "x");
        QDir(dir.path()).mkdir("Photos");
        QCOMPARE(FileOps::freeName(dir.path(), "fresh.txt"), QString("fresh.txt"));
        QCOMPARE(FileOps::freeName(dir.path(), "notes.tar.gz"), QString("notes (copy).tar.gz"));
        write(dir.filePath("notes (copy).tar.gz"), "x");
        QCOMPARE(FileOps::freeName(dir.path(), "notes.tar.gz"), QString("notes (copy 2).tar.gz"));
        QCOMPARE(FileOps::freeName(dir.path(), "Photos"), QString("Photos (copy)"));
        // The extension starts at the last dot, so dots in the name stay.
        QCOMPARE(FileOps::extensionStart("Report v1.2.pdf", false), 11);
        write(dir.filePath("Report v1.2.pdf"), "x");
        QCOMPARE(FileOps::freeName(dir.path(), "Report v1.2.pdf"), QString("Report v1.2 (copy).pdf"));
        // A name with a placeholder of its own in it keeps it.
        write(dir.filePath("50%2 off.pdf"), "x");
        write(dir.filePath("50%2 off (copy).pdf"), "x");
        QCOMPARE(FileOps::freeName(dir.path(), "50%2 off.pdf"), QString("50%2 off (copy 2).pdf"));
    }

    void fileOpsRenameNeverOverwrites()
    {
        // The rename itself refuses to overwrite. Nothing checks for the new
        // name first, since on drives that ignore case it "exists" when only
        // its case changes; the tests' drive heeds case, though, so the case
        // change at the end is an ordinary rename here.
        QTemporaryDir dir;
        write(dir.filePath("IMG.JPG"), "photo");
        write(dir.filePath("notes.txt"), "notes");
        QDir(dir.path()).mkdir("Album");
        QDir(dir.path()).mkdir("Empty");
        FileOps ops;
        QCOMPARE(ops.rename(dir.filePath("IMG.JPG"), "notes.txt"), QString("notes.txt already exists here."));
        QCOMPARE(ops.rename(dir.filePath("Album"), "Empty"), QString("Empty already exists here."));
        QFile kept(dir.filePath("notes.txt"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("notes"));
        QVERIFY(QFileInfo(dir.filePath("Album")).isDir());
        QCOMPARE(ops.rename(dir.filePath("IMG.JPG"), "img.jpg"), QString());
        QVERIFY(QFile::exists(dir.filePath("img.jpg")));
    }

    void fileOpsCopyAndMove()
    {
        QTemporaryDir from, to;
        QDir(from.path()).mkpath("album/inner");
        write(from.filePath("album/inner/a.jpg"), "picture");
        write(from.filePath("todo.txt"), "list");
        write(to.filePath("todo.txt"), "already here");

        QString error;
        QStringList made = FileOps::transfer({from.filePath("album"), from.filePath("todo.txt")}, to.path(), FileOps::Copy, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(made, QStringList({"album", "todo (copy).txt"}));
        QVERIFY(QFile::exists(to.filePath("album/inner/a.jpg")));
        QFile kept(to.filePath("todo.txt"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("already here"));
        QVERIFY(QFile::exists(from.filePath("todo.txt"))); // copied, not moved

        made = FileOps::transfer({from.filePath("todo.txt")}, to.path(), FileOps::Move, &error);
        QCOMPARE(made, QStringList({"todo (copy 2).txt"}));
        QVERIFY(!QFile::exists(from.filePath("todo.txt")));

        // A folder can't go inside itself.
        error.clear();
        FileOps::transfer({to.filePath("album")}, to.filePath("album/inner"), FileOps::Move, &error);
        QVERIFY(!error.isEmpty());
        QVERIFY(QFile::exists(to.filePath("album/inner/a.jpg")));
        // Nor through a link to a folder inside it, which would copy it into
        // its own copy until the drive was full.
        QVERIFY(QFile::link(to.filePath("album/inner"), from.filePath("inner")));
        error.clear();
        FileOps::transfer({to.filePath("album")}, from.filePath("inner"), FileOps::Copy, &error);
        QCOMPARE(error, QString("Can't put album inside itself."));
        QVERIFY(!QFile::exists(to.filePath("album/inner/album")));
    }

    void fileOpsMoveAcrossDrivesLeavesWhatArrivesMeanwhile()
    {
        // Across drives, a move copies, then removes the original. What
        // turns up in a folder while the rest of it copies was never copied,
        // so it stays where it is.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir from;
        QDir(from.path()).mkdir("box");
        // Copying a pipe waits until whoever writes to it is done, which
        // holds the move after it has looked in the folder.
        const QByteArray pipe = QFile::encodeName(from.filePath("box/pipe"));
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        QString error;
        QFuture<QStringList> moved = QtConcurrent::run([&] {
            return FileOps::transfer({from.filePath("box")}, other->path(), FileOps::Move, &error);
        });
        int writer = -1;
        // Even when a check fails, the move is let go of and finishes
        // before what it uses is gone.
        const auto finish = qScopeGuard([&] {
            if (writer >= 0)
                close(writer);
            moved.waitForFinished();
        });
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        write(from.filePath("box/new.txt"), "new");
        close(std::exchange(writer, -1));
        QCOMPARE(moved.result(), QStringList({"box"}));
        QVERIFY(QFile::exists(from.filePath("box/new.txt")));
        QCOMPARE(error, QString("Copied box, but couldn't remove the original."));
        QVERIFY(!QFileInfo::exists(from.filePath("box/pipe")));
        QVERIFY(QFileInfo::exists(other->filePath("box/pipe")));

        // With nothing new, all of it goes.
        QDir(from.path()).mkpath("album/inner");
        write(from.filePath("album/inner/a.jpg"), "picture");
        error.clear();
        QCOMPARE(FileOps::transfer({from.filePath("album")}, other->path(), FileOps::Move, &error), QStringList({"album"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!QFileInfo::exists(from.filePath("album")));
        QVERIFY(QFile::exists(other->filePath("album/inner/a.jpg")));
    }

    void fileOpsSayWhenAMoveAcrossDrivesStopsPartway()
    {
        // Each original goes as it's copied, so a move that stops partway
        // (a full USB stick) has already taken some of the folder. What
        // arrived is shown and said to be only part, not left looking like
        // a failed copy that's safe to delete.
        if (geteuid() == 0)
            QSKIP("Root can read every file.");
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir from;
        QDir(from.path()).mkpath("box/a");
        QDir(from.path()).mkpath("box/z");
        write(from.filePath("box/a/1.txt"), "1");
        write(from.filePath("box/m.txt"), "m");
        write(from.filePath("box/z/2.txt"), "2");
        QVERIFY(QFile::setPermissions(from.filePath("box/m.txt"), QFile::WriteOwner));
        QString error;
        QCOMPARE(FileOps::transfer({from.filePath("box")}, other->path(), FileOps::Move, &error), QStringList({"box"}));
        QCOMPARE(error, QString("Moved only part of box: Couldn't copy m.txt."));
        QVERIFY(QFile::exists(other->filePath("box/a/1.txt")));
        QVERIFY(!QFile::exists(from.filePath("box/a/1.txt")));
        QVERIFY(QFile::exists(from.filePath("box/z/2.txt")));
    }

    void fileOpsCopyLinksAsWritten()
    {
        // A link relative to its folder points into the copy, not back at
        // the original.
        QTemporaryDir from, to;
        QDir(from.path()).mkdir("project");
        write(from.filePath("project/data.txt"), "data");
        QVERIFY(QFile::link("data.txt", from.filePath("project/link")));
        QString error;
        QCOMPARE(FileOps::transfer({from.filePath("project")}, to.path(), FileOps::Copy, &error), QStringList({"project"}));
        QCOMPARE(QFileInfo(to.filePath("project/link")).readSymLink(), QString("data.txt"));
    }

    void fileOpsMoveLinksAcrossDrivesAsLinks()
    {
        // A link moved or restored to another drive arrives as the link it
        // was, not as a copy of what it leads to. One dropped from another
        // drive is copied, wherever it leads.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir here, data, folder;
        write(here.filePath("data.txt"), "data");
        QVERIFY(QFile::link("data.txt", here.filePath("relative")));
        QVERIFY(QFile::link(here.filePath("data.txt"), here.filePath("absolute")));
        QString error;
        QCOMPARE(FileOps::transfer({here.filePath("relative"), here.filePath("absolute")}, other->path(), FileOps::Move, &error),
                 QStringList({"relative", "absolute"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QFileInfo(other->filePath("relative")).readSymLink(), QString("data.txt"));
        QCOMPARE(QFileInfo(other->filePath("absolute")).readSymLink(), here.filePath("data.txt"));
        QVERIFY(!QFileInfo(here.filePath("relative")).isSymLink());

        const auto restore = dataHomeAt(data.path());
        QDir().mkpath(data.filePath("Trash/files"));
        QDir().mkpath(data.filePath("Trash/info"));
        QVERIFY(QFile::link(here.filePath("data.txt"), data.filePath("Trash/files/trashed")));
        write(data.filePath("Trash/info/trashed.trashinfo"), "[Trash Info]\nPath=" + other->filePath("trashed").toUtf8() + "\n");
        QCOMPARE(FileOps::restoreOne(data.filePath("Trash/files/trashed"), &error), other->filePath("trashed"));
        QCOMPARE(QFileInfo(other->filePath("trashed")).readSymLink(), here.filePath("data.txt"));

        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        ops.dropInto({QUrl::fromLocalFile(other->filePath("absolute"))}, folder.path());
        QVERIFY(done.wait(3000));
        QCOMPARE(QFileInfo(folder.filePath("absolute")).readSymLink(), here.filePath("data.txt"));
        QVERIFY(QFileInfo(other->filePath("absolute")).isSymLink());
    }

    void fileOpsCopyFolderPermissions()
    {
        // A private folder stays private, and a read-only one is filled
        // before it's made read-only.
        QTemporaryDir from, to;
        QDir(from.path()).mkdir("private");
        QDir(from.path()).mkdir("locked");
        write(from.filePath("locked/a.txt"), "a");
        const auto all = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
        QVERIFY(QFile::setPermissions(from.filePath("private"), all));
        QVERIFY(QFile::setPermissions(from.filePath("locked"), QFile::ReadOwner | QFile::ExeOwner));
        const auto unlock = qScopeGuard([&] {
            QFile::setPermissions(from.filePath("locked"), all);
            QFile::setPermissions(to.filePath("locked"), all);
        });
        QString error;
        QCOMPARE(FileOps::transfer({from.filePath("private"), from.filePath("locked")}, to.path(), FileOps::Copy, &error),
                 QStringList({"private", "locked"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFile::exists(to.filePath("locked/a.txt")));
        QCOMPARE(QFileInfo(to.filePath("private")).permissions(), QFileInfo(from.filePath("private")).permissions());
        QCOMPARE(QFileInfo(to.filePath("locked")).permissions(), QFileInfo(from.filePath("locked")).permissions());
    }

    void fileOpsCopyOnlyWhatCanBeRead()
    {
        // A folder that can't be read would look empty, so copying it fails
        // rather than making an empty copy.
        if (geteuid() == 0)
            QSKIP("Root can read every folder.");
        QTemporaryDir from, to;
        QDir(from.path()).mkdir("secret");
        write(from.filePath("secret/a.txt"), "a");
        QVERIFY(QFile::setPermissions(from.filePath("secret"), QFile::WriteOwner | QFile::ExeOwner));
        const auto unlock = qScopeGuard([&] {
            QFile::setPermissions(from.filePath("secret"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        QString error;
        QVERIFY(FileOps::transfer({from.filePath("secret")}, to.path(), FileOps::Copy, &error).isEmpty());
        QCOMPARE(error, QString("Couldn't read secret."));
        QVERIFY(!QFile::exists(to.filePath("secret")));

        // A copy that stops at a file that can't be read is left as private
        // as the folder it came from.
        QDir(from.path()).mkpath("private/keys");
        write(from.filePath("private/z.txt"), "z");
        QVERIFY(QFile::setPermissions(from.filePath("private"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        QVERIFY(QFile::setPermissions(from.filePath("private/z.txt"), QFile::WriteOwner));
        QVERIFY(FileOps::transfer({from.filePath("private")}, to.path(), FileOps::Copy, &error).isEmpty());
        QCOMPARE(error, QString("Couldn't copy z.txt."));
        QCOMPARE(QFileInfo(to.filePath("private")).permissions(), QFileInfo(from.filePath("private")).permissions());
    }

    void fileOpsClipboardFormats()
    {
        std::unique_ptr<QMimeData> data(FileOps::mimeDataFor({"/tmp/a b.txt", "/tmp/c.txt"}, true));
        QCOMPARE(data->text(), QString("/tmp/a b.txt\n/tmp/c.txt"));
        QVERIFY(data->data("x-special/gnome-copied-files").startsWith("cut\nfile:///tmp/a%20b.txt"));
        bool cut = false;
        QCOMPARE(FileOps::pathsFrom(data.get(), &cut), QStringList({"/tmp/a b.txt", "/tmp/c.txt"}));
        QVERIFY(cut);

        // From another app that only offers a URI list: a copy.
        QMimeData plain;
        plain.setUrls({QUrl::fromLocalFile("/tmp/x.png")});
        QCOMPARE(FileOps::pathsFrom(&plain, &cut), QStringList({"/tmp/x.png"}));
        QVERIFY(!cut);
    }

    void fileOpsCompressAcrossFolders()
    {
        // Search results and Recents can hold items from different folders:
        // the archive goes in the folder holding them all, with their paths.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Compressing needs bsdtar.");
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("a/inner");
        QDir(dir.path()).mkpath("b");
        write(dir.filePath("a/inner/x.txt"), "x");
        write(dir.filePath("b/y.txt"), "y");
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        const auto compress = [&](const QStringList &names) {
            QStringList paths;
            for (const QString &name : names)
                paths << dir.filePath(name);
            ops.compress(paths);
            if (!done.wait(5000))
                return QStringList();
            const QList<QVariant> made = done.takeFirst();
            QProcess list;
            list.start("bsdtar", {"-tf", QDir(made[0].toString()).filePath(made[1].toString())});
            list.waitForFinished();
            return QStringList({made[0].toString(), made[1].toString()})
                 + QString::fromUtf8(list.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        };
        QCOMPARE(compress({"a/inner/x.txt", "b/y.txt"}), QStringList({dir.path(), "Archive.zip", "a/inner/x.txt", "b/y.txt"}));
        // A folder beside a file inside it (as search results list them):
        // the archive holds the folder, and the file once, within it.
        QCOMPARE(compress({"a/inner/x.txt", "a/inner"}), QStringList({dir.filePath("a"), "inner.zip", "inner/", "inner/x.txt"}));
    }

    void fileOpsCompressNeverOverwritesAnArchive()
    {
        // Two windows compressing into one folder at once each get an
        // archive of their own, and one that fails takes only its own with
        // it, leaving nothing half-written behind.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Compressing needs bsdtar.");
        QTemporaryDir dir;
        for (const char *name : {"a.txt", "b.txt", "c.txt"})
            write(dir.filePath(name), name);
        FileOps one, two, three;
        QSignalSpy doneOne(&one, &FileOps::done), doneTwo(&two, &FileOps::done), failed(&three, &FileOps::failed);
        one.compress({dir.filePath("a.txt"), dir.filePath("b.txt")});
        two.compress({dir.filePath("b.txt"), dir.filePath("c.txt")});
        three.compress({dir.filePath("a.txt"), dir.filePath("gone.txt")});
        QTRY_VERIFY(doneOne.count() == 1 && doneTwo.count() == 1 && failed.count() == 1);
        const auto members = [&](const QSignalSpy &done) {
            QProcess list;
            list.start("bsdtar", {"-tf", dir.filePath(done.first().at(1).toString())});
            list.waitForFinished();
            return QString::fromUtf8(list.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
        };
        QCOMPARE(members(doneOne), QStringList({"a.txt", "b.txt"}));
        QCOMPARE(members(doneTwo), QStringList({"b.txt", "c.txt"}));
        QStringList names{doneOne.first().at(1).toString(), doneTwo.first().at(1).toString()};
        names.sort();
        QCOMPARE(names, QStringList({"Archive (copy).zip", "Archive.zip"}));
        QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name),
                 QStringList({"Archive (copy).zip", "Archive.zip", "a.txt", "b.txt", "c.txt"}));
    }

    void fileOpsTakeAFolderOnceWithWhatsInIt()
    {
        // Search results can list a folder and what's in it. The folder
        // takes those along, so they aren't copied twice, found gone once
        // it has moved, deleted twice, or restored without a note of their
        // own.
        QTemporaryDir from, copies, moves, trash;
        QDir(from.path()).mkpath("album/inner");
        write(from.filePath("album/inner/a.jpg"), "picture");
        const QStringList both{from.filePath("album"), from.filePath("album/inner/a.jpg")};
        QString error;
        QCOMPARE(FileOps::transfer(both, copies.path(), FileOps::Copy, &error), QStringList({"album"}));
        QVERIFY(!QFile::exists(copies.filePath("a.jpg")));
        QCOMPARE(FileOps::transfer(both, moves.path(), FileOps::Move, &error), QStringList({"album"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFile::exists(moves.filePath("album/inner/a.jpg")));

        QDir(trash.path()).mkpath("files/album");
        write(trash.filePath("files/album/a.jpg"), "picture");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.deleteForever({trash.filePath("files/album"), trash.filePath("files/album/a.jpg")});
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QVERIFY(!QFileInfo::exists(trash.filePath("files/album")));

        const auto restore = dataHomeAt(trash.path());
        QDir(trash.path()).mkpath("Trash/files/album");
        QDir(trash.path()).mkpath("Trash/info");
        write(trash.filePath("Trash/files/album/a.jpg"), "picture");
        write(trash.filePath("Trash/info/album.trashinfo"), "[Trash Info]\nPath=" + copies.filePath("restored").toUtf8() + "\n");
        ops.restore({trash.filePath("Trash/files/album"), trash.filePath("Trash/files/album/a.jpg")});
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QVERIFY(QFile::exists(copies.filePath("restored/a.jpg")));
    }

    void fileOpsKeepWhatsChosenBeyondALink()
    {
        // A link is copied as a link, not gone into, so something chosen
        // through it (Recents lists both) is still copied in its own right.
        QTemporaryDir from, to;
        QDir(from.path()).mkpath("real/inner");
        write(from.filePath("real/inner/a.txt"), "a");
        QVERIFY(QFile::link("real", from.filePath("link")));
        QDir(from.path()).mkdir("box");
        QVERIFY(QFile::link("../real", from.filePath("box/through")));
        QString error;
        const QStringList chosen{from.filePath("link"), from.filePath("link/inner/a.txt"),
                                 from.filePath("box"), from.filePath("box/through/inner/a.txt")};
        QCOMPARE(FileOps::transfer(chosen, to.path(), FileOps::Copy, &error),
                 QStringList({"a.txt", "a (copy).txt", "link", "box"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo(to.filePath("link")).isSymLink());
        QVERIFY(QFileInfo(to.filePath("box/through")).isSymLink());

        // Moved, what's beyond the link goes before the link does, while
        // it can still be reached through it.
        QTemporaryDir moves;
        QCOMPARE(FileOps::transfer({from.filePath("link"), from.filePath("link/inner/a.txt")}, moves.path(), FileOps::Move, &error),
                 QStringList({"a.txt", "link"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(QFile::exists(moves.filePath("a.txt")));
        QVERIFY(QFileInfo(moves.filePath("link")).isSymLink());
    }

    void fileOpsRestoreAcrossDrivesLeavesWhatArrivesMeanwhile()
    {
        // Restoring across drives moves the way a move across drives does:
        // what turns up in the trashed folder meanwhile stays in the trash,
        // with the note that can bring it back.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir home;
        const auto restore = dataHomeAt(home.path());
        const QString trash = home.filePath("Trash");
        QDir().mkpath(trash + "/files/box");
        QDir().mkpath(trash + "/info");
        write(trash + "/info/box.trashinfo", "[Trash Info]\nPath=" + other->filePath("box").toUtf8() + "\n");
        const QByteArray pipe = QFile::encodeName(trash + "/files/box/pipe");
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        QString error;
        QFuture<QString> restored = QtConcurrent::run([&] { return FileOps::restoreOne(trash + "/files/box", &error); });
        int writer = -1;
        const auto finish = qScopeGuard([&] {
            if (writer >= 0)
                close(writer);
            restored.waitForFinished();
        });
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        write(trash + "/files/box/new.txt", "new");
        close(std::exchange(writer, -1));
        QCOMPARE(restored.result(), QString());
        QCOMPARE(error, QString("Restored box, but couldn't remove it from the trash."));
        QVERIFY(QFile::exists(trash + "/files/box/new.txt"));
        QVERIFY(QFile::exists(trash + "/info/box.trashinfo"));
        QVERIFY(QFileInfo::exists(other->filePath("box/pipe")));

        // With nothing new, all of it goes, and its note with it.
        QDir().mkpath(trash + "/files/album/inner");
        write(trash + "/files/album/inner/a.jpg", "picture");
        write(trash + "/info/album.trashinfo", "[Trash Info]\nPath=" + other->filePath("album").toUtf8() + "\n");
        error.clear();
        QCOMPARE(FileOps::restoreOne(trash + "/files/album", &error), other->filePath("album"));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!QFileInfo::exists(trash + "/files/album"));
        QVERIFY(!QFile::exists(trash + "/info/album.trashinfo"));
        QVERIFY(QFile::exists(other->filePath("album/inner/a.jpg")));

        // One that can't be copied at all keeps its note.
        if (geteuid() != 0) {
            write(trash + "/files/locked.txt", "x");
            write(trash + "/info/locked.txt.trashinfo", "[Trash Info]\nPath=" + other->filePath("locked.txt").toUtf8() + "\n");
            QVERIFY(QFile::setPermissions(trash + "/files/locked.txt", QFile::WriteOwner));
            error.clear();
            QCOMPARE(FileOps::restoreOne(trash + "/files/locked.txt", &error), QString());
            QCOMPARE(error, QString("Couldn't copy locked.txt."));
            QVERIFY(QFile::exists(trash + "/info/locked.txt.trashinfo"));

            // One that stops partway says where what arrived went, and keeps
            // its note for the rest.
            QDir().mkpath(trash + "/files/part/a");
            write(trash + "/files/part/a/1.txt", "1");
            write(trash + "/files/part/m.txt", "m");
            QVERIFY(QFile::setPermissions(trash + "/files/part/m.txt", QFile::WriteOwner));
            write(trash + "/info/part.trashinfo", "[Trash Info]\nPath=" + other->filePath("part").toUtf8() + "\n");
            error.clear();
            QCOMPARE(FileOps::restoreOne(trash + "/files/part", &error), QString());
            QCOMPARE(error, QString("Restored only part of part, to %1: Couldn't copy m.txt.").arg(other->filePath("part")));
            QVERIFY(QFile::exists(other->filePath("part/a/1.txt")));
            QVERIFY(QFile::exists(trash + "/info/part.trashinfo"));
        }
    }

    void fileOpsWeedOutWhatsInsideQuickly()
    {
        // Select All in a big folder hands over every item. Comparing each
        // with every other to find what's inside another took most of a
        // minute for 20,000 photos before the first was touched.
        QTemporaryDir dir;
        QStringList paths{dir.filePath("gone")};
        for (int i = 0; i < 20000; ++i)
            paths << dir.filePath(QString("photos/%1.jpg").arg(i));
        QElapsedTimer timer;
        timer.start();
        // The first is missing, so it stops there, once they're weeded out.
        QString error;
        FileOps::transfer(paths, dir.path(), FileOps::Copy, &error);
        QCOMPARE(error, QString("gone is no longer there."));
        QVERIFY2(timer.elapsed() < 1000, qPrintable(QString("Took %1 ms.").arg(timer.elapsed())));
    }

    void fileOpsLetGoOfOnlyThePastedCut()
    {
        // Cut files are gone once pasted, so the clipboard lets go of them,
        // but not of something copied while they moved.
        QTemporaryDir from, to;
        write(from.filePath("a.txt"), "a");
        write(from.filePath("b.txt"), "b");
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        ops.cut({from.filePath("a.txt")});
        ops.paste(to.path());
        ops.copy({from.filePath("b.txt")});
        QVERIFY(done.wait(3000));
        bool cut = true;
        QCOMPARE(FileOps::pathsFrom(QGuiApplication::clipboard()->mimeData(), &cut), QStringList({from.filePath("b.txt")}));
        QVERIFY(!cut);

        ops.cut({from.filePath("b.txt")});
        ops.paste(to.path());
        QVERIFY(done.wait(3000));
        QVERIFY(!ops.canPaste());
    }

    void fileOpsDropMovesOnlyWhatsOnTheSameDrive()
    {
        // Each dropped file is moved from the folder's drive and copied from
        // any other, whichever comes first.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir here, folder;
        write(here.filePath("a.txt"), "a");
        write(other->filePath("b.txt"), "b");
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        ops.dropInto({QUrl::fromLocalFile(here.filePath("a.txt")), QUrl::fromLocalFile(other->filePath("b.txt"))},
                     folder.path());
        QVERIFY(done.wait(3000));
        QVERIFY(QFile::exists(folder.filePath("a.txt")));
        QVERIFY(QFile::exists(folder.filePath("b.txt")));
        QVERIFY(!QFile::exists(here.filePath("a.txt")));
        QVERIFY(QFile::exists(other->filePath("b.txt")));
    }

    void fileOpsDropNothingWhereItAlreadyIs()
    {
        // Dropping a folder onto itself, or into the folder it's in, even by
        // a path through a link to that folder, moves nothing and makes no
        // "(copy)".
        QTemporaryDir home;
        QDir(home.path()).mkpath("project/install");
        QVERIFY(QFile::link(home.filePath("project"), home.filePath("shortcut")));
        const QList<QUrl> install{QUrl::fromLocalFile(home.filePath("project/install"))};
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        for (const QString &folder : {"project/install", "project", "shortcut"}) {
            QVERIFY2(!ops.canDrop(install, home.filePath(folder)), qPrintable(folder));
            ops.dropInto(install, home.filePath(folder));
        }
        QString error;
        QVERIFY(FileOps::transfer({home.filePath("project/install")}, home.filePath("shortcut"), FileOps::Move, &error).isEmpty());
        QVERIFY(error.isEmpty());
        QTest::qWait(200);
        QCOMPARE(done.count(), 0);
        QCOMPARE(QDir(home.filePath("project")).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList({"install"}));
    }

    void folderDropRefusesWhatAFolderOverItRefuses()
    {
        // The view's backdrop takes drops on empty space into its folder, ~,
        // while a folder item over it takes drops on itself. A folder item
        // that refuses one (here ~/project, which install is already in)
        // mustn't let it fall through to ~, which moved install up there.
        QTemporaryDir home;
        QDir(home.path()).mkpath("project/install");
        FileOps ops;
        QQmlPropertyMap theme;
        theme.insert("accent", QColor(Qt::blue));
        theme.insert("radius", 0);
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("fileOps", &ops);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("home", home.path());
        QQmlComponent component(&engine);
        component.setData("import QtQuick\n"
                          "Window {\n"
                          "    width: 200; height: 200; visible: true\n"
                          "    FolderDrop { anchors.fill: parent; z: -1; folder: home; items: list }\n"
                          "    ListView {\n"
                          "        id: list; anchors.fill: parent; model: 1\n"
                          "        delegate: DropArea {\n"
                          "            readonly property bool isDir: true\n"
                          "            width: 200; height: 40\n"
                          "            onEntered: (drag) => { drag.accepted = fileOps.canDrop(drag.urls, home + '/project') }\n"
                          "        }\n"
                          "    }\n"
                          "}\n",
                          QUrl::fromLocalFile(QFINDTESTDATA("../src/FolderDrop.qml") + ".test.qml"));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        QVERIFY(QTest::qWaitForWindowExposed(window));

        QMimeData data;
        data.setUrls({QUrl::fromLocalFile(home.filePath("project/install"))});
        const auto dropAt = [&](QPointF at) {
            QDragEnterEvent enter(at.toPoint(), Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
            QGuiApplication::sendEvent(window, &enter);
            QDropEvent drop(at, Qt::MoveAction, &data, Qt::LeftButton, Qt::NoModifier);
            QGuiApplication::sendEvent(window, &drop);
            return drop.isAccepted();
        };
        QSignalSpy done(&ops, &FileOps::done);
        QVERIFY(!dropAt({100, 20}));
        QTest::qWait(200);
        QCOMPARE(done.count(), 0);
        QVERIFY(QFileInfo(home.filePath("project/install")).isDir());

        // On empty space, it lands in ~.
        QVERIFY(dropAt({100, 150}));
        QVERIFY(done.wait(3000));
        QVERIFY(QFileInfo(home.filePath("install")).isDir());
    }

    void fileOpsDragUriLists()
    {
        // Names holding what means something in a URI ("#", "?", "%") are
        // dragged as the same paths they came from.
        FileOps ops;
        const QStringList paths{"/tmp/Track #1.mp3", "/tmp/what?.txt", "/tmp/100% done/caf\u00e9.txt"};
        QMimeData data;
        data.setData("text/uri-list", ops.uriList(paths).toUtf8());
        QCOMPARE(ops.localPaths(data.urls()), paths);
        // Only local files are dropped as paths.
        QCOMPARE(ops.localPaths({QUrl("https://example.com/a"), QUrl::fromLocalFile("/tmp/x")}), QStringList({"/tmp/x"}));
    }

    void fileOpsGuardDropsRenamesAndNames()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("album/inner");
        write(dir.filePath("a.txt"), "a");
        write(dir.filePath("b.txt"), "b");
        write(dir.filePath("c.txt"), "c");
        const auto urls = [&](const QString &name) { return QList<QUrl>{QUrl::fromLocalFile(dir.filePath(name))}; };
        FileOps ops;

        // A drop has to take something somewhere new: not where it already
        // is, not onto or into itself, and not onto a file.
        QVERIFY(ops.canDrop(urls("a.txt"), dir.filePath("album")));
        QVERIFY(!ops.canDrop(urls("a.txt"), dir.path()));
        QVERIFY(!ops.canDrop(urls("album"), dir.filePath("album")));
        QVERIFY(!ops.canDrop(urls("album"), dir.filePath("album/inner")));
        QVERIFY(!ops.canDrop(urls("album"), dir.filePath("a.txt")));
        QVERIFY(QFile::link(dir.filePath("album/inner"), dir.filePath("shortcut")));
        QVERIFY(!ops.canDrop(urls("album"), dir.filePath("shortcut")));
        // A link is moved as a link, so it can go into the folder it leads to.
        QVERIFY(ops.canDrop(urls("shortcut"), dir.filePath("album/inner")));
        // Moving something into the folder it's in leaves it alone rather
        // than making a "(copy)" of it.
        QString error;
        QVERIFY(FileOps::transfer({dir.filePath("a.txt")}, dir.path(), FileOps::Move, &error).isEmpty());
        QVERIFY(error.isEmpty());
        QVERIFY(QFile::exists(dir.filePath("a.txt")));

        // A slash would move the file elsewhere, so it's refused; the same
        // name changes nothing, and spaces around a name are dropped.
        QVERIFY(!ops.rename(dir.filePath("a.txt"), "x/y").isEmpty());
        QCOMPARE(ops.rename(dir.filePath("a.txt"), "a.txt"), QString());
        QCOMPARE(ops.rename(dir.filePath("c.txt"), " d.txt "), QString());
        QVERIFY(QFile::exists(dir.filePath("d.txt")));

        QCOMPARE(FileOps::archiveName({dir.filePath("d.txt")}, dir.path()), QString("d.txt.zip"));
        QCOMPARE(FileOps::archiveName({dir.filePath("a.txt"), dir.filePath("b.txt")}, dir.path()), QString("Archive.zip"));

        QSignalSpy done(&ops, &FileOps::done);
        ops.newFolder(dir.path());
        ops.newFolder(dir.path());
        QCOMPARE(done.count(), 2);
        QCOMPARE(done.takeFirst().at(1).toString(), QString("Untitled Folder"));
        QCOMPARE(done.takeFirst().at(1).toString(), QString("Untitled Folder 2"));
    }

    void recentsReadGtkRecentFiles()
    {
        QTemporaryDir dir;
        write(dir.filePath("old.txt"), "x");
        write(dir.filePath("new.pdf"), "x");
        write(dir.filePath(".secret"), "x");
        const QByteArray url = QUrl::fromLocalFile(dir.path()).toEncoded();
        write(dir.filePath("recently-used.xbel"),
              "<?xml version=\"1.0\"?>\n<xbel version=\"1.0\">\n"
              "<bookmark href=\"" + url + "/old.txt\" modified=\"2026-01-01T10:00:00Z\" visited=\"2026-01-02T10:00:00Z\"/>\n"
              "<bookmark href=\"" + url + "/gone.txt\" modified=\"2026-05-01T10:00:00Z\"/>\n"
              "<bookmark href=\"" + url + "/.secret\" modified=\"2026-05-01T10:00:00Z\"/>\n"
              "<bookmark href=\"https://example.com/\" modified=\"2026-05-01T10:00:00Z\"/>\n"
              "<bookmark href=\"" + url + "/new.pdf\" modified=\"2026-03-01T10:00:00Z\" visited=\"2026-03-01T10:00:00Z\"/>\n"
              "</xbel>\n");
        const auto entries = FolderModel::listRecent(dir.filePath("recently-used.xbel"), false);
        // Missing, hidden and web entries are left out; newest use first.
        QCOMPARE(entries.size(), 2);
        QCOMPARE(entries.at(0).name, QString("new.pdf"));
        QCOMPARE(entries.at(1).name, QString("old.txt"));
        QCOMPARE(entries.at(0).kind, QString("pdf"));
        // Left before it's done, it stops checking what's on the list.
        int checks = 0;
        QVERIFY(FolderModel::listRecent(dir.filePath("recently-used.xbel"), false, 200, [&] { return ++checks > 3; }).size() == 1);
    }

    void folderSortsFiveWays()
    {
        QVector<FolderModel::Entry> e(4);
        e[0].name = "b.txt"; e[0].kind = "text"; e[0].extension = "TXT"; e[0].size = 10; e[0].modified = QDateTime::fromSecsSinceEpoch(300);
        e[1].name = "a.png"; e[1].kind = "image"; e[1].extension = "PNG"; e[1].size = 500; e[1].modified = QDateTime::fromSecsSinceEpoch(100);
        e[2].name = "c.txt"; e[2].kind = "text"; e[2].extension = "TXT"; e[2].size = 99; e[2].modified = QDateTime::fromSecsSinceEpoch(200);
        e[3].name = "Zeta"; e[3].isDir = true; e[3].modified = QDateTime::fromSecsSinceEpoch(50);
        auto order = [&](const QString &sort) {
            auto copy = e;
            FolderModel::sortEntries(copy, sort);
            QStringList names;
            for (const auto &x : copy) names << x.name;
            return names.join(' ');
        };
        QCOMPARE(order("name"), QString("Zeta a.png b.txt c.txt"));
        QCOMPARE(order("kind"), QString("Zeta a.png b.txt c.txt"));
        QCOMPARE(order("modified"), QString("Zeta b.txt c.txt a.png"));
        QCOMPARE(order("oldest"), QString("Zeta a.png c.txt b.txt"));
        QCOMPARE(order("size"), QString("Zeta a.png c.txt b.txt"));
    }

    void folderKeepsASelection()
    {
        QTemporaryDir dir;
        for (const char *name : {"a.txt", "b.txt", "c.txt", "d.txt"})
            write(dir.filePath(name), "x");
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(dir.path());
        QVERIFY(loaded.wait(3000));

        model.selectOnly(1);
        QCOMPARE(model.selectedCount(), 1);
        model.selectRange(1, 3, false);
        QCOMPARE(model.selectedPaths(), QStringList({dir.filePath("b.txt"), dir.filePath("c.txt"), dir.filePath("d.txt")}));
        model.toggleSelected(2);
        QCOMPARE(model.selectedCount(), 2);
        QVERIFY(!model.isSelected(2));
        model.selectRange(0, 0, true);
        QCOMPARE(model.selectedCount(), 3);

        // A file that goes away leaves the selection; the rest stay.
        QFile::remove(dir.filePath("b.txt"));
        model.reload();
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.selectedPaths(), QStringList({dir.filePath("a.txt"), dir.filePath("d.txt")}));

        model.selectAll();
        QCOMPARE(model.selectedCount(), 3);
        model.clearSelection();
        QCOMPARE(model.selectedCount(), 0);
    }

    void recentsSelectOneOfTwoSameNamedFiles()
    {
        // Recents can hold two files of the same name from different
        // folders; selecting one leaves the other alone.
        QTemporaryDir data;
        QDir(data.path()).mkpath("a");
        QDir(data.path()).mkpath("b");
        write(data.filePath("a/README.md"), "x");
        write(data.filePath("b/README.md"), "x");
        const QByteArray url = QUrl::fromLocalFile(data.path()).toEncoded();
        write(data.filePath("recently-used.xbel"),
              "<?xml version=\"1.0\"?>\n<xbel version=\"1.0\">\n"
              "<bookmark href=\"" + url + "/a/README.md\" modified=\"2026-01-01T10:00:00Z\"/>\n"
              "<bookmark href=\"" + url + "/b/README.md\" modified=\"2026-02-01T10:00:00Z\"/>\n"
              "</xbel>\n");
        const auto restore = dataHomeAt(data.path());

        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setPath(FolderModel::recentPath());
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 2);
        model.selectOnly(0);
        QCOMPARE(model.selectedCount(), 1);
        QVERIFY(!model.isSelected(1));
        QCOMPARE(model.selectedPaths(), QStringList({data.filePath("b/README.md")}));
        // The cursor comes back by path, which tells them apart too.
        QCOMPARE(model.indexOfPath(data.filePath("a/README.md")), 1);
        QCOMPARE(model.indexOfPath(data.filePath("b/README.md")), 0);
    }

    void trashRestoresAndEmpties()
    {
        QTemporaryDir home;
        const auto restore = dataHomeAt(home.path());
        const QString trash = home.filePath("Trash");
        QDir().mkpath(trash + "/files");
        QDir().mkpath(trash + "/info");
        QDir().mkpath(home.filePath("Docs"));
        auto trashed = [&](const QString &name, const QString &from) {
            write(trash + "/files/" + name, "x");
            write(trash + "/info/" + name + ".trashinfo",
                  "[Trash Info]\nPath=" + QUrl::toPercentEncoding(from, "/") + "\nDeletionDate=2026-11-10T12:00:00\n");
        };
        trashed("notes 1.txt", home.filePath("Docs/notes 1.txt"));
        trashed("gone.txt", home.filePath("Gone/Deep/gone.txt"));
        trashed("clash.txt", home.filePath("Docs/clash.txt"));
        write(home.filePath("Docs/clash.txt"), "newer");

        QCOMPARE(FileOps::originalPath(trash + "/files/notes 1.txt"), home.filePath("Docs/notes 1.txt"));
        QCOMPARE(FileOps::deletedAt(trash + "/files/notes 1.txt"), QDateTime(QDate(2026, 11, 10), QTime(12, 0)));

        QString error;
        QCOMPARE(FileOps::restoreOne(trash + "/files/notes 1.txt", &error), home.filePath("Docs/notes 1.txt"));
        QVERIFY(!QFile::exists(trash + "/info/notes 1.txt.trashinfo"));
        // A folder that's gone is made again; a name that's taken isn't overwritten.
        QCOMPARE(FileOps::restoreOne(trash + "/files/gone.txt", &error), home.filePath("Gone/Deep/gone.txt"));
        QCOMPARE(FileOps::restoreOne(trash + "/files/clash.txt", &error), home.filePath("Docs/clash (copy).txt"));
        QFile kept(home.filePath("Docs/clash.txt"));
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("newer"));

        trashed("a.txt", home.filePath("a.txt"));
        QDir().mkpath(trash + "/files/folder/inner");
        write(trash + "/info/folder.trashinfo", "[Trash Info]\nPath=" + home.filePath("folder").toUtf8() + "\n");
        // Something being trashed gets its note first, then its file, so a
        // note without a file may be for something arriving.
        write(trash + "/info/arriving.txt.trashinfo", "[Trash Info]\nPath=" + home.filePath("arriving.txt").toUtf8() + "\n");
        QCOMPARE(FileOps::emptyTrashDir(trash), 2);
        QVERIFY(QDir(trash + "/files").isEmpty());
        QCOMPARE(QDir(trash + "/info").entryList(QDir::Files), QStringList({"arriving.txt.trashinfo"}));
    }

    void trashKeepsTheNoteOfWhatStays()
    {
        // A note goes only once its item is gone, so what couldn't be
        // deleted, or removed from the trash once restored to another drive,
        // can still be restored.
        QTemporaryDir data;
        const auto restore = dataHomeAt(data.path());
        const QString trash = data.filePath("Trash");
        QDir().mkpath(trash + "/files/box");
        QDir().mkpath(trash + "/info");
        write(trash + "/files/box/x.txt", "x");
        const auto other = elsewhere();
        const QString original = other ? other->filePath("box") : data.filePath("box");
        write(trash + "/info/box.trashinfo", "[Trash Info]\nPath=" + QUrl::toPercentEncoding(original, "/") + "\n");
        // Nothing can be taken out of a folder that can't be written to.
        const auto locked = QFile::ReadOwner | QFile::ExeOwner;
        QVERIFY(QFile::setPermissions(trash + "/files/box", locked));
        const auto unlock = qScopeGuard([&] {
            for (const QString &box : {trash + "/files/box", original})
                QFile::setPermissions(box, locked | QFile::WriteOwner);
        });
        QVERIFY(!FileOps::deleteTrashed(trash + "/files/box"));
        QVERIFY(QFile::exists(trash + "/info/box.trashinfo"));

        if (!other)
            QSKIP("Needs a second drive.");
        QString error;
        QCOMPARE(FileOps::restoreOne(trash + "/files/box", &error), QString());
        QCOMPARE(error, QString("Restored box, but couldn't remove it from the trash."));
        QVERIFY(QFile::exists(original + "/x.txt"));
        QVERIFY(QFile::exists(trash + "/info/box.trashinfo"));
    }

    void trashFindsNotesOnlyForWhatWasTrashed()
    {
        // Only what's directly in Trash/files has a note in Trash/info.
        // Something inside a trashed folder came along with it and has none;
        // the same name in Trash/files/info is a file in a trashed folder
        // called "info".
        QTemporaryDir data;
        const auto restore = dataHomeAt(data.path());
        const QString trash = data.filePath("Trash");
        QDir().mkpath(trash + "/files/photos");
        QDir().mkpath(trash + "/files/info");
        write(trash + "/files/photos/a", "a");
        write(trash + "/files/info/a.trashinfo", "[Trash Info]\nPath=" + data.filePath("elsewhere/a").toUtf8() + "\n");

        QCOMPARE(FileOps::originalPath(trash + "/files/photos/a"), QString());
        QString error;
        QCOMPARE(FileOps::restoreOne(trash + "/files/photos/a", &error), QString());
        QCOMPARE(error, QString("a has no record of where it came from."));
        QVERIFY(QFile::exists(trash + "/files/photos/a"));
        QVERIFY(FileOps::deleteTrashed(trash + "/files/photos/a"));
        QVERIFY(QFile::exists(trash + "/files/info/a.trashinfo"));
    }

    void trashAndRestoreRunInTheBackground()
    {
        // Trashing asks GIO about each item, and restoring can copy a whole
        // folder from another drive, so neither holds up the window: each
        // shows that it's busy, and says how it went once it's done.
        QTemporaryDir data;
        const auto restore = dataHomeAt(data.path());
        const QString trash = data.filePath("Trash");
        QDir().mkpath(trash + "/files");
        QDir().mkpath(trash + "/info");
        write(trash + "/files/a.txt", "a");
        write(trash + "/info/a.txt.trashinfo", "[Trash Info]\nPath=" + data.filePath("a.txt").toUtf8() + "\n");
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice), failed(&ops, &FileOps::failed);
        ops.restore({trash + "/files/a.txt"});
        QVERIFY(!ops.busy().isEmpty());
        QCOMPARE(notice.count(), 0);
        QTRY_COMPARE(notice.count(), 1);
        QVERIFY(ops.busy().isEmpty());
        QCOMPARE(notice.first().first().toString(), QString("Restored a.txt to %1.").arg(data.path()));
        QVERIFY(QFile::exists(data.filePath("a.txt")));

        // What's no longer there fails before GIO goes near a trash.
        ops.trash({data.filePath("gone.txt")});
        QVERIFY(!ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(ops.busy().isEmpty());
        QVERIFY(failed.first().first().toString().startsWith("Couldn't move to the trash: gone.txt ("));

        // One piece of work runs at a time. What's asked for meanwhile
        // doesn't start, and says so rather than doing nothing.
        ops.trash({data.filePath("gone.txt")});
        ops.restore({trash + "/files/a.txt"});
        QCOMPARE(failed.count(), 2);
        QCOMPARE(failed.last().first().toString(), QString("Try again once the work under way is done."));
        QTRY_COMPARE(failed.count(), 3);
    }

    void trashTakesNothingButDeletes()
    {
        // A paste or a drop into the trash would leave items with no note
        // saying where they came from.
        QTemporaryDir data, from;
        const auto restore = dataHomeAt(data.path());
        const QString files = data.filePath("Trash/files");
        QDir().mkpath(files);
        write(from.filePath("a.txt"), "a");
        FileOps ops;
        QVERIFY(ops.inTrash(files));
        QVERIFY(!ops.canDrop({QUrl::fromLocalFile(from.filePath("a.txt"))}, files));
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.cut({from.filePath("a.txt")});
        ops.paste(files);
        QCOMPARE(failed.count(), 1);
        QVERIFY(QDir(files).isEmpty());
    }

    void fileOpsCancelStopsACopyPartway()
    {
        // Cancel stops at the next file, or the next piece of a big one: a
        // file copied partway is removed, and what was copied before stays.
        // Copying a pipe reads it as its writer writes, so the copy is
        // partway for as long as the test likes.
        QTemporaryDir from, to;
        write(from.filePath("a.txt"), "a");
        const QByteArray pipe = QFile::encodeName(from.filePath("pipe"));
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        FileOps::Job job;
        QString error;
        QFuture<QStringList> copied = QtConcurrent::run([&] {
            return FileOps::transfer({from.filePath("a.txt"), from.filePath("pipe")}, to.path(), FileOps::Copy, &error, {}, &job);
        });
        int writer = -1;
        const auto finish = qScopeGuard([&] {
            if (writer >= 0)
                close(writer);
            copied.waitForFinished();
        });
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        QCOMPARE(::write(writer, "partial", 7), 7);
        QTRY_VERIFY(job.bytes >= 8);
        job.cancelled = true;
        QCOMPARE(::write(writer, "more", 4), 4);
        close(std::exchange(writer, -1));
        QCOMPARE(copied.result(), QStringList({"a.txt"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(job.done.load(), 1);
        QCOMPARE(job.total.load(), 2);
        QVERIFY(QFile::exists(to.filePath("a.txt")));
        QVERIFY(!QFileInfo::exists(to.filePath("pipe")));

        // From the window, Cancel says how far it got.
        QTemporaryDir again;
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice);
        ops.copy({from.filePath("a.txt"), from.filePath("pipe")});
        ops.paste(again.path());
        QVERIFY(!ops.busy().isEmpty());
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        ops.cancel();
        QCOMPARE(::write(writer, "x", 1), 1);
        close(std::exchange(writer, -1));
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(notice.count(), 1);
        QCOMPARE(notice.first().first().toString(), QString("Stopped after 1 of 2 items."));
        QVERIFY(!QFileInfo::exists(again.filePath("pipe")));
    }

    void fileOpsCancelledMoveAcrossDrivesKeepsOriginals()
    {
        // Across drives each original goes once it's copied, so a move
        // cancelled partway removes only what fully arrived: the file it
        // was on stays, and so does what it never reached.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir from;
        QDir(from.path()).mkdir("box");
        write(from.filePath("box/a.txt"), "a");
        write(from.filePath("box/z.txt"), "z");
        const QByteArray pipe = QFile::encodeName(from.filePath("box/pipe"));
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        FileOps::Job job;
        QString error;
        QFuture<QStringList> moved = QtConcurrent::run([&] {
            return FileOps::transfer({from.filePath("box")}, other->path(), FileOps::Move, &error, {}, &job);
        });
        int writer = -1;
        const auto finish = qScopeGuard([&] {
            if (writer >= 0)
                close(writer);
            moved.waitForFinished();
        });
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        QCOMPARE(::write(writer, "partial", 7), 7);
        QTRY_VERIFY(job.bytes >= 8);
        job.cancelled = true;
        QCOMPARE(::write(writer, "more", 4), 4);
        close(std::exchange(writer, -1));
        // What arrived is the only copy of a.txt now, so it's shown.
        QCOMPARE(moved.result(), QStringList({"box"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        // A single folder counts what's in it.
        QCOMPARE(job.total.load(), 3);
        QCOMPARE(job.done.load(), 1);
        QVERIFY(QFile::exists(other->filePath("box/a.txt")));
        QVERIFY(!QFile::exists(from.filePath("box/a.txt")));
        QVERIFY(QFileInfo::exists(from.filePath("box/pipe")));
        QVERIFY(!QFileInfo::exists(other->filePath("box/pipe")));
        QVERIFY(QFile::exists(from.filePath("box/z.txt")));
        QVERIFY(!QFile::exists(other->filePath("box/z.txt")));
    }

    void fileOpsAskWhichClashes()
    {
        // Only a name that's taken by something else clashes: not a copy
        // onto itself (kept beside it, as always), nor a move into the
        // folder it's in.
        QTemporaryDir from, to;
        write(from.filePath("a.txt"), "a");
        write(from.filePath("b.txt"), "b");
        write(to.filePath("a.txt"), "old");
        QCOMPARE(FileOps::clashes({from.filePath("a.txt"), from.filePath("b.txt")}, to.path()),
                 QStringList({from.filePath("a.txt")}));
        QCOMPARE(FileOps::clashes({from.filePath("a.txt")}, from.path()), QStringList());
    }

    void fileOpsCopyOntoItselfKeepsBoth()
    {
        // Copied into its own folder, an item is kept beside itself, never
        // asked about, and never replaced by itself, also when the folder
        // is reached through a link to it, where replacing would send the
        // original to the trash before copying it.
        QTemporaryDir data, dir, links;
        const auto restore = dataHomeAt(data.path());
        write(dir.filePath("a.txt"), "a");
        QVERIFY(QFile::link(dir.path(), links.filePath("same")));
        const QString same = links.filePath("same");
        QCOMPARE(FileOps::clashes({dir.filePath("a.txt")}, same), QStringList());
        QString error;
        QCOMPARE(FileOps::transfer({dir.filePath("a.txt")}, dir.path(), FileOps::Copy, &error),
                 QStringList({"a (copy).txt"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(FileOps::transfer({dir.filePath("a.txt")}, same, FileOps::Copy, &error,
                                   {{dir.filePath("a.txt"), FileOps::Replace}}),
                 QStringList({"a (copy 2).txt"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        // Moved into where it already is, it stays.
        FileOps::transfer({dir.filePath("a.txt")}, same, FileOps::Move, &error, {{dir.filePath("a.txt"), FileOps::Replace}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(contents(dir.filePath("a.txt")), QByteArray("a"));
        QVERIFY(!QFileInfo::exists(data.filePath("Trash/files/a.txt")));
    }

    void fileOpsReplaceSkipOrKeepBoth()
    {
        QTemporaryDir data, from, to;
        const auto restore = dataHomeAt(data.path());
        for (const char *name : {"replace.txt", "skip.txt", "both.txt"}) {
            write(from.filePath(name), "new");
            write(to.filePath(name), "old");
        }
        const QStringList paths{from.filePath("replace.txt"), from.filePath("skip.txt"), from.filePath("both.txt")};
        QString error;
        QList<FileOps::Step> steps;
        const QStringList made = FileOps::transfer(paths, to.path(), FileOps::Copy, &error,
                                                   {{paths[0], FileOps::Replace}, {paths[1], FileOps::Skip}, {paths[2], FileOps::KeepBoth}},
                                                   nullptr, &steps);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(made, QStringList({"replace.txt", "both (copy).txt"}));
        QCOMPARE(contents(to.filePath("replace.txt")), QByteArray("new"));
        QCOMPARE(contents(to.filePath("skip.txt")), QByteArray("old"));
        QCOMPARE(contents(to.filePath("both.txt")), QByteArray("old"));
        QCOMPARE(contents(to.filePath("both (copy).txt")), QByteArray("new"));
        // What's replaced isn't deleted: it's in the trash, with its note.
        const QString trashed = data.filePath("Trash/files/replace.txt");
        QCOMPARE(contents(trashed), QByteArray("old"));
        QCOMPARE(FileOps::originalPath(trashed), to.filePath("replace.txt"));
        QCOMPARE(steps.size(), 3);
        QCOMPARE(steps[0].other, trashed);
    }

    void fileOpsKeepBothBesideALinkThatLeadsNowhere()
    {
        // A link that leads nowhere takes its name: Keep Both copies beside
        // it, and the link, never the copy's, is never noted for undo to
        // send to the trash.
        QTemporaryDir from, to;
        write(from.filePath("a.txt"), "a");
        QVERIFY(QFile::link(to.filePath("gone"), to.filePath("a.txt")));
        QCOMPARE(FileOps::freeName(to.path(), "a.txt"), QString("a (copy).txt"));
        QString error;
        QList<FileOps::Step> steps;
        QCOMPARE(FileOps::transfer({from.filePath("a.txt")}, to.path(), FileOps::Copy, &error, {}, nullptr, &steps),
                 QStringList({"a (copy).txt"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(steps.size(), 1);
        QCOMPARE(steps[0].path, to.filePath("a (copy).txt"));
        QVERIFY(QFileInfo(to.filePath("a.txt")).isSymLink());

        // A copy that can't make its item says it made nothing.
        bool made = true;
        QVERIFY(!FileOps::copyRecursively(from.filePath("a.txt"), to.filePath("a.txt"), &error, false, nullptr, &made));
        QVERIFY(!made);
    }

    void fileOpsReplaceThatFailsPutsTheOldOneBack()
    {
        // What's replaced goes to the trash first; when nothing takes its
        // place (a file that can't be read, a full drive, a cancel), it's
        // put back rather than left there.
        QTemporaryDir data, from, to;
        const auto restore = dataHomeAt(data.path());
        write(from.filePath("a.txt"), "new");
        write(to.filePath("a.txt"), "old");
        QFile::setPermissions(from.filePath("a.txt"), QFile::Permissions());
        if (QFile(from.filePath("a.txt")).open(QIODevice::ReadOnly))
            QSKIP("Running as root reads everything.");
        QString error;
        QList<FileOps::Step> steps;
        QVERIFY(FileOps::transfer({from.filePath("a.txt")}, to.path(), FileOps::Copy, &error,
                                  {{from.filePath("a.txt"), FileOps::Replace}}, nullptr, &steps)
                    .isEmpty());
        QCOMPARE(error, QString("Couldn't copy a.txt."));
        QCOMPARE(contents(to.filePath("a.txt")), QByteArray("old"));
        QVERIFY(!QFileInfo::exists(data.filePath("Trash/files/a.txt")));
        QVERIFY(steps.isEmpty());
    }

    void fileOpsMergeFolders()
    {
        // A folder onto a folder merges: what's new goes in, and a clash
        // inside is replaced the same way, the old one to the trash.
        QTemporaryDir data, from, to;
        const auto restore = dataHomeAt(data.path());
        QDir(from.path()).mkpath("album/sub");
        QDir(to.path()).mkpath("album/sub");
        write(from.filePath("album/a.jpg"), "new");
        write(from.filePath("album/sub/c.jpg"), "c");
        write(to.filePath("album/a.jpg"), "old");
        write(to.filePath("album/keep.jpg"), "keep");
        QString error;
        QCOMPARE(FileOps::transfer({from.filePath("album")}, to.path(), FileOps::Move, &error,
                                   {{from.filePath("album"), FileOps::Replace}}),
                 QStringList({"album"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(contents(to.filePath("album/a.jpg")), QByteArray("new"));
        QCOMPARE(contents(to.filePath("album/keep.jpg")), QByteArray("keep"));
        QCOMPARE(contents(to.filePath("album/sub/c.jpg")), QByteArray("c"));
        QCOMPARE(contents(data.filePath("Trash/files/a.jpg")), QByteArray("old"));
        // Moved, the emptied folder goes.
        QVERIFY(!QFileInfo::exists(from.filePath("album")));
    }

    void fileOpsAskAboutEachClashOrAll()
    {
        // Pasting asks about each clash before anything is copied; the
        // answer can go for the rest, and Escape cancels the lot.
        QTemporaryDir from, to;
        for (const char *name : {"x.txt", "y.txt", "z.txt"})
            write(from.filePath(name), "new");
        write(to.filePath("x.txt"), "old");
        write(to.filePath("y.txt"), "old");
        QDir(from.path()).mkdir("box");
        QDir(to.path()).mkdir("box");
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        ops.copy({from.filePath("box"), from.filePath("x.txt"), from.filePath("y.txt"), from.filePath("z.txt")});
        ops.paste(to.path());
        QVERIFY(ops.busy().isEmpty());
        QCOMPARE(ops.clash().value("name").toString(), QString("box"));
        QCOMPARE(ops.clash().value("folder").toString(), QFileInfo(to.path()).fileName());
        QVERIFY(ops.clash().value("merge").toBool());
        QCOMPARE(ops.clash().value("others").toInt(), 2);
        ops.answerClash("both", false);
        QCOMPARE(ops.clash().value("name").toString(), QString("x.txt"));
        QVERIFY(!ops.clash().value("merge").toBool());
        QCOMPARE(ops.clash().value("others").toInt(), 1);
        // Nothing is copied until the last answer.
        QVERIFY(!QFile::exists(to.filePath("z.txt")));
        ops.answerClash("skip", true);
        QVERIFY(ops.clash().isEmpty());
        QVERIFY(done.wait(3000));
        QVERIFY(QFileInfo(to.filePath("box (copy)")).isDir());
        QCOMPARE(contents(to.filePath("x.txt")), QByteArray("old"));
        QCOMPARE(contents(to.filePath("y.txt")), QByteArray("old"));
        QCOMPARE(contents(to.filePath("z.txt")), QByteArray("new"));
        QVERIFY(!QFile::exists(to.filePath("x (copy).txt")));

        // Cancelled, nothing happens at all.
        QTemporaryDir fresh;
        write(fresh.filePath("x.txt"), "old");
        ops.copy({from.filePath("x.txt"), from.filePath("z.txt")});
        ops.paste(fresh.path());
        QVERIFY(!ops.clash().isEmpty());
        ops.dropClash();
        QVERIFY(ops.clash().isEmpty());
        QVERIFY(ops.busy().isEmpty());
        QVERIFY(!QFile::exists(fresh.filePath("z.txt")));
    }

    void undoAndRedoRenameMoveAndTrash()
    {
        QTemporaryDir data, dir;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        write(dir.filePath("a.txt"), "a");
        write(dir.filePath("c.txt"), "c");
        QDir(dir.path()).mkdir("sub");
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice), failed(&ops, &FileOps::failed);
        QCOMPARE(ops.undoName(), QString());

        QCOMPARE(ops.rename(dir.filePath("a.txt"), "b.txt"), QString());
        QCOMPARE(ops.undoName(), QString("Undo Rename"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(QFile::exists(dir.filePath("a.txt")));
        QVERIFY(!QFile::exists(dir.filePath("b.txt")));
        QCOMPARE(notice.last().first().toString(), QString("Undid renaming a.txt."));
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(QFile::exists(dir.filePath("b.txt")));
        QCOMPARE(notice.last().first().toString(), QString("Redid renaming a.txt."));

        ops.cut({dir.filePath("b.txt"), dir.filePath("c.txt")});
        ops.paste(dir.filePath("sub"));
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(QFile::exists(dir.filePath("sub/b.txt")));
        QCOMPARE(ops.undoName(), QString("Undo Move"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(QFile::exists(dir.filePath("b.txt")));
        QVERIFY(QFile::exists(dir.filePath("c.txt")));
        QVERIFY(QDir(dir.filePath("sub")).isEmpty());
        QCOMPARE(notice.last().first().toString(), QString("Undid moving 2 items."));
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(QFile::exists(dir.filePath("sub/b.txt")));
        QVERIFY(QFile::exists(dir.filePath("sub/c.txt")));

        ops.trash({dir.filePath("sub/b.txt"), dir.filePath("sub/c.txt")});
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(QDir(dir.filePath("sub")).isEmpty());
        QCOMPARE(ops.undoName(), QString("Undo Delete"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(dir.filePath("sub/b.txt")), QByteArray("a"));
        QCOMPARE(contents(dir.filePath("sub/c.txt")), QByteArray("c"));
        QVERIFY(QDir(data.filePath("Trash/files")).isEmpty());
        QVERIFY(QDir(data.filePath("Trash/info")).isEmpty());
        QCOMPARE(notice.last().first().toString(), QString("Undid moving 2 items to the trash."));
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(QDir(dir.filePath("sub")).isEmpty());
        QCOMPARE(QDir(data.filePath("Trash/files")).entryList(QDir::Files), QStringList({"b.txt", "c.txt"}));
        QCOMPARE(failed.count(), 0);

        // Undo goes back through them all, the newest first.
        for (int i = 0; i < 3; ++i) {
            ops.undo();
            QTRY_VERIFY(ops.busy().isEmpty());
        }
        QCOMPARE(contents(dir.filePath("a.txt")), QByteArray("a"));
        QCOMPARE(contents(dir.filePath("c.txt")), QByteArray("c"));
        QCOMPARE(ops.undoName(), QString());
        QCOMPARE(failed.count(), 0);
    }

    void undoAndRedoPasteAndReplace()
    {
        // A copy is undone by moving it to the trash, and redone by
        // bringing it back; a replace puts the replaced one back too.
        QTemporaryDir data, from, to;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        write(from.filePath("a.txt"), "new");
        write(from.filePath("b.txt"), "new");
        write(from.filePath("r.txt"), "new");
        write(to.filePath("r.txt"), "old");
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice), failed(&ops, &FileOps::failed);
        ops.copy({from.filePath("a.txt"), from.filePath("b.txt")});
        ops.paste(to.path());
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(ops.undoName(), QString("Undo Copy"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(!QFile::exists(to.filePath("a.txt")));
        QVERIFY(!QFile::exists(to.filePath("b.txt")));
        QCOMPARE(contents(data.filePath("Trash/files/a.txt")), QByteArray("new"));
        QCOMPARE(notice.last().first().toString(), QString("Undid copying 2 items."));
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(to.filePath("a.txt")), QByteArray("new"));
        QCOMPARE(contents(to.filePath("b.txt")), QByteArray("new"));
        QVERIFY(QDir(data.filePath("Trash/files")).isEmpty());

        ops.copy({from.filePath("r.txt")});
        ops.paste(to.path());
        ops.answerClash("replace", false);
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(to.filePath("r.txt")), QByteArray("new"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(to.filePath("r.txt")), QByteArray("old"));
        // The new one went to the trash while the old one was still there.
        QCOMPARE(QDir(data.filePath("Trash/files")).entryList(QDir::Files), QStringList({"r.txt.2"}));
        QCOMPARE(contents(data.filePath("Trash/files/r.txt.2")), QByteArray("new"));
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(to.filePath("r.txt")), QByteArray("new"));
        QCOMPARE(contents(data.filePath("Trash/files/r.txt")), QByteArray("old"));
        QCOMPARE(failed.count(), 0);
    }

    void undoAndRedoAMergingMove()
    {
        // Moved onto a folder of the same name, a folder merges and goes;
        // undo makes it and each folder in it again, empty ones too, with
        // their permissions and times, and redo takes them away again.
        QTemporaryDir data, from, to;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        QDir(from.path()).mkpath("album/empty");
        QDir(to.path()).mkpath("album/empty");
        write(from.filePath("album/a.jpg"), "a");
        QFile::setPermissions(from.filePath("album/empty"), QFile::ReadOwner | QFile::ExeOwner | QFile::ReadGroup);
        QFile::setPermissions(from.filePath("album"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ExeOther);
        const QDateTime then = QDateTime::currentDateTime().addDays(-3);
        for (const char *folder : {"album", "album/empty"}) {
            const timespec times[2] = {{0, UTIME_OMIT}, {then.toSecsSinceEpoch(), 0}};
            QCOMPARE(::utimensat(AT_FDCWD, QFile::encodeName(from.filePath(folder)).constData(), times, 0), 0);
        }
        const auto permissions = [&](const char *folder) { return QFileInfo(from.filePath(folder)).permissions(); };
        const auto wasThen = [&](const char *folder) {
            return QFileInfo(from.filePath(folder)).lastModified().toSecsSinceEpoch() == then.toSecsSinceEpoch();
        };
        const auto emptyWas = permissions("album/empty"), albumWas = permissions("album");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.cut({from.filePath("album")});
        ops.paste(to.path());
        ops.answerClash("replace", false);
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(!QFileInfo::exists(from.filePath("album")));
        QCOMPARE(contents(to.filePath("album/a.jpg")), QByteArray("a"));

        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(from.filePath("album/a.jpg")), QByteArray("a"));
        QVERIFY(QFileInfo(from.filePath("album/empty")).isDir());
        QCOMPARE(permissions("album/empty"), emptyWas);
        QCOMPARE(permissions("album"), albumWas);
        QVERIFY(wasThen("album"));
        QVERIFY(wasThen("album/empty"));
        QVERIFY(QFileInfo(to.filePath("album/empty")).isDir());

        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(!QFileInfo::exists(from.filePath("album")));
        QCOMPARE(contents(to.filePath("album/a.jpg")), QByteArray("a"));
        QCOMPARE(failed.count(), 0);
    }

    void undoAndRedoTakeTurnsAcrossPanes()
    {
        // The history is every pane's: while one pane undoes, another can't
        // undo too.
        QTemporaryDir dir;
        FileOps::forgetHistory();
        write(dir.filePath("a.txt"), "a");
        write(dir.filePath("c.txt"), "c");
        FileOps left, right;
        QSignalSpy failed(&right, &FileOps::failed), leftFailed(&left, &FileOps::failed);
        QCOMPARE(left.rename(dir.filePath("a.txt"), "b.txt"), QString());
        QCOMPARE(left.rename(dir.filePath("c.txt"), "d.txt"), QString());
        left.undo();
        right.undo();
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.last().first().toString(), QString("Try again once the work under way is done."));
        QTRY_VERIFY(left.busy().isEmpty());
        QVERIFY(QFile::exists(dir.filePath("c.txt")));
        QVERIFY(QFile::exists(dir.filePath("b.txt")));

        // Nor can another pane change files while an undo runs, which could
        // put another item where the undo hasn't reached yet.
        left.undo();
        QCOMPARE(right.rename(dir.filePath("c.txt"), "e.txt"), QString("Try again once the work under way is done."));
        right.newFolder(dir.path());
        right.duplicate(dir.filePath("c.txt"));
        QCOMPARE(failed.count(), 3);
        QCOMPARE(failed.last().first().toString(), QString("Try again once the work under way is done."));
        QTRY_VERIFY(left.busy().isEmpty());
        QCOMPARE(QDir(dir.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot), QStringList({"a.txt", "c.txt"}));
        QCOMPARE(left.undoName(), QString());

        // And an undo waits for file work in another pane.
        right.duplicate(dir.filePath("c.txt"));
        left.redo();
        QCOMPARE(leftFailed.count(), 1);
        QCOMPARE(leftFailed.last().first().toString(), QString("Try again once the work under way is done."));
        QTRY_VERIFY(right.busy().isEmpty());
        QVERIFY(QFile::exists(dir.filePath("a.txt")));
    }

    void drivesChangeOnlyWhileNoFilesDo()
    {
        // A drive's change waits for every pane's file work, which may be
        // on it, and file work waits for a drive's change.
        QTemporaryDir dir;
        write(dir.filePath("a.txt"), "a");
        auto fake = fakeDisks();
        FileOps copying, formatting;
        QSignalSpy refused(&formatting, &FileOps::failed), duplicated(&copying, &FileOps::done);
        copying.duplicate(dir.filePath("a.txt"));
        formatting.format(sanDisk().identity, "Photos");
        QCOMPARE(refused.count(), 1);
        QVERIFY(fake->calls.isEmpty());
        QVERIFY(duplicated.wait(3000));

        std::atomic<bool> unmounting = false, release = false;
        fake->during = [&](const QString &method) {
            if (method != "unmount")
                return;
            unmounting = true;
            while (!release)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        };
        QSignalSpy formatted(&formatting, &FileOps::notice), stopped(&copying, &FileOps::failed);
        formatting.format(sanDisk().identity, "Photos");
        QTRY_VERIFY(unmounting);
        copying.duplicate(dir.filePath("a.txt"));
        QCOMPARE(stopped.count(), 1);
        QVERIFY(copying.busy().isEmpty());
        release = true;
        QTRY_COMPARE(formatted.count(), 1);
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList({"a (copy).txt", "a.txt"}));
    }

    void workOutlivesItsClosedPane()
    {
        // A pane closed while its work runs leaves the work to finish, and
        // what it changed can still be undone from another.
        QTemporaryDir data, dir;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        write(dir.filePath("a.txt"), "a");
        QPointer<FileOps> closed = new FileOps;
        closed->duplicate(dir.filePath("a.txt"));
        QVERIFY(!closed->busy().isEmpty());
        closed->deleteWhenDone();
        QVERIFY(closed);
        QTRY_VERIFY(!closed);
        FileOps other;
        QCOMPARE(other.undoName(), QString("Undo Duplicate"));
        QVERIFY(QFile::exists(dir.filePath("a (copy).txt")));

        // One waiting on a clash goes without asking.
        closed = new FileOps;
        closed->copy({dir.filePath("a.txt")});
        QTemporaryDir to;
        write(to.filePath("a.txt"), "old");
        closed->paste(to.path());
        QVERIFY(!closed->clash().isEmpty());
        closed->deleteWhenDone();
        QTRY_VERIFY(!closed);
        QCOMPARE(contents(to.filePath("a.txt")), QByteArray("old"));
    }

    void undoRefusesWhatsChangedSince()
    {
        // Undo checks everything is still where the change left it, and
        // that nothing's in the way, before it touches anything; refused,
        // the change stays there to undo.
        QTemporaryDir dir;
        FileOps::forgetHistory();
        write(dir.filePath("a.txt"), "a");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        QCOMPARE(ops.rename(dir.filePath("a.txt"), "b.txt"), QString());
        QVERIFY(QFile::rename(dir.filePath("b.txt"), dir.filePath("c.txt")));
        ops.undo();
        QVERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.last().first().toString(), QString("Can't undo that: b.txt isn't where it was left."));
        QCOMPARE(ops.undoName(), QString("Undo Rename"));

        QVERIFY(QFile::rename(dir.filePath("c.txt"), dir.filePath("b.txt")));
        write(dir.filePath("a.txt"), "newer");
        ops.undo();
        QCOMPARE(failed.last().first().toString(),
                 QString("Can't undo that: a.txt already exists in %1.").arg(QFileInfo(dir.path()).fileName()));
        QCOMPARE(contents(dir.filePath("a.txt")), QByteArray("newer"));
        QVERIFY(QFile::exists(dir.filePath("b.txt")));
        QCOMPARE(ops.undoName(), QString("Undo Rename"));
    }

    void undoRefusesAnotherItemPutWhereItsWas()
    {
        // Renamed, then moved away by another app, an item can have another
        // take its name; undo leaves that one be rather than renaming it.
        QTemporaryDir dir, away;
        FileOps::forgetHistory();
        write(dir.filePath("a.txt"), "mine");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        QCOMPARE(ops.rename(dir.filePath("a.txt"), "b.txt"), QString());
        QVERIFY(QFile::rename(dir.filePath("b.txt"), away.filePath("b.txt")));
        write(dir.filePath("b.txt"), "theirs");
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.last().first().toString(), QString("Can't undo that: b.txt isn't where it was left."));
        QCOMPARE(contents(dir.filePath("b.txt")), QByteArray("theirs"));
        QVERIFY(!QFileInfo::exists(dir.filePath("a.txt")));

        // Back where it was left, it's undone, and redone.
        QVERIFY(QFile::remove(dir.filePath("b.txt")));
        QVERIFY(QFile::rename(away.filePath("b.txt"), dir.filePath("b.txt")));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(dir.filePath("a.txt")), QByteArray("mine"));
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(dir.filePath("b.txt")), QByteArray("mine"));
        QCOMPARE(failed.count(), 1);
    }

    void undoRefusesATrashedItemThatsSinceAnother()
    {
        // Deleted from the trash for good, an item's name there can go to
        // the next one trashed, by another app. Undoing the first trashing
        // would then bring that one back in its place.
        QTemporaryDir data, dir, elsewhere;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        write(dir.filePath("a.txt"), "mine");
        write(elsewhere.filePath("a.txt"), "theirs");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.trash({dir.filePath("a.txt")});
        QTRY_VERIFY(ops.busy().isEmpty());
        const QString trashed = data.filePath("Trash/files/a.txt");
        QVERIFY(FileOps::deleteTrashed(trashed));
        QString error;
        QCOMPARE(FileOps::trashOne(elsewhere.filePath("a.txt"), &error), trashed);
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.last().first().toString(), QString("Can't undo that: a.txt isn't where it was left."));
        QVERIFY(!QFileInfo::exists(dir.filePath("a.txt")));
        QCOMPARE(FileOps::originalPath(trashed), elsewhere.filePath("a.txt"));
    }

    void renamesNeverReplaceWhereDrivesCantPromiseIt()
    {
        // A drive that can't rename without replacing still never replaces:
        // a file or link is linked under its new name, a name that's taken
        // is passed over for the next, and a folder is copied.
        QTemporaryDir data, dir, to;
        const auto restore = dataHomeAt(data.path());
        FileOps::drivesRefuseNoReplace = true;
        const auto reset = qScopeGuard([] { FileOps::drivesRefuseNoReplace = false; });
        write(dir.filePath("a.txt"), "mine");
        QDir(data.path()).mkpath("Trash/files");
        write(data.filePath("Trash/files/a.txt"), "left behind");
        QString error;
        QCOMPARE(FileOps::trashOne(dir.filePath("a.txt"), &error), data.filePath("Trash/files/a.txt.2"));
        QCOMPARE(error, QString());
        QCOMPARE(contents(data.filePath("Trash/files/a.txt")), QByteArray("left behind"));
        QCOMPARE(contents(data.filePath("Trash/files/a.txt.2")), QByteArray("mine"));
        QVERIFY(!QFileInfo::exists(dir.filePath("a.txt")));

        QVERIFY(QFile::link("nowhere", dir.filePath("link")));
        QCOMPARE(FileOps::trashOne(dir.filePath("link"), &error), data.filePath("Trash/files/link"));
        QCOMPARE(QFileInfo(data.filePath("Trash/files/link")).readSymLink(), QString("nowhere"));

        QDir(dir.path()).mkdir("box");
        write(dir.filePath("box/b.txt"), "b");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.cut({dir.filePath("box")});
        ops.paste(to.path());
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QCOMPARE(contents(to.filePath("box/b.txt")), QByteArray("b"));
        QVERIFY(!QFileInfo::exists(dir.filePath("box")));
    }

    void fileOpsTakeAnItemChosenTwiceOnce()
    {
        // The same item can be handed over twice (from another app's
        // clipboard); it's copied once, not once more as a "(copy)".
        QTemporaryDir from, to;
        write(from.filePath("a.txt"), "a");
        QString error;
        QCOMPARE(FileOps::transfer({from.filePath("a.txt"), from.filePath("a.txt")}, to.path(), FileOps::Copy, &error),
                 QStringList({"a.txt"}));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(QDir(to.path()).entryList(QDir::Files), QStringList({"a.txt"}));
    }

    void extractOneItemBesideTheArchive()
    {
        // An archive holding one folder (or file) puts it beside the
        // archive, under its own name; nothing else is left behind.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir dir, source;
        QDir(source.path()).mkpath("album/sub");
        write(source.filePath("album/a.jpg"), "a");
        write(source.filePath("album/sub/b.jpg"), "b");
        pack(dir.filePath("photos.tar.gz"), source.path(), {"album"});
        FileOps ops;
        QVERIFY(ops.canExtract({dir.filePath("photos.tar.gz")}));
        QSignalSpy done(&ops, &FileOps::done), failed(&ops, &FileOps::failed);
        ops.extract({dir.filePath("photos.tar.gz")});
        QVERIFY(done.wait(5000));
        QCOMPARE(done.first(), QList<QVariant>({dir.path(), "album"}));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(contents(dir.filePath("album/a.jpg")), QByteArray("a"));
        QCOMPARE(contents(dir.filePath("album/sub/b.jpg")), QByteArray("b"));
        QCOMPARE(everything(dir.path()), QStringList({"album", "photos.tar.gz"}));
    }

    void extractManyItemsIntoAFolder()
    {
        // Several items go into a new folder named after the archive,
        // without its extensions, so they don't spill over the folder.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir dir, source;
        write(source.filePath("a.txt"), "a");
        write(source.filePath("b.txt"), "b");
        pack(dir.filePath("notes.tar.xz"), source.path(), {"a.txt", "b.txt"});
        pack(dir.filePath("more.zip"), source.path(), {"a.txt", "b.txt"});
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        ops.extract({dir.filePath("notes.tar.xz"), dir.filePath("more.zip")});
        QVERIFY(done.wait(5000));
        QCOMPARE(done.first(), QList<QVariant>({dir.path(), "notes"}));
        QCOMPARE(contents(dir.filePath("notes/a.txt")), QByteArray("a"));
        QCOMPARE(contents(dir.filePath("notes/b.txt")), QByteArray("b"));
        QCOMPARE(contents(dir.filePath("more/b.txt")), QByteArray("b"));
        QCOMPARE(everything(dir.path()), QStringList({"more", "more.zip", "notes", "notes.tar.xz"}));
        // Only archives are offered: not a lone compressed file, a disk
        // image (that's for burning), a document, nor a folder.
        write(dir.filePath("single.txt.gz"), "x");
        write(dir.filePath("installer.iso"), QByteArray(64 * 1024, '\0'));
        write(dir.filePath("notes.txt"), "text");
        QVERIFY(!ops.canExtract({dir.filePath("installer.iso")}));
        QVERIFY(!ops.canExtract({dir.filePath("notes.txt")}));
        QVERIFY(ops.canExtract({dir.filePath("notes.tar.xz"), dir.filePath("more.zip")}));
        QVERIFY(!ops.canExtract({dir.filePath("notes.tar.xz"), dir.filePath("single.txt.gz")}));
        QVERIFY(!ops.canExtract({dir.filePath("notes")}));
        QVERIFY(!ops.canExtract({}));
    }

    void extractAsksWhenTheNameIsTaken()
    {
        // The name that would appear is asked about like a paste's, Keep
        // Both first; cancelling leaves nothing, not even the hidden
        // extraction.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir data, dir, source;
        const auto restore = dataHomeAt(data.path());
        write(source.filePath("a.txt"), "new");
        write(source.filePath("b.txt"), "new");
        pack(dir.filePath("notes.zip"), source.path(), {"a.txt", "b.txt"});
        QDir(dir.path()).mkdir("notes");
        write(dir.filePath("notes/a.txt"), "old");
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done), clash(&ops, &FileOps::clashChanged);
        ops.extract({dir.filePath("notes.zip")});
        QVERIFY(clash.wait(5000));
        QCOMPARE(ops.clash().value("name").toString(), QString("notes"));
        QCOMPARE(ops.clash().value("merge").toBool(), true);
        ops.dropClash();
        QTRY_COMPARE(everything(dir.path()), QStringList({"notes", "notes.zip"}));
        QCOMPARE(everything(dir.filePath("notes")), QStringList({"a.txt"}));

        ops.extract({dir.filePath("notes.zip")});
        QVERIFY(clash.wait(5000));
        ops.answerClash("both", false);
        QVERIFY(done.wait(5000));
        QCOMPARE(done.first(), QList<QVariant>({dir.path(), "notes (copy)"}));
        QCOMPARE(contents(dir.filePath("notes (copy)/a.txt")), QByteArray("new"));
        QCOMPARE(contents(dir.filePath("notes/a.txt")), QByteArray("old"));

        // Merged, what's replaced inside goes to the trash.
        ops.extract({dir.filePath("notes.zip")});
        QVERIFY(clash.wait(5000));
        ops.answerClash("replace", false);
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(dir.filePath("notes/a.txt")), QByteArray("new"));
        QCOMPARE(contents(dir.filePath("notes/b.txt")), QByteArray("new"));
        QCOMPARE(contents(data.filePath("Trash/files/a.txt")), QByteArray("old"));
        QCOMPARE(everything(dir.path()), QStringList({"notes", "notes (copy)", "notes.zip"}));
    }

    void extractAsksAboutNamesTwoArchivesShare()
    {
        // Two archives each giving readme.txt, a name that's free: the
        // second is asked about, not quietly kept beside.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir data, dir, one, two;
        const auto restore = dataHomeAt(data.path());
        write(one.filePath("readme.txt"), "one");
        write(two.filePath("readme.txt"), "two");
        pack(dir.filePath("one.zip"), one.path(), {"readme.txt"});
        pack(dir.filePath("two.zip"), two.path(), {"readme.txt"});
        FileOps ops;
        QSignalSpy clash(&ops, &FileOps::clashChanged);
        ops.extract({dir.filePath("one.zip"), dir.filePath("two.zip")});
        QVERIFY(clash.wait(5000));
        QCOMPARE(ops.clash().value("name").toString(), QString("readme.txt"));
        ops.answerClash("skip", true);
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(everything(dir.path()), QStringList({"one.zip", "readme.txt", "two.zip"}));
        QCOMPARE(contents(dir.filePath("readme.txt")), QByteArray("one"));
    }

    void extractPlacesInTheBackgroundAndCanStop()
    {
        // Merging what's extracted into a folder that's there goes through
        // it all, off the UI thread, and Cancel stops it: what's not placed
        // goes with the hidden folder. The worker is held back until the
        // cancel, so it's stopped before it starts.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir data, dir, source;
        const auto restore = dataHomeAt(data.path());
        write(source.filePath("a.txt"), "new");
        write(source.filePath("b.txt"), "new");
        pack(dir.filePath("notes.zip"), source.path(), {"a.txt", "b.txt"});
        QDir(dir.path()).mkdir("notes");
        write(dir.filePath("notes/a.txt"), "old");
        FileOps ops;
        QSignalSpy clash(&ops, &FileOps::clashChanged), notice(&ops, &FileOps::notice);
        ops.extract({dir.filePath("notes.zip")});
        QVERIFY(clash.wait(5000));
        QTRY_VERIFY(ops.busy().isEmpty());

        QThreadPool *pool = QThreadPool::globalInstance();
        const int threads = pool->maxThreadCount();
        pool->setMaxThreadCount(1);
        std::atomic<bool> hold = true;
        pool->start([&] {
            while (hold)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        });
        ops.answerClash("replace", false);
        QCOMPARE(ops.busy(), QString("Extracting notes.zip…"));
        ops.cancel();
        hold = false;
        QTRY_VERIFY(ops.busy().isEmpty());
        pool->setMaxThreadCount(threads);
        QCOMPARE(notice.last().first().toString(), QString("Stopped extracting notes.zip; nothing was extracted."));
        QCOMPARE(everything(dir.path()), QStringList({"notes", "notes.zip"}));
        QCOMPARE(everything(dir.filePath("notes")), QStringList({"a.txt"}));
        QCOMPARE(contents(dir.filePath("notes/a.txt")), QByteArray("old"));
    }

    void extractRefusesWhatWouldLandOutside()
    {
        // bsdtar refuses ".." and links leading out, and an archive with
        // either is extracted not at all: nothing escapes, and nothing is
        // left half-made.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir outer;
        QDir(outer.path()).mkpath("dir");
        QDir(outer.path()).mkpath("target");
        const QString dir = outer.filePath("dir");
        write(outer.filePath("f"), "evil");
        const auto mtree = [&](const QString &archive, const QByteArray &spec) {
            write(outer.filePath("spec"), "#mtree\n" + spec);
            QProcess tar;
            tar.start("bsdtar", {"-cf", archive, "@" + outer.filePath("spec")});
            QVERIFY(tar.waitForFinished() && tar.exitCode() == 0);
        };
        const QByteArray f = QFile::encodeName(outer.filePath("f"));
        mtree(dir + "/dots.tar", "./ok.txt type=file contents=" + f + "\n../evil.txt type=file contents=" + f + "\n");
        mtree(dir + "/link.tar", "./l type=link link=" + QFile::encodeName(outer.filePath("target"))
                                     + "\n./l/evil.txt type=file contents=" + f + "\n");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.extract({dir + "/dots.tar"});
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.last().first().toString(), QString("Couldn't extract dots.tar: it would put files outside its folder."));
        QTRY_VERIFY(ops.busy().isEmpty());
        ops.extract({dir + "/link.tar"});
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.last().first().toString(), QString("Couldn't extract link.tar: it would put files outside its folder."));
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(everything(outer.path()), QStringList({"dir", "f", "spec", "target"}));
        QCOMPARE(everything(dir), QStringList({"dots.tar", "link.tar"}));
        QCOMPARE(everything(outer.filePath("target")), QStringList());
    }

    void extractSaysWhenItCant()
    {
        // A password, or a damaged archive, is said as such.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir dir;
        write(dir.filePath("secret.txt"), "secret");
        QProcess tar;
        // AES, whose password check is two bytes: ZipCrypto's one byte let
        // Folio's stand-in password through once in 256 runs, and the
        // archive read as damaged.
        tar.start("bsdtar", {"--options", "zip:encryption=aes256", "--passphrase", "x", "-acf",
                             dir.filePath("locked.zip"), "-C", dir.path(), "secret.txt"});
        QVERIFY(tar.waitForFinished() && tar.exitCode() == 0);
        QFile::remove(dir.filePath("secret.txt"));
        write(dir.filePath("broken.zip"), QByteArray(300, 'x'));
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.extract({dir.filePath("locked.zip"), dir.filePath("broken.zip")});
        QVERIFY(failed.wait(5000));
        QCOMPARE(failed.first().first().toString(),
                 QString("Couldn't extract locked.zip: it's protected by a password. "
                         "Couldn't extract broken.zip: it's damaged, or isn't an archive Folio can read."));
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(everything(dir.path()), QStringList({"broken.zip", "locked.zip"}));
    }

    void extractCancelLeavesNothing()
    {
        // Cancel stops bsdtar partway, and what it had made goes with it.
        // Reading the archive from a pipe keeps it partway for as long as
        // the test likes.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir dir, source;
        write(source.filePath("a.txt"), QByteArray(4096, 'a'));
        write(source.filePath("b.txt"), QByteArray(1 << 20, 'b'));
        pack(source.filePath("whole.tar"), source.path(), {"a.txt", "b.txt"});
        const QByteArray archive = contents(source.filePath("whole.tar"));
        const QByteArray pipe = QFile::encodeName(dir.filePath("slow.tar"));
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice);
        ops.extract({dir.filePath("slow.tar")});
        int writer = -1;
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        // The first file whole, and some of the second: bsdtar reads ahead,
        // so it takes more than the first file to see it through.
        fcntl(writer, F_SETFL, 0);
        QCOMPARE(::write(writer, archive.constData(), 256 << 10), 256 << 10);
        QTRY_VERIFY(ops.busy().contains("1"));
        ops.cancel();
        close(writer);
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(notice.count(), 1);
        QCOMPARE(notice.first().first().toString(), QString("Stopped extracting slow.tar; nothing was extracted."));
        QCOMPARE(everything(dir.path()), QStringList({"slow.tar"}));
    }

    void undoAndRedoExtract()
    {
        // Undo sends what was extracted to the trash; redo brings it back.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir data, dir, source;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        write(source.filePath("a.txt"), "a");
        write(source.filePath("b.txt"), "b");
        pack(dir.filePath("notes.zip"), source.path(), {"a.txt", "b.txt"});
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done), notice(&ops, &FileOps::notice);
        ops.extract({dir.filePath("notes.zip")});
        QVERIFY(done.wait(5000));
        QCOMPARE(ops.undoName(), QString("Undo Extract"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(everything(dir.path()), QStringList({"notes.zip"}));
        QCOMPARE(contents(data.filePath("Trash/files/notes/a.txt")), QByteArray("a"));
        QCOMPARE(notice.last().first().toString(), QString("Undid extracting notes.zip."));
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(dir.filePath("notes/b.txt")), QByteArray("b"));
        QCOMPARE(notice.last().first().toString(), QString("Redid extracting notes.zip."));
    }

    void extractKeepsAnArchiveOfDotsToItsFolder()
    {
        // "...tar" without its extension is "..": its items go in a folder
        // of its own, never into the folder above the archive's.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir dir, source;
        write(source.filePath("a.txt"), "a");
        write(source.filePath("b.txt"), "b");
        QDir(dir.path()).mkdir("sub");
        pack(dir.filePath("sub/...tar"), source.path(), {"a.txt", "b.txt"});
        FileOps ops;
        QSignalSpy done(&ops, &FileOps::done);
        ops.extract({dir.filePath("sub/...tar")});
        QVERIFY(done.wait(5000));
        QCOMPARE(everything(dir.path()), QStringList({"sub"}));
        QCOMPARE(everything(dir.filePath("sub")), QStringList({"...tar", "...tar contents"}));
        QCOMPARE(contents(dir.filePath("sub/...tar contents/b.txt")), QByteArray("b"));
    }

    void undoAndRedoAMergingExtract()
    {
        // Merged into a folder that's there, an extraction is undone like
        // any other: what it brought goes, and what it replaced is back.
        if (QStandardPaths::findExecutable("bsdtar").isEmpty())
            QSKIP("Extracting needs bsdtar.");
        QTemporaryDir data, dir, source;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        write(source.filePath("a.txt"), "new");
        write(source.filePath("b.txt"), "new");
        pack(dir.filePath("notes.zip"), source.path(), {"a.txt", "b.txt"});
        QDir(dir.path()).mkdir("notes");
        write(dir.filePath("notes/a.txt"), "old");
        FileOps ops;
        QSignalSpy clash(&ops, &FileOps::clashChanged), failed(&ops, &FileOps::failed);
        ops.extract({dir.filePath("notes.zip")});
        QVERIFY(clash.wait(5000));
        ops.answerClash("replace", false);
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(dir.filePath("notes/b.txt")), QByteArray("new"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QCOMPARE(everything(dir.filePath("notes")), QStringList({"a.txt"}));
        QCOMPARE(contents(dir.filePath("notes/a.txt")), QByteArray("old"));
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QCOMPARE(contents(dir.filePath("notes/a.txt")), QByteArray("new"));
        QCOMPARE(contents(dir.filePath("notes/b.txt")), QByteArray("new"));
    }

    void fileOpsCopyIsNeverOpenToOthersWhileCopying()
    {
        // A private file's copy is private from the start, not only once
        // it's all there. Copying a pipe keeps the copy partway.
        const mode_t mask = umask(022);
        const auto unmask = qScopeGuard([mask] { umask(mask); });
        QTemporaryDir from, to;
        const QByteArray pipe = QFile::encodeName(from.filePath("secret"));
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        FileOps::Job job;
        QString error;
        QFuture<bool> copied = QtConcurrent::run(
            [&] { return FileOps::copyRecursively(from.filePath("secret"), to.filePath("secret"), &error, false, &job); });
        int writer = -1;
        const auto finish = qScopeGuard([&] {
            if (writer >= 0)
                close(writer);
            copied.waitForFinished();
        });
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        QCOMPARE(::write(writer, "partial", 7), 7);
        QTRY_VERIFY(job.bytes >= 7);
        QCOMPARE(QFileInfo(to.filePath("secret")).permissions() & (QFile::ReadGroup | QFile::ReadOther),
                 QFile::Permissions());
        close(std::exchange(writer, -1));
        QVERIFY(copied.result());
    }

    void undoACancelledMoveAcrossDrives()
    {
        // Stopped partway across drives, what moved is only where it went:
        // undo brings it back, and takes away the folder made for it.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir data, from;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        QDir(from.path()).mkpath("box/sub");
        write(from.filePath("box/a.txt"), "a");
        write(from.filePath("box/sub/b.txt"), "b");
        write(from.filePath("box/z.txt"), "z");
        const QByteArray pipe = QFile::encodeName(from.filePath("box/pipe"));
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.cut({from.filePath("box")});
        ops.paste(other->path());
        int writer = -1;
        const auto finish = qScopeGuard([&] {
            if (writer >= 0)
                close(writer);
        });
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        ops.cancel();
        QCOMPARE(::write(writer, "x", 1), 1);
        close(std::exchange(writer, -1));
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(other->filePath("box/a.txt")), QByteArray("a"));
        QVERIFY(!QFileInfo::exists(from.filePath("box/a.txt")));
        QCOMPARE(ops.undoName(), QString("Undo Move"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QCOMPARE(contents(from.filePath("box/a.txt")), QByteArray("a"));
        QCOMPARE(contents(from.filePath("box/sub/b.txt")), QByteArray("b"));
        QCOMPARE(contents(from.filePath("box/z.txt")), QByteArray("z"));
        QVERIFY(!QFileInfo::exists(other->filePath("box")));
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QCOMPARE(contents(other->filePath("box/a.txt")), QByteArray("a"));
        QVERIFY(!QFileInfo::exists(from.filePath("box/a.txt")));
    }

    void undoAMoveAcrossDrivesWhoseOriginalsStayed()
    {
        // A folder that can be read but not changed is copied, its files
        // staying where they are. Undo sends those copies to the trash, so
        // the folders made for them can go too.
        if (geteuid() == 0)
            QSKIP("Root can change every folder.");
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        // The trash is on the drive the copies are on, as a USB stick's is.
        QTemporaryDir from;
        const auto restore = dataHomeAt(other->filePath(".data"));
        FileOps::forgetHistory();
        QDir(from.path()).mkpath("box/locked");
        write(from.filePath("box/a.txt"), "a");
        write(from.filePath("box/locked/f.txt"), "f");
        QVERIFY(QFile::setPermissions(from.filePath("box/locked"), QFile::ReadOwner | QFile::ExeOwner));
        const auto unlock = qScopeGuard([&] {
            QFile::setPermissions(from.filePath("box/locked"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.cut({from.filePath("box")});
        ops.paste(other->path());
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(contents(other->filePath("box/locked/f.txt")), QByteArray("f"));
        QCOMPARE(contents(from.filePath("box/locked/f.txt")), QByteArray("f"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(contents(from.filePath("box/a.txt")), QByteArray("a"));
        QCOMPARE(contents(from.filePath("box/locked/f.txt")), QByteArray("f"));
        QVERIFY(!QFileInfo::exists(other->filePath("box")));
        QCOMPARE(contents(other->filePath(".data/Trash/files/f.txt")), QByteArray("f"));
    }

    void undoStoppedAcrossDrivesCanBeDoneAgain()
    {
        // An undo stopped partway across drives has brought some back: the
        // next undo still brings back the rest.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir data, from;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        QDir(from.path()).mkpath("box/sub");
        write(from.filePath("box/a.txt"), "a");
        write(from.filePath("box/sub/b.txt"), "b");
        write(from.filePath("box/z.txt"), "z");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.cut({from.filePath("box")});
        ops.paste(other->path());
        QTRY_VERIFY(ops.busy().isEmpty());
        const QByteArray pipe = QFile::encodeName(other->filePath("box/pipe"));
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        ops.undo();
        int writer = -1;
        const auto finish = qScopeGuard([&] {
            if (writer >= 0)
                close(writer);
        });
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        ops.cancel();
        QCOMPARE(::write(writer, "x", 1), 1);
        close(std::exchange(writer, -1));
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(from.filePath("box/a.txt")), QByteArray("a"));
        QCOMPARE(contents(other->filePath("box/z.txt")), QByteArray("z"));
        QVERIFY(QFile::remove(other->filePath("box/pipe")));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QCOMPARE(contents(from.filePath("box/a.txt")), QByteArray("a"));
        QCOMPARE(contents(from.filePath("box/sub/b.txt")), QByteArray("b"));
        QCOMPARE(contents(from.filePath("box/z.txt")), QByteArray("z"));
        QVERIFY(!QFileInfo::exists(other->filePath("box")));
        QCOMPARE(ops.undoName(), QString());
        ops.redo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QCOMPARE(contents(other->filePath("box/a.txt")), QByteArray("a"));
        QVERIFY(!QFileInfo::exists(from.filePath("box")));
    }

    void undoStoppedAcrossDrivesFollowsWhatItMovesTwice()
    {
        // Undone again after a stop, what came back goes out and back in
        // once more, made again twice in one go: the rename before it
        // follows it to the last.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir data, from;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        QDir(from.path()).mkdir("box");
        write(from.filePath("box/a.txt"), "a");
        write(from.filePath("box/z.txt"), "z");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        QCOMPARE(ops.rename(from.filePath("box/a.txt"), "b.txt"), QString());
        ops.cut({from.filePath("box")});
        ops.paste(other->path());
        QTRY_VERIFY(ops.busy().isEmpty());
        const QByteArray pipe = QFile::encodeName(other->filePath("box/pipe"));
        QVERIFY(mkfifo(pipe.constData(), 0600) == 0);
        ops.undo();
        int writer = -1;
        const auto finish = qScopeGuard([&] {
            if (writer >= 0)
                close(writer);
        });
        QVERIFY(QTest::qWaitFor([&] { return (writer = open(pipe.constData(), O_WRONLY | O_NONBLOCK)) >= 0; }));
        ops.cancel();
        QCOMPARE(::write(writer, "x", 1), 1);
        close(std::exchange(writer, -1));
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(from.filePath("box/b.txt")), QByteArray("a"));
        QVERIFY(QFile::remove(other->filePath("box/pipe")));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(from.filePath("box/b.txt")), QByteArray("a"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QCOMPARE(contents(from.filePath("box/a.txt")), QByteArray("a"));
    }

    void undoAReplacingMoveThatCouldntRemoveItsOriginal()
    {
        // Cut from a folder that can't be changed, the file is copied but
        // stays: undo takes the copy away and brings back what it replaced.
        if (geteuid() == 0)
            QSKIP("Root can change every folder.");
        QTemporaryDir data, from, to;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        write(from.filePath("a.txt"), "new");
        write(to.filePath("a.txt"), "old");
        QVERIFY(QFile::setPermissions(from.path(), QFile::ReadOwner | QFile::ExeOwner));
        const auto unlock = qScopeGuard([&] {
            QFile::setPermissions(from.path(), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        });
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.cut({from.filePath("a.txt")});
        ops.paste(to.path());
        ops.answerClash("replace", false);
        QTRY_COMPARE(failed.count(), 1);
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(to.filePath("a.txt")), QByteArray("new"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 1);
        QCOMPARE(contents(to.filePath("a.txt")), QByteArray("old"));
        QCOMPARE(contents(from.filePath("a.txt")), QByteArray("new"));
    }

    void undoFollowsAFolderMadeAgain()
    {
        // Undoing a merge makes the folder it removed again, under another
        // inode; the rename before it is still undone.
        QTemporaryDir data, from, to;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        QDir(from.path()).mkdir("photos");
        QDir(to.path()).mkdir("album");
        write(from.filePath("photos/a.jpg"), "a");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        QCOMPARE(ops.rename(from.filePath("photos"), "album"), QString());
        ops.cut({from.filePath("album")});
        ops.paste(to.path());
        ops.answerClash("replace", false);
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(!QFileInfo::exists(from.filePath("album")));
        for (int i = 0; i < 2; ++i) {
            ops.undo();
            QTRY_VERIFY(ops.busy().isEmpty());
        }
        QCOMPARE(failed.count(), 0);
        QCOMPARE(contents(from.filePath("photos/a.jpg")), QByteArray("a"));
        for (int i = 0; i < 2; ++i) {
            ops.redo();
            QTRY_VERIFY(ops.busy().isEmpty());
        }
        QCOMPARE(failed.count(), 0);
        QCOMPARE(contents(to.filePath("album/a.jpg")), QByteArray("a"));
    }

    void undoFollowsAMoveAcrossDrives()
    {
        // Moved across drives and back, a file is made again each time,
        // under another inode; what was done to it before is still undone.
        const auto other = elsewhere();
        if (!other)
            QSKIP("Needs a second drive.");
        QTemporaryDir data, dir;
        const auto restore = dataHomeAt(data.path());
        FileOps::forgetHistory();
        QDir(dir.path()).mkdir("box");
        write(dir.filePath("box/a.txt"), "a");
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        QCOMPARE(ops.rename(dir.filePath("box/a.txt"), "b.txt"), QString());
        ops.cut({dir.filePath("box")});
        ops.paste(other->path());
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(contents(other->filePath("box/b.txt")), QByteArray("a"));
        for (int i = 0; i < 2; ++i) {
            ops.undo();
            QTRY_VERIFY(ops.busy().isEmpty());
        }
        QCOMPARE(failed.count(), 0);
        QCOMPARE(contents(dir.filePath("box/a.txt")), QByteArray("a"));
        for (int i = 0; i < 2; ++i) {
            ops.redo();
            QTRY_VERIFY(ops.busy().isEmpty());
        }
        QCOMPARE(failed.count(), 0);
        QCOMPARE(contents(other->filePath("box/b.txt")), QByteArray("a"));
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        ops.undo();
        QTRY_VERIFY(ops.busy().isEmpty());
        QCOMPARE(failed.count(), 0);
        QCOMPARE(contents(dir.filePath("box/a.txt")), QByteArray("a"));
    }

    void trashFindsWhatItJustTrashed()
    {
        // Trashed twice in one second from one place, the two tie on their
        // time: the one just trashed is the one that wasn't there before.
        QTemporaryDir data;
        const auto restore = dataHomeAt(data.path());
        const QString trash = data.filePath("Trash");
        QDir().mkpath(trash + "/files");
        QDir().mkpath(trash + "/info");
        const QString original = data.filePath("docs/a.txt");
        const QByteArray note = "[Trash Info]\nPath=" + QUrl::toPercentEncoding(original, "/")
                                + "\nDeletionDate=2026-11-10T12:00:00\n";
        write(trash + "/files/a.txt.2", "first");
        write(trash + "/info/a.txt.2.trashinfo", note);
        const QStringList before = FileOps::trashedFrom(original);
        QCOMPARE(before, QStringList({trash + "/files/a.txt.2"}));
        write(trash + "/files/a.txt.3", "second");
        write(trash + "/info/a.txt.3.trashinfo", note);
        QCOMPARE(FileOps::trashedSince(original, before), trash + "/files/a.txt.3");
    }

    void kindsFromMimeTypes()
    {
        QCOMPARE(FolderModel::kindForMime("application/pdf"), QString("pdf"));
        QCOMPARE(FolderModel::kindForMime("video/mp4"), QString("video"));
        QCOMPARE(FolderModel::kindForMime("audio/mp4"), QString("audio"));
        QCOMPARE(FolderModel::kindForMime("application/zip"), QString("archive"));
        QCOMPARE(FolderModel::kindForMime("application/x-xz-compressed-tar"), QString("archive"));
        // Whatever reads as text is text: code, config and data formats too.
        for (const char *mime : {"application/x-ruby", "application/json", "application/x-shellscript",
                                 "application/toml", "application/x-yaml", "application/javascript"})
            QCOMPARE(FolderModel::kindForMime(mime), QString("text"));
        QCOMPARE(FolderModel::kindForMime("application/octet-stream"), QString("other"));
    }

    void backendKeepsHistoryAndCrumbs()
    {
        QTemporaryDir home;
        QDir(home.path()).mkpath("Pictures/Trip");
        Backend backend(home.path());
        backend.navigate(home.path());
        backend.navigate(home.filePath("Pictures/Trip"));
        QCOMPARE(backend.title(), QString("Trip"));
        const auto crumbs = backend.crumbs();
        QCOMPARE(crumbs.size(), 3);
        QCOMPARE(crumbs.first().toMap().value("name").toString(), QString("~"));

        backend.goUp();
        QCOMPARE(backend.path(), home.filePath("Pictures"));
        // Coming back up selects the folder we left.
        QCOMPARE(backend.takeSelection(), QString("Trip"));
        backend.back();
        QCOMPARE(backend.path(), home.filePath("Pictures/Trip"));
        QVERIFY(backend.canForward());
    }

    void backendStaysPutWhenTheHistoryIsGone()
    {
        QTemporaryDir home;
        QDir(home.path()).mkpath("Old");
        QDir(home.path()).mkpath("New");
        Backend backend(home.path());
        backend.navigate(home.filePath("Old"));
        backend.navigate(home.filePath("New"));
        QDir(home.filePath("Old")).removeRecursively();

        // Going back to a folder that's gone says so and leaves the history
        // on the folder shown, with what it was to select next untouched.
        backend.expectSelection("pasted.txt");
        backend.back();
        QCOMPARE(backend.path(), home.filePath("New"));
        QVERIFY(!backend.message().isEmpty());
        QVERIFY(backend.canBack());
        QVERIFY(!backend.canForward());
        QCOMPARE(backend.takeSelection(), QString("pasted.txt"));

        QDir(home.path()).mkpath("Old");
        backend.back();
        QCOMPARE(backend.path(), home.filePath("Old"));
        QDir(home.filePath("New")).removeRecursively();
        backend.forward();
        QCOMPARE(backend.path(), home.filePath("Old"));
        QVERIFY(!backend.canBack());
        QVERIFY(backend.canForward());
    }

    void backendCompletesTypedPaths()
    {
        QTemporaryDir home;
        for (const char *dir : {"Work/folio", "Work/forge", "Documents", ".config"})
            QDir(home.path()).mkpath(dir);
        const QString h = home.path();
        QCOMPARE(Backend::expandTyped("~/Work", h, h), h + "/Work");
        QCOMPARE(Backend::expandTyped("Work/folio", h, h), h + "/Work/folio");
        QCOMPARE(Backend::expandTyped("/etc", h, h), QString("/etc"));
        QCOMPARE(Backend::expandTyped("Work", h, "recent:"), h + "/Work");
        QCOMPARE(Backend::complete("~/Doc", h, h), QString("~/Documents/"));
        QCOMPARE(Backend::complete("~/Work/fo", h, h), QString("~/Work/fo"));   // folio or forge
        QCOMPARE(Backend::complete("~/Work/fol", h, h), QString("~/Work/folio/"));
        QCOMPARE(Backend::complete("~/.co", h, h), QString("~/.config/"));
        QCOMPARE(Backend::complete("~/c", h, h), QString("~/c"));               // hidden stays hidden
        QCOMPARE(Backend::complete("Wo", h, h), QString("Work/"));
        QCOMPARE(Backend::complete("Wo", h, "recent:"), QString("Work/"));
    }

    void backendKeepsTheWorkingDirectoryOnItsFolder()
    {
        const QString before = QDir::currentPath();
        const auto restore = qScopeGuard([before] { QDir::setCurrent(before); });
        QTemporaryDir home;
        QDir(home.path()).mkpath("Work");
        const QString work = QFileInfo(home.filePath("Work")).canonicalFilePath();
        Backend kept(home.path()), other(home.path());
        kept.navigate(work);
        QCOMPARE(QDir::currentPath(), before);   // Not until it's asked to.

        kept.keepWorkingDirectory();
        QCOMPARE(QDir::currentPath(), work);
        kept.navigate(home.path());
        QCOMPARE(QDir::currentPath(), QFileInfo(home.path()).canonicalFilePath());
        kept.navigate(work);
        // A view isn't a folder to work in: it stays where it was.
        kept.navigate(FolderModel::recentPath());
        QCOMPARE(QDir::currentPath(), work);
        // Only the pane used last moves it, until another one takes it.
        other.navigate(home.path());
        QCOMPARE(QDir::currentPath(), work);
        other.keepWorkingDirectory();
        QCOMPARE(QDir::currentPath(), QFileInfo(home.path()).canonicalFilePath());
        kept.navigate(work);
        QCOMPARE(QDir::currentPath(), QFileInfo(home.path()).canonicalFilePath());
    }

    void backendShowItemSelectsTheFile()
    {
        QTemporaryDir home;
        write(home.filePath("report.pdf"), "x");
        Backend backend(home.path());
        backend.showItem(home.filePath("report.pdf"));
        QCOMPARE(backend.path(), home.path());
        QCOMPARE(backend.takeSelection(), QString("report.pdf"));
    }

    void backendReadsDetailsOffTheUiThread()
    {
        // A picture's size and a text's opening lines are read in the
        // background: on a slow mount, reading them as the selection moves
        // would hold up the window.
        QTemporaryDir dir;
        const QString photo = dir.filePath("photo.png"), notes = dir.filePath("notes.txt");
        QVERIFY(QImage(40, 30, QImage::Format_RGB32).save(photo));
        write(notes, "hello");
        Backend backend(dir.path());
        backend.describe({{"path", photo}, {"kind", "image"}, {"mime", "image/png"}});
        QVERIFY(backend.details().isEmpty());
        QTRY_COMPARE(backend.details().value("width").toInt(), 40);
        QCOMPARE(backend.details().value("height").toInt(), 30);

        // The next item starts with nothing, and what was read for one left
        // behind is dropped.
        backend.describe({{"path", photo}, {"kind", "image"}, {"mime", "image/png"}});
        backend.describe({{"path", notes}, {"kind", "text"}, {"mime", "text/plain"}});
        QVERIFY(!backend.details().contains("width"));
        QTRY_COMPARE(backend.details().value("text").toString(), QString("hello"));
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QVERIFY(!backend.details().contains("width"));

        // A picture replaced by a folder of the same name doesn't keep the
        // picture's size.
        backend.describe({{"path", photo}, {"kind", "image"}, {"mime", "image/png"}});
        QTRY_VERIFY(backend.details().contains("width"));
        backend.describe({{"path", photo}, {"kind", "folder"}, {"mime", "inode/directory"}});
        QVERIFY(!backend.details().contains("width"));
    }

    void backendReadsTrashNotesOffTheUiThread()
    {
        // Finding a trashed item's note looks over every mounted drive, so
        // it's read with the rest of the details.
        QTemporaryDir home;
        const auto restore = dataHomeAt(home.path());
        QDir().mkpath(home.filePath("Trash/files"));
        QDir().mkpath(home.filePath("Trash/info"));
        write(home.filePath("Trash/files/old.pdf"), "x");
        write(home.filePath("Trash/info/old.pdf.trashinfo"),
              "[Trash Info]\nPath=" + home.filePath("Docs/old.pdf").toUtf8() + "\nDeletionDate=2026-11-10T12:00:00\n");
        Backend backend(home.path());
        backend.describe({{"path", home.filePath("Trash/files/old.pdf")}, {"kind", "pdf"}, {"mime", "application/pdf"}}, true);
        QVERIFY(!backend.details().contains("origin"));
        QTRY_COMPARE(backend.details().value("origin").toString(), home.filePath("Docs/old.pdf"));
        QCOMPARE(backend.details().value("deleted").toDateTime(), QDateTime(QDate(2026, 11, 10), QTime(12, 0)));
    }

    void mounterOutlivedByGiosReply()
    {
        // A local folder can't be mounted, so GIO answers with an error on
        // a later turn of the main loop, as it does for a slow tailnet
        // machine. By then the first Mounter's window has closed.
        QTemporaryDir dir;
        const QString uri = QUrl::fromLocalFile(dir.path()).toString();
        auto *closed = new Mounter;
        closed->mount(uri, false);
        delete closed;
        Mounter open;
        QSignalSpy failed(&open, &Mounter::failed);
        open.mount(uri, false);
        QVERIFY(failed.wait(2000));
        QVERIFY(!open.busy());
    }

    void trashFollowsXdgDataHome()
    {
        // g_file_trash puts things in $XDG_DATA_HOME/Trash, wherever that
        // is, so the sidebar's Trash opens there and is called "Trash".
        QTemporaryDir home, data;
        const auto restore = dataHomeAt(data.path());
        const QString trash = data.filePath("Trash/files");
        Places places(home.path());
        QCOMPARE(places.views().last().toMap().value("path").toString(), trash);
        Backend backend(home.path());
        backend.navigate(trash);
        QCOMPARE(backend.path(), trash);
        QCOMPARE(backend.title(), QString("Trash"));
    }

    void placesListEveryFolderInHome()
    {
        QTemporaryDir home;
        for (const char *dir : {"Work", "Documents", "omarchy", ".config", "project10", "project9"})
            QDir(home.path()).mkpath(dir);
        write(home.filePath("notes.txt"), "not a folder");
        auto names = [](const Places &p) {
            QStringList list;
            for (const auto &v : p.places()) list << v.toMap().value("name").toString();
            return list;
        };
        QCOMPARE(names(Places(home.path())),
                 QStringList({"Documents", "Omarchy", "project9", "project10", "Work"}));

        // The XDG folders have glyphs of their own; Dropbox's mark comes
        // from its plugin.
        QDir(home.path()).mkpath("Dropbox");
        const Places withDropbox(home.path());
        QCOMPARE(names(withDropbox).at(1), QString("Dropbox"));
        QCOMPARE(withDropbox.places().at(1).toMap().value("icon").toString(), QString("folder"));
        QCOMPARE(withDropbox.places().at(0).toMap().value("icon").toString(), QString("documents"));
    }

    void placesPinFoldersAsGtkBookmarks()
    {
        QTemporaryDir home;
        for (const char *dir : {"Work/folio", "Downloads", "Work/hype"})
            QDir(home.path()).mkpath(dir);
        const QString bookmarks = home.filePath(".config/gtk-3.0/bookmarks");
        QDir().mkpath(QFileInfo(bookmarks).absolutePath());
        // Omarchy's seeded home folders and a remote bookmark are kept in
        // the file but not shown as pins.
        write(bookmarks, QUrl::fromLocalFile(home.filePath("Downloads")).toEncoded() + " Downloads\n"
                         "sftp://studio/srv Studio\n"
                         + QUrl::fromLocalFile(home.filePath("Work/hype")).toEncoded() + " Slides\n");

        Places places(home.path());
        QCOMPARE(places.pinned().size(), 1);
        QCOMPARE(places.pinned().at(0).toMap().value("name").toString(), QString("Slides"));

        QCOMPARE(places.pin(home.filePath("Work/folio")), QString());
        QVERIFY(places.isPinned(home.filePath("Work/folio")));
        QCOMPARE(places.pinned().size(), 2);
        QVERIFY(!places.pin(home.filePath("Downloads")).isEmpty()); // already under Home
        // Hidden folders in home aren't listed under Home, so they can be
        // pinned.
        QCOMPARE(places.pin(home.filePath(".config")), QString());
        QVERIFY(places.isPinned(home.filePath(".config")));
        places.unpin(home.filePath(".config"));
        QVERIFY(!places.isPinned(home.filePath(".config")));

        places.unpin(home.filePath("Work/hype"));
        QFile file(bookmarks);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray saved = file.readAll();
        QVERIFY(saved.contains("sftp://studio/srv Studio"));
        QVERIFY(saved.contains("Downloads"));
        QVERIFY(saved.contains("Work/folio"));
        QVERIFY(!saved.contains("Work/hype"));
        QCOMPARE(places.pinned().size(), 1);
    }

    void typedViewsGoToTheirView()
    {
        // The path bar shows a view as its own name, so Return on it, or
        // the name typed, goes there rather than to a folder it isn't.
        QTemporaryDir home;
        Backend backend(home.path());
        backend.navigate(FolderModel::recentPath());
        QVERIFY(backend.goTyped(backend.typedPath()));
        QCOMPARE(backend.path(), FolderModel::recentPath());
        QVERIFY(backend.message().isEmpty());
        backend.navigate(home.path());
        QVERIFY(backend.goTyped(" recent: "));
        QCOMPARE(backend.path(), FolderModel::recentPath());
    }

    void mountsGoByTheirServersName()
    {
        // A plugin's item gives its mount its own name (see
        // pluginsFillTheirSectionMarksAndView); others go by their server.
        QCOMPARE(Backend::mountLabel("sftp:host=studio.example.ts.net,user=me"), QString("studio.example.ts.net"));
        QCOMPARE(Backend::mountLabel("sftp:host=files.example.com"), QString("files.example.com"));
        QCOMPARE(Backend::mountLabel("smb-share:server=nas,share=media"), QString("nas"));
        QCOMPARE(Backend::mountLabel("ftp:host=ftp.example.com"), QString("ftp.example.com"));
        QCOMPARE(Backend::mountLabel("dav:host=cloud.example.com,ssl=true,prefix=%2Fremote.php"), QString("cloud.example.com"));
    }

    void typedServerAddressesAreMounted()
    {
        // What's typed is what's mounted, cleaned up for GIO: GVfs knows
        // where on the mount the path is, which for WebDAV can be part of
        // the mount itself.
        QCOMPARE(Backend::serverAddress("sftp://nas"), QString("sftp://nas/"));
        QCOMPARE(Backend::serverAddress("  SFTP://me@nas:2222/srv/My Files "), QString("sftp://me@nas:2222/srv/My%20Files"));
        QCOMPARE(Backend::serverAddress("smb://nas/media/Films"), QString("smb://nas/media/Films"));
        QCOMPARE(Backend::serverAddress("ftp://ftp.example.com/pub/"), QString("ftp://ftp.example.com/pub/"));
        QCOMPARE(Backend::serverAddress("davs://cloud.example.com/remote.php/dav/files/me"),
                 QString("davs://cloud.example.com/remote.php/dav/files/me"));
        QCOMPARE(Backend::serverAddress("dav://10.0.0.5:8080"), QString("dav://10.0.0.5:8080/"));
        // Paths, other schemes and addresses without a server are not.
        for (const char *text : {"~/Work", "/etc", "Work/folio", "file:///etc", "http://example.com", "sftp://", "smb:/nas"})
            QVERIFY2(Backend::serverAddress(text).isEmpty(), text);
    }

    void pluginsReadTheirManifests()
    {
        QString error;
        const auto manifest = Plugins::parseManifest(R"({
            "folio": 1, "requires": ["tailscale"], "someday": true,
            "poll": {"command": ["./machines", "--all"], "every": 2},
            "section": {"title": "Tailnet", "rows": "opened", "limit": 3},
            "view": {"title": "Tailnet", "label": "Machines", "icon": "tailnet", "empty": "No machines are online"},
            "actions": [{"id": "taildrop", "label": "Taildrop", "menu": "share", "to": "items", "files": true,
                         "command": ["./send"], "busy": "Sending to %1…"},
                        {"id": "link", "label": "Link", "command": ["./link"], "under": ["~/Dropbox/"],
                         "types": ["image/png", "application/pdf"], "single": true, "detach": true}],
            "details": {"command": ["./status"], "under": ["~/Dropbox/"], "types": ["text/plain"]}})", &error);
        QVERIFY2(manifest, qPrintable(error));
        QCOMPARE(manifest->required, QStringList({"tailscale"}));
        QCOMPARE(manifest->poll, QStringList({"./machines", "--all"}));
        // Polling more often than every 5 seconds isn't allowed.
        QCOMPARE(manifest->every, 5);
        QVERIFY(manifest->section && manifest->opened);
        QCOMPARE(manifest->sectionTitle, QString("Tailnet"));
        QCOMPARE(manifest->limit, 3);
        QVERIFY(manifest->view);
        QCOMPARE(manifest->viewLabel, QString("Machines"));
        QCOMPARE(manifest->viewEmpty, QString("No machines are online"));
        QCOMPARE(manifest->actions.size(), 2);
        QVERIFY(manifest->actions[0].share && manifest->actions[0].toItems && manifest->actions[0].files);
        QVERIFY(!manifest->actions[0].single);
        QCOMPARE(manifest->actions[0].busy, QString("Sending to %1…"));
        // An action is in the menu itself unless it says Share.
        QVERIFY(!manifest->actions[1].share);
        QCOMPARE(manifest->actions[1].under, QStringList({"~/Dropbox/"}));
        QCOMPARE(manifest->actions[1].types, QStringList({"image/png", "application/pdf"}));
        QVERIFY(manifest->actions[1].single && manifest->actions[1].detach);
        QCOMPARE(manifest->details, QStringList({"./status"}));
        QCOMPARE(manifest->detailsUnder, QStringList({"~/Dropbox/"}));
        QCOMPARE(manifest->detailsTypes, QStringList({"text/plain"}));

        // Nor less often than once a day, which a timer can still count.
        QCOMPARE(Plugins::parseManifest(R"({"folio": 1, "poll": {"command": ["./x"], "every": 5000000}})", &error)->every,
                 86400);

        // The defaults: polled every 15 seconds, every item in the section.
        const auto plain = Plugins::parseManifest(R"({"folio": 1, "poll": {"command": ["./x"]}, "section": {"title": "X"}})", &error);
        QVERIFY(plain);
        QCOMPARE(plain->every, 15);
        QVERIFY(!plain->opened);
        QCOMPARE(plain->limit, 6);

        // What can't be followed is refused, saying why.
        for (const char *broken : {"not json", "[]", R"({"requires": []})", R"({"folio": 2})",
                                   R"({"folio": 1, "poll": {"command": []}})",
                                   R"({"folio": 1, "poll": {"command": "./x"}})",
                                   R"({"folio": 1, "requires": "tailscale"})",
                                   R"({"folio": 1, "section": {"title": "X", "rows": "opened"}})",
                                   R"({"folio": 1, "section": {"rows": "all"}})",
                                   R"({"folio": 1, "actions": [{"id": "a", "label": "A"}]})",
                                   R"({"folio": 1, "actions": [{"id": "a", "label": "A", "command": ["x"]},
                                                               {"id": "a", "label": "B", "command": ["y"]}]})",
                                   R"({"folio": 1, "actions": [{"id": "a", "label": "A", "command": ["x"], "menu": "top"}]})",
                                   R"({"folio": 1, "actions": [{"id": "a", "label": "A", "command": ["x"], "types": "image/png"}]})",
                                   R"({"folio": 1, "actions": {"id": "a", "label": "A", "command": ["x"]}})",
                                   R"({"folio": 1, "details": {"under": ["~/Dropbox/"]}})",
                                   R"({"folio": 1, "details": {"command": ["./x"], "types": [""]}})"}) {
            error.clear();
            QVERIFY2(!Plugins::parseManifest(broken, &error), broken);
            QVERIFY2(!error.isEmpty(), broken);
        }
    }

    void pluginsSkipWhatCantRun()
    {
        QTemporaryDir folder;
        writePlugin(folder.path(), "good", R"({"folio": 1})");
        writePlugin(folder.path(), "broken", "{");
        writePlugin(folder.path(), "needs-more", R"({"folio": 1, "requires": ["folio-no-such-program"]})");
        // Only lowercase letters, digits and dashes make an id.
        writePlugin(folder.path(), "Not_An_Id", R"({"folio": 1})");
        QDir(folder.path()).mkpath("no-manifest");
        // A folder anyone may write to could hold anyone's commands.
        writePlugin(folder.path(), "open-to-all", R"({"folio": 1})");
        QFile::setPermissions(folder.filePath("open-to-all"), QFile::permissions(folder.filePath("open-to-all"))
                                                                  | QFileDevice::WriteOther);
        // Nor could one a group may write to, or whose manifest others may
        // change.
        writePlugin(folder.path(), "open-to-group", R"({"folio": 1})");
        QFile::setPermissions(folder.filePath("open-to-group"), QFile::permissions(folder.filePath("open-to-group"))
                                                                    | QFileDevice::WriteGroup);
        writePlugin(folder.path(), "open-manifest", R"({"folio": 1})");
        QFile::setPermissions(folder.filePath("open-manifest/manifest.json"),
                              QFile::permissions(folder.filePath("open-manifest/manifest.json")) | QFileDevice::WriteGroup);
        QStringList ids;
        for (const auto &m : Plugins::read({folder.path(), folder.filePath("missing")}, "en"))
            ids << m.id;
        QCOMPARE(ids, QStringList({"good"}));
    }

    void pluginCommandsOthersMayChangeArentRun()
    {
        QTemporaryDir folder;
        writePlugin(folder.path(), "shared", R"({"folio": 1})",
                    {{"mine", "#!/bin/sh\necho '{}'\n"}, {"theirs", "#!/bin/sh\necho '{}'\n"},
                     {"bin/inside", "#!/bin/sh\necho '{}'\n"}});
        const auto plugin = Plugins::read({folder.path()}, "en").value(0);
        QCOMPARE(plugin.id, QString("shared"));
        const auto env = QProcessEnvironment::systemEnvironment();
        QVERIFY(Plugins::execute(plugin, {"./mine"}, env, 5000).ok());
        QVERIFY(Plugins::execute(plugin, {"bin/inside"}, env, 5000).ok());
        // A program a group may change, one in a folder a group may change,
        // or one outside the plugin's folder isn't run.
        QFile::setPermissions(folder.filePath("shared/theirs"),
                              QFile::permissions(folder.filePath("shared/theirs")) | QFileDevice::WriteGroup);
        QFile::setPermissions(folder.filePath("shared/bin"),
                              QFile::permissions(folder.filePath("shared/bin")) | QFileDevice::WriteGroup);
        QVERIFY(!Plugins::execute(plugin, {"./theirs"}, env, 5000).started);
        QVERIFY(!Plugins::execute(plugin, {"bin/inside"}, env, 5000).started);
        QFile outside(folder.filePath("outside"));
        QVERIFY(outside.open(QIODevice::WriteOnly));
        outside.write("#!/bin/sh\necho '{}'\n");
        outside.close();
        outside.setPermissions(outside.permissions() | QFileDevice::ExeOwner);
        QVERIFY(!Plugins::execute(plugin, {"../outside"}, env, 5000).started);
    }

    void pluginsOfTheUserReplacePackagedOnes()
    {
        QTemporaryDir packaged, user;
        writePlugin(packaged.path(), "dropbox", R"({"folio": 1, "section": {"title": "Packaged"}})");
        writePlugin(packaged.path(), "tailscale", R"({"folio": 1})");
        writePlugin(user.path(), "dropbox", R"({"folio": 1, "section": {"title": "Mine"}})");
        const auto plugins = Plugins::read({packaged.path(), user.path()}, "en");
        QCOMPARE(plugins.size(), 2);
        QCOMPARE(plugins[0].id, QString("dropbox"));
        QCOMPARE(plugins[0].sectionTitle, QString("Mine"));
        QCOMPARE(plugins[0].dir, user.filePath("dropbox"));
        QCOMPARE(plugins[1].dir, packaged.filePath("tailscale"));
    }

    void pluginFoldersAreAbsolute()
    {
        // A relative folder (FOLIO_PLUGINS_PATH=plugins) is made absolute
        // once: commands run in their plugin's folder, so it would otherwise
        // be resolved twice.
        QTemporaryDir parent;
        QDir(parent.path()).mkpath("rel");
        writePlugin(parent.filePath("rel"), "demo", R"({"folio": 1})");
        const QString before = QDir::currentPath();
        QDir::setCurrent(parent.path());
        const auto plugins = Plugins::read({"rel"}, "en");
        QDir::setCurrent(before);
        QCOMPARE(plugins.size(), 1);
        QCOMPARE(plugins[0].dir, parent.filePath("rel/demo"));
    }

    void mountsRememberWhereGVfsPutThem()
    {
        // Only SFTP's folder can be worked out ahead; any other address is
        // known once it has been mounted, so its row can light up.
        QVERIFY(Mounter::folderFor("smb://nas/media").isEmpty());
        const QString folder = Mounter::root() + "/smb-share:server=nas,share=media";
        Mounter::known()["smb://nas/media"] = folder;
        QCOMPARE(Mounter::folderFor("smb://nas/media"), folder);
        QCOMPARE(Mounter::destinationFor("smb://nas/media"), folder);
        // A folder inside a share opens there, and its row lights up on
        // the share's.
        Mounter::known()["smb://nas/media/films"] = folder + "/films";
        QCOMPARE(Mounter::destinationFor("smb://nas/media/films"), folder + "/films");
        QCOMPARE(Mounter::folderFor("smb://nas/media/films"), folder);
        Mounter::known().remove("smb://nas/media");
        Mounter::known().remove("smb://nas/media/films");
        QVERIFY(Mounter::destinationFor("smb://nas/media").isEmpty());
        QCOMPARE(Mounter::destinationFor("sftp://studio/srv/www"), Mounter::root() + "/sftp:host=studio/srv/www");
        QCOMPARE(Mounter::destinationFor("sftp://studio/"), Mounter::root() + "/sftp:host=studio");
        // ".." stops at the server's top, never climbing out of the mount to
        // a local folder.
        QCOMPARE(Mounter::destinationFor("sftp://nas/../../../../../tmp"), Mounter::root() + "/sftp:host=nas/tmp");
        QCOMPARE(Mounter::destinationFor("sftp://nas/srv/../.."), Mounter::root() + "/sftp:host=nas");
        QCOMPARE(Mounter::folderFor("sftp://me@studio:2222/"), Mounter::root() + "/sftp:host=studio,port=2222,user=me");
    }

    void pluginCommandsRunInTheirFolderAndEndWhole()
    {
        QTemporaryDir folder;
        writePlugin(folder.path(), "slow", R"({"folio": 1})",
                    {{"where", "#!/bin/sh\necho \"{\\\"dir\\\": \\\"$PWD\\\", \\\"plugin\\\": \\\"$FOLIO_PLUGIN_DIR\\\", "
                               "\\\"language\\\": \\\"$FOLIO_LANGUAGE\\\", \\\"args\\\": \\\"$*\\\"}\"\n"
                               "echo 'first' >&2\necho 'last words' >&2\nread line && echo stdin >&2\nexit 3\n"},
                     {"hang", "#!/bin/sh\nsleep 30 &\necho $! > \"$1\"\nwait\n"},
                     {"leave", "#!/bin/sh\nsleep 30 >/dev/null 2>&1 &\necho $! > \"$1\"\n"}});
        const auto plugins = Plugins::read({folder.path()}, "da");
        QCOMPARE(plugins.size(), 1);
        const auto &plugin = plugins[0];
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert("FOLIO_PLUGIN_DIR", plugin.dir);
        env.insert("FOLIO_LANGUAGE", "da");

        // In the plugin's folder, with its arguments intact and nothing on
        // standard input; standard error's last line is kept.
        const Plugins::Run run = Plugins::execute(plugin, {"./where", "a b"}, env, 5000);
        QVERIFY(run.started);
        QCOMPARE(run.code, 3);
        QVERIFY(!run.ok());
        const QJsonObject said = QJsonDocument::fromJson(run.out).object();
        QCOMPARE(QFileInfo(said.value("dir").toString()).canonicalFilePath(), QFileInfo(plugin.dir).canonicalFilePath());
        QCOMPARE(said.value("plugin").toString(), plugin.dir);
        QCOMPARE(said.value("language").toString(), QString("da"));
        QCOMPARE(said.value("args").toString(), QString("a b"));
        QCOMPARE(run.lastError, QString("last words"));

        // One that takes too long is ended, and so is what it started.
        const QString pidFile = folder.filePath("child");
        QElapsedTimer clock;
        clock.start();
        const Plugins::Run hung = Plugins::execute(plugin, {"./hang", pidFile}, env, 300);
        QVERIFY(hung.timedOut);
        QVERIFY(!hung.ok());
        QVERIFY(clock.elapsed() < 3000);
        QFile file(pidFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const pid_t child = file.readAll().trimmed().toInt();
        QVERIFY(child > 0);
        QTRY_VERIFY(ended(child));

        // One that finishes takes what it left running with it.
        const Plugins::Run left = Plugins::execute(plugin, {"./leave", pidFile}, env, 5000);
        QVERIFY(left.ok());
        QFile leftFile(pidFile);
        QVERIFY(leftFile.open(QIODevice::ReadOnly));
        const pid_t helper = leftFile.readAll().trimmed().toInt();
        QVERIFY(helper > 0 && helper != child);
        QTRY_VERIFY(ended(helper));

        // A program that isn't there never starts.
        QVERIFY(!Plugins::execute(plugin, {"folio-no-such-program"}, env, 1000).started);
    }

    void pluginCommandsThatFloodAreHeldToLittleMemory()
    {
        QTemporaryDir folder;
        writePlugin(folder.path(), "floods", R"({"folio": 1})");
        const auto plugin = Plugins::read({folder.path()}, "en").value(0);
        const auto env = QProcessEnvironment::systemEnvironment();
        auto peak = [] {
            rusage usage;
            getrusage(RUSAGE_SELF, &usage);
            return usage.ru_maxrss / 1024;
        };
        const long before = peak();

        // Standard output is ended soon after the limit, not read on.
        const Plugins::Run out = Plugins::execute(plugin, {"sh", "-c", "exec cat /dev/zero"}, env, 5000);
        QVERIFY(out.tooLong && !out.ok());
        QVERIFY(out.out.size() < 2 * Plugins::maxOutput);

        // Standard error without line breaks is cut into lines of its own,
        // not gathered up for as long as the command runs.
        const Plugins::Run err = Plugins::execute(plugin, {"sh", "-c", "exec cat /dev/zero >&2"}, env, 1000);
        QVERIFY(err.timedOut);
        QVERIFY(err.lastError.size() <= 1000);
        QVERIFY2(peak() - before < 100, qPrintable(QString("grew by %1 MB").arg(peak() - before)));
    }

    void mountsAreFoundWhereGvfsPutsThem()
    {
        // GVfs names an SFTP mount by the URL's parts, in key order.
        QCOMPARE(Mounter::folderFor("sftp://studio.example.ts.net/"), Mounter::root() + "/sftp:host=studio.example.ts.net");
        QCOMPARE(Mounter::folderFor("sftp://me@nas:2222/srv"), Mounter::root() + "/sftp:host=nas,port=2222,user=me");
        QCOMPARE(Mounter::folderFor("smb://nas/share"), QString());
    }

    void pluginIconsTakeTheThemesColor()
    {
        QTemporaryDir folder;
        const QString svg = folder.filePath("mark.svg");
        QFile file(svg);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path fill='#00ff00' d='M0 0h12v24H0z'/></svg>");
        file.close();
        PluginIconProvider provider;
        QSize size;
        const QImage image = provider.requestImage("ff0000/" + QUrl::toPercentEncoding(svg), &size, QSize(24, 24));
        QCOMPARE(size, QSize(24, 24));
        // Drawn in the color asked for, whatever color the SVG has, and
        // clear where the SVG draws nothing.
        QCOMPARE(image.pixelColor(4, 12), QColor(255, 0, 0));
        QCOMPARE(image.pixelColor(20, 12).alpha(), 0);
        // A file that can't be read is an empty square, not an error.
        QCOMPARE(provider.requestImage("ff0000/%2Fnowhere.svg", nullptr, QSize(24, 24)).pixelColor(4, 12).alpha(), 0);
    }

    void pluginOutputIsReadOrRefused()
    {
        const auto output = Plugins::parseOutput(R"({
            "items": [{"id": "studio.example.ts.net", "name": "studio", "icon": "peer", "subtitle": "Linux",
                       "details": [ ["Host", "studio.example.ts.net"] ], "open": {"mount": "sftp:\/\/studio.example.ts.net\/"}},
                      {"id": "no-name"}, {"name": "no id"},
                      {"id": "studio.example.ts.net", "name": "twice"}],
            "places": [{"path": "~/Dropbox", "icon": "dropbox.svg", "status": "syncing", "tone": "warning"}, {"status": "no path"}],
            "actions": ["link"]})");
        QVERIFY(output);
        // Items without an id or a name, or with an id already listed, are left out.
        QCOMPARE(output->items.size(), 1);
        QCOMPARE(output->items[0].name, QString("studio"));
        QCOMPARE(output->items[0].open.value("mount").toString(), QString("sftp://studio.example.ts.net/"));
        QCOMPARE(output->places.size(), 1);
        QCOMPARE(output->places[0].tone, QString("warning"));
        QCOMPARE(output->actions, std::optional<QStringList>(QStringList{"link"}));
        QVERIFY(!Plugins::parseOutput("{}")->actions);
        QVERIFY(!Plugins::parseOutput("garbage"));
        QVERIFY(!Plugins::parseOutput("[1, 2]"));
        QVERIFY(!Plugins::parseOutput(""));
    }

    void pluginsTranslateWithTheirOwnCatalog()
    {
        QTemporaryDir folder;
        writePlugin(folder.path(), "servers", R"({"folio": 1})",
                    {{"i18n/pt.json", R"({"Servers": "Servidores", "Sending to %1 failed: %2": "Falha ao enviar para %1: %2"})"},
                     {"i18n/pt-BR.json", R"({"Servers": "Servidores BR"})"}});
        const auto plugin = Plugins::read({folder.path()}, "pt-BR").value(0);
        // The regional catalog first, then the base language's, then English.
        QCOMPARE(Plugins::text(plugin, QJsonValue("Servers")), QString("Servidores BR"));
        QCOMPARE(Plugins::text(plugin, QJsonValue("Upload")), QString("Upload"));
        // A template's values are filled in as they are, even ones that
        // look like a template's places.
        QCOMPARE(Plugins::text(plugin, QJsonArray{"Sending to %1 failed: %2", "studio", "no %1 here"}),
                 QString("Falha ao enviar para studio: no %1 here"));
        QCOMPARE(Plugins::text(plugin, QJsonArray{"%1 of %2", 3, 7}), QString("3 of 7"));
    }

    void pluginsFillTheirSectionMarksAndView()
    {
        QTemporaryDir folder, home;
        // Text the plugin prints is translated as it's shown, so a change
        // of language needs no new poll.
        writePlugin(folder.path(), "servers", R"({"folio": 1,
            "poll": {"command": ["./servers"]},
            "section": {"title": "Servers"},
            "view": {"title": "All servers", "label": "Everything", "icon": "server.svg", "empty": "No servers"}})",
                    {{"servers", "#!/bin/sh\necho poll >> polls\ncat <<EOF\n"
                                 "{\"items\": [{\"id\": \"b\", \"name\": \"beta\", \"icon\": \"server.svg\", \"subtitle\": \"SFTP\","
                                 " \"details\": [[\"Address\", \"sftp://beta/\"]], \"open\": {\"path\": \"" + home.path().toUtf8() + "\"}},"
                                 " {\"id\": \"a\", \"name\": \"alpha\", \"icon\": \"missing.svg\", \"open\": {\"mount\": \"sftp://alpha/\"}}],"
                                 " \"places\": [{\"path\": \"" + home.path().toUtf8() + "/Dropbox\", \"icon\": \"server.svg\", \"status\": [\"%1 left\", 3], \"tone\": \"warning\"}]}\n"
                                 "EOF\n"},
                     {"server.svg", "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24'><path d='M4 4h16v16H4z'/></svg>"},
                     {"i18n/da.json", R"({"Servers": "Servere", "Everything": "Alle", "Address": "Adresse", "%1 left": "%1 tilbage"})"}});
        // One that hangs and one that prints garbage don't hold up the
        // others, and show nothing.
        writePlugin(folder.path(), "hangs", R"({"folio": 1, "poll": {"command": ["sleep", "60"]}, "section": {"title": "Hangs"}})");
        writePlugin(folder.path(), "garbage", R"({"folio": 1, "poll": {"command": ["./poll"]}, "section": {"title": "Garbage"}})",
                    {{"poll", "#!/bin/sh\necho 'not json'\n"}});

        Plugins plugins("da", {folder.path()}, home.filePath("folio.conf"));
        QTRY_COMPARE(plugins.sections().size(), 1);
        const QVariantMap section = plugins.sections().at(0).toMap();
        QCOMPARE(section.value("id").toString(), QString("servers"));
        QCOMPARE(section.value("title").toString(), QString("Servere"));
        // Every item, in the plugin's order, then the view's row with how
        // many there are.
        const QVariantList rows = section.value("rows").toList();
        QCOMPARE(namesOf(rows), QStringList({"beta", "alpha"}));
        QCOMPARE(rows[0].toMap().value("path").toString(), QString("plugin:servers/b"));
        QCOMPARE(rows[0].toMap().value("icon").toString(), folder.filePath("servers/server.svg"));
        QCOMPARE(rows[1].toMap().value("icon").toString(), QString("file"));
        // A local folder takes drops and is lit while you're in it; a mount
        // is lit inside GVfs's folder for it.
        QCOMPARE(rows[0].toMap().value("folder").toString(), home.path());
        QCOMPARE(rows[0].toMap().value("root").toString(), home.path());
        QVERIFY(rows[1].toMap().value("root").toString().endsWith("/gvfs/sftp:host=alpha"));
        QVERIFY(!rows[0].toMap().value("forgettable").toBool());
        const QVariantMap viewRow = section.value("view").toMap();
        QCOMPARE(viewRow.value("name").toString(), QString("Alle"));
        QCOMPARE(viewRow.value("path").toString(), QString("plugin:servers"));
        QCOMPARE(viewRow.value("count").toInt(), 2);

        const QVariantMap mark = plugins.marks().value(home.path() + "/Dropbox").toMap();
        QCOMPARE(mark.value("status").toString(), QString("3 tilbage"));
        QCOMPARE(mark.value("tone").toString(), QString("warning"));
        QCOMPARE(mark.value("icon").toString(), folder.filePath("servers/server.svg"));

        QVERIFY(plugins.hasView("plugin:servers"));
        QVERIFY(!plugins.hasView("plugin:hangs"));
        const QVariantMap view = plugins.views().value("plugin:servers").toMap();
        QCOMPARE(view.value("title").toString(), QString("All servers"));
        QCOMPARE(view.value("empty").toString(), QString("No servers"));
        const QVariantMap beta = view.value("items").toList().at(0).toMap();
        QCOMPARE(beta.value("subtitle").toString(), QString("SFTP"));
        QCOMPARE(beta.value("details").toList().at(0).toList(), QVariantList({"Adresse", "sftp://beta/"}));
        QCOMPARE(plugins.item("plugin:servers/a")->name, QString("alpha"));
        QVERIFY(!plugins.item("plugin:servers/nope"));
        QCOMPARE(plugins.nameForMount(rows[1].toMap().value("root").toString()), QString("alpha"));

        // Switching language changes the text without polling again.
        QFile polls(folder.filePath("servers/polls"));
        QVERIFY(polls.open(QIODevice::ReadOnly));
        const QByteArray before = polls.readAll();
        polls.close();
        QSignalSpy changed(&plugins, &Plugins::changed);
        plugins.setLanguage("en");
        QTRY_COMPARE(plugins.sections().value(0).toMap().value("title").toString(), QString("Servers"));
        QVERIFY(polls.open(QIODevice::ReadOnly));
        QCOMPARE(polls.readAll(), before);
    }

    void pluginSectionsRememberOpenedItems()
    {
        QTemporaryDir folder, home;
        QByteArray items = "{\"items\": [";
        for (int i = 0; i < 10; ++i)
            items += QByteArray(i ? "," : "") + "{\"id\": \"m" + QByteArray::number(i) + "\", \"name\": \"machine" + QByteArray::number(i) + "\"}";
        items += "]}";
        writePlugin(folder.path(), "tailscale", R"({"folio": 1, "poll": {"command": ["./machines"]},
            "section": {"title": "Tailnet", "rows": "opened"}, "view": {"title": "Tailnet", "label": "Machines"}})",
                    {{"machines", "#!/bin/sh\ncat <<'EOF'\n" + items + "\nEOF\n"}});
        // What older versions remembered of the tailnet carries over.
        const QString conf = home.filePath("folio.conf");
        {
            QSettings settings(conf, QSettings::IniFormat);
            settings.beginWriteArray("opened-machines", 2);
            settings.setArrayIndex(0);
            settings.setValue("host", "m3");
            settings.setValue("when", "2026-11-01T10:00:00");
            settings.setArrayIndex(1);
            settings.setValue("host", "gone");
            settings.setValue("when", "2026-11-01T09:00:00");
            settings.endArray();
        }
        Plugins plugins("en", {folder.path()}, conf);
        QCOMPARE(plugins.opened("tailscale"), QStringList({"m3", "gone"}));
        QVERIFY(!QSettings(conf, QSettings::IniFormat).contains("opened-machines/size"));

        // Only the opened ones that are among the items show, most recent
        // first, each with a ✕, then the row for the view.
        auto rows = [&] { return plugins.sections().value(0).toMap().value("rows").toList(); };
        QTRY_COMPARE(namesOf(rows()), QStringList({"machine3"}));
        QVERIFY(rows()[0].toMap().value("forgettable").toBool());
        QCOMPARE(plugins.sections().value(0).toMap().value("view").toMap().value("count").toInt(), 10);
        plugins.remember("plugin:tailscale/m1");
        plugins.remember("plugin:tailscale/m3");
        QCOMPARE(namesOf(rows()), QStringList({"machine3", "machine1"}));
        // A handful at most.
        for (int i = 0; i < 10; ++i)
            plugins.remember(QString("plugin:tailscale/m%1").arg(i));
        QCOMPARE(namesOf(rows()), QStringList({"machine9", "machine8", "machine7", "machine6", "machine5", "machine4"}));
        plugins.forget("tailscale", "m9");
        QCOMPARE(namesOf(rows()).first(), QString("machine8"));
        // Kept in the settings, the 20 most recent.
        for (int i = 0; i < 25; ++i)
            plugins.remember(QString("plugin:tailscale/x%1").arg(i));
        QCOMPARE(plugins.opened("tailscale").size(), 20);
        QCOMPARE(Plugins("en", {folder.filePath("none")}, conf).opened("tailscale").first(), QString("x24"));
        QCOMPARE(rows().size(), 0);

        // When the plugin goes away, its section goes but its ids stay.
        plugins.remember("plugin:tailscale/m2");
        QFile::remove(folder.filePath("tailscale/manifest.json"));
        plugins.refresh();
        QTRY_COMPARE(plugins.sections().size(), 0);
        QCOMPARE(plugins.opened("tailscale").first(), QString("m2"));
    }

    void pluginActionsUnderShareFollowTheirConditions()
    {
        QTemporaryDir folder, files;
        QDir(files.path()).mkpath("Dropbox/folder");
        write(files.filePath("Dropbox/a.txt"), "a");
        write(files.filePath("Dropbox/b.txt"), "b");
        write(files.filePath("c.txt"), "c");
        const QString under = files.path() + "/Dropbox/";
        writePlugin(folder.path(), "zeta", R"({"folio": 1, "poll": {"command": ["./poll"]},
            "actions": [{"id": "send", "label": "Send", "menu": "share", "to": "items", "files": true, "command": ["./send"], "busy": "Sending to %1…"}]})",
                    {{"poll", "#!/bin/sh\necho '{\"items\": [{\"id\": \"s.example\", \"name\": \"studio\"}]}'\n"},
                     {"send", "#!/bin/sh\nprintf '%s|' \"$FOLIO_TARGET\" \"$FOLIO_TARGET_NAME\" \"$@\" > sent\n"
                              "echo '{\"notice\": [\"Sent to %1.\", \"'\"$FOLIO_TARGET_NAME\"'\"]}'\n"}});
        writePlugin(folder.path(), "alpha", "{\"folio\": 1, \"poll\": {\"command\": [\"./poll\"]}, \"actions\": ["
                                            "{\"id\": \"link\", \"label\": \"Link\", \"menu\": \"share\", \"under\": [\"" + under.toUtf8() + "\"], \"single\": true, \"command\": [\"./link\"]},"
                                            "{\"id\": \"off\", \"label\": \"Off\", \"menu\": \"share\", \"command\": [\"./link\"]}]}",
                    {{"poll", "#!/bin/sh\necho '{\"actions\": [\"link\"]}'\n"},
                     {"link", "#!/bin/sh\necho \"no link for $1\" >&2\nexit 1\n"}});
        writePlugin(folder.path(), "beta", R"({"folio": 1, "actions": [{"id": "copy", "label": "Copy It", "menu": "share", "command": ["./copy"]}]})",
                    {{"copy", "#!/bin/sh\necho '{\"copy\": \"https://example.com/x\", \"error\": [\"%1 is too big\", \"x\"]}'\n"}});

        Plugins plugins("en", {folder.path()}, files.filePath("folio.conf"));
        auto labels = [&](const QStringList &paths) {
            QStringList list;
            for (const QVariant &v : plugins.actionsFor(paths))
                list << v.toMap().value("label").toString();
            return list;
        };
        // Entries wait for their plugin's poll, and only those it names show.
        QTRY_COMPARE(labels({files.filePath("Dropbox/a.txt")}), QStringList({"Link", "Copy It", "Send"}));
        // Plain entries first, by plugin id, then submenus with their targets.
        const QVariantMap send = plugins.actionsFor({files.filePath("c.txt")}).last().toMap();
        QCOMPARE(send.value("targets").toList().at(0).toMap().value("name").toString(), QString("studio"));
        QCOMPARE(labels({files.filePath("c.txt")}), QStringList({"Copy It", "Send"}));
        QCOMPARE(labels({files.filePath("Dropbox/a.txt"), files.filePath("Dropbox/b.txt")}), QStringList({"Copy It", "Send"}));
        QCOMPARE(labels({files.filePath("Dropbox/folder")}), QStringList({"Link", "Copy It"}));
        QCOMPARE(labels({files.path() + "/Dropbox"}), QStringList({"Copy It"}));

        // An entry gets the paths as arguments and the target in its
        // environment, and says what to do when it's done.
        std::optional<Plugins::Result> result;
        const QString busy = plugins.perform("zeta", "send", {files.filePath("c.txt"), files.filePath("Dropbox/a.txt")},
                                           "s.example", [&](const Plugins::Result &r) { result = r; });
        QCOMPARE(busy, QString("Sending to studio…"));
        QTRY_VERIFY(result);
        QCOMPARE(result->notice, QString("Sent to studio."));
        QVERIFY(result->error.isEmpty());
        QFile sent(folder.filePath("zeta/sent"));
        QVERIFY(sent.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(sent.readAll()),
                 "s.example|studio|" + files.filePath("c.txt") + "|" + files.filePath("Dropbox/a.txt") + "|");

        // A failure without an error of its own shows its last word on
        // standard error after the entry's label.
        result.reset();
        plugins.perform("alpha", "link", {files.filePath("Dropbox/a.txt")}, {}, [&](const Plugins::Result &r) { result = r; });
        QTRY_VERIFY(result);
        QCOMPARE(result->error, "Link: no link for " + files.filePath("Dropbox/a.txt"));
        // An error printed is a failure even with a zero exit, and a
        // failure does nothing else it asks.
        result.reset();
        plugins.perform("beta", "copy", {files.filePath("c.txt")}, {}, [&](const Plugins::Result &r) { result = r; });
        QTRY_VERIFY(result);
        QCOMPARE(result->error, QString("x is too big"));
        QCOMPARE(result->copy, QString());
    }

    void pluginActionsFollowTheirTypesAndMenus()
    {
        QTemporaryDir folder, files;
        QDir(files.path()).mkpath("repo/sub");
        for (const char *name : {"repo/a.txt", "repo/main.c", "repo/photo.png", "repo/report.pdf", "elsewhere.txt"})
            write(files.filePath(name), "x");
        const QByteArray repo = files.filePath("repo").toUtf8() + "/";
        writePlugin(folder.path(), "git", "{\"folio\": 1, \"actions\": ["
            "{\"id\": \"link\", \"label\": \"Copy Git Link\", \"under\": [\"" + repo + "\"], \"types\": [\"text/plain\"],"
            " \"single\": true, \"command\": [\"./link\"]},"
            "{\"id\": \"thumb\", \"label\": \"Make Thumbnail\", \"types\": [\"image/*\", \"application/pdf\"], \"files\": true,"
            " \"command\": [\"./link\"]},"
            "{\"id\": \"mail\", \"label\": \"Mail\", \"menu\": \"share\", \"command\": [\"./link\"]}]}",
                    {{"link", "#!/bin/sh\necho \"{\\\"copy\\\": \\\"https://git.example.com/$(basename \"$1\")\\\"}\"\n"}});
        writePlugin(folder.path(), "hosts", R"({"folio": 1, "poll": {"command": ["./poll"], "every": 86400},
            "actions": [{"id": "copy", "label": "Copy To", "to": "items", "command": ["./link"]}]})",
                    {{"poll", "#!/bin/sh\necho '{\"items\": [{\"id\": \"nas\", \"name\": \"nas\"}]}'\n"}});

        Plugins plugins("en", {folder.path()}, files.filePath("folio.conf"));
        // Each as menu:label, in the order they come.
        auto actions = [&](const QStringList &names) {
            QStringList paths, list;
            for (const QString &name : names)
                paths << files.filePath(name);
            for (const QVariant &v : plugins.actionsFor(paths))
                list << v.toMap().value("menu").toString() + ":" + v.toMap().value("label").toString();
            return list;
        };
        // Plain entries first, by plugin id, then submenus, wherever they go.
        QTRY_COMPARE(actions({"repo/a.txt"}), QStringList({"main:Copy Git Link", "share:Mail", "main:Copy To"}));
        // A type takes the types that are kinds of it: C is plain text.
        QCOMPARE(actions({"repo/main.c"}), QStringList({"main:Copy Git Link", "share:Mail", "main:Copy To"}));
        QCOMPARE(actions({"elsewhere.txt"}), QStringList({"share:Mail", "main:Copy To"}));
        QCOMPARE(actions({"repo/photo.png"}), QStringList({"main:Make Thumbnail", "share:Mail", "main:Copy To"}));
        QCOMPARE(actions({"repo/photo.png", "repo/report.pdf"}), QStringList({"main:Make Thumbnail", "share:Mail", "main:Copy To"}));
        // Every item has to fit, and a folder is of no type but its own.
        QCOMPARE(actions({"repo/photo.png", "repo/a.txt"}), QStringList({"share:Mail", "main:Copy To"}));
        QCOMPARE(actions({"repo/sub"}), QStringList({"share:Mail", "main:Copy To"}));

        // One in the menu itself runs as one under Share does.
        std::optional<Plugins::Result> result;
        plugins.perform("git", "link", {files.filePath("repo/a.txt")}, {}, [&](const Plugins::Result &r) { result = r; });
        QTRY_VERIFY(result);
        QCOMPARE(result->copy, QString("https://git.example.com/a.txt"));
    }

    void fileMenuPutsPluginActionsBeforeShare()
    {
        QTemporaryDir home, folder;
        write(home.filePath("notes.txt"), "x");
        write(home.filePath("photo.png"), "x");
        writePlugin(folder.path(), "git", R"({"folio": 1, "actions": [
            {"id": "link", "label": "Copy Git Link", "types": ["text/plain"], "command": ["true"]},
            {"id": "mail", "label": "Mail", "menu": "share", "command": ["true"]}]})");
        writePlugin(folder.path(), "hosts", R"({"folio": 1, "poll": {"command": ["./poll"], "every": 86400},
            "actions": [{"id": "copy", "label": "Copy To", "to": "items", "command": ["true"]}]})",
                    {{"poll", "#!/bin/sh\necho '{\"items\": [{\"id\": \"nas\", \"name\": \"nas\"}]}'\n"}});
        Plugins plugins("en", {folder.path()}, home.filePath("folio.conf"));
        QTRY_COMPARE(plugins.actionsFor({home.filePath("notes.txt")}).size(), 3);
        Backend backend(home.path());
        backend.setPlugins(&plugins);
        Theme theme(home.filePath("theme"));
        FileOps fileOps;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("fileOps", &fileOps);
        engine.rootContext()->setContextProperty("plugins", &plugins);
        engine.rootContext()->setContextProperty("translator", QVariantMap{{"language", "en"}});
        QQmlComponent component(&engine);
        component.setData("import QtQuick\n"
                          "Window {\n"
                          "    width: 800; height: 600; visible: true\n"
                          "    FileMenu { id: menu }\n"
                          "    function entries(path, mime) {\n"
                          "        menu.show({ path: path, name: path.split('/').pop(), isDir: false, mime: mime },\n"
                          "                  path.replace(/\\/[^\\/]*$/, ''))\n"
                          "        const shown = []\n"
                          "        for (let i = 0; i < menu.count; ++i) {\n"
                          "            const entry = menu.itemAt(i)\n"
                          "            if (entry.visible && entry.text) shown.push(entry.text)\n"
                          "        }\n"
                          "        menu.close()\n"
                          "        return shown\n"
                          "    }\n"
                          "}\n",
                          QUrl::fromLocalFile(sourceFolder() + "/FileMenuTest.qml"));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        auto around = [&](const QString &name, const QString &mime) {
            QVariant shown;
            QMetaObject::invokeMethod(window.get(), "entries", Q_RETURN_ARG(QVariant, shown),
                                      Q_ARG(QVariant, home.filePath(name)), Q_ARG(QVariant, mime));
            // What comes between Compress and Share.
            const QStringList all = shown.toStringList();
            const qsizetype from = all.indexOf("Compress"), to = all.indexOf("Share");
            return from < 0 || to < from ? all : all.mid(from + 1, to - from - 1);
        };
        // Plain entries, then submenus, together after Compress and before
        // Share, and in that order again however often the menu opens.
        QCOMPARE(around("notes.txt", "text/plain"), QStringList({"Copy Git Link", "Copy To"}));
        QCOMPARE(around("photo.png", "image/png"), QStringList({"Copy To"}));
        QCOMPARE(around("notes.txt", "text/plain"), QStringList({"Copy Git Link", "Copy To"}));
    }

    void pluginDetailsAreAskedForOnceAndKept()
    {
        QTemporaryDir folder, files;
        for (const char *name : {"a.txt", "b.txt", "photo.png", "slow.txt", "broken.txt", "late.txt"})
            write(files.filePath(name), "x");
        writePlugin(folder.path(), "notes", "{\"folio\": 1, \"details\": {\"command\": [\"./describe\"], \"under\": [\""
                                                + files.path().toUtf8() + "/\"], \"types\": [\"text/*\"]}}",
                    {{"describe", "#!/bin/sh\necho \"$1\" >> asked\ncase \"$1\" in\n"
                                  "  *slow*) exec sleep 5 ;;\n  *broken*) echo 'not json' ;;\n"
                                  "  *late*) sleep 1; echo '[[\"Late\", \"yes\"]]' ;;\n"
                                  "  *) echo '[[\"Lines\", 3], [\"Kind\", \"Lines\"], \"not a row\"]' ;;\nesac\n"},
                     {"i18n/da.json", R"({"Lines": "Linjer"})"}});
        writePlugin(folder.path(), "alpha", R"({"folio": 1, "details": {"command": ["./describe"]}})",
                    {{"describe", "#!/bin/sh\necho '[[\"First\", \"yes\"]]'\n"}});
        Plugins plugins("da", {folder.path()}, files.filePath("folio.conf"));
        QSignalSpy read(&plugins, &Plugins::detailsChanged);
        QVERIFY(read.wait(5000));
        auto asked = [&] {
            QFile file(folder.filePath("notes/asked"));
            return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).split('\n', Qt::SkipEmptyParts)
                                                  : QStringList();
        };
        const QDateTime when = QFileInfo(files.filePath("a.txt")).lastModified();
        int answered = 0;
        auto describe = [&](const QString &name, const QString &mime, const QDateTime &modified, bool wanted = true) {
            plugins.describe(files.filePath(name), mime, modified, [wanted] { return wanted; }, [&] { ++answered; });
        };

        // Nothing's known until the commands answer, each with the path,
        // and then their rows show in plugin id order: labels translated,
        // values as they are, and what isn't a row left out.
        describe("a.txt", "text/plain", when);
        QVERIFY(plugins.details(files.filePath("a.txt"), when).isEmpty());
        QTRY_COMPARE(answered, 2);
        const QVariantList rows{QVariantList{"First", "yes"}, QVariantList{"Linjer", "3"}, QVariantList{"Kind", "Lines"}};
        QCOMPARE(plugins.details(files.filePath("a.txt"), when), rows);
        QCOMPARE(asked(), QStringList({files.filePath("a.txt")}));

        // Coming back to it runs nothing; once it's changed, it's asked again.
        describe("a.txt", "text/plain", when);
        QTest::qWait(300);
        QCOMPARE(answered, 2);
        describe("a.txt", "text/plain", when.addSecs(1));
        QTRY_COMPARE(answered, 4);
        QCOMPARE(asked().size(), 2);

        // A command is only run for what its manifest says: of its types,
        // inside its folders.
        describe("photo.png", "image/png", when);
        QTRY_COMPARE(answered, 5);
        QCOMPARE(plugins.details(files.filePath("photo.png"), when), QVariantList({QVariantList{"First", "yes"}}));
        QCOMPARE(asked().size(), 2);

        // One the selection has moved on from before its turn isn't run.
        describe("b.txt", "text/plain", when, false);
        QTest::qWait(300);
        QCOMPARE(answered, 5);
        QCOMPARE(asked().size(), 2);

        // One that takes too long is ended, and one that prints nonsense
        // has no rows; the other plugins' rows still show.
        QElapsedTimer clock;
        clock.start();
        describe("slow.txt", "text/plain", when);
        describe("broken.txt", "text/plain", when);
        QTRY_COMPARE_WITH_TIMEOUT(answered, 9, 4000);
        QVERIFY(clock.elapsed() < 4000);
        QCOMPARE(plugins.details(files.filePath("slow.txt"), when), QVariantList({QVariantList{"First", "yes"}}));
        QCOMPARE(plugins.details(files.filePath("broken.txt"), when), QVariantList({QVariantList{"First", "yes"}}));
        // Neither is asked again while it's as it was.
        describe("slow.txt", "text/plain", when);
        QTest::qWait(300);
        QCOMPARE(answered, 9);

        // Asked again while it runs, a command runs once and answers both.
        int both = 0;
        for (int i = 0; i < 2; ++i)
            plugins.describe(files.filePath("b.txt"), "text/plain", when, [] { return true; }, [&] { ++both; });
        QTRY_COMPARE(both, 4);
        QCOMPARE(asked().count(files.filePath("b.txt")), 1);

        // F5 forgets what was said, and has it asked again.
        plugins.refresh();
        QVERIFY(plugins.details(files.filePath("a.txt"), when).isEmpty());
        QVERIFY(read.wait(5000));
        describe("a.txt", "text/plain", when);
        QTRY_COMPARE(plugins.details(files.filePath("a.txt"), when), rows);
        // An answer begun before F5 might be older than what replaces it,
        // so it's dropped.
        describe("late.txt", "text/plain", when);
        plugins.refresh();
        QVERIFY(read.wait(5000));
        QTest::qWait(1500);
        QVERIFY(plugins.details(files.filePath("late.txt"), when).isEmpty());
    }

    void pluginDetailsOfOthersSurviveAPollChange()
    {
        // A plugin whose poll says something new is asked again about the
        // file; another's answer on its way still counts, and isn't asked
        // for twice.
        QTemporaryDir folder, files;
        write(files.filePath("late.txt"), "x");
        writePlugin(folder.path(), "notes", R"({"folio": 1, "details": {"command": ["./describe"]}})",
                    {{"describe", "#!/bin/sh\necho \"$1\" >> asked\nsleep 1\necho '[[\"Late\", \"yes\"]]'\n"}});
        writePlugin(folder.path(), "pulse",
                    R"({"folio": 1, "poll": {"command": ["./poll"]}, "details": {"command": ["./describe"]},
                        "actions": [{"id": "bump", "label": "Bump", "command": ["./bump"]}]})",
                    {{"poll", "#!/bin/sh\nprintf '{\"actions\": [\"bump\"], \"items\": [{\"id\": \"n\", \"name\": \"%s\"}]}' \"$(cat count 2>/dev/null || echo 0)\"\n"},
                     {"bump", "#!/bin/sh\necho $(( $(cat count 2>/dev/null || echo 0) + 1 )) > count\n"},
                     {"describe", "#!/bin/sh\necho '[]'\n"}});
        Plugins plugins("en", {folder.path()}, files.filePath("folio.conf"));
        QSignalSpy changed(&plugins, &Plugins::detailsChanged);
        QVERIFY(changed.wait(5000));
        const QString path = files.filePath("late.txt");
        const QDateTime when = QFileInfo(path).lastModified();
        plugins.describe(path, "text/plain", when, [] { return true; }, [] {});
        changed.clear();
        plugins.perform("pulse", "bump", {path}, {}, [](const Plugins::Result &) {});
        QVERIFY(changed.wait(3000));
        QTRY_COMPARE(plugins.details(path, when), QVariantList({QVariantList{"Late", "yes"}}));
        QFile asked(folder.filePath("notes/asked"));
        QVERIFY(asked.open(QIODevice::ReadOnly));
        QCOMPARE(asked.readAll().count('\n'), 1);
    }

    void backendShowsPluginDetailsForTheSelectedItem()
    {
        QTemporaryDir folder, home;
        for (const char *name : {"a.txt", "late.txt"})
            write(home.filePath(name), "x");
        // Its state says how files stand; Sync Now changes it, and with it
        // what its poll prints.
        writePlugin(folder.path(), "sync", R"({"folio": 1, "poll": {"command": ["cat", "state"], "every": 86400},
            "actions": [{"id": "sync", "label": "Sync Now", "command": ["./sync"]}],
            "details": {"command": ["./describe"]}})",
                    {{"state", "{}"},
                     {"describe", "#!/bin/sh\ncase \"$1\" in *late*) sleep 1; echo '[[\"Late\", \"yes\"]]' ;;\n"
                                  "  *) echo \"[[\\\"State\\\", \\\"$(cat word 2>/dev/null || echo waiting)\\\"]]\" ;; esac\n"},
                     {"sync", "#!/bin/sh\necho synced > word\necho '{\"places\": []}' > state\n"}});
        Plugins plugins("en", {folder.path()}, home.filePath("folio.conf"));
        QSignalSpy read(&plugins, &Plugins::detailsChanged);
        QVERIFY(read.wait(5000));
        Backend backend(home.path());
        backend.setPlugins(&plugins);
        auto item = [&](const QString &name) {
            return QVariantMap{{"path", home.filePath(name)}, {"kind", "text"}, {"mime", "text/plain"},
                               {"modified", QFileInfo(home.filePath(name)).lastModified()}};
        };
        auto rows = [&] { return backend.details().value("plugins").toList(); };

        backend.describe(item("a.txt"));
        QTRY_COMPARE(rows(), QVariantList({QVariantList{"State", "waiting"}}));

        // An answer for an item the selection has moved on from is dropped.
        backend.describe(item("late.txt"));
        backend.describe(item("a.txt"));
        QCOMPARE(rows(), QVariantList({QVariantList{"State", "waiting"}}));
        QTest::qWait(1500);
        QCOMPARE(rows(), QVariantList({QVariantList{"State", "waiting"}}));

        // Neither trashed items nor plugins' own are asked about.
        backend.describe(item("a.txt"), true);
        QVERIFY(rows().isEmpty());
        backend.describe({{"path", "plugin:sync/x"}, {"kind", "item"}});
        QVERIFY(rows().isEmpty());

        // A poll saying something new has the item asked about again.
        backend.describe(item("a.txt"));
        QTRY_COMPARE(rows(), QVariantList({QVariantList{"State", "waiting"}}));
        backend.perform("sync", "sync", {home.filePath("a.txt")});
        QTRY_COMPARE(rows(), QVariantList({QVariantList{"State", "synced"}}));

        // A plugin that stops saying anything takes its rows with it.
        writePlugin(folder.path(), "sync", R"({"folio": 1})");
        plugins.refresh();
        QTRY_VERIFY(rows().isEmpty());
    }

    void pluginViewsListTheirItems()
    {
        QTemporaryDir folder, home;
        writePlugin(folder.path(), "servers", R"({"folio": 1, "poll": {"command": ["./servers"]},
            "view": {"title": "Servers", "label": "All", "icon": "drive"}})",
                    {{"servers", "#!/bin/sh\ncat <<EOF\n{\"items\": ["
                                 "{\"id\": \"z\", \"name\": \"zulu\", \"subtitle\": \"SMB\", \"icon\": \"drive\", \"open\": {\"path\": \"" + home.path().toUtf8() + "\"}},"
                                 "{\"id\": \"a/b\", \"name\": \"alpha\", \"details\": [[\"Host\", \"alpha.example\"]]},"
                                 "{\"id\": \"r\", \"name\": \"relative\", \"open\": {\"path\": \".\"}}]}\nEOF\n"}});
        Plugins plugins("en", {folder.path()}, home.filePath("folio.conf"));
        QTRY_VERIFY(plugins.hasView("plugin:servers"));
        QTRY_COMPARE(plugins.views().value("plugin:servers").toMap().value("items").toList().size(), 3);

        // In the plugin's order, not sorted; / searches the names.
        FolderModel model;
        QSignalSpy loaded(&model, &FolderModel::loaded);
        model.setItems(plugins.views().value("plugin:servers").toMap().value("items").toList());
        model.setPath("plugin:servers");
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 3);
        const QVariantMap zulu = model.get(0);
        QCOMPARE(zulu.value("name").toString(), QString("zulu"));
        QCOMPARE(zulu.value("kind").toString(), QString("item"));
        QCOMPARE(zulu.value("path").toString(), QString("plugin:servers/z"));
        QCOMPARE(zulu.value("extension").toString(), QString("SMB"));
        QCOMPARE(zulu.value("icon").toString(), QString("drive"));
        QCOMPARE(model.get(1).value("details").toList().at(0).toList(), QVariantList({"Host", "alpha.example"}));
        model.setQuery("ALP");
        QVERIFY(loaded.wait(3000));
        QCOMPARE(model.count(), 1);

        // A view is a place of its own, named by its title, and opening an
        // item goes where it says.
        Backend backend(home.path());
        backend.setPlugins(&plugins);
        backend.navigate("plugin:servers");
        QCOMPARE(backend.path(), QString("plugin:servers"));
        QCOMPARE(backend.title(), QString("Servers"));
        QCOMPARE(backend.crumbs().size(), 1);
        QCOMPARE(backend.crumbs().at(0).toMap().value("name").toString(), QString("Servers"));
        backend.goUp();
        QCOMPARE(backend.path(), QString("plugin:servers"));
        QVERIFY(backend.goTyped(backend.typedPath()));
        QCOMPARE(backend.path(), QString("plugin:servers"));
        backend.open("plugin:servers/z");
        QCOMPARE(backend.path(), QDir::cleanPath(home.path()));
        // A path that isn't absolute would be wherever Folio was started.
        backend.open("plugin:servers/r");
        QCOMPARE(backend.path(), QDir::cleanPath(home.path()));
        // Nor does it take dropped files.
        QCOMPARE(plugins.views().value("plugin:servers").toMap().value("items").toList().at(2).toMap().value("folder"),
                 QVariant(QString()));
        // A view no plugin has isn't anywhere to go.
        QVERIFY(!backend.goTyped("plugin:nothing"));
        QCOMPARE(backend.path(), QDir::cleanPath(home.path()));
    }

    void pluginPollsArentHeldUpByActions()
    {
        QTemporaryDir folder, home;
        // Its items are what items.json says, each running a command.
        auto items = [](const QStringList &ids, const QString &command) {
            QJsonArray list;
            for (const QString &id : ids)
                list << QJsonObject{{"id", id}, {"name", id}, {"open", QJsonObject{{"run", QJsonArray{"sh", "-c", command}}}}};
            return QJsonDocument(QJsonObject{{"items", list}}).toJson();
        };
        QStringList stuck;
        for (int i = 0; i < 40; ++i)
            stuck << QString("stuck%1").arg(i);
        writePlugin(folder.path(), "acts", R"({"folio": 1, "poll": {"command": ["cat", "items.json"], "every": 86400},
            "view": {"title": "Acts"}})", {{"items.json", items({"quick"}, "true")}});
        Plugins plugins("en", {folder.path()}, home.filePath("folio.conf"));
        auto names = [&] { return namesOf(plugins.views().value("plugin:acts").toMap().value("items").toList()); };
        QTRY_COMPARE(names(), QStringList({"quick"}));

        // An item's command says it's running in the status bar until it's done.
        Backend backend(home.path());
        backend.setPlugins(&plugins);
        backend.open("plugin:acts/quick");
        QCOMPARE(backend.busy(), QString("Opening quick…"));
        QTRY_VERIFY(backend.busy().isEmpty());

        // Actions that never end leave polls and F5 their own threads.
        writePlugin(folder.path(), "acts", R"({"folio": 1, "poll": {"command": ["cat", "items.json"], "every": 86400},
            "view": {"title": "Acts"}})", {{"items.json", items(stuck, "sleep 60")}});
        plugins.refresh();
        QTRY_COMPARE(names(), stuck);
        for (const QString &id : stuck)
            plugins.run("plugin:acts/" + id, [](const Plugins::Result &) {});
        writePlugin(folder.path(), "acts", R"({"folio": 1, "poll": {"command": ["cat", "items.json"], "every": 86400},
            "view": {"title": "Acts"}})", {{"items.json", items({"fresh"}, "true")}});
        plugins.refresh();
        QTRY_COMPARE(names(), QStringList({"fresh"}));
    }

    void pluginPollsOfAReplacedManifestAreDropped()
    {
        QTemporaryDir folder, home;
        const QByteArray old = "#!/bin/sh\ntouch started\nsleep 1\necho '{\"items\": [{\"id\": \"old\", \"name\": \"old\"}]}'\n";
        const QByteArray fresh = "#!/bin/sh\necho '{\"items\": [{\"id\": \"new\", \"name\": \"new\"}]}'\n";
        writePlugin(folder.path(), "swap", R"({"folio": 1, "poll": {"command": ["./old"], "every": 86400},
            "view": {"title": "Swap"}})", {{"old", old}, {"new", fresh}});
        Plugins plugins("en", {folder.path()}, home.filePath("folio.conf"));
        Backend backend(home.path());
        backend.setPlugins(&plugins);
        backend.navigate(home.path());
        QSignalSpy place(&backend, &Backend::placeChanged);
        QTRY_VERIFY(QFileInfo::exists(folder.filePath("swap/started")));

        // F5 while the old manifest's poll runs: its answer is dropped, and
        // the new manifest's command asked instead.
        writePlugin(folder.path(), "swap", R"({"folio": 1, "poll": {"command": ["./new"], "every": 86400},
            "view": {"title": "Swap"}})");
        plugins.refresh();
        auto names = [&] { return namesOf(plugins.views().value("plugin:swap").toMap().value("items").toList()); };
        QTRY_COMPARE(names(), QStringList({"new"}));
        QTest::qWait(1500);
        QCOMPARE(names(), QStringList({"new"}));
        // What the plugins show changed, so the place's names may have too.
        QVERIFY(place.count() > 0);
    }

    void tailscalePluginListsOnlineDesktopsByName()
    {
        QTemporaryDir dir;
        const QString bin = binWith(dir.filePath("bin"), {{"tailscale", "#!/bin/sh\ncat <<'EOF'\n" + tailnet() + "\nEOF\n"}});
        const auto path = pathAt(bin);
        const auto plugin = bundledPlugin("tailscale");
        QCOMPARE(plugin.id, QString("tailscale"));
        const Plugins::Run run = runBundled(plugin, plugin.poll, bin, dir.path());
        QVERIFY2(run.ok(), qPrintable(run.lastError));
        const auto output = Plugins::parseOutput(run.out);
        QVERIFY(output);
        // Offline machines and phones are left out; the rest go by name,
        // whatever their case.
        QStringList names;
        for (const auto &item : output->items)
            names << item.name;
        QCOMPARE(names, QStringList({"build", "Conference-Room", "studio"}));
        const auto &build = output->items.at(0);
        QCOMPARE(build.id, QString("build.example.ts.net"));
        QCOMPARE(build.icon, QString("peer"));
        QCOMPARE(build.subtitle.toString(), QString("Windows"));
        QCOMPARE(build.details, QJsonArray({QJsonArray{"Host", "build.example.ts.net"}}));
        // Opened over SFTP as itself, its host key accepted: Tailscale has
        // already made sure who's on the other end.
        QCOMPARE(build.open.value("mount").toString(), QString("sftp://build.example.ts.net/"));
        QVERIFY(build.open.value("verified").toBool());

        // Without tailscale (or jq), the plugin is off.
        const auto none = pathAt(dir.filePath("nothing"));
        QVERIFY(bundledPlugin("tailscale").id.isEmpty());
    }

    void tailscalePluginSendsWithTaildrop()
    {
        QTemporaryDir dir;
        const QString bin = binWith(dir.filePath("bin"), {{"tailscale",
            "#!/bin/sh\nprintf '%s|' \"$@\" > \"$HOME/sent\"\n"
            "case \"$*\" in *offline*) echo 'no such peer: offline' >&2; exit 1 ;; esac\n"}});
        const auto path = pathAt(bin);
        const auto plugin = bundledPlugin("tailscale");
        QCOMPARE(plugin.actions.size(), 1);
        QVERIFY(plugin.actions[0].toItems && plugin.actions[0].files);
        const Plugins::Run sent = runBundled(plugin, plugin.actions[0].command + QStringList{"/a b.txt", "/c.txt"}, bin, dir.path(),
                                             {{"FOLIO_TARGET", "studio.example.ts.net"}, {"FOLIO_TARGET_NAME", "studio"}});
        QVERIFY(sent.ok());
        QFile file(dir.filePath("sent"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArray("file|cp|/a b.txt|/c.txt|studio.example.ts.net:|"));

        // Its failure says which machine, in words Folio can translate.
        const Plugins::Run failed = runBundled(plugin, plugin.actions[0].command + QStringList{"/c.txt"}, bin, dir.path(),
                                               {{"FOLIO_TARGET", "offline.example.ts.net"}, {"FOLIO_TARGET_NAME", "offline"}});
        QVERIFY(!failed.ok());
        const QJsonValue error = QJsonDocument::fromJson(failed.out).object().value("error");
        QCOMPARE(Plugins::text(plugin, error), QString("Couldn't send to offline: no such peer: offline"));
    }

    void dropboxPluginMarksItsFolder()
    {
        QTemporaryDir home, dir;
        const auto plugin = bundledPlugin("dropbox");
        QCOMPARE(plugin.id, QString("dropbox"));
        auto run = [&](const QString &says, bool installed) {
            const QString bin = dir.filePath(installed ? "bin-" + QString::number(qHash(says)) : "bin-none");
            binWith(bin, installed ? QHash<QString, QByteArray>{{"dropbox-cli", "#!/bin/sh\necho '" + says.toUtf8() + "'\n"}}
                                   : QHash<QString, QByteArray>{});
            const Plugins::Run r = runBundled(plugin, plugin.poll, bin, home.path());
            return r.ok() ? Plugins::parseOutput(r.out) : std::nullopt;
        };
        // Nothing to say without a Dropbox folder.
        auto output = run("Up to date", true);
        QVERIFY(output && output->places.isEmpty());

        QDir(home.path()).mkpath("Dropbox");
        // The mark, even without dropbox-cli, but no links.
        output = run({}, false);
        QCOMPARE(output->places.size(), 1);
        QCOMPARE(output->places[0].path, QString("~/Dropbox"));
        QCOMPARE(output->places[0].icon, QString("dropbox.svg"));
        QVERIFY(output->places[0].status.isUndefined());
        QCOMPARE(output->actions, std::optional<QStringList>(QStringList{}));
        // Up to date says nothing; links work while Dropbox runs.
        output = run("Up to date", true);
        QVERIFY(output->places[0].status.isUndefined());
        QCOMPARE(output->actions, std::optional<QStringList>(QStringList{"link"}));
        output = run("Syncing 3 files", true);
        QCOMPARE(output->places[0].status.toString(), QString("syncing"));
        QCOMPARE(output->places[0].tone, QString("warning"));
        QCOMPARE(output->actions, std::optional<QStringList>(QStringList{"link"}));
        output = run("Dropbox isn't running!", true);
        QCOMPARE(output->places[0].status.toString(), QString("stopped"));
        QCOMPARE(output->places[0].tone, QString("error"));
        QCOMPARE(output->actions, std::optional<QStringList>(QStringList{}));
    }

    void dropboxPluginCopiesLinks()
    {
        QTemporaryDir dir;
        const QString bin = binWith(dir.filePath("bin"), {{"dropbox-cli",
            "#!/bin/sh\ncase \"$2\" in *nope*) echo \"Couldn't get link: not in Dropbox\"; exit 1 ;; esac\n"
            "echo 'https://www.dropbox.com/s/x/'\"$(basename \"$2\")\"\n"}});
        const auto plugin = bundledPlugin("dropbox");
        QCOMPARE(plugin.actions.size(), 1);
        QVERIFY(plugin.actions[0].single);
        QCOMPARE(plugin.actions[0].under, QStringList({"~/Dropbox/"}));
        const Plugins::Run linked = runBundled(plugin, plugin.actions[0].command + QStringList{"/home/me/Dropbox/a \"b\".pdf"}, bin, dir.path());
        QVERIFY(linked.ok());
        const QJsonObject said = QJsonDocument::fromJson(linked.out).object();
        QCOMPARE(said.value("copy").toString(), QString("https://www.dropbox.com/s/x/a \"b\".pdf"));
        QCOMPARE(Plugins::text(plugin, said.value("notice")), QString("Dropbox link for a \"b\".pdf copied."));

        const Plugins::Run failed = runBundled(plugin, plugin.actions[0].command + QStringList{"/home/me/Dropbox/nope.pdf"}, bin, dir.path());
        QVERIFY(!failed.ok());
        QCOMPARE(Plugins::text(plugin, QJsonDocument::fromJson(failed.out).object().value("error")),
                 QString("Couldn't get a Dropbox link for nope.pdf: Couldn't get link: not in Dropbox"));
    }

    void dropboxPluginSaysHowAFileStands()
    {
        QTemporaryDir dir;
        const auto plugin = bundledPlugin("dropbox");
        QCOMPARE(plugin.details, QStringList({"./filestatus"}));
        QCOMPARE(plugin.detailsUnder, QStringList({"~/Dropbox/"}));
        // dropbox-cli filestatus prints "<path>: <status>", or why it can't.
        const QString bin = binWith(dir.filePath("bin"), {{"dropbox-cli",
            "#!/bin/sh\ncase \"$2\" in *stopped*) echo \"Dropbox isn't running!\" ;; *new*) echo \"$2: syncing\" ;;\n"
            "  *ignored*) echo \"$2: unwatched\" ;; *) echo \"$2: up to date\" ;; esac\n"}});
        auto rows = [&](const QString &path, const QString &programs) {
            const Plugins::Run run = runBundled(plugin, plugin.details + QStringList{path}, programs, dir.path());
            return run.ok() ? QJsonDocument::fromJson(run.out).array() : QJsonArray{"failed"};
        };
        QCOMPARE(rows("/home/me/Dropbox/a: b.pdf", bin), QJsonArray({QJsonArray{"Dropbox", "up to date"}}));
        QCOMPARE(rows("/home/me/Dropbox/new.pdf", bin), QJsonArray({QJsonArray{"Dropbox", "syncing"}}));
        // Nothing when Dropbox isn't running, about what it doesn't watch,
        // or without dropbox-cli.
        QCOMPARE(rows("/home/me/Dropbox/stopped.pdf", bin), QJsonArray());
        QCOMPARE(rows("/home/me/Dropbox/ignored.pdf", bin), QJsonArray());
        QCOMPARE(rows("/home/me/Dropbox/a.pdf", binWith(dir.filePath("none"), {})), QJsonArray());
    }

    void localSendPluginNeedsLocalSend()
    {
        QTemporaryDir dir;
        {
            const auto path = pathAt(dir.filePath("nothing"));
            QVERIFY(bundledPlugin("localsend").id.isEmpty());
        }
        const QString bin = binWith(dir.filePath("bin"), {{"localsend", "#!/bin/sh\n"}});
        const auto path = pathAt(bin);
        const auto plugin = bundledPlugin("localsend");
        QCOMPARE(plugin.actions.size(), 1);
        QCOMPARE(plugin.actions[0].label, QString("LocalSend…"));
        QCOMPARE(plugin.actions[0].command, QStringList({"localsend", "--headless", "send"}));
        QVERIFY(plugin.actions[0].detach);
    }

    void bundledPluginsTranslateTheirText()
    {
        // Every text the bundled plugins show is English source text, to be
        // looked up in their catalogs like any other plugin's.
        QTemporaryDir copy;
        QVERIFY(QProcess::execute("cp", {"-r", bundled() + "/tailscale", copy.path()}) == 0);
        QDir(copy.filePath("tailscale")).mkpath("i18n");
        write(copy.filePath("tailscale/i18n/da.json"), R"({"Machines": "Maskiner", "Sending to %1…": "Sender til %1…"})");
        const QString bin = binWith(copy.filePath("bin"), {{"tailscale", "#!/bin/sh\n"}});
        const auto path = pathAt(bin);
        const auto plugin = Plugins::read({copy.path()}, "da").value(0);
        QCOMPARE(Plugins::text(plugin, QJsonValue(plugin.viewLabel)), QString("Maskiner"));
        QCOMPARE(Plugins::text(plugin, QJsonArray{plugin.actions[0].busy, "studio"}), QString("Sender til studio…"));
    }

    void placesListOnlyPhysicalDrives()
    {
        const QList<Places::Mount> mounts{
            {"/", "/dev/mapper/root", "btrfs", ""},
            {"/home", "/dev/mapper/root", "btrfs", ""},
            {"/.snapshots", "/dev/mapper/root", "btrfs", ""},
            {"/boot", "/dev/nvme0n1p1", "vfat", "EFI"},
            {"/tmp", "tmpfs", "tmpfs", ""},
            {"/var/lib/snapd/snap/core", "/dev/loop3", "squashfs", ""},
            {"/mnt/archive", "/dev/sda1", "ext4", ""},
            {"/run/media/me/Photos", "/dev/sdb1", "exfat", "Photos"},
            {"/run/media/me/Photos/again", "/dev/sdb1", "exfat", "Photos"},
            {"/run/user/1000/gvfs", "gvfsd-fuse", "fuse.gvfsd-fuse", ""},
        };
        const auto drives = Places::physicalDrives(mounts, "/dev/mapper/root");
        QCOMPARE(drives.size(), 2);
        QCOMPARE(drives.at(0).toMap().value("name").toString(), QString("archive"));
        QCOMPARE(drives.at(1).toMap().value("name").toString(), QString("Photos"));
        QCOMPARE(drives.at(1).toMap().value("path").toString(), QString("/run/media/me/Photos"));
    }

    void drivesSayHowTheyreLetGoOf()
    {
        // A stick or card is ejected, so it's safe to pull out; a disk that
        // can only be unmounted is unmounted; one GIO can't let go of (only
        // fstab knows it) has no button at all.
        Places::Mount stick{"/run/media/me/Photos", "/dev/sdb1", "exfat", "Photos"};
        stick.canEject = stick.canUnmount = true;
        stick.free = 120'000'000'000;
        Places::Mount disk{"/run/media/me/Backup", "/dev/sdc1", "ext4", "Backup"};
        disk.canUnmount = true;
        const Places::Mount fixed{"/mnt/archive", "/dev/sda1", "ext4", ""};
        const auto drives = Places::physicalDrives({stick, disk, fixed}, "/dev/nvme0n1p2");
        QCOMPARE(drives.size(), 3);
        QCOMPARE(drives.at(0).toMap().value("release").toString(), QString("eject"));
        QCOMPARE(drives.at(1).toMap().value("release").toString(), QString("unmount"));
        QCOMPARE(drives.at(2).toMap().value("release").toString(), QString());
        QCOMPARE(drives.at(0).toMap().value("free").toLongLong(), 120'000'000'000);
        QCOMPARE(drives.at(2).toMap().value("free").toLongLong(), -1);
    }

    void disksParseWholeDisksFromUdisks()
    {
        // As this machine's udisks lists them: the system disk, encrypted,
        // with the system unlocked on its second partition, and an
        // installer stick; loop devices and zram aren't disks.
        const QString at = "/org/freedesktop/UDisks2/block_devices/";
        const QString nvme = "/org/freedesktop/UDisks2/drives/SAMSUNG_MZALC4T0HBL1";
        const QString usb = "/org/freedesktop/UDisks2/drives/USB_SanDisk_3_2e2_Gen";
        Disks::Objects objects;
        objects[at + "nvme0n1"] = {{"Block", {{"Device", "/dev/nvme0n1"}, {"Drive", nvme}, {"Size", 4'096'805'658'624LL}, {"HintSystem", true}, {"CryptoBackingDevice", "/"}}},
                                   {"PartitionTable", {{"Type", "gpt"}}}};
        objects[at + "nvme0n1p1"] = {{"Block", {{"Device", "/dev/nvme0n1p1"}, {"Drive", nvme}, {"Size", 2'147'483'648LL}, {"IdUsage", "filesystem"}, {"CryptoBackingDevice", "/"}}},
                                     {"Partition", {{"Table", at + "nvme0n1"}}},
                                     {"Filesystem", {{"MountPoints", QStringList{"/boot"}}}}};
        objects[at + "nvme0n1p2"] = {{"Block", {{"Device", "/dev/nvme0n1p2"}, {"Drive", nvme}, {"Size", 4'094'655'725'568LL}, {"IdUsage", "crypto"}, {"CryptoBackingDevice", "/"}}},
                                     {"Partition", {{"Table", at + "nvme0n1"}}},
                                     {"Encrypted", {}}};
        objects[at + "dm_2d0"] = {{"Block", {{"Device", "/dev/dm-0"}, {"Drive", "/"}, {"Size", 4'094'638'948'352LL}, {"IdUsage", "filesystem"}, {"IdLabel", "omarchy-root"}, {"CryptoBackingDevice", at + "nvme0n1p2"}}},
                                  {"Filesystem", {{"MountPoints", QStringList{"/", "/home", "/var/log"}}}}};
        objects[at + "loop0"] = {{"Block", {{"Device", "/dev/loop0"}, {"Drive", "/"}, {"Size", 0}}}, {"Loop", {}}};
        objects[at + "zram0"] = {{"Block", {{"Device", "/dev/zram0"}, {"Drive", "/"}, {"Size", 130'916'810'752LL}}}, {"Swapspace", {}}};
        objects[at + "sda"] = {{"Block", {{"Device", "/dev/sda"}, {"Drive", usb}, {"Size", 250'148'290'560LL}, {"IdUsage", "filesystem"}, {"IdLabel", "OMARCHY_202611"}, {"CryptoBackingDevice", "/"}}},
                               {"PartitionTable", {{"Type", "gpt"}}}};
        objects[at + "sda1"] = {{"Block", {{"Device", "/dev/sda1"}, {"Drive", usb}, {"Size", 6'701'465'600LL}, {"IdUsage", "filesystem"}, {"IdLabel", "OMARCHY_202611"}, {"CryptoBackingDevice", "/"}}},
                                {"Partition", {{"Table", at + "sda"}}},
                                {"Filesystem", {{"MountPoints", QStringList{"/run/media/me/OMARCHY_202611"}}}}};
        objects[at + "sda2"] = {{"Block", {{"Device", "/dev/sda2"}, {"Drive", usb}, {"Size", 16'777'216LL}, {"IdUsage", "filesystem"}, {"IdLabel", "ARCHISO_EFI"}, {"CryptoBackingDevice", "/"}}},
                                {"Partition", {{"Table", at + "sda"}}},
                                {"Filesystem", {{"MountPoints", QStringList()}}}};
        objects[nvme] = {{"Drive", {{"Model", "SAMSUNG MZALC4T0HBL1-00B07"}, {"Removable", false}, {"ConnectionBus", ""}}}};
        objects[usb] = {{"Drive", {{"Vendor", "USB"}, {"Model", "SanDisk 3.2 Gen"}, {"Removable", true}, {"ConnectionBus", "usb"}, {"CanPowerOff", true}, {"Ejectable", true}, {"MediaCompatibility", QStringList()}}}};

        const QList<Disk> disks = Disks::parse(objects);
        QCOMPARE(disks.size(), 2);
        const Disk &system = disks.at(0);
        QCOMPARE(system.device, QString("/dev/nvme0n1"));
        QCOMPARE(system.block, at + "nvme0n1");
        QCOMPARE(system.drive, nvme);
        QVERIFY(system.hintSystem);
        QVERIFY(system.mounts.contains("/"));
        QVERIFY(system.mounts.contains("/boot"));
        QVERIFY(Disks::holdsSystem(system));
        QCOMPARE(system.unlocked, QStringList{at + "nvme0n1p2"});
        const Disk &stick = disks.at(1);
        QCOMPARE(stick.device, QString("/dev/sda"));
        QCOMPARE(stick.label, QString("OMARCHY_202611"));
        QCOMPARE(stick.mounts, QStringList{"/run/media/me/OMARCHY_202611"});
        QCOMPARE(stick.filesystems, QStringList{at + "sda1"});
        QCOMPARE(stick.size, 250'148'290'560LL);
        QCOMPARE(stick.bus, QString("usb"));
        QVERIFY(stick.removable && stick.canPowerOff && stick.ejectable && !stick.optical && !stick.readOnly);
        QVERIFY(!Disks::holdsSystem(stick));
        QVERIFY(stick.unlocked.isEmpty());
    }

    void disksSwappedForACopyArentTheSameDisk()
    {
        // A card swapped in its reader for a copy of it has the same path,
        // drive, size and filesystems; only when it was put in differs.
        const QString card = "/org/freedesktop/UDisks2/block_devices/mmcblk0";
        const QString reader = "/org/freedesktop/UDisks2/drives/SD_Reader";
        Disks::Objects objects;
        objects[card] = {{"Block", {{"Device", "/dev/mmcblk0"}, {"Drive", reader}, {"Size", 31'914'983'424LL}, {"IdUUID", "2026-11-11"}}}};
        objects[reader] = {{"Drive", {{"Id", "SD-Reader-1"}, {"ConnectionBus", "sdio"}, {"TimeMediaDetected", qulonglong(1)}}}};
        const QString before = Disks::parse(objects).value(0).identity;
        objects[reader]["Drive"]["TimeMediaDetected"] = qulonglong(2);
        QVERIFY(Disks::parse(objects).value(0).identity != before);
    }

    void disksStayTheSameDiskOnceLocked()
    {
        // Burning locks an encrypted stick's volume, which udisks then no
        // longer lists: it's still the stick that was picked.
        const QString at = "/org/freedesktop/UDisks2/block_devices/";
        const QString drive = "/org/freedesktop/UDisks2/drives/Kingston";
        Disks::Objects objects;
        objects[at + "sdb"] = {{"Block", {{"Device", "/dev/sdb"}, {"Drive", drive}, {"Size", 64'000'000'000LL}}}};
        objects[at + "sdb1"] = {{"Block", {{"Device", "/dev/sdb1"}, {"Drive", drive}, {"Size", 63'000'000'000LL}, {"IdUsage", "crypto"}, {"IdUUID", "luks"}}},
                                {"Partition", {{"Table", at + "sdb"}}},
                                {"Encrypted", {}}};
        objects[at + "dm_2d1"] = {{"Block", {{"Device", "/dev/dm-1"}, {"Drive", "/"}, {"Size", 62'900'000'000LL}, {"IdUsage", "filesystem"}, {"IdUUID", "ext4"}, {"CryptoBackingDevice", at + "sdb1"}}},
                                  {"Filesystem", {{"MountPoints", QStringList{"/run/media/me/Vault"}}}}};
        objects[drive] = {{"Drive", {{"Id", "Kingston-1"}, {"ConnectionBus", "usb"}, {"Removable", true}}}};
        const Disk unlocked = Disks::parse(objects).value(0);
        QCOMPARE(unlocked.unlocked, QStringList{at + "sdb1"});
        objects.remove(at + "dm_2d1");
        const Disk locked = Disks::parse(objects).value(0);
        QVERIFY(locked.unlocked.isEmpty());
        QCOMPARE(locked.identity, unlocked.identity);
    }

    void disksHoldingVolumesOrSwapCountAsTheSystems()
    {
        // The system on an external SSD under LVM: its root is mounted from
        // a logical volume udisks doesn't tie back to the disk, so the
        // volume group's member is what gives it away. Swap in use counts
        // the same.
        const QString at = "/org/freedesktop/UDisks2/block_devices/";
        const QString drive = "/org/freedesktop/UDisks2/drives/Samsung_T7";
        Disks::Objects objects;
        objects[at + "sda"] = {{"Block", {{"Device", "/dev/sda"}, {"Drive", drive}, {"Size", 1'000'000'000'000LL}}}};
        objects[at + "sda1"] = {{"Block", {{"Device", "/dev/sda1"}, {"Drive", drive}, {"Size", 999'000'000'000LL}, {"IdUsage", "raid"}, {"IdType", "LVM2_member"}}},
                                {"Partition", {{"Table", at + "sda"}}}};
        objects[at + "dm_2d0"] = {{"Block", {{"Device", "/dev/dm-0"}, {"Drive", "/"}, {"Size", 999'000'000'000LL}, {"IdUsage", "filesystem"}}},
                                  {"Filesystem", {{"MountPoints", QStringList{"/"}}}}};
        objects[at + "sdb"] = {{"Block", {{"Device", "/dev/sdb"}, {"Drive", drive + "2"}, {"Size", 64'000'000'000LL}}}};
        objects[at + "sdb1"] = {{"Block", {{"Device", "/dev/sdb1"}, {"Drive", drive + "2"}, {"Size", 8'000'000'000LL}, {"IdUsage", "other"}, {"IdType", "swap"}}},
                                {"Partition", {{"Table", at + "sdb"}}},
                                {"Swapspace", {{"Active", true}}}};
        objects[drive] = {{"Drive", {{"ConnectionBus", "usb"}, {"Removable", false}, {"CanPowerOff", true}}}};
        objects[drive + "2"] = objects[drive];
        const QList<Disk> disks = Disks::parse(objects);
        QCOMPARE(disks.size(), 2);
        for (const Disk &disk : disks) {
            QVERIFY(Disks::holdsSystem(disk));
            QCOMPARE(Disks::fit(disk, 1), Disks::Unfit);
        }

        // Swap that isn't on, on a stick, is just a partition.
        objects[at + "sdb1"]["Swapspace"]["Active"] = false;
        QVERIFY(!Disks::holdsSystem(Disks::parse(objects).value(1)));
    }

    void disksDecideWhereAnImageCanBurn()
    {
        const qint64 image = 4'000'000'000;
        const Disk stick = sanDisk();
        QCOMPARE(Disks::fit(stick, image), Disks::Fits);

        // An SD card in a laptop's own reader isn't removable to udisks,
        // but its bus is.
        Disk card = sanDisk();
        card.vendor = card.model = "";
        card.bus = "sdio";
        card.removable = card.canPowerOff = card.ejectable = false;
        QCOMPARE(Disks::fit(card, image), Disks::Fits);

        Disk small = sanDisk();
        small.size = 2'000'000'000;
        QCOMPARE(Disks::fit(small, image), Disks::TooSmall);

        Disk system = sanDisk();
        system.mounts = {"/run/media/me/STICK", "/boot"};
        QCOMPARE(Disks::fit(system, image), Disks::Unfit);

        Disk hinted = sanDisk();
        hinted.hintSystem = true;
        QCOMPARE(Disks::fit(hinted, image), Disks::Unfit);

        Disk internal = sanDisk();
        internal.bus = "";
        internal.removable = internal.canPowerOff = internal.ejectable = false;
        QCOMPARE(Disks::fit(internal, image), Disks::Unfit);

        Disk locked = sanDisk();
        locked.readOnly = true;
        QCOMPARE(Disks::fit(locked, image), Disks::Unfit);

        Disk optical = sanDisk();
        optical.optical = true;
        QCOMPARE(Disks::fit(optical, image), Disks::Unfit);

        // A reader with no card in it has no size.
        Disk empty = sanDisk();
        empty.size = 0;
        QCOMPARE(Disks::fit(empty, image), Disks::Unfit);
    }

    void disksAreNamedTheWayPeopleKnowThem()
    {
        Disk disk = sanDisk();
        QCOMPARE(Disks::name(disk), QString("SanDisk Ultra"));
        QCOMPARE(Disks::title(disk), QString("SanDisk Ultra · 32 GB"));
        // A placeholder maker is left out, and one the model already names
        // isn't said twice.
        disk.vendor = "USB";
        disk.model = "SanDisk 3.2 Gen";
        QCOMPARE(Disks::name(disk), QString("SanDisk 3.2 Gen"));
        disk.vendor = "Kingston";
        disk.model = "Kingston DataTraveler";
        QCOMPARE(Disks::name(disk), QString("Kingston DataTraveler"));
        // Without a maker or model, its label, then its device.
        disk.vendor = disk.model = "";
        QCOMPARE(Disks::name(disk), QString("STICK"));
        disk.label = "";
        disk.device = "/dev/mmcblk0";
        QCOMPARE(Disks::name(disk), QString("mmcblk0"));
        QCOMPARE(Disks::size(1'500'000'000'000), QString("1.5 TB"));
        QCOMPARE(Disks::size(3'904'897'024), QString("3.9 GB"));
        QCOMPARE(Disks::size(250'148'290'560), QString("250 GB"));
    }

    void formattingNamesDrivesAsExfatAllows()
    {
        // Each filesystem keeps its own length of name.
        const auto limit = [](const char *type) { return Disks::fileSystem(type)->labelLimit; };
        QCOMPARE(limit("exfat"), 11);
        QCOMPARE(limit("vfat"), 11);
        QVERIFY(!Disks::fileSystem("ntfs"));
        QCOMPARE(limit("ext4"), 16);
        QVERIFY(!Disks::fileSystem("zfs"));
        auto fake = fakeDisks();
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.format(sanDisk().identity, "Holiday Photos");
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QString("exFAT keeps at most 11 characters of a drive's name."));
        QVERIFY(fake->calls.isEmpty());
        QVERIFY(ops.busy().isEmpty());
    }

    void formattingUnmountsThenMakesOneExfatPartition()
    {
        auto fake = fakeDisks();
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice), releasing(&ops, &FileOps::releasing), changed(&ops, &FileOps::drivesChanged);
        QStringList busy;
        connect(&ops, &FileOps::busyChanged, this, [&] { busy << ops.busy(); });
        ops.format(sanDisk().identity, " Photos ");
        QTRY_COMPARE(notice.count(), 1);
        QCOMPARE(notice.at(0).at(0).toString(), QString("Formatted SanDisk Ultra."));
        QCOMPARE(releasing.count(), 1);
        QCOMPARE(releasing.at(0).at(0).toString(), QString("/run/media/me/STICK"));
        QCOMPARE(busy.value(0), QString("Formatting SanDisk Ultra…"));
        QCOMPARE(changed.count(), 1);
        const QString sdb = sanDisk().block;
        QCOMPARE(fake->calls, (QStringList{"unmount " + sdb + "1", "makeTable " + sdb, "addPartition " + sdb + " exfat Photos",
                                           "mount " + sdb + "1"}));
    }

    void formatDialogSaysWhatItMakes()
    {
        // The filesystem Format makes is always said: picked from a menu
        // when there's more than one, and as it is when there's only one.
        QTemporaryDir home;
        auto fake = fakeDisks();
        Theme theme(home.filePath("theme"));
        FileOps fileOps;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("fileOps", &fileOps);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\n"
                          "Window {\n"
                          "    width: 800; height: 600; visible: true\n"
                          "    FormatDialog { id: dialog; anchors.fill: parent }\n"
                          "    function shown() {\n"
                          "        dialog.open({ id: 'stick', label: 'Photos', title: 'SanDisk Ultra' })\n"
                          "        const texts = []\n"
                          "        const walk = item => {\n"
                          "            if (!item.visible) return\n"
                          "            if (item instanceof Text) texts.push(item.text)\n"
                          "            for (const child of item.children) walk(child)\n"
                          "        }\n"
                          "        walk(dialog)\n"
                          "        return texts\n"
                          "    }\n"
                          "}\n",
                          QUrl::fromLocalFile(sourceFolder() + "/FormatDialogTest.qml"));
        std::unique_ptr<QObject> window(component.create());
        QVERIFY2(window, qPrintable(component.errorString()));
        const auto shown = [&] {
            QVariant texts;
            QMetaObject::invokeMethod(window.get(), "shown", Q_RETURN_ARG(QVariant, texts));
            return texts.toStringList();
        };
        fake->missing = {"ext4"};
        QVERIFY(shown().contains("exFAT  ▾"));
        fake->missing = {"vfat", "ext4"};
        QVERIFY(shown().contains("exFAT"));
        QVERIFY(!shown().contains("exFAT  ▾"));
    }

    void dialogsKeepTheKeyboard()
    {
        // Tab goes round a dialog's own buttons, never on to one behind
        // it, and Return presses the button Tab has reached.
        QTemporaryDir home;
        Theme theme(home.filePath("theme"));
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("theme", &theme);
        QQmlComponent component(&engine);
        component.setData("import QtQuick\n"
                          "Window {\n"
                          "    width: 800; height: 600; visible: true\n"
                          "    property string answer\n"
                          "    PlainButton { text: 'Behind' }\n"
                          "    ConfirmDialog { id: dialog; anchors.fill: parent }\n"
                          "    function ask() { answer = ''; dialog.ask('Empty?', 'For good.', 'Empty', () => answer = 'emptied') }\n"
                          "    readonly property bool asking: dialog.visible\n"
                          "}\n",
                          QUrl::fromLocalFile(sourceFolder() + "/ConfirmDialogTest.qml"));
        std::unique_ptr<QObject> root(component.create());
        QVERIFY2(root, qPrintable(component.errorString()));
        auto *window = qobject_cast<QQuickWindow *>(root.get());
        window->requestActivate();
        QVERIFY(QTest::qWaitForWindowFocused(window));
        const auto focused = [&] { return window->activeFocusItem() ? window->activeFocusItem()->property("text").toString() : QString(); };

        QMetaObject::invokeMethod(window, "ask");
        QCOMPARE(focused(), QString("Empty"));
        for (const QString &next : {"Cancel", "Empty", "Cancel"}) {
            QTest::keyClick(window, Qt::Key_Tab);
            QCOMPARE(focused(), next);
        }
        QTest::keyClick(window, Qt::Key_Backtab, Qt::ShiftModifier);
        QCOMPARE(focused(), QString("Empty"));
        QTest::keyClick(window, Qt::Key_Tab);
        QTest::keyClick(window, Qt::Key_Return);
        QVERIFY(!window->property("asking").toBool());
        QCOMPARE(window->property("answer").toString(), QString());

        QMetaObject::invokeMethod(window, "ask");
        QTest::keyClick(window, Qt::Key_Return);
        QCOMPARE(window->property("answer").toString(), QString("emptied"));

        QMetaObject::invokeMethod(window, "ask");
        QTest::keyClick(window, Qt::Key_Escape);
        QVERIFY(!window->property("asking").toBool());
        QCOMPARE(window->property("answer").toString(), QString());
    }

    void formattingOffersWhatThisMachineCanMake()
    {
        // exFAT first, and only the filesystems whose tools are installed.
        auto fake = fakeDisks();
        fake->missing = {"vfat"};
        FileOps ops;
        QStringList types;
        for (const QVariant &choice : ops.formatChoices())
            types << choice.toMap().value("type").toString();
        QCOMPARE(types, (QStringList{"exfat", "ext4"}));

        // The one picked is made, with a name as long as it keeps.
        ops.format(sanDisk().identity, "Linux Backups 26", "ext4");
        QTRY_VERIFY(ops.busy().isEmpty());
        QVERIFY(fake->calls.contains("addPartition " + sanDisk().block + " ext4 Linux Backups 26"));

        // A name too long for the filesystem, or a filesystem Folio doesn't
        // make, touches nothing.
        fake->calls.clear();
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.format(sanDisk().identity, "Holiday Photos", "vfat");
        ops.format(sanDisk().identity, "Photos", "zfs");
        // Nor one that fits only counted in characters, or has what FAT
        // can't keep, which udisks would find only once it's erased.
        ops.format(sanDisk().identity, QString(9, QChar(0xe6)), "ext4");
        ops.format(sanDisk().identity, "BACKUP:2026", "vfat");
        ops.format(sanDisk().identity, QString("Caf") + QChar(0xe9), "vfat");
        QCOMPARE(failed.count(), 5);
        QCOMPARE(failed.at(3).at(0).toString(), QString("FAT32 can't have : in a drive's name."));
        QVERIFY(fake->calls.isEmpty());
    }

    void formattingNeedsWhatMakesTheFilesystem()
    {
        // Without mkfs.exfat, the drive would be erased and left empty.
        auto fake = fakeDisks();
        fake->missing = {"exfat"};
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.format(sanDisk().identity, "Photos");
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QString("This computer can't make exFAT drives."));
        QVERIFY(ops.busy().isEmpty());
        QVERIFY(fake->calls.isEmpty());
    }

    void formattingExt4HandsTheDriveToItsOwner()
    {
        // A new ext4 filesystem belongs to root unless udisks is told to
        // hand it over; the others have no owners.
        QCOMPARE(Disks::formatOptions("ext4", "Backups").value("take-ownership").toBool(), true);
        QVERIFY(!Disks::formatOptions("exfat", "Photos").contains("take-ownership"));
        QCOMPARE(Disks::formatOptions("vfat", "Photos").value("label").toString(), QString("Photos"));
    }

    void formattingStopsAtWhatUdisksRefuses()
    {
        auto fake = fakeDisks();
        fake->refuse["makeTable"] = "Permission was refused.";
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.format(sanDisk().identity, "Photos");
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QString("Couldn't format SanDisk Ultra. Permission was refused."));
        const QString sdb = sanDisk().block;
        QCOMPARE(fake->calls, (QStringList{"unmount " + sdb + "1", "makeTable " + sdb}));

        // A disk gone from udisks' list isn't touched.
        fake->calls.clear();
        fake->disks.clear();
        ops.format(sanDisk().identity, "Photos");
        QCOMPARE(failed.count(), 2);
        QCOMPARE(failed.at(1).at(0).toString(), QString("That drive is gone."));
        QVERIFY(fake->calls.isEmpty());
    }

    void drivesAreFoundByWhereTheyreMounted()
    {
        Disk system = sanDisk();
        system.block = "/org/freedesktop/UDisks2/block_devices/nvme0n1";
        system.mounts = {"/", "/run/media/me/Spare"};
        fakeDisks({sanDisk(), system});
        FileOps ops;
        const QVariantMap stick = ops.driveAt("/run/media/me/STICK");
        QCOMPARE(stick.value("id").toString(), sanDisk().identity);
        QCOMPARE(stick.value("title").toString(), QString("SanDisk Ultra · 32 GB"));
        QCOMPARE(stick.value("label").toString(), QString("STICK"));
        // Nothing on the system's own disk can be formatted from here.
        QVERIFY(ops.driveAt("/run/media/me/Spare").isEmpty());
        QVERIFY(ops.driveAt("/run/media/me/Elsewhere").isEmpty());
    }

    void onlyPluggedInDrivesCanBeFormatted()
    {
        // A second drive inside the computer, holding data or another
        // system, isn't erased from a menu.
        Disk internal = sanDisk();
        internal.identity = "internal";
        internal.bus = "";
        internal.removable = internal.canPowerOff = internal.ejectable = false;
        internal.hintSystem = true;
        internal.mounts = {"/run/media/me/Data"};
        Disk locked = sanDisk();
        locked.identity = "locked";
        locked.readOnly = true;
        locked.mounts = {"/run/media/me/Locked"};
        auto fake = fakeDisks({internal, locked});
        FileOps ops;
        QVERIFY(ops.driveAt("/run/media/me/Data").isEmpty());
        QVERIFY(ops.driveAt("/run/media/me/Locked").isEmpty());
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.format("internal", "Photos");
        ops.format("locked", "Photos");
        QCOMPARE(failed.count(), 2);
        QVERIFY(fake->calls.isEmpty());
    }

    void burningOffersOnlyImages()
    {
        QTemporaryDir dir;
        FileOps ops;
        for (const char *name : {"ubuntu.iso", "raspios.img", "notes.txt"})
            writeImageFile(dir.filePath(name), 4096);
        QVERIFY(ops.canBurn(dir.filePath("ubuntu.iso")));
        QVERIFY(ops.canBurn(dir.filePath("raspios.img")));
        QVERIFY(!ops.canBurn(dir.filePath("notes.txt")));
        QVERIFY(!ops.canBurn(dir.path()));
    }

    void burningListsTheDrivesThatCanTakeTheImage()
    {
        QTemporaryDir dir;
        const QString image = dir.filePath("ubuntu.iso");
        writeImageFile(image, 3 << 20);
        Disk small = sanDisk();
        small.block = "/org/freedesktop/UDisks2/block_devices/sdc";
        small.vendor = "Kingston";
        small.model = "DataTraveler";
        small.size = 1'000'000;
        Disk system = sanDisk();
        system.block = "/org/freedesktop/UDisks2/block_devices/nvme0n1";
        system.mounts = {"/"};
        fakeDisks({sanDisk(), small, system});
        FileOps ops;
        const QVariantList targets = ops.burnTargets(image);
        QCOMPARE(targets.size(), 2);
        QCOMPARE(targets.at(0).toMap().value("text").toString(), QString("SanDisk Ultra · 32 GB"));
        QVERIFY(targets.at(0).toMap().value("enabled").toBool());
        QCOMPARE(targets.at(1).toMap().value("text").toString(), QString("Kingston DataTraveler · 1.0 MB (too small)"));
        QVERIFY(!targets.at(1).toMap().value("enabled").toBool());

        fakeDisks({system});
        QVERIFY(ops.burnTargets(image).isEmpty());
    }

    void burningNeverOffersTheDriveTheImageIsOn()
    {
        // Erasing it would erase the image too, half way through.
        const QString image = "/run/media/me/STICK/isos/ubuntu.iso";
        const Disk holder = sanDisk();
        auto fake = fakeDisks({holder});
        FileOps ops;
        QVERIFY(ops.burnTargets(image).isEmpty());
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.burn(image, holder.identity);
        QCOMPARE(failed.count(), 1);
        QVERIFY(fake->calls.isEmpty());
    }

    void onlyOneDriveIsChangedAtATime()
    {
        // Each pane has its own file work, but drives are the computer's:
        // one pane's burn and another's format mustn't overlap.
        QTemporaryDir dir;
        const QString image = dir.filePath("ubuntu.iso");
        writeImageFile(image, 1 << 20);
        auto fake = fakeDisks();
        fake->device = dir.filePath("sdb");
        std::atomic<bool> unmounting = false, release = false;
        fake->during = [&](const QString &method) {
            if (method != "unmount")
                return;
            unmounting = true;
            while (!release)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        };
        FileOps burning, formatting;
        QSignalSpy failed(&formatting, &FileOps::failed), burnt(&burning, &FileOps::notice);
        burning.burn(image, sanDisk().identity);
        QTRY_VERIFY(unmounting);
        formatting.format(sanDisk().identity, "Photos");
        formatting.burn(image, sanDisk().identity);
        QCOMPARE(failed.count(), 2);
        QVERIFY(formatting.busy().isEmpty());
        release = true;
        QTRY_COMPARE(burnt.count(), 1);
        const QString sdb = sanDisk().block;
        QCOMPARE(fake->calls, (QStringList{"unmount " + sdb + "1", "open " + sdb, "eject " + sdb}));

        // Once it's done, the next may go.
        fake->calls.clear();
        fake->during = nullptr;
        QSignalSpy formatted(&formatting, &FileOps::notice);
        formatting.format(sanDisk().identity, "Photos");
        QTRY_COMPARE(formatted.count(), 1);
    }

    void burningLocksWhatsUnlockedOnTheDrive()
    {
        // An encrypted stick's volume, unlocked, holds the disk even once
        // its filesystem is unmounted.
        QTemporaryDir dir;
        const QString image = dir.filePath("ubuntu.iso");
        writeImageFile(image, 1 << 20);
        Disk stick = sanDisk();
        stick.filesystems = {"/org/freedesktop/UDisks2/block_devices/dm_2d1"};
        stick.unlocked = {stick.block + "1"};
        auto fake = fakeDisks({stick});
        fake->device = dir.filePath("sdb");
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice);
        ops.burn(image, stick.identity);
        QTRY_COMPARE(notice.count(), 1);
        QCOMPARE(fake->calls, (QStringList{"unmount " + stick.filesystems.first(), "lock " + stick.block + "1",
                                           "open " + stick.block, "eject " + stick.block}));
    }

    void burningWritesTheImageThenEjects()
    {
        QTemporaryDir dir;
        const QString image = dir.filePath("ubuntu.iso");
        // More than two 4 MiB chunks, and not a whole number of them.
        const QByteArray bytes = writeImageFile(image, (9 << 20) + 123);
        auto fake = fakeDisks();
        fake->device = dir.filePath("sdb");
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice), failed(&ops, &FileOps::failed);
        QStringList busy;
        connect(&ops, &FileOps::busyChanged, this, [&] { busy << ops.busy(); });
        ops.burn(image, sanDisk().identity);
        QTRY_COMPARE(notice.count(), 1);
        QCOMPARE(failed.count(), 0);
        QCOMPARE(notice.at(0).at(0).toString(), QString("Burned ubuntu.iso to SanDisk Ultra. It's safe to remove now."));
        QCOMPARE(busy.value(0), QString("Burning ubuntu.iso to SanDisk Ultra…"));
        QFile device(fake->device);
        QVERIFY(device.open(QIODevice::ReadOnly));
        QVERIFY(device.readAll() == bytes);
        const QString sdb = sanDisk().block;
        QCOMPARE(fake->calls, (QStringList{"unmount " + sdb + "1", "open " + sdb, "eject " + sdb}));
    }

    void burningCanBeCancelledLeavingPartOfTheImage()
    {
        QTemporaryDir dir;
        const QString image = dir.filePath("ubuntu.iso");
        const QByteArray bytes = writeImageFile(image, 12 << 20);
        // The device is a pipe the test reads at its own pace: the first
        // chunk, then nothing until the burn is cancelled, then the rest.
        int pipe[2];
        QCOMPARE(::pipe2(pipe, O_CLOEXEC), 0);
        auto fake = fakeDisks();
        fake->fd = pipe[1];
        std::atomic<bool> gotFirst = false, cancelled = false;
        QByteArray received;
        std::thread reader([&] {
            char buffer[65536];
            ssize_t n;
            while ((n = ::read(pipe[0], buffer, sizeof buffer)) > 0) {
                received.append(buffer, n);
                if (received.size() >= (4 << 20) && !gotFirst) {
                    gotFirst = true;
                    while (!cancelled)
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
            }
            ::close(pipe[0]);
        });
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice), failed(&ops, &FileOps::failed);
        ops.burn(image, sanDisk().identity);
        QTRY_VERIFY(gotFirst);
        // While it's stuck, it says how far it's got, and nothing else
        // runs beside it.
        QTRY_COMPARE(ops.busy(), QString("Burning ubuntu.iso to SanDisk Ultra: 4.2 MB of 13 MB…"));
        ops.format(sanDisk().identity, "Photos");
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QString("Try again once the work under way is done."));
        ops.cancel();
        cancelled = true;
        QTRY_COMPARE(notice.count(), 1);
        reader.join();
        QCOMPARE(notice.at(0).at(0).toString(), QString("Stopped burning. SanDisk Ultra now holds only part of ubuntu.iso."));
        // It stopped after the chunk it was writing, short of the whole.
        QCOMPARE(received.size(), 8 << 20);
        QVERIFY(received == bytes.left(received.size()));
        const QString sdb = sanDisk().block;
        QCOMPARE(fake->calls, (QStringList{"unmount " + sdb + "1", "open " + sdb}));
    }

    void burningCancelledBeforeItWritesLeavesTheDriveAlone()
    {
        QTemporaryDir dir;
        const QString image = dir.filePath("ubuntu.iso");
        writeImageFile(image, 1 << 20);
        auto fake = fakeDisks();
        fake->device = dir.filePath("sdb");
        std::atomic<bool> unmounting = false, release = false;
        fake->during = [&](const QString &method) {
            unmounting = true;
            while (method == "unmount" && !release)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        };
        FileOps ops;
        QSignalSpy notice(&ops, &FileOps::notice);
        ops.burn(image, sanDisk().identity);
        QTRY_VERIFY(unmounting);
        ops.cancel();
        release = true;
        QTRY_COMPARE(notice.count(), 1);
        QCOMPARE(notice.at(0).at(0).toString(), QString("Stopped before burning ubuntu.iso. Nothing on SanDisk Ultra was erased."));
        QCOMPARE(fake->calls, QStringList{"unmount " + sanDisk().block + "1"});
        QVERIFY(!QFile::exists(fake->device));
    }

    void burningStopsAtWhatUdisksRefuses()
    {
        QTemporaryDir dir;
        const QString image = dir.filePath("ubuntu.iso");
        writeImageFile(image, 1 << 20);
        auto fake = fakeDisks();
        fake->device = dir.filePath("sdb");
        fake->refuse["open"] = "Permission was refused.";
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed), notice(&ops, &FileOps::notice);
        ops.burn(image, sanDisk().identity);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QString("Couldn't burn ubuntu.iso to SanDisk Ultra. Permission was refused."));
        const QString sdb = sanDisk().block;
        QCOMPARE(fake->calls, (QStringList{"unmount " + sdb + "1", "open " + sdb}));
        QVERIFY(!QFile::exists(fake->device));
        QCOMPARE(notice.count(), 0);
    }

    void burningSaysWhenTheDriveGoes()
    {
        QTemporaryDir dir;
        const QString image = dir.filePath("ubuntu.iso");
        writeImageFile(image, 1 << 20);
        // Pulled out before it started: udisks no longer lists it.
        auto fake = fakeDisks({});
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.burn(image, sanDisk().identity);
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QString("ubuntu.iso can't be burned onto that drive."));
        QVERIFY(fake->calls.isEmpty());

        // Pulled out while writing: the device takes no more.
        fake = fakeDisks();
        QFile gone(dir.filePath("gone"));
        QVERIFY(gone.open(QIODevice::WriteOnly));
        gone.close();
        fake->fd = ::open(QFile::encodeName(gone.fileName()).constData(), O_RDONLY | O_CLOEXEC);
        ops.burn(image, sanDisk().identity);
        QTRY_COMPARE(failed.count(), 2);
        QVERIFY(failed.at(1).at(0).toString().startsWith("Couldn't burn ubuntu.iso to SanDisk Ultra. The drive stopped taking data ("));
        const QString sdb = sanDisk().block;
        QCOMPARE(fake->calls, (QStringList{"unmount " + sdb + "1", "open " + sdb}));
    }

    void drivesAreKnownByMoreThanWhereTheyAre()
    {
        // Each disk has an identity beyond its object path: its drive's Id
        // (maker, model, serial), its size, and what's on it.
        const QString at = "/org/freedesktop/UDisks2/block_devices/";
        const QString drive = "/org/freedesktop/UDisks2/drives/Reader";
        auto objects = [&](const QString &uuid) {
            Disks::Objects o;
            o[at + "sdb"] = {{"Block", {{"Device", "/dev/sdb"}, {"Drive", drive}, {"Size", 32'000'000'000LL}}}};
            o[at + "sdb1"] = {{"Block", {{"Device", "/dev/sdb1"}, {"Drive", drive}, {"Size", 31'000'000'000LL}, {"IdUUID", uuid}}},
                              {"Partition", {{"Table", at + "sdb"}}}};
            o[drive] = {{"Drive", {{"Id", "Generic-SD_Reader-000000001"}, {"ConnectionBus", "usb"}, {"Removable", true}}}};
            return o;
        };
        // Another card in the same reader is another disk.
        const QString first = Disks::parse(objects("1234-ABCD")).value(0).identity;
        QVERIFY(first.contains(at + "sdb"));
        QVERIFY(first.contains("Generic-SD_Reader-000000001"));
        QCOMPARE(Disks::parse(objects("1234-ABCD")).value(0).identity, first);
        QVERIFY(Disks::parse(objects("9876-FEDC")).value(0).identity != first);
    }

    void drivesSwappedBeforeTheyreChangedArentTouched()
    {
        QTemporaryDir dir;
        const QString image = dir.filePath("ubuntu.iso");
        writeImageFile(image, 1 << 20);
        auto fake = fakeDisks();
        FileOps ops;
        const QString id = ops.burnTargets(image).value(0).toMap().value("id").toString();
        QCOMPARE(ops.driveAt("/run/media/me/STICK").value("id").toString(), id);
        // Pulled out after its menu opened, and another put in its place.
        fake->disks = {otherStick()};
        QSignalSpy failed(&ops, &FileOps::failed);
        ops.burn(image, id);
        ops.format(id, "Photos");
        QCOMPARE(failed.count(), 2);
        QVERIFY(fake->calls.isEmpty());
    }

    void drivesSwappedWhilePolkitAsksArentTouched()
    {
        QTemporaryDir dir;
        const QString image = dir.filePath("ubuntu.iso");
        writeImageFile(image, 1 << 20);
        auto fake = fakeDisks();
        fake->device = dir.filePath("sdb");
        // Swapped while the password was asked for, before it's written to.
        fake->during = [&](const QString &method) { if (method == "open") fake->disks = {otherStick()}; };
        FileOps ops;
        QSignalSpy failed(&ops, &FileOps::failed), notice(&ops, &FileOps::notice);
        ops.burn(image, sanDisk().identity);
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QString("Couldn't burn ubuntu.iso to SanDisk Ultra. The drive was changed or removed."));
        QCOMPARE(QFileInfo(fake->device).size(), 0);
        const QString sdb = sanDisk().block;
        QCOMPARE(fake->calls, (QStringList{"unmount " + sdb + "1", "open " + sdb}));

        // The same before it's formatted.
        fake->calls.clear();
        fake->disks = {sanDisk()};
        fake->during = [&](const QString &method) { if (method == "unmount") fake->disks = {otherStick()}; };
        ops.format(sanDisk().identity, "Photos");
        QTRY_COMPARE(failed.count(), 2);
        QCOMPARE(failed.at(1).at(0).toString(), QString("Couldn't format SanDisk Ultra. The drive was changed or removed."));
        QCOMPARE(fake->calls, (QStringList{"unmount " + sdb + "1"}));
        QCOMPARE(notice.count(), 0);
    }

    void placesListDrivesNotMountedYet()
    {
        // GIO's volumes come first, in its order: one not mounted, one
        // mounted, and one mounted where the system lives.
        const QList<Drives::Volume> volumes{
            {"Windows", "/dev/nvme0n1p3", ""},
            {"Photos", "/dev/sdb1", "/run/media/me/Photos"},
            {"Home", "/dev/nvme0n1p2", "/home"},
        };
        // The physical drives: a mounted volume is its drive, with what's
        // free and how it's let go of; one GIO doesn't list (an fstab
        // mount) follows.
        const QVariantList drives{
            QVariantMap{{"name", "archive"}, {"path", "/mnt/archive"}, {"icon", "drive"}, {"release", ""}, {"free", 5}},
            QVariantMap{{"name", "Photos"}, {"path", "/run/media/me/Photos"}, {"icon", "drive"}, {"release", "eject"}, {"free", 7}},
        };
        const auto devices = Places::devicesFrom(volumes, drives);
        QCOMPARE(devices.size(), 3);
        const QVariantMap windows = devices.at(0).toMap();
        QCOMPARE(windows.value("name").toString(), QString("Windows"));
        QVERIFY(windows.value("path").toString().isEmpty());
        QCOMPARE(windows.value("volume").toString(), QString("/dev/nvme0n1p3"));
        QVERIFY(windows.value("release").toString().isEmpty());
        const QVariantMap photos = devices.at(1).toMap();
        QCOMPARE(photos.value("path").toString(), QString("/run/media/me/Photos"));
        QCOMPARE(photos.value("volume").toString(), QString("/dev/sdb1"));
        QCOMPARE(photos.value("release").toString(), QString("eject"));
        QCOMPARE(photos.value("free").toLongLong(), 7);
        // The system's own stays out, as physicalDrives left it.
        QCOMPARE(devices.at(2).toMap().value("path").toString(), QString("/mnt/archive"));
        QVERIFY(!devices.at(2).toMap().contains("volume"));
    }

    void drivesSayWhatsFreeInTheLanguagesDigits()
    {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport \"Format.js\" as Format\n"
                          "QtObject { function free(bytes) { return Format.free(bytes) } }",
                          QUrl::fromLocalFile(QFINDTESTDATA("../src/Format.js")));
        std::unique_ptr<QObject> format(component.create());
        QVERIFY(format);
        auto free = [&](qint64 bytes) {
            QVariant text;
            QMetaObject::invokeMethod(format.get(), "free", Q_RETURN_ARG(QVariant, text), Q_ARG(QVariant, bytes));
            return text.toString();
        };
        QCOMPARE(free(120'000'000'000), QString("120.0 GB free"));
        QCOMPARE(free(512), QString("512 B free"));

        const QLocale before;
        const auto restore = qScopeGuard([before] { QLocale::setDefault(before); });
        QLocale::setDefault(QLocale("ar_EG"));
        QCOMPARE(free(120'000'000'000), QLocale("ar_EG").toString(120.0, 'f', 1) + " GB free");
    }

    void thumbnailsFollowTheFreedesktopCache()
    {
        QTemporaryDir dir;
        QImage picture(800, 400, QImage::Format_RGB32);
        picture.fill(Qt::red);
        const QString file = dir.filePath("wide.png");
        QVERIFY(picture.save(file));

        Thumbnails thumbnails(dir.filePath("cache"), {dir.filePath("none")});
        const QImage thumb = thumbnails.load(file, "image/png", 256);
        QCOMPARE(thumb.size(), QSize(256, 128));

        // Cached under the spec's name, tagged so other apps trust it.
        const QString cached = dir.filePath("cache/large/" + Thumbnails::cacheKey(file));
        QVERIFY(QFile::exists(cached));
        const QImage stored(cached);
        QCOMPARE(stored.text("Thumb::URI"), QUrl::fromLocalFile(file).toString(QUrl::FullyEncoded));
        QVERIFY(!stored.text("Thumb::MTime").isEmpty());

        // A fresh cache entry is served as is: overwrite it with a marker
        // color and the next load must return the cached pixels.
        QImage marked = stored;
        marked.fill(Qt::blue);
        QVERIFY(marked.save(cached));
        QCOMPARE(thumbnails.load(file, "image/png", 256).pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(Thumbnails::bucketFor(128), QString("normal"));
        QCOMPARE(Thumbnails::bucketFor(512), QString("x-large"));
    }

    void thumbnailsFollowAPictureSavedOver()
    {
        // Qt keeps the images it has shown by their URL, and a re-read makes
        // new tiles that ask again, so a picture saved over must ask with a
        // URL of its own, and the cache must make it anew.
        QTemporaryDir dir;
        const QString file = dir.filePath("photo.png");
        QVERIFY(QImage(64, 32, QImage::Format_RGB32).save(file));
        const QDateTime then = QFileInfo(file).lastModified();

        Thumbnails thumbnails(dir.filePath("cache"), {dir.filePath("none")});
        QQmlEngine engine;
        engine.addImageProvider("thumb", new ThumbnailProvider(&thumbnails));
        QQmlComponent tile(&engine);
        tile.setData("import QtQuick\nimport \"Format.js\" as Format\n"
                     "Image { required property string path; required property var modified\n"
                     "        asynchronous: true; source: Format.thumb(path, \"image/png\", 128, modified) }",
                     QUrl::fromLocalFile(QFINDTESTDATA("../src/Format.js")));
        auto shownWidth = [&](const QDateTime &modified) {
            std::unique_ptr<QObject> image(tile.createWithInitialProperties({{"path", file}, {"modified", modified}}));
            const bool shown = image && QTest::qWaitFor([&] { return image->property("implicitWidth").toInt() > 0; }, 5000);
            return shown ? image->property("implicitWidth").toInt() : -1;
        };
        QCOMPARE(shownWidth(then), 128);

        // Saved over twice within one second, as an editor's quick saves
        // can be: the spec's whole seconds can't tell them apart.
        const QDateTime second = QDateTime::fromSecsSinceEpoch(then.toSecsSinceEpoch() + 60);
        auto saveAt = [&](const QSize &size, const QDateTime &when) {
            QVERIFY(QImage(size, QImage::Format_RGB32).save(file));
            QFile saved(file);
            QVERIFY(saved.open(QIODevice::ReadWrite));
            QVERIFY(saved.setFileTime(when, QFileDevice::FileModificationTime));
        };
        saveAt({32, 64}, second.addMSecs(100));
        QCOMPARE(shownWidth(second.addMSecs(100)), 64);
        saveAt({64, 32}, second.addMSecs(600));
        QCOMPARE(shownWidth(second.addMSecs(600)), 128);
    }

    void thumbnailsReadInstalledThumbnailers()
    {
        QTemporaryDir dir;
        write(dir.filePath("demo.thumbnailer"),
              "[Thumbnailer Entry]\nTryExec=/bin/sh\nExec=/bin/sh -c \"exit 1\" %i %o %s\nMimeType=video/mp4;video/webm;\n");
        Thumbnails thumbnails(dir.filePath("cache"), {dir.path()});
        QVERIFY(thumbnails.thumbnailerFor("video/webm").startsWith("/bin/sh"));
        QVERIFY(thumbnails.thumbnailerFor("video/ogg").isEmpty());

        // A thumbnailer that fails leaves a failure marker, so it isn't retried.
        write(dir.filePath("clip.mp4"), "not a video");
        QVERIFY(thumbnails.load(dir.filePath("clip.mp4"), "video/mp4", 128).isNull());
        QVERIFY(QFile::exists(dir.filePath("cache/fail/folio/" + Thumbnails::cacheKey(dir.filePath("clip.mp4")))));
    }

    void thumbnailsRememberOnlyWhatFailed()
    {
        QTemporaryDir dir;
        QImage frame(64, 32, QImage::Format_RGB32);
        frame.fill(Qt::green);
        QVERIFY(frame.save(dir.filePath("frame.png")));
        write(dir.filePath("heif.thumbnailer"),
              "[Thumbnailer Entry]\nExec=/bin/cp \"" + dir.filePath("frame.png").toUtf8() + "\" %o\nMimeType=image/heif;\n");
        Thumbnails thumbnails(dir.filePath("cache"), {dir.path()});

        // Nothing could make one, so nothing failed: a thumbnailer installed
        // later still gets its turn.
        write(dir.filePath("bundle.zip"), "not a zip");
        QVERIFY(thumbnails.load(dir.filePath("bundle.zip"), "application/zip", 128).isNull());
        QVERIFY(!QFile::exists(dir.filePath("cache/fail/folio/" + Thumbnails::cacheKey(dir.filePath("bundle.zip")))));

        // A folder's peek asks for any picture, and gets the thumbnailer for
        // the one it names.
        write(dir.filePath("photo.heic"), "not a real heic");
        QCOMPARE(thumbnails.load(dir.filePath("photo.heic"), "image/*", 128).size(), QSize(64, 32));
    }

    void thumbnailsSetTextOnAPage()
    {
        QTemporaryDir dir;
        write(dir.filePath("notes.md"), "# Packing\n\n- passport\n- charger\n");
        const QImage page = Thumbnails::textPage(dir.filePath("notes.md"), 256, QColor("#fafaf7"), QColor("#303034"));
        QCOMPARE(page.size(), QSize(218, 256));
        // Dark ink near the top left, where the first line is; plain paper
        // at the bottom right.
        bool ink = false;
        for (int y = 0; y < 40 && !ink; ++y)
            for (int x = 0; x < 120 && !ink; ++x)
                ink = qGray(page.pixel(x, y)) < 128;
        QVERIFY(ink);
        QVERIFY(qGray(page.pixel(214, 252)) > 240);

        // A FIFO named like text is never opened: reading it would block.
        const QString fifo = dir.filePath("events.txt");
        QCOMPARE(mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
        QVERIFY(Thumbnails::textPage(fifo, 256, Qt::white, Qt::black).isNull());

        // Binary that claims to be text gets the stand-in.
        write(dir.filePath("blob.txt"), QByteArray("abc\0def", 7));
        QVERIFY(Thumbnails::textPage(dir.filePath("blob.txt"), 256, Qt::white, Qt::black).isNull());
    }

    void thumbnailsDrawAPdfsFirstPage()
    {
        if (QStandardPaths::findExecutable("pdftoppm").isEmpty())
            QSKIP("pdftoppm (Poppler) isn't installed");
        QTemporaryDir dir;
        {
            QPdfWriter pdf(dir.filePath("itinerary.pdf"));
            pdf.setPageSize(QPageSize(QPageSize::A4));
            QPainter painter(&pdf);
            painter.fillRect(QRect(0, 0, 2000, 2000), Qt::black);
        }
        Thumbnails thumbnails(dir.filePath("cache"), {dir.path()});
        const QImage page = thumbnails.load(dir.filePath("itinerary.pdf"), "application/pdf", 256);
        QCOMPARE(page.height(), 256);
        QVERIFY(page.width() > 150 && page.width() < 200);
        QVERIFY(qGray(page.pixel(5, 5)) < 64);
        QVERIFY(qGray(page.pixel(page.width() - 3, page.height() - 3)) > 200);
    }

    void previewerPicksAViewerForEachType()
    {
        QCOMPARE(Previewer::viewerFor("image/png", false), QString("image"));
        QCOMPARE(Previewer::viewerFor("image/gif", false), QString("animated"));
        QCOMPARE(Previewer::viewerFor("image/x-canon-cr2", false), QString("thumbnail"));
        QCOMPARE(Previewer::viewerFor("video/mp4", false), QString("video"));
        QCOMPARE(Previewer::viewerFor("audio/mpeg", false), QString("audio"));
        QCOMPARE(Previewer::viewerFor("application/pdf", false), QString("pdf"));
        QCOMPARE(Previewer::viewerFor("text/markdown", false), QString("markdown"));
        QCOMPARE(Previewer::viewerFor("text/x-c++src", false), QString("text"));
        QCOMPARE(Previewer::viewerFor("application/x-shellscript", false), QString("text"));
        QCOMPARE(Previewer::viewerFor("application/zip", false), QString("other"));
        QCOMPARE(Previewer::viewerFor("inode/directory", true), QString("folder"));
    }

    void previewerReadsTextButNotBinaries()
    {
        QTemporaryDir dir;
        write(dir.filePath("notes.txt"), "hello\nworld\n");
        write(dir.filePath("blob.txt"), QByteArray("abc\0def", 7));
        QCOMPARE(Previewer::readText(dir.filePath("notes.txt")), QString("hello\nworld\n"));
        QVERIFY(Previewer::readText(dir.filePath("blob.txt")).isEmpty());
        QCOMPARE(Previewer::readText(dir.filePath("notes.txt"), 5), QString("hello"));

        // A binary with a text name falls back to the plain file view.
        Previewer previewer;
        previewer.start(dir.filePath("blob.txt"), false, "name");
        QTRY_COMPARE(previewer.viewer(), QString("other"));
    }

    void previewerShowsOnlyWhatLaysOutQuickly()
    {
        // The preview lays out all it's given on its UI thread, about a
        // third of a millisecond per KB: a long log is cut to its opening
        // part, at the end of a line.
        QTemporaryDir dir;
        QByteArray log;
        for (int i = 0; log.size() < 300 * 1024; ++i)
            log += "line " + QByteArray::number(i) + " of the log\n";
        write(dir.filePath("big.log"), log);
        const QString opening = Previewer::readText(dir.filePath("big.log"));
        QVERIFY(opening.size() <= 128 * 1024);
        QVERIFY(opening.size() > 100 * 1024);
        QVERIFY(opening.endsWith("of the log\n"));

        // Markdown tables cost far more: 16 KB of one kept the preview
        // frozen for 150 ms, and a few MB for many seconds. The text stops
        // at the row where its tables pass 600 cells; prose, and a lone
        // line of | that isn't a table, are left alone.
        QByteArray table = "# Rows\n\n| a | b | c |\n|---|---|---|\n";
        for (int i = 0; i < 5000; ++i)
            table += "| " + QByteArray::number(i) + " | x | y |\n";
        write(dir.filePath("rows.md"), table);
        Previewer previewer;
        previewer.start(dir.filePath("rows.md"), false, "name");
        QTRY_VERIFY(!previewer.text().isEmpty());
        QCOMPARE(previewer.viewer(), QString("markdown"));
        QVERIFY(previewer.text().startsWith("# Rows\n\n| a | b | c |\n|---|---|---|\n| 0 | x | y |\n"));
        QVERIFY(previewer.text().count('|') <= 600);
        QVERIFY(previewer.text().count('|') > 500);
        QVERIFY(previewer.text().endsWith(" | x | y |\n"));
        const QString prose = QString("Words, words.\n").repeated(2000);
        QCOMPARE(Previewer::markdownOpening(prose), prose);
        const QString pipes = QString("|").repeated(2000) + "\nafter\n";
        QCOMPARE(Previewer::markdownOpening(pipes), QString("|").repeated(2000) + "\n");

        // A code example full of | isn't a table: what follows it stays.
        QString code = "# Usage\n\n```\n";
        for (int i = 0; i < 400; ++i)
            code += "a | b | c\n";
        code += "```\n\nThe end.\n";
        QCOMPARE(Previewer::markdownOpening(code, 600), code);

        // Four spaces before ``` make indented code, not a fence, and ~~~
        // doesn't close a ``` fence: the table after either still counts.
        QString rows;
        for (int i = 0; i < 400; ++i)
            rows += "| a | b |\n";
        for (const QString &example : {QString("    ```\n\n"), QString("```\n~~~\n```\n\n")})
            QVERIFY(Previewer::markdownOpening(example + rows, 600).count('|') <= 600);

        // A single line of three-byte characters is cut between characters,
        // never inside one.
        QTemporaryDir unicode;
        write(unicode.filePath("one-line.txt"), QString(50000, QChar(0x20AC)).toUtf8());
        const QString read = Previewer::readText(unicode.filePath("one-line.txt"), 131072);
        QVERIFY(!read.contains(QChar::ReplacementCharacter));
        QCOMPARE(read.size(), 131072 / 3);

        // A longer fence holds shorter ones as code, and only its own length
        // closes it: the table after it still counts.
        QString longFence = "````\n```\nnot a close\n````\n\n";
        for (int i = 0; i < 400; ++i)
            longFence += "| a | b | c |\n";
        QVERIFY(Previewer::markdownOpening(longFence, 600).count('|') <= 600 + 4);
    }

    void previewerStepsThroughTheFolder()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("album");
        write(dir.filePath("a.md"), "# A");
        write(dir.filePath("b.txt"), "b");
        write(dir.filePath("c.pdf"), "%PDF");
        write(dir.filePath(".hidden"), "h");

        Previewer previewer;
        QSignalSpy changed(&previewer, &Previewer::changed);
        previewer.start(dir.filePath("b.txt"), false, "name");
        // Shown at once, before the folder is listed.
        QCOMPARE(previewer.item().value("name").toString(), QString("b.txt"));
        QTRY_COMPARE(previewer.text(), QString("b"));

        // The listing moves the index without touching what's shown, which
        // would restart a playing video.
        const int shown = changed.count();
        QTRY_COMPARE(previewer.count(), 4);
        QCOMPARE(previewer.index(), 2);
        QCOMPARE(changed.count(), shown);
        previewer.previous();
        QCOMPARE(previewer.viewer(), QString("markdown"));
        previewer.previous();
        QCOMPARE(previewer.viewer(), QString("folder"));
        QTRY_COMPARE(previewer.item().value("itemCount").toInt(), 0);
        previewer.previous();
        QCOMPARE(previewer.index(), 0);
        previewer.next();
        previewer.next();
        previewer.next();
        QCOMPARE(previewer.viewer(), QString("pdf"));
        previewer.next();
        QCOMPARE(previewer.index(), 3);

        // Folio pointing at a sibling keeps the list; another folder starts over.
        previewer.show(dir.filePath("a.md"));
        QCOMPARE(previewer.index(), 1);
        QCOMPARE(previewer.count(), 4);
        QTemporaryDir other;
        write(other.filePath("x.txt"), "x");
        write(other.filePath("y.txt"), "y");
        previewer.show(other.filePath("y.txt"));
        QCOMPARE(previewer.count(), 1);
        QCOMPARE(previewer.index(), 0);
        QTRY_COMPARE(previewer.count(), 2);
        QCOMPARE(previewer.index(), 1);
        QVERIFY(changed.count() > 0);
    }

    void previewerReadsOffTheUiThread()
    {
        // A big folder, or any file on a slow mount, would hold up the
        // window for as long as reading it takes, so an item shows at once
        // and its text, or a folder's count and first pictures, follow.
        QTemporaryDir dir;
        write(dir.filePath("notes.txt"), "hello");
        QDir(dir.path()).mkdir("album");
        write(dir.filePath("album/a.jpg"), "x");
        Previewer previewer;
        previewer.start(dir.filePath("notes.txt"), false, "name");
        QCOMPARE(previewer.text(), QString());
        QTRY_COMPARE(previewer.text(), QString("hello"));
        previewer.show(dir.filePath("album"));
        QCOMPARE(previewer.item().value("itemCount").toInt(), -1);
        QTRY_COMPARE(previewer.item().value("itemCount").toInt(), 1);

        // What's read for an item left behind is dropped.
        previewer.show(dir.filePath("notes.txt"));
        previewer.show(dir.filePath("album"));
        QThreadPool::globalInstance()->waitForDone();
        QCoreApplication::processEvents();
        QCOMPARE(previewer.text(), QString());
    }

    void previewerCountsHiddenFilesWhenTheyShow()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("dots");
        write(dir.filePath("dots/.config"), "c");

        // The first folder shown counts what Folio's tile counts.
        Previewer previewer;
        previewer.start(dir.filePath("dots"), true, "name");
        QTRY_COMPARE(previewer.item().value("itemCount").toInt(), 1);
    }

    void previewerPeeksIntoFolders()
    {
        // A folder's first pictures are for the preview window, so its item
        // has them and the folder model's (which never fills them) doesn't.
        QTemporaryDir dir;
        QDir(dir.path()).mkdir("album");
        write(dir.filePath("album/a.jpg"), "x");
        Previewer previewer;
        previewer.start(dir.filePath("album"), false, "name");
        QTRY_COMPARE(previewer.item().value("peek").toStringList(), QStringList({dir.filePath("album/a.jpg")}));
        QVERIFY(!FolderModel::toMap(FolderModel::Entry()).contains("peek"));
    }

    void gridBandHitsTheTilesBothWays()
    {
        // The rubber band's hit areas sit where the grid put the tiles, also
        // mirrored with a width the columns don't divide evenly.
        QTemporaryDir home;
        for (const char *name : {"a", "b", "c", "d", "e", "f", "g"})
            write(home.filePath(name), "x");
        Backend backend(home.path());
        backend.navigate(home.path());
        QTRY_COMPARE(backend.folder()->rowCount(), 7);
        Theme theme(home.filePath("theme"));
        FileOps fileOps;
        QQmlEngine engine;
        engine.rootContext()->setContextProperty("backend", &backend);
        engine.rootContext()->setContextProperty("theme", &theme);
        engine.rootContext()->setContextProperty("fileOps", &fileOps);
        engine.rootContext()->setContextProperty("translator", QVariantMap{{"language", "en"}});
        Thumbnails thumbnails(home.filePath("cache"), {home.filePath("none")});
        engine.addImageProvider("thumb", new ThumbnailProvider(&thumbnails));
        QQmlComponent component(&engine);
        component.setData("import QtQuick\n"
                          "Window {\n"
                          "    property bool mirrored\n"
                          "    width: 1003; height: 600; visible: true\n"
                          "    Item { anchors.fill: parent\n"
                          "        LayoutMirroring.enabled: mirrored; LayoutMirroring.childrenInherit: true\n"
                          "        FolderGrid { id: grid; anchors.fill: parent } }\n"
                          "    function misplaced() {\n"
                          "        grid.forceLayout()\n"
                          "        const band = Array.from(grid.children).concat(Array.from(grid.contentItem.children))\n"
                          "                          .find(child => child.rectOf !== undefined)\n"
                          "        const off = []\n"
                          "        for (let i = 0; i < grid.count; ++i) {\n"
                          "            if (!grid.itemAtIndex(i)) return 'no tile ' + i\n"
                          "            const at = grid.itemAtIndex(i).mapToItem(band, 20, 12)\n"
                          "            if (band.rectOf(i).x !== at.x || band.rectOf(i).y !== at.y)\n"
                          "                off.push(i + ': ' + band.rectOf(i).x + ',' + band.rectOf(i).y + ' not ' + at.x + ',' + at.y)\n"
                          "        }\n"
                          "        return grid.count + (off.length ? ' ' + off.join('; ') : '')\n"
                          "    }\n"
                          "}\n",
                          QUrl::fromLocalFile(sourceFolder() + "/GridBandTest.qml"));
        for (const bool mirrored : {false, true}) {
            std::unique_ptr<QObject> window(component.createWithInitialProperties({{"mirrored", mirrored}}));
            QVERIFY2(window, qPrintable(component.errorString()));
            auto misplaced = [&] {
                QVariant result;
                QMetaObject::invokeMethod(window.get(), "misplaced", Q_RETURN_ARG(QVariant, result));
                return result.toString();
            };
            QTRY_COMPARE(misplaced(), QString("7"));
        }
    }

    void themeFallsBack()
    {
        QTemporaryDir dir;
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#FFD60A"));
        QCOMPARE(theme.accentForeground(), QColor("black"));
        QVERIFY(!theme.light());
    }

    void themeReadsThePalette()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.path() + "/theme/colors.toml",
              "mode = \"light\"\naccent = \"#112233\"\nbackground = \"#FFFCF0\"\nforeground = \"#100F0F\"\n");
        Theme theme(dir.path());
        QCOMPARE(theme.accent(), QColor("#112233"));
        QCOMPARE(theme.accentForeground(), QColor("white"));
        QCOMPARE(theme.background(), QColor("#FFFCF0"));
        // Missing derived backgrounds fall back to the base.
        QCOMPARE(theme.darkBackground(), QColor("#FFFCF0"));
        QVERIFY(theme.light());
    }

    void themeDerivesALighterBackgroundWhenItMatches()
    {
        QTemporaryDir dir;
        QDir(dir.path()).mkpath("theme");
        write(dir.filePath("theme/colors.toml"), "background = \"#101315\"\nforeground = \"#e0e0e0\"\nlighter_background = \"#101315\"\n");
        Theme same(dir.path());
        QCOMPARE(same.background(), QColor("#101315"));
        QVERIFY(same.lighterBackground() != same.background());
        QVERIFY(same.lighterBackground().lightnessF() > same.background().lightnessF());

        write(dir.filePath("theme/colors.toml"), "background = \"#101315\"\nforeground = \"#e0e0e0\"\nlighter_background = \"#2a2e36\"\n");
        Theme distinct(dir.path());
        QCOMPARE(distinct.lighterBackground(), QColor("#2a2e36"));
    }

    void themeFollowsHyprlandRounding()
    {
        QTemporaryDir dir;
        const QString response = dir.filePath("response.json"), command = dir.filePath("hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":4}");

        Theme theme(dir.path(), nullptr, command);
        QTRY_COMPARE(theme.radius(), 4);
        write(response, "{\"int\":6}");
        QTRY_COMPARE(theme.radius(), 6);
    }

    void themeAsksForRoundingOnlyWhileActive()
    {
        // The fake hyprctl lives outside the theme folder: Theme watches that
        // folder and its parent, and a change there re-reads the rounding
        // whatever the state. Both sit in a private parent, so other programs'
        // temporary files can't count as a change.
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkpath("theme") && QDir(root.path()).mkpath("bin"));
        const QString themeDir = root.filePath("theme");
        const QString response = root.filePath("bin/response.json"), command = root.filePath("bin/hyprctl");
        write(command, "#!/bin/sh\ncat '" + response.toUtf8() + "'\n");
        QVERIFY(QFile::setPermissions(command, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        write(response, "{\"int\":3}");

        Theme theme(themeDir, nullptr, command);
        QTRY_COMPARE(theme.radius(), 3);
        // Away from Folio, a change goes unnoticed for longer than the poll,
        // and it's picked up once Folio is back.
        emit qGuiApp->applicationStateChanged(Qt::ApplicationInactive);
        // A read already under way may still finish (it's killed within
        // 500ms), so the change comes after it.
        QTest::qWait(600);
        write(response, "{\"int\":6}");
        QTest::qWait(1500);
        QCOMPARE(theme.radius(), 3);
        emit qGuiApp->applicationStateChanged(Qt::ApplicationActive);
        QTRY_COMPARE(theme.radius(), 6);
    }

    void translatorFallsBackToEnglish()
    {
        QTemporaryDir dir;
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.language(), QString("en"));
        QVERIFY(translator.translate("Main", "Open").isNull());
    }

    void translatorPicksPluralForms()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "ru\n");
        write(dir.path() + "/en.json", R"({"%Ln items": {"one": "%Ln item", "other": "%Ln items"}})");
        write(dir.path() + "/ru.json", R"({"%Ln items": {"one": "%Ln элемент", "few": "%Ln элемента", "many": "%Ln элементов", "other": "%Ln элемента"}})");
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(translator.translate("Main", "%Ln items", nullptr, 21), QString("%Ln элемент"));
        QCOMPARE(translator.translate("Main", "%Ln items", nullptr, 25), QString("%Ln элементов"));
    }

    void translatorCountsEnglishFormsTheEnglishWay()
    {
        // A string no catalog has translated yet falls back to English, and
        // its English forms follow English's rule, not the language's:
        // Russian's "one" for 21 and Japanese's lone "other" would read
        // "21 item" and "1 items".
        QTemporaryDir dir;
        write(dir.path() + "/en.json", R"({"%Ln items": {"one": "%Ln item", "other": "%Ln items"}})");
        write(dir.path() + "/language", "ru\n");
        Translator russian(dir.path() + "/language", dir.path());
        QCOMPARE(russian.translate("Main", "%Ln items", nullptr, 21), QString("%Ln items"));
        write(dir.path() + "/language", "ja\n");
        Translator japanese(dir.path() + "/language", dir.path());
        QCOMPARE(japanese.translate("Main", "%Ln items", nullptr, 1), QString("%Ln item"));
    }

    void translatorWritesCountsInTheLanguagesDigits()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "fa\n");
        write(dir.path() + "/en.json", R"({"%Ln items": {"one": "%Ln item", "other": "%Ln items"}})");
        Translator translator(dir.path() + "/language", dir.path());
        QCoreApplication::installTranslator(&translator);
        const QString text = QCoreApplication::translate("Main", "%Ln items", nullptr, 1200);
        QCoreApplication::removeTranslator(&translator);
        QCOMPARE(text, QString("۱۲۰۰ items"));
    }

    void formatLeavesUnitsAndSeparatorsToTheLanguage()
    {
        // A size is one string with its number, and a file's summary keeps
        // the separator in the catalog, as a folder's already does.
        QTemporaryDir dir;
        write(dir.path() + "/language", "fr\n");
        write(dir.path() + "/fr.json", R"({"%1 · %2": "%1 ・ %2", "%1 KB": "%1\u00a0Ko"})");
        Translator translator(dir.path() + "/language", dir.path());
        QCoreApplication::installTranslator(&translator);
        QQmlEngine engine;
        QQmlComponent component(&engine);
        component.setData("import QtQuick\nimport \"Format.js\" as Format\n"
                          "QtObject { property string summary: Format.summary({ path: \"/x\", size: 1500 }, \"Texte\") }",
                          QUrl::fromLocalFile(QFINDTESTDATA("../src/Format.js")));
        std::unique_ptr<QObject> object(component.create());
        QCoreApplication::removeTranslator(&translator);
        QVERIFY2(object, qPrintable(component.errorString()));
        QCOMPARE(object->property("summary").toString(), QString("Texte ・ 1,5\u00a0Ko"));
    }

    void englishCatalogMatchesTheSources()
    {
        // en.json is the source translators work from: every text the
        // sources translate is in it, as itself or (when a count is
        // passed) as its English plural forms, and nothing else is.
        QStringList problems;
        const QMap<QString, bool> used = translatables(sourceFolder(), &problems);
        QVERIFY(used.size() > 100);
        QVERIFY(used.value("%Ln items"));
        const QJsonObject english = readCatalog(sourceFolder() + "/i18n/en.json", &problems);
        for (auto it = used.begin(); it != used.end(); ++it) {
            const QJsonValue value = english.value(it.key());
            if (value.isUndefined())
                problems << "missing from en.json: " + it.key();
            else if (it.value() && (value.toObject().keys() != QStringList{"one", "other"}
                                    || value.toObject().value("other").toString() != it.key()))
                problems << "counted, so needs its one and other forms: " + it.key();
            else if (!it.value() && value.toString() != it.key())
                problems << "should be itself: " + it.key();
        }
        for (const QString &key : english.keys())
            if (!used.contains(key))
                problems << "in en.json but no source translates it: " + key;
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void catalogsMatchTheEnglishOne()
    {
        const QStringList problems = catalogProblems(sourceFolder() + "/i18n");
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void bundledPluginsCatalogTheirText()
    {
        // Each plugin that comes with Folio has an en.json for translators
        // to work from: its manifest's text is in it, everything in it shows
        // somewhere in the plugin (a script's printf writes %1 as %%1), and
        // its other catalogs match it, as Folio's own do.
        QStringList problems;
        for (const QString &id : QDir(bundled()).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString dir = bundled() + "/" + id;
            const QJsonObject english = readCatalog(dir + "/i18n/en.json", &problems);
            QString files;
            for (const QFileInfo &info : QDir(dir).entryInfoList(QDir::Files)) {
                QFile file(info.filePath());
                if (file.open(QIODevice::ReadOnly))
                    files += QString::fromUtf8(file.readAll()).replace("%%", "%").replace("'\\''", "'");
            }
            const QJsonObject manifest = readCatalog(dir + "/manifest.json", &problems);
            const QJsonObject view = manifest.value("view").toObject();
            QStringList shown{manifest.value("section").toObject().value("title").toString(),
                              view.value("title").toString(), view.value("label").toString(),
                              view.value("empty").toString()};
            for (const QJsonValue &action : manifest.value("actions").toArray())
                shown << action.toObject().value("label").toString() << action.toObject().value("busy").toString();
            for (const QString &text : std::as_const(shown))
                if (!text.isEmpty() && !english.contains(text))
                    problems << id + ": missing from en.json: " + text;
            for (auto it = english.begin(); it != english.end(); ++it) {
                if (it.value().toString() != it.key())
                    problems << id + ": should be itself: " + it.key();
                if (!files.contains(it.key()))
                    problems << id + ": in en.json but the plugin never shows it: " + it.key();
            }
            problems << catalogProblems(dir + "/i18n");
        }
        QVERIFY2(problems.isEmpty(), qPrintable(problems.join('\n')));
    }

    void catalogChecksCatchBrokenTranslations()
    {
        QTemporaryDir dir;
        write(dir.filePath("en.json"), R"({"Open": "Open", "Open %1": "Open %1",
                                          "%Ln items": {"one": "%Ln item", "other": "%Ln items"},
                                          "%Ln items in %1": {"one": "%Ln item in %1", "other": "%Ln items in %1"}})");
        // Polish needs one, few, many and other; a form may say "one" in
        // words, but not drop the folder.
        write(dir.filePath("pl.json"), R"({"Open": "Otwórz", "Open %1": "Otwórz",
                                          "%Ln items": {"one": "jeden element", "few": "%Ln elementy", "other": "%Ln elementu"},
                                          "%Ln items in %1": {"one": "jeden element", "few": "%Ln elementy w %1",
                                                              "many": "%Ln elementów w %1", "other": "%Ln elementu w %1"}})");
        write(dir.filePath("da.json"), R"({"Open": "Åbn", "Open %1": "Åbn %1",
                                          "%Ln items": {"one": "ét element", "other": "%Ln elementer"},
                                          "%Ln items in %1": {"one": "%Ln element i %1", "other": "%Ln elementer i %1"}})");
        // German hasn't caught up yet: one string still to translate and
        // one the English has since dropped.
        write(dir.filePath("de.json"), R"({"Open": "Öffnen", "Opened": "Geöffnet"})");
        write(dir.filePath("xx.json"), "{}");
        QStringList problems = catalogProblems(dir.path());
        problems.sort();
        QCOMPARE(problems, QStringList({"pl: %Ln items in %1: one has other placeholders than the English",
                                        "pl: %Ln items: needs exactly the plural forms few, many, one, other",
                                        "pl: Open %1: has other placeholders than the English",
                                        "xx.json: not one of Omarchy's language codes"}));
    }

    void everyCatalogIsCompiledIn()
    {
        QFile resources(sourceFolder() + "/resources.qrc");
        QVERIFY(resources.open(QIODevice::ReadOnly));
        const QString listed = QString::fromUtf8(resources.readAll());
        QStringList left;
        for (const QString &name : QDir(sourceFolder() + "/i18n").entryList({"*.json"}, QDir::Files))
            if (!listed.contains("<file>i18n/" + name + "</file>"))
                left << name;
        QVERIFY2(left.isEmpty(), qPrintable("not in resources.qrc: " + left.join(", ")));
    }

    void translatorLeavesNumbersUngrouped()
    {
        QTemporaryDir dir;
        write(dir.path() + "/language", "de\n");
        Translator translator(dir.path() + "/language", dir.path());
        QCOMPARE(QLocale().toString(12345), QString("12345"));
        QCOMPARE(QLocale().toString(1.5, 'f', 1), QString("1,5"));
    }

private:
    // Packs `items`, from `folder`, into `archive`, its kind from its name.
    static void pack(const QString &archive, const QString &folder, const QStringList &items)
    {
        QProcess tar;
        tar.start("bsdtar", QStringList{"-acf", archive, "-C", folder} + items);
        QVERIFY(tar.waitForFinished() && tar.exitCode() == 0);
    }

    // Everything in a folder, the hidden too, by name.
    static QStringList everything(const QString &folder)
    {
        return QDir(folder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::Name);
    }

    static QByteArray contents(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    static void write(const QString &path, const QByteArray &contents)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
    }
};

QTEST_MAIN(FolioTests)
#include "folio_tests.moc"
