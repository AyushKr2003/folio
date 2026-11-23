import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Format.js" as Format

// Home's folders, pinned folders, views, drives and plugins' sections. The one holding the current folder stays lit while you
// dig into it, so you always know which root you're under.
Rectangle {
    id: sidebar

    color: theme.background
    // A long sidebar scrolls inside itself instead of spilling over the
    // status bar below it.
    clip: true

    // Keyboard: Tab moves here from the files and back. ↑ ↓ move between
    // rows, ⏎ (or →, toward the files) opens one and hands the keyboard
    // back to the files, and Delete does what a row's ✕ does.
    signal finished()
    // Format… in a drive's menu, with what udisks knows of the drive.
    signal formatRequested(var disk)
    // Rename… and Delete… in a folder's menu.
    signal renameRequested(string path)
    signal deleteRequested(string path)
    // Trash in a folder's menu, done by the pane used last, as the rest are.
    signal trashRequested(string path)
    // A place picked here: the columns start at it, rather than at the
    // folders above it.
    signal placeChosen(string path)
    function go(path) {
        placeChosen(path)
        target.navigate(path)
    }
    property int cursor: 0
    // The pane the sidebar acts on: the one used last.
    property var target: backend

    // Something is being dragged over the sidebar (anywhere on it, rows
    // included), so the pin box shows.
    property int dropHovers: 0
    readonly property bool dragging: drop.containsDrag || dropHovers > 0 || pinDrop.containsDrag
    // Each plugin section's rows, then the row for its view.
    function sectionRows(section) { return section.view ? section.rows.concat([section.view]) : section.rows }
    readonly property var pluginRows: plugins.sections.reduce((all, section) => all.concat(sectionRows(section)), [])
    readonly property var rows: [].concat(places.places, places.pinned, places.views, places.devices, pluginRows)
    readonly property int pinnedAt: places.places.length
    readonly property int viewsAt: pinnedAt + places.pinned.length
    readonly property int devicesAt: viewsAt + places.views.length
    readonly property int pluginsAt: devicesAt + places.devices.length
    // Where a plugin section's rows start in `rows`.
    function sectionAt(index) {
        let at = pluginsAt
        for (let i = 0; i < index; ++i)
            at += sectionRows(plugins.sections[i]).length
        return at
    }
    // A plugin's row is lit anywhere inside what it opens.
    function inside(root) {
        const here = sidebar.target.path
        return root.length > 0 && (here === root || here.startsWith(root + "/"))
    }

    function focusSidebar() {
        // Start on the row for where we are (a plugin's item for anywhere
        // in what it opens), or the top.
        let at = 0
        for (let i = 0; i < rows.length; ++i)
            if (rows[i].root !== undefined ? inside(rows[i].root) : rows[i].path === activePath)
                at = i
        cursor = at
        forceActiveFocus()
    }
    function activate(i) {
        const row = rows[i]
        if (!row)
            return
        if (row.root !== undefined)
            sidebar.target.open(row.path)
        else
            sidebar.go(row.path)
        finished()
    }
    function reveal(item) {
        const y = item.mapToItem(column, 0, 0).y
        if (y < flick.contentY)
            flick.contentY = Math.max(0, y - 20)
        else if (y + item.height > flick.contentY + flick.height)
            flick.contentY = y + item.height - flick.height + 20
    }

    Keys.onUpPressed: cursor = Math.max(0, cursor - 1)
    Keys.onDownPressed: cursor = Math.min(rows.length - 1, cursor + 1)
    Keys.onReturnPressed: (event) => { if (!openDriveMenu(event)) activate(cursor) }
    Keys.onEnterPressed: (event) => { if (!openDriveMenu(event)) activate(cursor) }
    // The way to the files: left, when mirrored.
    Keys.onRightPressed: if (!LayoutMirroring.enabled) activate(cursor)
    Keys.onLeftPressed: if (LayoutMirroring.enabled) activate(cursor)
    Keys.onEscapePressed: finished()
    // A pinned folder is unpinned, a plugin's item taken off the sidebar.
    Keys.onDeletePressed: {
        const row = rows[cursor]
        if (cursor >= pinnedAt && cursor < viewsAt)
            places.unpin(row.path)
        else if (row && row.forgettable)
            plugins.forget(row.plugin, row.id)
    }
    // Rows taken away under the keyboard leave it on the last one.
    onRowsChanged: cursor = Math.max(0, Math.min(cursor, rows.length - 1))

    // Alt+⏎ (or the Menu key) on a drive or a folder of Home or Pinned
    // opens its menu beside it; the files' menu stands aside for it.
    readonly property var keyedDrive: activeFocus && cursor >= devicesAt && cursor < pluginsAt
                                      ? driveRows.itemAt(cursor - devicesAt) : null
    readonly property var keyedPlace: !activeFocus ? null
                                    : cursor < pinnedAt ? homeRows.itemAt(cursor)
                                    : cursor < viewsAt ? pinRows.itemAt(cursor - pinnedAt) : null
    function menuKey(event) {
        return ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.AltModifier))
            || event.key === Qt.Key_Menu || (event.key === Qt.Key_F10 && (event.modifiers & Qt.ShiftModifier))
    }
    Keys.onShortcutOverride: (event) => event.accepted = menuKey(event) && (keyedDrive !== null || keyedPlace !== null)
    // Return has its own handler, which Keys calls before this one.
    Keys.onPressed: (event) => event.accepted = openDriveMenu(event)
    function showPlaceMenu(row, isPinned, near) {
        placeMenuLoader.active = true
        placeMenuLoader.item.show(row, isPinned, near)
    }
    function showDriveMenu(row, near) {
        driveMenuLoader.active = true
        driveMenuLoader.item.show(row, near)
    }
    function openDriveMenu(event) {
        if (!menuKey(event))
            return false
        if (keyedDrive)
            showDriveMenu(keyedDrive.modelData, keyedDrive)
        else if (keyedPlace)
            showPlaceMenu(keyedPlace.modelData, cursor >= pinnedAt, keyedPlace)
        else
            return false
        return true
    }

    Loader {
        id: placeMenuLoader
        active: false
        visible: false
        source: "PlaceMenu.qml"
    }
    Connections {
        target: placeMenuLoader.item
        function onRenameRequested(path) { sidebar.renameRequested(path) }
        function onDeleteRequested(path) { sidebar.deleteRequested(path) }
        function onTrashRequested(path) { sidebar.trashRequested(path) }
    }

    Loader {
        id: driveMenuLoader
        active: false
        visible: false
        source: "DriveMenu.qml"
    }
    Connections {
        target: driveMenuLoader.item
        function onOpenRequested(path) { sidebar.go(path); sidebar.finished() }
        function onFormatRequested(disk) { sidebar.formatRequested(disk) }
    }

    // The deepest place the current folder sits under.
    readonly property string activePath: {
        let best = ""
        const here = sidebar.target.path
        for (const list of [places.places, places.pinned, places.views, places.devices,
                            plugins.sections.filter(section => section.view).map(section => section.view)]) {
            for (const p of list) {
                if (p.path !== "/" && (here === p.path || here.startsWith(p.path + "/")) && p.path.length > best.length)
                    best = p.path
            }
        }
        return best
    }

    // A drive that couldn't be let go of says why where the files are.
    Connections {
        target: places
        function onFailed(message) { sidebar.target.showMessage(message) }
    }

    component SectionLabel: FolioText {
        Layout.topMargin: 21
        Layout.leftMargin: 14
        Layout.bottomMargin: 6
        color: theme.secondaryForeground
        size: 11
        font.letterSpacing: 1.3
        font.capitalization: Font.AllUppercase
    }

    component PlaceRow: AbstractButton {
        id: row
        property string glyph
        property bool active: false
        // The keyboard is on this row (the sidebar has focus).
        property bool keyed: false
        // Files dropped here go into this folder, or into the trash.
        property string dropPath
        property bool dropTrashes: false
        readonly property bool dropHover: rowDrop.containsDrag
        property string status
        property color statusColor
        // A quiet second line under the name, for what's too long to sit
        // beside it in the narrow sidebar.
        property string detail
        // Room at the row's far end (its left, when mirrored), for a button
        // over it. Padding isn't mirrored by itself.
        property real endPadding: 14
        // What a plugin says about this folder: a status, and an icon of
        // its own.
        readonly property var mark: dropPath.length > 0 ? plugins.marks[dropPath] : undefined

        Layout.fillWidth: true
        implicitHeight: detail.length > 0 ? 44 : 34
        leftPadding: mirrored ? endPadding : 14
        rightPadding: mirrored ? 14 : endPadding
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        Accessible.name: text

        background: Rectangle {
            radius: Math.min(4, theme.radius)
            color: row.active || row.dropHover ? Qt.alpha(theme.accent, 0.16)
                 : row.hovered && row.enabled ? Qt.alpha(theme.foreground, 0.06) : "transparent"
            border.width: row.dropHover ? 2 : row.keyed ? 1 : 0
            border.color: theme.accent
        }

        DropArea {
            id: rowDrop
            anchors.fill: parent
            enabled: row.dropPath.length > 0 || row.dropTrashes
            keys: ["text/uri-list"]
            onEntered: (drag) => { drag.accepted = row.dropTrashes ? drag.hasUrls : fileOps.canDrop(drag.urls, row.dropPath) }
            onDropped: (drop) => {
                if (row.dropTrashes) {
                    fileOps.trash(fileOps.localPaths(drop.urls))
                } else {
                    fileOps.dropInto(drop.urls, row.dropPath)
                }
                drop.accept(Qt.MoveAction)
            }
        }
        contentItem: RowLayout {
            spacing: 14
            // Kept in its place, but empty, while the row shows a button
            // over it. A plugin's mark can give the row an icon of its own.
            Glyph {
                name: row.mark && row.mark.icon ? row.mark.icon : row.glyph
                opacity: row.glyph ? 1 : 0
                color: row.active ? theme.brightForeground : theme.foreground
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2
                // Set, so that they're mirrored: left as they are, Latin
                // text would follow its own script and stay on the left.
                FolioText {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignLeft
                    text: row.text
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    color: row.active ? theme.brightForeground : theme.foreground
                }
                FolioText {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignLeft
                    visible: row.detail.length > 0
                    text: row.detail
                    elide: Text.ElideRight
                    color: theme.secondaryForeground
                    size: 11
                }
            }
            FolioText {
                readonly property string said: row.mark ? row.mark.status : row.status
                visible: said.length > 0
                text: said
                color: !row.mark ? row.statusColor : row.mark.tone === "error" ? theme.red
                     : row.mark.tone === "warning" ? theme.yellow : theme.secondaryForeground
                size: 11
            }
        }
    }

    // Notices a drag over the sidebar, to show the pin box; rows and the
    // box take the drop itself.
    DropArea {
        id: drop
        anchors.fill: parent
        keys: ["text/uri-list"]
    }

    Flickable {
        id: flick
        anchors.fill: parent
        contentHeight: column.implicitHeight + 20
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: column
            x: 12
            y: 20
            width: sidebar.width - 24
            spacing: 1

            // The section's label is home itself: click it to go there.
            SectionLabel {
                Layout.topMargin: 0
                text: qsTr("Home")
                color: sidebar.target.path === places.home ? theme.accent
                     : homeHover.hovered ? theme.foreground : theme.secondaryForeground
                Accessible.role: Accessible.Button
                HoverHandler { id: homeHover; cursorShape: Qt.PointingHandCursor }
                TapHandler { onTapped: sidebar.go(places.home) }
            }
            Repeater {
                id: homeRows
                model: places.places
                PlaceRow {
                    id: homeRow
                    required property var modelData
                    required property int index
                    dropPath: modelData.path
                    onDropHoverChanged: sidebar.dropHovers += dropHover ? 1 : -1
                    keyed: sidebar.activeFocus && sidebar.cursor === 0 + index
                    onKeyedChanged: if (keyed) sidebar.reveal(this)
                    text: modelData.name
                    glyph: modelData.icon
                    active: sidebar.activePath === modelData.path
                    onClicked: sidebar.go(modelData.path)
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: sidebar.showPlaceMenu(homeRow.modelData, false, null)
                    }
                }
            }

            // Pinned folders from anywhere, kept in GTK's bookmarks. While a
            // folder is dragged over the sidebar, the section shows (even
            // empty) as the place to drop it.
            SectionLabel { text: qsTr("Pinned"); visible: places.pinned.length > 0 || sidebar.dragging }
            Repeater {
                id: pinRows
                model: places.pinned
                PlaceRow {
                    id: pinRow
                    required property var modelData
                    required property int index
                    dropPath: modelData.path
                    onDropHoverChanged: sidebar.dropHovers += dropHover ? 1 : -1
                    keyed: sidebar.activeFocus && sidebar.cursor === sidebar.pinnedAt + index
                    onKeyedChanged: if (keyed) sidebar.reveal(this)
                    text: modelData.name
                    glyph: modelData.icon
                    active: sidebar.activePath === modelData.path
                    onClicked: sidebar.go(modelData.path)
                    // The name ends short of the unpin button while it shows:
                    // hovered, or on the keyboard's row, where Delete does
                    // the same.
                    endPadding: unpin.visible ? unpin.implicitWidth + 6 : 14
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: sidebar.showPlaceMenu(pinRow.modelData, true, null)
                    }
                    IconButton {
                        id: unpin
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: 28
                        implicitHeight: 28
                        visible: pinRow.hovered || hovered || pinRow.keyed
                        glyph: "close"
                        text: qsTr("Unpin")
                        onClicked: places.unpin(pinRow.modelData.path)
                    }
                }
            }
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 34
                visible: sidebar.dragging
                color: Qt.alpha(theme.accent, pinDrop.containsDrag ? 0.22 : 0.08)
                border.width: pinDrop.containsDrag ? 2 : 1
                border.color: theme.accent
                radius: Math.min(4, theme.radius)
                FolioText {
                    anchors.centerIn: parent
                    text: qsTr("Drop to pin")
                    color: theme.accent
                    size: 12
                }
                DropArea {
                    id: pinDrop
                    anchors.fill: parent
                    keys: ["text/uri-list"]
                    onDropped: (drop) => {
                        for (const path of fileOps.localPaths(drop.urls)) {
                            const problem = places.pin(path)
                            if (problem.length > 0)
                                sidebar.target.showMessage(problem)
                        }
                        drop.accept(Qt.LinkAction)
                    }
                }
            }

            // Views: lists that aren't a folder of their own.
            SectionLabel { text: qsTr("Views") }
            Repeater {
                model: places.views
                PlaceRow {
                    required property var modelData
                    required property int index
                    dropTrashes: modelData.icon === "trash"
                    onDropHoverChanged: sidebar.dropHovers += dropHover ? 1 : -1
                    text: modelData.icon === "trash" ? qsTr("Trash") : qsTr("Recents")
                    glyph: modelData.icon
                    active: sidebar.activePath === modelData.path
                    onClicked: sidebar.go(modelData.path)
                    keyed: sidebar.activeFocus && sidebar.cursor === sidebar.viewsAt + index
                    onKeyedChanged: if (keyed) sidebar.reveal(this)
                }
            }

            // Each drive says how much is free under its name, where there's
            // room for it, and while it's hovered or has the keyboard its icon
            // becomes the button that ejects it. Right-clicked, it has a menu
            // of what else can be done to it.
            SectionLabel { text: qsTr("Devices"); visible: places.devices.length > 0 }
            Repeater {
                id: driveRows
                model: places.devices
                PlaceRow {
                    id: driveRow
                    required property var modelData
                    required property int index
                    dropPath: modelData.path
                    onDropHoverChanged: sidebar.dropHovers += dropHover ? 1 : -1
                    keyed: sidebar.activeFocus && sidebar.cursor === sidebar.devicesAt + index
                    onKeyedChanged: if (keyed) sidebar.reveal(this)
                    text: modelData.name
                    glyph: eject.visible ? "" : modelData.icon
                    active: sidebar.activePath === modelData.path
                    onClicked: sidebar.go(modelData.path)
                    detail: {
                        translator.language // Re-read when the language (and so its digits) changes.
                        return modelData.free < 0 ? "" : Format.free(modelData.free)
                    }
                    TapHandler {
                        acceptedButtons: Qt.RightButton
                        onTapped: sidebar.showDriveMenu(driveRow.modelData, null)
                    }
                    IconButton {
                        id: eject
                        // Where the drive's own icon is, centered on it.
                        anchors.left: parent.left
                        anchors.leftMargin: driveRow.leftPadding + 8 - width / 2
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: 28
                        implicitHeight: 28
                        visible: driveRow.modelData.release !== "" && (driveRow.hovered || hovered || driveRow.keyed)
                        glyph: "eject"
                        text: driveRow.modelData.release === "eject" ? qsTr("Eject") : qsTr("Unmount")
                        onClicked: places.eject(driveRow.modelData.path)
                    }
                }
            }

            // Plugins' sections: their items, or the ones opened lately,
            // then the row for the view of them all.
            Repeater {
                model: plugins.sections
                ColumnLayout {
                    id: section
                    required property var modelData
                    required property int index
                    readonly property int at: sidebar.sectionAt(index)
                    Layout.fillWidth: true
                    spacing: 1
                    SectionLabel { text: section.modelData.title }
                    Repeater {
                        model: section.modelData.rows
                        PlaceRow {
                            id: itemRow
                            required property var modelData
                            required property int index
                            dropPath: modelData.folder
                            onDropHoverChanged: sidebar.dropHovers += dropHover ? 1 : -1
                            keyed: sidebar.activeFocus && sidebar.cursor === section.at + index
                            onKeyedChanged: if (keyed) sidebar.reveal(this)
                            text: modelData.name
                            glyph: modelData.icon
                            active: sidebar.inside(modelData.root)
                            onClicked: sidebar.target.open(modelData.path)
                            endPadding: forgetItem.visible ? forgetItem.implicitWidth + 6 : 14
                            IconButton {
                                id: forgetItem
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                implicitWidth: 28
                                implicitHeight: 28
                                visible: itemRow.modelData.forgettable && (itemRow.hovered || hovered || itemRow.keyed)
                                glyph: "close"
                                text: qsTr("Remove from the sidebar")
                                onClicked: plugins.forget(itemRow.modelData.plugin, itemRow.modelData.id)
                            }
                        }
                    }
                    PlaceRow {
                        visible: !!section.modelData.view
                        keyed: sidebar.activeFocus && sidebar.cursor === section.at + section.modelData.rows.length
                        onKeyedChanged: if (keyed) sidebar.reveal(this)
                        text: section.modelData.view ? section.modelData.view.name : ""
                        glyph: section.modelData.view ? section.modelData.view.icon : ""
                        active: sidebar.activePath === (section.modelData.view ? section.modelData.view.path : "")
                        // How many there are, quietly, in the language's digits.
                        status: {
                            translator.language // Re-read when the language (and so its digits) changes.
                            return section.modelData.view ? section.modelData.view.count.toLocaleString(Qt.locale(), "f", 0) : ""
                        }
                        statusColor: theme.secondaryForeground
                        onClicked: sidebar.go(section.modelData.view.path)
                    }
                }
            }
        }
    }
}
