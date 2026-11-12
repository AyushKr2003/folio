#include "disks.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusUnixFileDescriptor>
#include <QFileInfo>
#include <QLocale>
#include <QMutex>
#include <algorithm>
#include <fcntl.h>

namespace {

const QString service = "org.freedesktop.UDisks2";
const QString prefix = "org.freedesktop.UDisks2.";

// Byte strings from udisks end in a NUL.
QString fromBytes(const QByteArray &bytes) {
    return QString::fromUtf8(bytes.endsWith('\0') ? bytes.chopped(1) : bytes);
}

// GetManagedObjects' property values as Disks::Objects holds them.
QVariant plain(const QVariant &value) {
    if (value.metaType() == QMetaType::fromType<QDBusObjectPath>())
        return value.value<QDBusObjectPath>().path();
    if (value.metaType() == QMetaType::fromType<QByteArray>())
        return fromBytes(value.toByteArray());
    if (value.metaType() == QMetaType::fromType<QDBusArgument>()) {
        const QDBusArgument arg = value.value<QDBusArgument>();
        if (arg.currentSignature() != "aay")
            return {};
        QStringList strings;
        arg.beginArray();
        while (!arg.atEnd()) {
            QByteArray bytes;
            arg >> bytes;
            strings << fromBytes(bytes);
        }
        arg.endArray();
        return strings;
    }
    return value;
}

// udisks on the system bus. A change waits for its answer, which can take
// as long as someone takes to answer polkit's password prompt, so it's made
// from a worker, never the UI thread. Listing disks and asking what can be
// made ask polkit nothing, and the UI thread gives them a few seconds.
class UDisks : public Disks {
public:
    QList<Disk> list() override {
        const QDBusMessage reply = call("/org/freedesktop/UDisks2", "org.freedesktop.DBus.ObjectManager",
                                        "GetManagedObjects", {}, 5000);
        if (reply.type() == QDBusMessage::ErrorMessage)
            return {};
        Objects objects;
        const QDBusArgument arg = reply.arguments().value(0).value<QDBusArgument>();
        arg.beginMap();
        while (!arg.atEnd()) {
            QDBusObjectPath path;
            arg.beginMapEntry();
            arg >> path;
            arg.beginMap();
            while (!arg.atEnd()) {
                QString interface;
                QVariantMap properties;
                arg.beginMapEntry();
                arg >> interface >> properties;
                arg.endMapEntry();
                if (!interface.startsWith(prefix))
                    continue;
                for (auto it = properties.begin(); it != properties.end(); ++it)
                    it.value() = plain(it.value());
                objects[path.path()][interface.mid(prefix.size())] = properties;
            }
            arg.endMap();
            arg.endMapEntry();
        }
        arg.endMap();
        return parse(objects);
    }

    QString unmount(const QString &filesystem) override {
        const QDBusMessage reply = call(filesystem, prefix + "Filesystem", "Unmount", {QVariantMap()});
        // Unmounted meanwhile is what was wanted.
        return reply.errorName() == "org.freedesktop.UDisks2.Error.NotMounted" ? QString() : failure(reply);
    }

    QString lock(const QString &encrypted) override {
        return failure(call(encrypted, prefix + "Encrypted", "Lock", {QVariantMap()}));
    }

    QString makeTable(const Disk &disk) override {
        // Tearing down also locks anything unlocked on it, which unmounting
        // alone leaves open.
        return failure(call(disk.block, prefix + "Block", "Format", {QString("gpt"), QVariantMap{{"tear-down", true}}}));
    }

