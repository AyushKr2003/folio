import QtQuick

// The menu of a drive in the sidebar: Mount, Eject (or Unmount, as its
// button does), and Format… for a stick, card or other drive plugged in
// that udisks knows, as long as it doesn't hold the system.
FolioMenu {
    id: menu

    // The sidebar's row for the drive: {name, path, release}, with no path
    // but its `volume` while it isn't mounted.
    property var drive: ({})
    // What udisks knows of it (fileOps.driveAt), or empty.
    property var disk: ({})

    signal openRequested(var drive)
    signal formatRequested(var disk)

    // Opens at the pointer, or (from the keyboard) beside the row.
    function show(row, near) {
        drive = row
        disk = row.path ? fileOps.driveAt(row.path) : ({})
        if (near)
            popup(near, Math.min(near.width / 2, 120), near.height / 2)
        else
            popup()
    }

    FolioMenuItem {
        text: qsTr("Mount")
        onTriggered: menu.openRequested(menu.drive)
    }
    FolioMenuItem {
        shown: !!menu.drive.release
        text: menu.drive.release === "eject" ? qsTr("Eject") : qsTr("Unmount")
        onTriggered: places.eject(menu.drive.path)
    }
    MenuLine { shown: menu.disk.id !== undefined }
    FolioMenuItem {
        shown: menu.disk.id !== undefined
        text: qsTr("Format…")
        onTriggered: menu.formatRequested(menu.disk)
    }
}
