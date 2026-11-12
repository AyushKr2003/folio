#pragma once

#include <QCoreApplication>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <memory>

// A whole disk (a USB stick, an SD card, an internal drive) as udisks
// describes it, with what's on it gathered from its partitions and whatever
// is unlocked on top of them.
struct Disk {
    // udisks' objects: the disk's own block device, and its drive.
    QString block;
    QString drive;
    QString device;
    // Who it is, not only where: its object path, its drive's Id (maker,
    // model and serial), its size, when it was put in and what's on it. A
    // stick pulled out and another put in takes the same object path, and a
    // card swapped in a reader the same drive (a copy of the card, the same
    // contents too), so a disk chosen in a menu is only changed while this
    // still matches.
    QString identity;
    QString vendor, model, label;
    qint64 size = 0;
    QString bus;
    bool removable = false;
    bool canPowerOff = false;
    bool ejectable = false;
    bool readOnly = false;
    bool optical = false;
    // udisks' own guess that it belongs to the computer rather than being
    // a device plugged into it.
    bool hintSystem = false;
    // Part of it makes up another device (an LVM volume group, a RAID
    // array) or is swap in use. Whatever is mounted from those can't be
    // followed back here, so it may hold the system without saying so.
    bool inUse = false;
    // Where anything on it is mounted, and the filesystems mounted there,
    // which have to let go before the disk can be written.
    QStringList mounts;
    QStringList filesystems;
    // Encrypted volumes on it that are unlocked, which hold it until
    // they're locked again.
    QStringList unlocked;
};

// Folio's way to udisks2: listing disks and the few changes it makes to
// them. Every change goes through udisks, so polkit decides who may make it
// and udisks refuses what's in use. Tests stand a fake in for it, which
// writes into a file instead of a device. Each change returns an empty
// string, or a sentence saying why it couldn't be done.
class Disks {
    Q_DECLARE_TR_FUNCTIONS(Disks)

public:
    virtual ~Disks() = default;

    // The one Folio uses: udisks on the system bus, unless something (a
    // test) has put another in its place.
    static std::shared_ptr<Disks> instance();
    static void replace(std::shared_ptr<Disks> disks);

    virtual QList<Disk> list() = 0;
    virtual QString unmount(const QString &filesystem) = 0;
    virtual QString lock(const QString &encrypted) = 0;
    // A new, empty GPT partition table over the whole disk.
    virtual QString makeTable(const Disk &disk) = 0;
    // One partition filling the table, formatted `type` (a FileSystem's)
    // with `label`; `partition` is the filesystem made.
    virtual QString addPartition(const Disk &disk, const QString &type, const QString &label, QString *partition) = 0;
    // Whether this machine has what it takes to make `type` (mkfs.exfat,
    // mkfs.fat…), as udisks says.
    virtual bool canFormat(const QString &type) = 0;
    virtual QString mount(const QString &filesystem) = 0;
    // A file descriptor writing to the whole disk, which only udisks may
    // hand out. The caller closes it.
    virtual QString open(const Disk &disk, int *fd) = 0;
    // Lets the disk go for good, so it's safe to pull out.
    virtual QString eject(const Disk &disk) = 0;

    // udisks' objects as GetManagedObjects lists them: object path, then
    // interface (without "org.freedesktop.UDisks2."), then its properties.
    // Object paths and byte strings are plain strings here, MountPoints a
    // string list.
    using Objects = QMap<QString, QMap<QString, QVariantMap>>;
    static QList<Disk> parse(const Objects &objects);

    // Whether a filesystem mounted at `root` is part of the system itself
    // (/, /boot, /home…), which no drive menu may touch.
    static bool isSystemMount(const QString &root);
    static bool holdsSystem(const Disk &disk);

    // How a person knows the disk: its maker and model ("SanDisk Ultra"),
    // or its label, and with its size ("SanDisk Ultra · 32 GB").
    static QString name(const Disk &disk);
    static QString title(const Disk &disk);
    static QString size(qint64 bytes);

    // Whether the disk may be erased, by formatting it or burning an image
    // of `imageSize` bytes onto it: only disks plugged into the computer
    // rather than part of it, that can be written, not the system's, and
    // big enough for the image.
    enum Fit { Unfit, TooSmall, Fits };
    static Fit fit(const Disk &disk, qint64 imageSize);

    // What a drive can be formatted as, in the order offered: udisks' name
    // for it, how a person knows it, what it's good for, the longest label
    // its mkfs keeps (in UTF-16 characters, ext4's in UTF-8 bytes), and its
    // GPT partition type.
    struct FileSystem {
        const char *type;
        const char *name;
        const char *detail;
        int labelLimit;
        const char *partitionType;
    };
    static const QList<FileSystem> &fileSystems();
    static const FileSystem *fileSystem(const QString &type);
    // The options udisks formats `type` with.
    static QVariantMap formatOptions(const QString &type, const QString &label);
};