    QString addPartition(const Disk &disk, const QString &type, const QString &label, QString *partition) override {
        const FileSystem *fs = fileSystem(type);
        if (!fs)
            return QObject::tr("Folio can't make that kind of drive.");
        // From 1 MiB, as parted's "1MiB 100%" does; a size of 0 fills the
        // rest of the disk.
        const QDBusMessage reply = call(disk.block, prefix + "PartitionTable", "CreatePartitionAndFormat",
                                        {QVariant::fromValue(qulonglong(1 << 20)), QVariant::fromValue(qulonglong(0)),
                                         QString::fromLatin1(fs->partitionType), QString(), QVariantMap(), type,
                                         formatOptions(type, label)});
        if (reply.type() != QDBusMessage::ErrorMessage)
            *partition = reply.arguments().value(0).value<QDBusObjectPath>().path();
        return failure(reply);
    }

    bool canFormat(const QString &type) override {
        const QDBusMessage reply = call("/org/freedesktop/UDisks2/Manager", prefix + "Manager", "CanFormat", {type}, 5000);
        if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty())
            return false;
        // (available, the program missing when it isn't).
        const QDBusArgument answer = reply.arguments().constFirst().value<QDBusArgument>();
        bool available = false;
        QString missing;
        answer.beginStructure();
        answer >> available >> missing;
        answer.endStructure();
        return available;
    }

    QString mount(const QString &filesystem) override {
        return failure(call(filesystem, prefix + "Filesystem", "Mount", {QVariantMap()}));
    }

    QString open(const Disk &disk, int *fd) override {
        // Exclusive, so nothing else can have it open meanwhile, and each
        // write synchronous, so the progress shown is what's on the disk.
        const QDBusMessage reply = call(disk.block, prefix + "Block", "OpenDevice",
                                        {QString("w"), QVariantMap{{"flags", int(O_EXCL | O_SYNC | O_CLOEXEC)}}});
        if (reply.type() == QDBusMessage::ErrorMessage)
            return failure(reply);
        const auto descriptor = reply.arguments().value(0).value<QDBusUnixFileDescriptor>();
        // The message's own copy closes with it.
        *fd = descriptor.isValid() ? fcntl(descriptor.fileDescriptor(), F_DUPFD_CLOEXEC, 0) : -1;
        return *fd < 0 ? tr("The drive couldn't be opened.") : QString();
    }

    QString eject(const Disk &disk) override {
        // Powered off, a stick or card reader is safe to pull out; a drive
        // that can't be only ejects its medium.
        if (disk.canPowerOff)
            return failure(call(disk.drive, prefix + "Drive", "PowerOff", {QVariantMap()}));
        if (disk.ejectable)
            return failure(call(disk.drive, prefix + "Drive", "Eject", {QVariantMap()}));
        return {};
    }

private:
    // Calls udisks and waits for the answer.
    static QDBusMessage call(const QString &path, const QString &interface, const QString &method,
                             const QVariantList &args, int timeout = 60 * 60 * 1000) {
        QDBusMessage message = QDBusMessage::createMethodCall(service, path, interface, method);
        message.setArguments(args);
        // Lets polkit ask for a password when one is needed.
        message.setInteractiveAuthorizationAllowed(true);
        return QDBusConnection::systemBus().call(message, QDBus::Block, timeout);
    }

    // A failed call as a sentence a person can act on, udisks' own message
    // for anything else; empty when it didn't fail.
    static QString failure(const QDBusMessage &reply) {
        if (reply.type() != QDBusMessage::ErrorMessage)
            return {};
        const QString error = reply.errorName();
        if (error.startsWith("org.freedesktop.UDisks2.Error.NotAuthorized"))
            return tr("Permission was refused.");
        if (error == "org.freedesktop.UDisks2.Error.DeviceBusy")
            return tr("Something is still using it.");
        if (error == "org.freedesktop.DBus.Error.UnknownObject" || error == "org.freedesktop.DBus.Error.UnknownMethod")
            return tr("The drive is gone.");
        if (error == "org.freedesktop.DBus.Error.ServiceUnknown")
            return tr("Drives can't be changed without udisks2.");
        return reply.errorMessage();
    }
};

QMutex s_lock;
std::shared_ptr<Disks> s_instance;

}

