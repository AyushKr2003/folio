#pragma once

#include <QList>
#include <QObject>
#include <QString>

typedef struct _GVolume GVolume;
typedef struct _GVolumeMonitor GVolumeMonitor;

// The drives GIO knows, through GVfs's udisks monitor: the list Nautilus
// shows, partitions not mounted yet included (another system's, a USB stick
// just plugged in), and the system's own and hidden ones (EFI, recovery)
// already left out by udisks. Mounting a drive as it's plugged in goes
// through here too; mounting one to open it is the window's Mounter, which
// can put a passphrase request to the user, and letting go of one is
// Places::eject.
class Drives : public QObject {
    Q_OBJECT

public:
    struct Volume {
        QString name;
        // What finds the volume again: its device, else its UUID, else its name.
        QString id;
        // Where it's mounted; empty while it isn't.
        QString root;
        // While mounted: how it's let go of.
        bool canEject = false;
        bool canUnmount = false;
    };

    explicit Drives(QObject *parent = nullptr);
    ~Drives() override;

    QList<Volume> volumes() const;

    // Mounts it with nobody to ask for a passphrase, for a drive plugged in
    // when no window can.
    void mountQuietly(const QString &id);

    // The volume `id` names, with a reference the caller drops; null when
    // it's gone.
    static GVolume *find(const QString &id);
    static QString idOf(GVolume *volume);
    // GNOME's "mount drives as they're plugged in" setting, on where it
    // isn't installed.
    static bool automountWanted();
    // What udev found on the block device `name` (sda1, dm-0): its
    // ID_FS_USAGE, "filesystem" or "crypto" (LUKS), say; empty if nothing.
    static QString usageOf(const QString &name);
    // Whether it's an encrypted volume, by what udev found on it.
    static bool encrypted(GVolume *volume);
    // How long a plugged drive is left for another automounter first.
    static constexpr int automountDelayMs = 2000;

signals:
    void changed();
    // A drive was plugged in that udisks says should mount by itself.
    void plugged(const QString &id, const QString &name);
    // `root` just went away: whatever shows a folder on it moves out.
    void leaving(const QString &root);
    void notice(const QString &notice);
    void failed(const QString &message);

private:
    friend struct DrivesCallbacks;
    GVolumeMonitor *m_monitor = nullptr;
};
