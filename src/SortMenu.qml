import QtQuick

// The orders a folder can be in. Folders stay first in each; S steps
// through them from the keyboard.
FolioMenu {
    id: menu

    // Provided by PathBar so sort order definitions live in one place.
    property var orders: []

    Repeater {
        model: menu.orders
        FolioMenuItem {
            required property var modelData
            text: modelData.label
            checked: backend.folder.sortMode === modelData.mode
            onTriggered: backend.chooseSort(modelData.mode)
        }
    }
}