const QList<Disks::FileSystem> &Disks::fileSystems() {
    // exFAT works everywhere and holds anything; FAT32 is for old devices
    // that know nothing newer; ext4 for drives only Linux will use. (NTFS
    // adds nothing exFAT doesn't, and a Mac can't write to it.) Microsoft
    // basic data for the first two, Linux filesystem data for ext4. Labels:
    // exFAT and FAT keep 11 characters, ext4 16.
    static const QList<FileSystem> all{
        {"exfat", "exFAT", QT_TR_NOOP("Works with Mac, Windows and Linux, and holds big files."), 11,
         "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"},
        {"vfat", "FAT32", QT_TR_NOOP("Works almost anywhere, old cameras and car stereos too, but no file can be over 4 GB."), 11,
         "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7"},
        {"ext4", "ext4", QT_TR_NOOP("For Linux only, keeping Linux's permissions."), 16,
         "0fc63daf-8483-4772-8e79-3d69d8477de4"},
    };
    return all;
}

const Disks::FileSystem *Disks::fileSystem(const QString &type) {
    for (const FileSystem &fs : fileSystems())
        if (type == QLatin1String(fs.type))
            return &fs;
    return nullptr;
}

QVariantMap Disks::formatOptions(const QString &type, const QString &label) {
    QVariantMap options{{"label", label}};
    // A Linux filesystem keeps owners, and a new one belongs to root, so
    // the person formatting it couldn't put anything on it; udisks hands
    // it to them. (exFAT and FAT have no owners to give.)
    if (type == "ext4")
        options["take-ownership"] = true;
    return options;
}

std::shared_ptr<Disks> Disks::instance() {
    QMutexLocker locker(&s_lock);
    if (!s_instance)
        s_instance = std::make_shared<UDisks>();
    return s_instance;
}

void Disks::replace(std::shared_ptr<Disks> disks) {
    QMutexLocker locker(&s_lock);
    s_instance = std::move(disks);
}

QList<Disk> Disks::parse(const Objects &objects) {
    auto property = [&](const QString &path, const QString &interface, const QString &name) {
        return objects.value(path).value(interface).value(name);
    };
    // The whole disk a block device is part of: a partition's table, and
    // an unlocked volume's encrypted device, followed down to a disk.
    auto diskOf = [&](QString path) {
        for (int depth = 0; depth < 8; ++depth) {
            const QString table = property(path, "Partition", "Table").toString();
            const QString backing = property(path, "Block", "CryptoBackingDevice").toString();
            if (!table.isEmpty() && table != "/")
                path = table;
            else if (!backing.isEmpty() && backing != "/")
                path = backing;
            else
                break;
        }
        return path;
    };

    QMap<QString, Disk> disks;
    for (auto it = objects.begin(); it != objects.end(); ++it) {
        const QVariantMap block = it.value().value("Block");
        const QString drive = block.value("Drive").toString();
        if (block.isEmpty() || it.value().contains("Partition") || it.value().contains("Loop") || drive.isEmpty()
            || drive == "/" || block.value("Size").toLongLong() <= 0)
            continue;
        const QVariantMap about = objects.value(drive).value("Drive");
        Disk disk;
        disk.block = it.key();
        disk.drive = drive;
        disk.device = block.value("Device").toString();
        disk.identity = QStringList{disk.block, about.value("Id").toString(), block.value("Size").toString(),
                                    about.value("TimeMediaDetected").toString()}
                            .join('\n');
        disk.size = block.value("Size").toLongLong();
        disk.readOnly = block.value("ReadOnly").toBool();
        disk.hintSystem = block.value("HintSystem").toBool();
        disk.vendor = about.value("Vendor").toString().trimmed();
        disk.model = about.value("Model").toString().trimmed();
        disk.bus = about.value("ConnectionBus").toString();
        disk.removable = about.value("Removable").toBool() || about.value("MediaRemovable").toBool();
        disk.canPowerOff = about.value("CanPowerOff").toBool();
        disk.ejectable = about.value("Ejectable").toBool();
        const QStringList media = about.value("MediaCompatibility").toStringList();
        disk.optical = about.value("Optical").toBool()
                       || std::any_of(media.begin(), media.end(), [](const QString &m) { return m.startsWith("optical"); });
        disks.insert(disk.block, disk);
    }

    // What's on each disk: its filesystems, wherever they're mounted, what's
    // unlocked on it, the first label among them, and whether it's part of
    // something bigger.
    for (auto it = objects.begin(); it != objects.end(); ++it) {
        auto disk = disks.find(diskOf(it.key()));
        if (disk == disks.end())
            continue;
        const QVariantMap block = it.value().value("Block");
        // What's unlocked comes and goes with its key (burning locks it),
        // so it's no part of which disk this is.
        if (const QString backing = block.value("CryptoBackingDevice").toString(); !backing.isEmpty() && backing != "/")
            disk->unlocked << backing;
        else
            disk->identity += '\n' + block.value("IdUUID").toString();
        // LVM2 and RAID members are "raid" to udisks.
        if (block.value("IdUsage").toString() == "raid"
            || it.value().value("Swapspace").value("Active").toBool())
            disk->inUse = true;
        if (disk->label.isEmpty() && block.value("IdUsage").toString() == "filesystem")
            disk->label = block.value("IdLabel").toString();
        const QStringList mounts = it.value().value("Filesystem").value("MountPoints").toStringList();
        if (!mounts.isEmpty()) {
            disk->mounts << mounts;
            disk->filesystems << it.key();
        }
    }

    QList<Disk> list = disks.values();
    std::sort(list.begin(), list.end(), [](const Disk &a, const Disk &b) { return a.device < b.device; });
    return list;
}

