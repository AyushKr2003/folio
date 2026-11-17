import QtQuick

// The orders a folder can be in. Folders stay first in each; S steps
// through them from the keyboard.
FolioMenu {
    id: menu

    // Each with its entry here, and the sort button's glyph and tooltip.
    readonly property var orders: [
        { mode: "name", label: qsTr("Name"), glyph: "sort-name", tip: qsTr("Sorted by name") },
        { mode: "kind", label: qsTr("Kind"), glyph: "sort-kind", tip: qsTr("Sorted by kind") },
        { mode: "modified", label: qsTr("Newest"), glyph: "sort-newest", tip: qsTr("Newest first") },
        { mode: "oldest", label: qsTr("Oldest"), glyph: "sort-oldest", tip: qsTr("Oldest first") },
        { mode: "size", label: qsTr("Size"), glyph: "sort-size", tip: qsTr("Biggest first") }
    ]
    readonly property var current: orders.find(o => o.mode === backend.folder.sortMode) || orders[0]
    function step(by) {
        const i = orders.findIndex(o => o.mode === backend.folder.sortMode)
        backend.chooseSort(orders[(i + by + orders.length) % orders.length].mode)
    }

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
