import QtQuick

// The menu of a folder in the sidebar's Home or Pinned section: Rename…,
// Hide (taking it out of Home, until Show in Home in its own menu puts it
// back) or Unpin, and Trash, which with Alt held becomes Delete… (for
// good, after asking).
FolioMenu {
    id: menu

    // The sidebar's row for the folder: {name, path}, and whether it's
    // pinned rather than one of home's own.
    property var place: ({})
    property bool pinned: false

    signal renameRequested(string path)
    signal trashRequested(string path)
    signal deleteRequested(string path)

    // Opens at the pointer, or (from the keyboard) beside the row, with
    // its first entry already highlighted.
    function show(row, isPinned, near) {
        place = row
        pinned = isPinned
        held = fileOps.altHeld()
        if (near)
            popup(near, Math.min(near.width / 2, 120), near.height / 2)
        else
            popup()
        currentIndex = 0
    }

    // Alt can go down or up anywhere while the menu's open, so it's checked
    // a few times a second rather than through key events.
    property bool held: false
    Timer {
        interval: 50
        repeat: true
        running: menu.visible
        onTriggered: menu.held = fileOps.altHeld()
    }

    FolioMenuItem {
        text: qsTr("Rename…")
        onTriggered: menu.renameRequested(menu.place.path)
    }
    FolioMenuItem {
        text: menu.pinned ? qsTr("Unpin") : qsTr("Hide")
        onTriggered: menu.pinned ? places.unpin(menu.place.path) : places.hideFromHome(menu.place.path)
    }
    MenuLine { }
    FolioMenuItem {
        text: menu.held ? qsTr("Delete…") : qsTr("Trash")
        onTriggered: menu.held ? menu.deleteRequested(menu.place.path) : menu.trashRequested(menu.place.path)
    }
}
