import QtQuick
import Folio 1.0
import QtQuick.Layouts
import QtQuick.Effects
import "Format.js" as Format

// One item as a row, for the list and column views: a small picture or
// glyph, the name, and in the list the size and date beside it.
Item {
    id: row

    required property int index
    required property string name
    required property string path
    required property bool isDir
    required property string kind
    required property string mime
    required property string extension
    required property var size
    required property var modified
    required property int itemCount
    required property string icon
    required property bool selected
    required property int depth
    required property bool expanded

    property bool current: false
    // Cut and not yet pasted: drawn in the colour of a disabled menu entry.
    readonly property bool cut: fileOps.cutCount > 0 && fileOps.isCut(path)
    // In a column to the left: the folder the next column is showing.
    property bool trail: false
    property bool detailed: false
    property bool chevron: false
    // In the list, a tree: each level indented, and a folder's arrow opens
    // it in place (or closes it).
    property bool tree: false
    // The list's size step: the row's height and picture grow with it, the
    // name and details don't, as with the grid's tiles.
    property real zoom: 1
    readonly property int picture: Math.round(20 * zoom)

    signal clicked(int modifiers)
    signal activated()
    signal toggled()

    implicitHeight: Math.round(32 * zoom)

    // The name always gets room: in a narrow window the date goes first,
    // then the size. (The icon says what kind a file is; there's no column
    // for it.)
    readonly property bool showDate: detailed && width >= 480
    readonly property bool showSize: detailed && width >= 380

    readonly property string glyph: row.isDir ? "folder"
                                  : row.kind === "item" ? row.icon
                                  : row.kind === "image" ? "pictures"
                                  : row.kind === "video" ? "videos"
                                  : row.kind === "audio" ? "music"
                                  : row.kind === "pdf" || row.kind === "text" ? "documents"
                                  : "file"

    // A folder row takes files dropped on it, lighting up while they hover.
    DropArea {
        id: folderDrop
        anchors.fill: parent
        enabled: row.isDir
        keys: ["text/uri-list"]
        onEntered: (drag) => { drag.accepted = fileOps.canDrop(drag.urls, row.path) }
        onDropped: (drop) => { fileOps.dropInto(drop.urls, row.path); drop.accept(Qt.MoveAction) }
    }

    Rectangle {
        anchors.fill: parent
        anchors.leftMargin: 4
        anchors.rightMargin: 4
        radius: Math.min(4, theme.radius)
        border.width: folderDrop.containsDrag ? 2 : 0
        border.color: theme.accent
        color: row.current || folderDrop.containsDrag ? Qt.alpha(theme.accent, 0.18)
             : row.trail ? Qt.alpha(theme.foreground, 0.08)
             : hover.hovered ? Qt.alpha(theme.foreground, 0.05)
             : "transparent"
    }

    RowLayout {
        anchors.fill: parent
        // Each level is indented by half a picture and 10, which keeps an
        // open folder's arrows under its picture at any size.
        anchors.leftMargin: row.tree ? 4 + row.depth * (Math.round(row.picture / 2) + 10) : 14
        anchors.rightMargin: 14
        spacing: 12

        // The folder's arrow: pointing on (with the reading direction)
        // while it's closed, down while it's open.
        Item {
            visible: row.tree
            Layout.rightMargin: -8
            implicitWidth: 18
            implicitHeight: 32
            Glyph {
                visible: row.isDir
                anchors.centerIn: parent
                name: row.LayoutMirroring.enabled ? "back" : "forward"
                size: 12
                rotation: row.expanded ? (row.LayoutMirroring.enabled ? -90 : 90) : 0
                color: arrowHover.hovered ? theme.foreground : theme.secondaryForeground
            }
            HoverHandler { id: arrowHover; enabled: row.isDir }
            TapHandler {
                enabled: row.isDir
                // Before the row's own tap, which would select it instead.
                grabPermissions: PointerHandler.CanTakeOverFromAnything
                onTapped: row.toggled()
            }
        }

        Item {
            objectName: "picture"
            opacity: row.cut ? 0.4 : 1
            implicitWidth: row.picture
            implicitHeight: row.picture
            Image {
                id: thumb
                anchors.fill: parent
                layer.enabled: theme.radius > 0 && status === Image.Ready
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: rowMask
                    maskThresholdMin: 0.5
                    maskSpreadAtMin: 1
                }
                asynchronous: true
                // Pictures fill the square; a page (a PDF's) shows whole.
                fillMode: row.kind === "image" || row.kind === "video" ? Image.PreserveAspectCrop : Image.PreserveAspectFit
                // A small picture of any file there's one of, as the tiles
                // have; a text's opening lines would be unreadable this
                // small, and folders and plugins' items keep their glyph.
                source: row.isDir || row.kind === "item" || row.kind === "text" ? "" : Format.thumb(row.path, row.mime, 128, row.modified)
                visible: status === Image.Ready
            }
            Rectangle {
                id: rowMask
                anchors.fill: parent
                radius: Math.min(4, theme.radius)
                visible: false
                layer.enabled: true
            }
            Glyph {
                anchors.centerIn: parent
                visible: !thumb.visible
                name: row.glyph
                size: Math.round(16 * row.zoom)
                color: row.isDir && !row.cut ? theme.accent : theme.secondaryForeground
            }
        }
        // The name and the date are aligned explicitly, so that they're
        // mirrored: left as they are, they'd follow their own script, and a
        // Latin name would sit at the far end of a right-to-left row.
        FolioText {
            Layout.fillWidth: true
            Layout.minimumWidth: 120
            horizontalAlignment: Text.AlignLeft
            text: row.name
            textFormat: Text.PlainText
            color: row.cut ? theme.secondaryForeground : row.current ? theme.accent : theme.foreground
            elide: Text.ElideMiddle
        }
        // The size and date columns grow with the text, so the system's text
        // size doesn't push them into the name.
        FolioText {
            visible: row.showSize
            Layout.preferredWidth: Math.round(96 * theme.textScale)
            horizontalAlignment: Text.AlignRight
            text: row.kind === "item" ? row.extension : row.isDir ? Format.count(row.itemCount) : Format.size(row.size)
            color: theme.secondaryForeground
            size: 12
            elide: Text.ElideRight
        }
        FolioText {
            visible: row.showDate
            Layout.preferredWidth: Math.round(96 * theme.textScale)
            Layout.leftMargin: 12
            horizontalAlignment: Text.AlignLeft
            text: Format.when(row.modified, translator.dateLocale)
            color: theme.secondaryForeground
            size: 12
            elide: Text.ElideRight
        }
        // Points to where the next column opens.
        Glyph {
            visible: row.chevron && row.isDir
            name: row.LayoutMirroring.enabled ? "back" : "forward"
            size: 12
            color: theme.secondaryForeground
        }
    }

    DragPicture {
        id: dragPicture
        // Cleared first: Qt (6.12) reloads a new picture at the size of the
        // one before, which for a grab only warns ("Ignoring sourceSize").
        onReady: { row.Drag.imageSource = ""; row.Drag.imageSource = url }
    }

    HoverHandler {
        id: hover
        onHoveredChanged: if (hovered) dragPicture.capture(row)
    }
    // Given another file by the list, a row still under the pointer stays
    // hovered: its picture is retaken, not kept from the file before.
    ListView.onReused: {
        row.Drag.imageSource = ""
        if (hover.hovered)
            dragPicture.capture(row)
    }
    TapHandler {
        id: tap
        onTapped: row.clicked(tap.point.modifiers)
        onDoubleTapped: row.activated()
    }

    // Like the tiles: dragged, a row is a file for the sidebar or another app.
    Drag.dragType: Drag.Automatic
    Drag.supportedActions: Qt.CopyAction | Qt.LinkAction
    // The pointer holds the picture by its icon and name, which are at its
    // right end when mirrored.
    Drag.hotSpot.x: LayoutMirroring.enabled ? row.width : 0
    DragHandler {
        target: null
        enabled: row.kind !== "item"
        // Dragging a selected item takes the whole selection along. The list
        // is built as the drag starts, not again on every change of selection.
        onActiveChanged: {
            if (active)
                row.Drag.mimeData = { "text/uri-list": fileOps.uriList(row.selected && backend.folder.selectedCount > 1 ? backend.folder.selectedPaths() : [row.path]) }
            row.Drag.active = active
        }
    }
}