bool Disks::isSystemMount(const QString &root) {
    // Where the system itself lives; a drive mounted here is the computer,
    // not a device.
    static const QStringList roots{"/", "/boot", "/efi", "/home", "/var", "/usr", "/opt", "/srv",
                                   "/root", "/tmp", "/nix", "/snap", "/swap"};
    return roots.contains(root)
           || std::any_of(roots.begin() + 1, roots.end(), [&](const QString &r) { return root.startsWith(r + "/"); })
           || root.startsWith("/.");
}

bool Disks::holdsSystem(const Disk &disk) {
    return disk.inUse || std::any_of(disk.mounts.begin(), disk.mounts.end(), isSystemMount);
}

QString Disks::name(const Disk &disk) {
    // Card readers and cheap sticks report a placeholder for their maker.
    static const QStringList generic{"usb", "generic", "ata", "mass", "mass storage", "linux"};
    QString vendor = generic.contains(disk.vendor.toLower()) ? QString() : disk.vendor;
    if (disk.model.startsWith(vendor, Qt::CaseInsensitive))
        vendor.clear();
    const QString made = QStringList{vendor, disk.model}.join(' ').trimmed();
    if (!made.isEmpty())
        return made;
    return disk.label.isEmpty() ? QFileInfo(disk.device).fileName() : disk.label;
}

QString Disks::size(qint64 bytes) {
    // Whole units, as drives are sold ("32 GB"), but a tenth below ten of
    // them ("1.5 TB").
    double value = bytes;
    while (value >= 1000)
        value /= 1000;
    return QLocale().formattedDataSize(bytes, value < 9.95 ? 1 : 0, QLocale::DataSizeSIFormat);
}

QString Disks::title(const Disk &disk) {
    return tr("%1 · %2").arg(name(disk), size(disk.size));
}

Disks::Fit Disks::fit(const Disk &disk, qint64 imageSize) {
    if (holdsSystem(disk) || disk.hintSystem || disk.readOnly || disk.optical || disk.size <= 0)
        return Unfit;
    const bool plugged = disk.removable || disk.bus == "usb" || disk.bus == "sdio" || disk.canPowerOff;
    if (!plugged)
        return Unfit;
    return disk.size < imageSize ? TooSmall : Fits;
}
