import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import "Format.js" as Format

// A larger look at the selected file and the facts about it, with the one
// action that matters: open it. The last of the columns, after the file.
Rectangle {
    id: pane

    // The pane whose selected item this shows, and whose backend and file
    // work the button acts through.
    property var folderPane: null
    // Hidden, it shows nothing, so moving through files doesn't read them,
    // look for their trash notes or make large thumbnails for nobody.
    readonly property var item: visible && folderPane ? folderPane.current : ({})
    readonly property bool hasItem: item !== undefined && item.path !== undefined
    // An item in the trash shows where it came from and when it was
    // deleted, and offers Restore rather than Open. Its note is read in the
    // background; until it's in, the item looks trashed, as nearly all there
    // are, but Restore waits. What's inside a trashed folder has no note.
    readonly property bool noting: hasItem && folderPane.inTrash && info.origin === undefined
    readonly property string origin: info.origin || ""
    readonly property bool trashed: hasItem && origin.length > 0
    // A plugin's item shows its subtitle and its own details, and has no
    // date.
    readonly property bool pluginItem: hasItem && item.kind === "item"
    // The drive whose top folder is selected or shown, for its free space.
    readonly property var drive: {
        const shown = folderPane ? folderPane.paneBackend.path : ""
        for (const d of places.devices)
            if (d.free >= 0 && ((hasItem && d.path === item.path) || d.path === shown))
                return d
        return null
    }
    // Tab reaches the details after the panes while there's an app to pick
    // for the file, and Escape hands the keyboard back to the files.
    readonly property bool takesKeyboard: opensWith.visible
    readonly property bool hasKeyboard: opensWith.activeFocus
    function focusKeyboard() { opensWith.forceActiveFocus(Qt.TabFocusReason) }
    signal finished()
    function homeless(path) { return path.startsWith(places.home + "/") ? "~" + path.slice(places.home.length) : path }
    // A picture's size or a text's opening lines once they've been read off
    // the UI thread.
    readonly property var info: backend.details
    // The item's type: its name's, or the one read from what it holds when its name gave none, never one read for another item.
    readonly property string type: (info.path === item.path && info.mime) || item.mime || ""
    // Text files show their opening lines in a frame, archives the names inside.
    readonly property bool listed: hasItem && (item.kind === "text" || backend.listsArchive(type))
    // Described once the folder's loaded has put the cursor back on its
    // item: a re-read leaves another item on the cursor's row for a moment,
    // and describing that would drop what's been read of this one.
    onItemChanged: Qt.callLater(describe)
    function describe() { backend.describe(item, !!folderPane && folderPane.inTrash) }
    // Gone (a folder selected, or another view), it describes nothing, so
    // plugins stop being asked about the file it showed last.
    Component.onDestruction: backend.describe({}, false)
    // Made executable or not from the menu: the item looks the same in the
    // folder, so its permissions are read again here.
    Connections {
        target: pane.folderPane ? pane.folderPane.paneFileOps : null
        function onModesChanged(paths) { if (pane.hasItem && paths.includes(pane.item.path)) pane.describe() }
    }

    // It sits on the columns' background, and their separator marks its
    // edge.
    color: "transparent"
    // It takes all the room right of the columns. The facts and the button
    // keep to a width that still reads as one block, centered under the
    // picture.
    readonly property int textWidth: 480

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 28
        spacing: 22
        visible: pane.hasItem

        // The picture grows with the room, short of crowding out the facts.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.round(Math.min(width * 0.75, pane.height * 0.55))

            // Moving quickly through files, each shows at once the small
            // thumbnail its row has already made (soft, but instant), and
            // the sharp picture once the cursor rests on it, or at once when
            // it was read ahead: resting on a file reads the ones beside it
            // too, so stepping through a folder finds each one ready. The
            // slow work (a large picture read, a video's frame made) is never
            // started for files only passed over.
            Image {
                id: quick
                anchors.fill: parent
                asynchronous: true
                fillMode: Image.PreserveAspectFit
                source: big.shown ? Format.thumb(pane.item.path, pane.item.mime, 128, pane.item.modified) : ""
                visible: status === Image.Ready && !big.visible
            }
            Timer {
                id: rest
                interval: 120
                onTriggered: big.rest()
            }
            Connections {
                target: pane
                // The same file again (its folder re-read) stays as it is.
                function onItemChanged() { if (!pane.hasItem || pane.item.path !== big.rested) rest.restart() }
            }
            // The files beside the one rested on, read ahead out of sight.
            Repeater {
                model: big.ahead
                Image {
                    required property var modelData
                    visible: false
                    asynchronous: true
                    source: big.sourceFor(modelData)
                }
            }
            Image {
                id: big
                anchors.fill: parent
                asynchronous: true
                fillMode: Image.PreserveAspectFit
                // As many pixels as it's drawn with, in steps, so resizing
                // the window doesn't read it again at every width.
                readonly property int pixels: Math.min(8192, Math.ceil(Math.max(width, height) * Screen.devicePixelRatio / 512) * 512)
                readonly property bool shown: pane.hasItem && !pane.item.isDir && !pane.listed && !pane.pluginItem
                // The file the cursor last rested on, and those read ahead.
                property string rested: ""
                property var ahead: []
                function rest() {
                    rested = pane.hasItem ? pane.item.path : ""
                    const folder = pane.folderPane ? pane.folderPane.paneBackend.folder : null
                    const at = pane.folderPane ? pane.folderPane.currentIndex : -1
                    const near = []
                    if (folder && at >= 0)
                        for (const index of [at + 1, at - 1])
                            if (index >= 0 && index < folder.count) {
                                const item = folder.get(index)
                                if (!item.isDir && (item.kind === "image" || item.kind === "video" || item.kind === "pdf"))
                                    near.push(item)
                            }
                    ahead = near
                }
                readonly property bool ready: shown && (rested === pane.item.path || ahead.some(item => item.path === pane.item.path))
                // A picture as sharp as the column is wide (one Qt can't
                // read, HEIC, gets the largest thumbnail); anything else the
                // largest thumbnail there is (1024 pixels: a video's frame, a
                // PDF's first page).
                function sourceFor(item) {
                    return item.kind === "image" ? Format.sharp(item.path, item.mime, pixels, item.modified)
                                                 : Format.thumb(item.path, item.mime, 1024, item.modified)
                }
                // Gave up: no picture of it to be had.
                readonly property bool failed: status === Image.Error
                source: ready ? sourceFor(pane.item) : ""
                visible: status === Image.Ready
            }

            // Text files show their opening lines instead of a picture, archives the names inside.
            Rectangle {
                anchors.fill: parent
                visible: pane.listed
                color: theme.background
                border.width: 1
                border.color: Qt.alpha(theme.foreground, 0.12)
                clip: true
                FolioText {
                    anchors.fill: parent
                    anchors.margins: 14
                    text: pane.info.text || ""
                    color: theme.foreground
                    size: 10
                    wrapMode: Text.WrapAnywhere
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                }
            }

            // Only for what has no picture: while one's on its way (moving
            // quickly through files) the space stays empty rather than
            // flashing the icon.
            Glyph {
                anchors.centerIn: parent
                visible: pane.hasItem && !pane.listed
                         && (!big.shown || (big.failed && quick.status !== Image.Ready))
                name: pane.pluginItem ? pane.item.icon : pane.hasItem && pane.item.isDir ? "folder" : "file"
                size: 72
                stroke: 1.2
                color: pane.hasItem && pane.item.isDir ? theme.accent : theme.secondaryForeground
            }
        }

        ColumnLayout {
            spacing: 4
            Layout.fillWidth: true
            Layout.maximumWidth: pane.textWidth
            Layout.alignment: Qt.AlignHCenter
            // Aligned explicitly, so that they're mirrored: left as they
            // are, a Latin name would stay on the left.
            FolioText {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignLeft
                text: pane.item.name || ""
                textFormat: Text.PlainText
                color: theme.brightForeground
                size: 15
                font.weight: Font.DemiBold
                wrapMode: Text.WrapAnywhere
                maximumLineCount: 3
                elide: Text.ElideRight
            }
            FolioText {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignLeft
                text: {
                    translator.language // The type's name comes from the system, in the language of the moment.
                    return Format.summary(pane.item, backend.typeName(pane.type))
                }
                color: theme.secondaryForeground
                size: 12
                elide: Text.ElideRight
            }
        }

        GridLayout {
            Layout.fillWidth: true
            Layout.maximumWidth: pane.textWidth
            Layout.alignment: Qt.AlignHCenter
            columns: 2
            rowSpacing: 12
            columnSpacing: 16

            FolioText { visible: pane.trashed; text: qsTr("Original location"); color: theme.secondaryForeground; size: 12}
            FolioText {
                visible: pane.trashed
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: pane.trashed ? pane.homeless(pane.origin.replace(/\/[^\/]*$/, "") || "/") : ""
                textFormat: Text.PlainText
                color: theme.foreground
                size: 12
                elide: Text.ElideMiddle
            }
            // When it was made, where the filesystem keeps that.
            FolioText { visible: !!pane.info.created; text: qsTr("Created"); color: theme.secondaryForeground; size: 12}
            FolioText {
                visible: !!pane.info.created
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: pane.info.created ? Format.stamp(pane.info.created, translator.dateLocale) : ""
                color: theme.foreground
                size: 12
            }
            FolioText { visible: !pane.pluginItem; text: pane.trashed || pane.noting ? qsTr("Deleted") : qsTr("Modified"); color: theme.secondaryForeground; size: 12}
            FolioText {
                visible: !pane.pluginItem
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: {
                    const when = pane.trashed ? pane.info.deleted : pane.noting ? null : pane.item.modified
                    return pane.hasItem && when ? Format.stamp(when, translator.dateLocale) : ""
                }
                color: theme.foreground
                size: 12
            }
            // As ls writes them; Make Executable in the menu changes them. The
            // row keeps its place while they're read, so moving through files
            // doesn't shift the rows below it. Not in the trash, where nothing
            // changes them.
            readonly property bool permissionsShown: pane.hasItem && !(pane.folderPane && pane.folderPane.inTrash) && !pane.pluginItem
                                                     && pane.item.path.startsWith("/")
            FolioText { visible: parent.permissionsShown; text: qsTr("Permissions"); color: theme.secondaryForeground; size: 12 }
            FolioText {
                visible: parent.permissionsShown
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: pane.info.permissions || ""
                color: theme.foreground
                size: 12
            }
            FolioText { visible: !!pane.drive; text: qsTr("Free space"); color: theme.secondaryForeground; size: 12}
            FolioText {
                visible: !!pane.drive
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: {
                    translator.language // Re-read when the language (and so its digits) changes.
                    return pane.drive ? Format.size(pane.drive.free) : ""
                }
                color: theme.foreground
                size: 12
            }
            // What the file opens with, picked from the apps that can open
            // its type, which makes that app the type's default.
            FolioText { visible: opensWith.visible; text: qsTr("Opens with"); color: theme.secondaryForeground; size: 12}
            PlainButton {
                id: opensWith
                // Read again after picking one, which changes the default.
                property int picked: 0
                readonly property var apps: {
                    picked
                    return pane.hasItem && !pane.item.isDir && !pane.trashed && !pane.pluginItem && pane.folderPane
                        ? pane.folderPane.paneFileOps.appsFor(pane.item.path) : []
                }
                readonly property var current: apps.find(app => app.isDefault)
                // Another app's file dialog is no place to change one's apps.
                visible: apps.length > 1 && !chooser
                // Its width at most: a long app name shrinks it and loses its middle to …
                // rather than pushing the whole pane past its edge.
                Layout.alignment: Qt.AlignRight
                Layout.fillWidth: true
                Layout.maximumWidth: implicitWidth
                leftPadding: 16
                rightPadding: 16
                implicitWidth: Math.max(88, appName.implicitWidth + arrow.implicitWidth + 8 + leftPadding + rightPadding)
                implicitHeight: 28
                text: current ? current.name : qsTr("Choose")
                contentItem: RowLayout {
                    spacing: 8
                    FolioText { id: appName; Layout.fillWidth: true; text: opensWith.text; textFormat: Text.PlainText; elide: Text.ElideMiddle; color: theme.foreground; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    FolioText { id: arrow; text: "▾"; color: theme.foreground; verticalAlignment: Text.AlignVCenter }
                }
                // A second click closes it again.
                onClicked: appsMenu.visible ? appsMenu.close() : appsMenu.popup(opensWith, 0, opensWith.height + 4)
                Keys.onEscapePressed: pane.finished()
                // Gone with the keyboard on it (another item selected, or the
                // details hidden), it hands the keyboard back.
                onVisibleChanged: if (!visible && activeFocus) pane.finished()
                FolioMenu {
                    id: appsMenu
                    // A press on the button is left to its click, which closes it.
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                    Repeater {
                        model: opensWith.apps
                        FolioMenuItem {
                            required property var modelData
                            text: modelData.name
                            keys: modelData.isDefault ? "✓" : ""
                            onTriggered: {
                                pane.folderPane.paneFileOps.setDefaultApp(pane.item.path, modelData.id)
                                opensWith.picked++
                            }
                        }
                    }
                }
            }
            FolioText { visible: !!pane.info.width; text: qsTr("Dimensions"); color: theme.secondaryForeground; size: 12}
            FolioText {
                visible: !!pane.info.width
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
                text: pane.info.width ? qsTr("%1 × %2").arg(pane.info.width.toLocaleString(Qt.locale(), "f", 0))
                                                     .arg(pane.info.height.toLocaleString(Qt.locale(), "f", 0)) : ""
                color: theme.foreground
                size: 12
            }
            // Plugins' rows, label and value in turn, after Folio's own: a
            // plugin's item's details, or what plugins say about a file.
            Repeater {
                model: (pane.pluginItem ? pane.item.details : pane.info.plugins || []).reduce((all, row) => all.concat([row[0], row[1]]), [])
                FolioText {
                    required property string modelData
                    required property int index
                    readonly property bool label: index % 2 === 0
                    Layout.fillWidth: !label
                    horizontalAlignment: label ? Text.AlignLeft : Text.AlignRight
                    text: modelData
                    color: label ? theme.secondaryForeground : theme.foreground
                    size: 12
                    elide: label ? Text.ElideNone : Text.ElideMiddle
                }
            }
        }

        Item { Layout.fillHeight: true }

        AbstractButton {
            id: openButton
            // A file chooser answers from its own bar, not by opening.
            visible: !chooser
            Layout.fillWidth: true
            Layout.maximumWidth: pane.textWidth
            Layout.alignment: Qt.AlignHCenter
            implicitHeight: 40
            focusPolicy: Qt.NoFocus
            hoverEnabled: true
            enabled: !pane.noting
            // Names the app the file opens in: "Open with Plain", read again
            // when Opens with picks another.
            readonly property string app: {
                opensWith.picked
                return pane.hasItem && !pane.item.isDir && pane.folderPane ? pane.folderPane.paneFileOps.defaultAppName(pane.type) : ""
            }
            text: pane.trashed || pane.noting ? qsTr("Restore") : pane.hasItem && pane.item.isDir ? qsTr("Open folder")
                //: %1 is the name of the app the file opens in, like "Plain".
                : app ? qsTr("Open with %1").arg(app) : qsTr("Open")
            Accessible.name: text
            onClicked: pane.trashed ? pane.folderPane.paneFileOps.restore([pane.item.path]) : pane.folderPane.paneBackend.open(pane.item.path)
            background: Rectangle {
                radius: Math.min(4, theme.radius)
                color: openButton.hovered ? Qt.lighter(theme.accent, 1.08) : theme.accent
            }
            contentItem: RowLayout {
                // A long app name ends in … rather than running past the button.
                FolioText { Layout.leftMargin: 14; Layout.fillWidth: true; text: openButton.text; textFormat: Text.PlainText; elide: Text.ElideRight; color: theme.accentForeground; font.weight: Font.DemiBold }
                FolioText { Layout.rightMargin: 14; text: "⏎"; color: theme.accentForeground }
            }
        }
    }
}
