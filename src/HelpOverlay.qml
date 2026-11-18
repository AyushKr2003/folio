import QtQuick
import QtQuick.Layouts
import QtQuick.Controls

// Every shortcut, one keypress away (?), in groups: what it does on the
// left, its keys as keycaps on the right. The groups flow into as many
// columns as the window has room for. It has the keyboard while it's shown,
// so the arrows scroll it rather than move through the files behind.
Rectangle {
    id: overlay

    signal dismissed()

    color: Qt.rgba(0, 0, 0, 0.55)

    // ↑ ↓, Page Up, Page Down, Home and End, for a window too short for it
    // all. Tab stays here too, rather than going on to a button behind the
    // overlay.
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
            event.accepted = true
            return
        }
        const page = flick.height - 40
        const by = { [Qt.Key_Up]: -40, [Qt.Key_Down]: 40, [Qt.Key_PageUp]: -page, [Qt.Key_PageDown]: page,
                     [Qt.Key_Home]: -flick.contentHeight, [Qt.Key_End]: flick.contentHeight }[event.key]
        if (by === undefined)
            return
        flick.contentY = Math.max(0, Math.min(flick.contentHeight - flick.height, flick.contentY + by))
        event.accepted = true
    }

    // Each shortcut is what it does, then its keys: the ways to do it, each
    // a list of keys pressed together. A word starting with ~ is something
    // done with the mouse ("click"), said rather than drawn as a key. Key
    // names (Shift, Alt, Ctrl) stay as they are in every language.
    // The preview window gives its own, in the same form.
    readonly property bool mirrored: LayoutMirroring.enabled
    property var groups: [
        { title: qsTr("Moving around"), shortcuts: [
            [qsTr("Move the selection"), [["←", "↑", "→", "↓"]]],
            [qsTr("Into a folder / back out"), mirrored ? [["←"], ["→"]] : [["→"], ["←"]]],
            [qsTr("A screenful up or down"), [["Page Up"], ["Page Down"]]],
            [qsTr("Open"), [["⏎"]]],
            [qsTr("Go to the parent folder"), [["Backspace"]]],
            [qsTr("Back / forward"), [["Alt", "←"], ["Alt", "→"]]],
            [qsTr("Go home"), [["Alt", "Home"]]],
            [qsTr("Type a path or a server's address"), [["Ctrl", "L"]]],
            [qsTr("Search this folder and below"), [["/"]]],
            [qsTr("Sidebar, panes and details"), [["Tab"], ["Shift", "Tab"]]]
        ]},
        { title: qsTr("Selecting"), shortcuts: [
            [qsTr("Select a range"), [["Shift", "~" + qsTr("click")], ["Shift", "↑↓"]]],
            [qsTr("Add or remove one"), [["Alt", "~" + qsTr("click")]]],
            [qsTr("Select with a rectangle"), [["~" + qsTr("drag on empty space")]]],
            [qsTr("Select everything"), [["Ctrl", "A"]]]
        ]},
        { title: qsTr("Files"), shortcuts: [
            [qsTr("Copy, cut, paste"), [["Ctrl", "C"], ["Ctrl", "X"], ["Ctrl", "V"]]],
            [qsTr("Copy the path as text"), [["Ctrl", "Shift", "C"]]],
            [qsTr("Rename"), [["F2"]]],
            [qsTr("Duplicate"), [["Ctrl", "Shift", "D"]]],
            [qsTr("New folder"), [["Ctrl", "Shift", "N"]]],
            [qsTr("Preview the selected file"), [["Space"]]],
            [qsTr("Move to the trash"), [["Delete"]]],
            [qsTr("Delete permanently (asks first)"), [["Shift", "Delete"]]],
            [qsTr("Undo / redo"), [["Ctrl", "Z"], ["Ctrl", "Shift", "Z"]]],
            [qsTr("Pin or unpin the folder"), [["Ctrl", "P"]]],
            [qsTr("Everything else"), [["~" + qsTr("right-click")], ["Alt", "⏎"]]],
            [qsTr("Stop the work under way"), [["Escape"]]]
        ]},
        { title: qsTr("View"), shortcuts: [
            [qsTr("Columns, list or grid"), [["Ctrl", "1"], ["Ctrl", "2"], ["Ctrl", "3"]]],
            [qsTr("Bigger, smaller or normal tiles"), [["Ctrl", "="], ["Ctrl", "-"], ["Ctrl", "0"]]],
            [qsTr("Next or previous sort order"), [["S"], ["Shift", "S"]]],
            [qsTr("Show hidden files"), [["Ctrl", "H"]]],
            [qsTr("Reload"), [["F5"]]]
        ]},
        { title: qsTr("Window"), shortcuts: [
            [qsTr("New window"), [["Ctrl", "N"]]],
            [qsTr("Split into two panes"), [["Ctrl", "Alt", "N"]]],
            [qsTr("Close the right pane"), [["Ctrl", "W"]]],
            [qsTr("Unpin from the sidebar"), [["Delete"]]],
            [qsTr("Show these shortcuts"), [["?"]]],
            [qsTr("Close the window"), [["Q"]]]
        ]}
    ]

    // As many columns as fit, and the groups dealt out among them in turn
    // to whichever is shortest, so they come out about as long. A column
    // narrows to fit a small window (the preview's can be 480 wide), its
    // descriptions eliding first, and what still doesn't fit scrolls.
    readonly property int columnWidth: Math.min(400, Math.max(240, width - 64 - 56))
    readonly property int columnGap: 48
    readonly property int columnCount: Math.max(1, Math.min(3, Math.floor((width - 64 - 56 + columnGap) / (columnWidth + columnGap))))
    readonly property var columns: {
        const columns = [], lengths = []
        for (let i = 0; i < columnCount; ++i) {
            columns.push([])
            lengths.push(0)
        }
        for (const group of groups) {
            const shortest = lengths.indexOf(Math.min(...lengths))
            columns[shortest].push(group)
            lengths[shortest] += group.shortcuts.length + 2
        }
        return columns
    }

    MouseArea { anchors.fill: parent; onClicked: overlay.dismissed() }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, content.implicitWidth + 56)
        height: Math.min(parent.height - 64, content.implicitHeight + 56)
        color: theme.background
        border.width: 1
        border.color: Qt.alpha(theme.foreground, 0.16)
        radius: theme.radius
        // Clicks inside don't dismiss it.
        MouseArea { anchors.fill: parent }

        Flickable {
            id: flick
            anchors.fill: parent
            anchors.margins: 28
            contentWidth: Math.max(width, content.implicitWidth)
            contentHeight: content.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: FolioScrollBar { }

            ColumnLayout {
                id: content
                width: Math.max(implicitWidth, parent.width)
                spacing: 20

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignLeft
                        text: qsTr("Keyboard shortcuts")
                        color: theme.brightForeground
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }
                    Text { text: qsTr("Escape closes"); color: theme.secondaryForeground; font.pixelSize: 12 }
                }

                RowLayout {
                    spacing: overlay.columnGap
                    Repeater {
                        model: overlay.columns
                        ColumnLayout {
                            id: column
                            required property var modelData
                            Layout.preferredWidth: overlay.columnWidth
                            Layout.maximumWidth: overlay.columnWidth
                            Layout.alignment: Qt.AlignTop
                            spacing: 22
                            Repeater {
                                model: column.modelData
                                Group { }
                            }
                        }
                    }
                }
            }
        }
    }

    // A group: its title, then a row for each shortcut.
    component Group: ColumnLayout {
        id: group
        required property var modelData
        Layout.fillWidth: true
        spacing: 2
        Text {
            Layout.bottomMargin: 6
            text: group.modelData.title
            color: theme.secondaryForeground
            font.pixelSize: 11
            font.letterSpacing: 1.3
            font.capitalization: Font.AllUppercase
        }
        Repeater {
            model: group.modelData.shortcuts
            RowLayout {
                id: shortcut
                required property var modelData
                Layout.fillWidth: true
                Layout.preferredHeight: 28
                spacing: 16
                // Aligned explicitly, so that it's mirrored with the rest.
                Text {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignLeft
                    text: shortcut.modelData[0]
                    color: theme.foreground
                    elide: Text.ElideRight
                }
                Row {
                    spacing: 6
                    Repeater {
                        model: shortcut.modelData[1]
                        Row {
                            id: way
                            required property var modelData
                            required property int index
                            spacing: 4
                            Text {
                                visible: way.index > 0
                                anchors.verticalCenter: parent.verticalCenter
                                text: "/"
                                color: theme.secondaryForeground
                                font.pixelSize: 12
                            }
                            Repeater {
                                model: way.modelData
                                Key { }
                            }
                        }
                    }
                }
            }
        }
    }

    // A key as a keycap, or something done with the mouse as a word.
    component Key: Item {
        id: key
        required property string modelData
        readonly property bool word: modelData.startsWith("~")
        implicitWidth: word ? said.implicitWidth : Math.max(24, cap.implicitWidth + 14)
        implicitHeight: 22
        Rectangle {
            visible: !key.word
            anchors.fill: parent
            radius: Math.min(4, theme.radius)
            color: Qt.alpha(theme.foreground, 0.06)
            border.width: 1
            border.color: Qt.alpha(theme.foreground, 0.18)
        }
        Text {
            id: cap
            visible: !key.word
            anchors.centerIn: parent
            text: key.modelData
            color: theme.accent
            font.pixelSize: 12
        }
        Text {
            id: said
            visible: key.word
            anchors.verticalCenter: parent.verticalCenter
            text: key.modelData.slice(1)
            color: theme.secondaryForeground
            font.pixelSize: 12
        }
    }
}
