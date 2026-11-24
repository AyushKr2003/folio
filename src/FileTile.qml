import QtQuick
import Folio 1.0
import QtQuick.Shapes
import QtQuick.Effects
import "Format.js" as Format

// One item in the grid: a preview on top, its name below. Folders are a
// quiet card; files show a real thumbnail when one
// can be made, and a drawn stand-in for their kind when not; a plugin's
// items are their icon.
Item {
    id: tile

    required property int index
    property real zoom: 1
    required property string name
    required property string path
    required property bool isDir
    required property string kind
    required property string mime
    required property string extension
    required property int itemCount
    required property string icon
    required property var size
    required property var modified
    required property bool selected

    property bool current: false
    // Cut and not yet pasted: its picture faded and its name in the colour of
    // a disabled menu entry.
    readonly property bool cut: fileOps.cutCount > 0 && fileOps.isCut(path)

    signal clicked(int modifiers)
    signal activated()

    // Pictures and drawings round their corners with the theme: square on
    // Omarchy's default, softly rounded on themes that round windows.
    readonly property real iconRadius: Math.min(10, theme.radius)

    // The badge and stand-in color for each kind, from the theme's palette.
    readonly property color kindColor: kind === "pdf" ? theme.red
                                     : kind === "text" ? theme.blue
                                     : kind === "audio" ? theme.magenta
                                     : kind === "video" ? theme.orange
                                     : kind === "archive" ? theme.yellow
                                     : kind === "image" ? theme.cyan
                                     : theme.secondaryForeground

    // The art is drawn at 128×92 and scaled, so a zoom keeps its shapes;
    // the box around it takes the scaled size for the layout.
    Item {
        id: artBox
        objectName: "picture"
        opacity: tile.cut ? 0.4 : 1
        width: 128 * tile.zoom
        height: 92 * tile.zoom
        anchors.top: parent.top
        anchors.topMargin: 18
        anchors.horizontalCenter: parent.horizontalCenter

        Item {
            id: art
            width: 128
            height: 92
            scale: tile.zoom
            transformOrigin: Item.TopLeft

            Loader {
                anchors.fill: parent
                sourceComponent: tile.isDir ? folderArt : tile.kind === "item" ? itemArt : fileArt
            }

        }
    }

    FolioText {
        id: label
        anchors.top: artBox.bottom
        anchors.topMargin: 12
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width - 28
        text: tile.name
        textFormat: Text.PlainText
        color: tile.cut ? theme.secondaryForeground : tile.current ? theme.accent : theme.foreground
        size: 12
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
        maximumLineCount: 2
        elide: Text.ElideRight
        lineHeight: 1.15
    }

    // Size and when, quietly, under the name: "28.0 KB · 2:14 PM" today,
    // "28.0 KB · 11/9/26" before; just the item count for a folder, the
    // subtitle for a plugin's item.
    FolioText {
        anchors.top: label.bottom
        anchors.topMargin: 3
        anchors.horizontalCenter: parent.horizontalCenter
        width: parent.width - 28
        text: {
            translator.language // Re-read when the language (and so the default locale) changes.
            if (tile.kind === "item")
                return tile.extension
            const size = tile.isDir ? Format.count(tile.itemCount) : Format.size(tile.size)
            // A folder's own date says little (it changes when anything in
            // it is added or removed), so only files show one.
            const date = tile.isDir ? "" : Format.when(tile.modified, translator.dateLocale)
            return size && date ? qsTr("%1 · %2").arg(size).arg(date) : size || date
        }
        color: theme.secondaryForeground
        size: 10
        horizontalAlignment: Text.AlignHCenter
        elide: Text.ElideRight
    }

    // A folder takes files dropped anywhere on its tile, picture or name,
    // lighting up while they hover: the view's own drop area refuses the
    // whole cell, so the tile has to take all of it.
    DropArea {
        id: folderDrop
        anchors.fill: parent
        enabled: tile.isDir
        keys: ["text/uri-list"]
        onEntered: (drag) => { drag.accepted = fileOps.canDrop(drag.urls, tile.path) }
        onDropped: (drop) => { fileOps.dropInto(drop.urls, tile.path); drop.accept(Qt.MoveAction) }
    }

    DragPicture {
        id: dragPicture
        // Cleared first: Qt (6.12) reloads a new picture at the size of the
        // one before, which for a grab only warns ("Ignoring sourceSize").
        onReady: { tile.Drag.imageSource = ""; tile.Drag.imageSource = url }
    }

    HoverHandler {
        id: hover
        // The picture that follows the pointer while dragging; taken ahead
        // of time because a drag can't wait for it.
        onHoveredChanged: if (hovered) dragPicture.capture(art)
    }
    // Given another file by the grid, clear any previous drag picture and
    // retake it if the pointer is still over this tile.
    GridView.onReused: {
        tile.Drag.imageSource = ""
        if (hover.hovered)
            dragPicture.capture(art)
    }

    // Dragging hands the file to whatever it lands on as a URI list: the
    // sidebar pins a folder, other apps get the file.
    Drag.dragType: Drag.Automatic
    Drag.supportedActions: Qt.CopyAction | Qt.LinkAction
    Drag.hotSpot.x: tile.width / 2
    Drag.hotSpot.y: 40
    // Presses take only the picture and name, the area the band hits (see
    // FolderGrid): the padding around them is empty space to start a band.
    Item {
        x: 14
        y: 12
        width: tile.width - 28
        height: tile.height - 24

        // A plugin's item isn't a file to hand anywhere.
        DragHandler {
            target: null
            enabled: tile.kind !== "item"
            // Dragging a selected item takes the whole selection along. The list
            // is built as the drag starts, not again on every change of selection.
            onActiveChanged: {
                if (active)
                    tile.Drag.mimeData = { "text/uri-list": fileOps.uriList(tile.selected && backend.folder.selectedCount > 1 ? backend.folder.selectedPaths() : [tile.path]) }
                tile.Drag.active = active
            }
        }

        TapHandler {
            id: tap
            onTapped: tile.clicked(tap.point.modifiers)
            onDoubleTapped: tile.activated()
        }
    }

    // A folder's outline: a card with a tab on its top left edge, drawn as
    // one silhouette, its corners rounded by `r`.
    function folderPath(r) {
        if (r <= 0)
            return "M4.5 88.5V0.5H46L50 6.5H123.5V88.5Z"
        // Corners as quadratic curves: the curve renderer draws those
        // natively (filled arcs went missing on the GPU).
        const t = Math.min(r, 3)
        return "M4.5 " + (88.5 - r) + "V" + (0.5 + r) + "Q4.5 0.5 " + (4.5 + r) + " 0.5"
             + "H" + (46 - t) + "Q46 0.5 " + (46 + t * 0.6) + " " + (0.5 + t * 0.9) + "L" + (50 - t * 0.6) + " " + (6.5 - t * 0.9)
             + "Q50 6.5 " + (50 + t) + " 6.5H" + (123.5 - r) + "Q123.5 6.5 123.5 " + (6.5 + r)
             + "V" + (88.5 - r) + "Q123.5 88.5 " + (123.5 - r) + " 88.5H" + (4.5 + r)
             + "Q4.5 88.5 4.5 " + (88.5 - r) + "Z"
    }

    // Folders are a plain card, nothing on it; the name below says the rest.
    Component {
        id: folderArt

        Shape {
            width: 128
            height: 92
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                fillColor: tile.current || folderDrop.containsDrag ? Qt.tint(theme.folderFill, Qt.alpha(theme.accent, 0.3))
                    : hover.hovered ? Qt.tint(theme.folderFill, Qt.alpha(theme.foreground, 0.06))
                    : theme.folderFill
                strokeColor: tile.current || folderDrop.containsDrag ? theme.accent : "transparent"
                strokeWidth: folderDrop.containsDrag ? 2.5 : 1.5
                joinStyle: tile.iconRadius > 0 ? ShapePath.RoundJoin : ShapePath.MiterJoin
                PathSvg { path: tile.folderPath(tile.iconRadius) }
            }
        }
    }

    Component {
        id: fileArt

        Item {
            // The picture is laid out at its own shape (cropped to fill for
            // photos and video, fitted for pages), so its corners can round.
            readonly property bool crop: tile.kind === "image" || tile.kind === "video"
            readonly property size natural: thumb.implicitWidth > 0 ? Qt.size(thumb.implicitWidth, thumb.implicitHeight) : Qt.size(1, 1)
            readonly property real fit: Math.min(width / natural.width, height / natural.height)
            Image {
                id: thumb
                width: parent.crop ? parent.width : parent.natural.width * parent.fit
                height: parent.crop ? parent.height : parent.natural.height * parent.fit
                x: (parent.width - width) / 2
                y: (parent.height - height) / 2
                asynchronous: true
                fillMode: parent.crop ? Image.PreserveAspectCrop : Image.Stretch
                source: tile.kind === "text" ? Format.page(tile.path, tile.zoom > 1 ? 512 : 256, tile.modified, theme.lighterBackground, theme.foreground)
                                              : Format.thumb(tile.path, tile.mime, tile.zoom > 1 ? 512 : 256, tile.modified)
                visible: status === Image.Ready
                layer.enabled: tile.iconRadius > 0 && status === Image.Ready
                // The layer is drawn before the zoom scales it, so it's made
                // at the zoomed size to keep the bigger thumbnail's detail.
                layer.textureSize: Qt.size(Math.ceil(width * tile.zoom * Screen.devicePixelRatio),
                                           Math.ceil(height * tile.zoom * Screen.devicePixelRatio))
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: thumbMask
                    maskThresholdMin: 0.5
                    maskSpreadAtMin: 1
                }
            }
            Rectangle {
                id: thumbMask
                width: thumb.width
                height: thumb.height
                radius: tile.iconRadius
                visible: false
                layer.enabled: thumb.layer.enabled
                layer.textureSize: thumb.layer.textureSize
            }

            // Videos get a play mark over their frame, made when there is a frame: every other tile goes without its items.
            Loader {
                anchors.centerIn: parent
                active: thumb.status === Image.Ready && tile.kind === "video"
                sourceComponent: Rectangle {
                    width: 30; height: 30
                    radius: Math.min(4, theme.radius)
                    color: Qt.alpha(theme.darkBackground, 0.75)
                    Glyph { anchors.centerIn: parent; name: "play"; size: 16; color: theme.brightForeground }
                }
            }

            // Stand-ins while a thumbnail loads, or when none can be made.
            Loader {
                anchors.fill: parent
                active: thumb.status !== Image.Ready
                sourceComponent: tile.kind === "audio" ? waveArt : tile.kind === "video" ? reelArt : pageArt
            }

            // Selection is a border on what's drawn, hugging it: the
            // thumbnail at its painted size, the page stand-in, or the box
            // of the others.
            readonly property rect shown: {
                const w = width, h = height
                if (thumb.status === Image.Ready)
                    return Qt.rect(thumb.x, thumb.y, thumb.width, thumb.height)
                if (tile.kind === "audio" || tile.kind === "video")
                    return Qt.rect(0, 0, w, h)
                return Qt.rect((w - 66) / 2, 0, 66, h)
            }
            Rectangle {
                visible: tile.current || hover.hovered
                x: parent.shown.x - 3
                y: parent.shown.y - 3
                width: parent.shown.width + 6
                height: parent.shown.height + 6
                color: "transparent"
                border.width: tile.current ? 2 : 1
                border.color: tile.current ? theme.accent : Qt.alpha(theme.foreground, 0.2)
                radius: tile.iconRadius > 0 ? tile.iconRadius + 3 : 0
            }
        }
    }

    // Hovered, a plugin's icon brightens: to the theme's bright color, or,
    // in a theme without one apart from its text's, a touch of the accent.
    readonly property color hoverColor: Qt.colorEqual(theme.brightForeground, theme.foreground)
                                        ? Qt.tint(theme.foreground, Qt.alpha(theme.accent, 0.35)) : theme.brightForeground

    Component {
        id: itemArt

        // A plugin's item is only its icon, so selecting it colors the icon
        // (and the name) rather than framing it: a frame would box in
        // empty space.
        Item {
            Glyph {
                anchors.centerIn: parent
                name: tile.icon
                size: 76
                stroke: 1.2
                color: tile.current ? theme.accent : hover.hovered ? tile.hoverColor : theme.foreground
            }
        }
    }

    Component {
        id: pageArt

        Item {
            Rectangle {
                id: page
                anchors.horizontalCenter: parent.horizontalCenter
                width: 66; height: 92
                color: theme.lighterBackground
                border.width: 1
                border.color: Qt.alpha(theme.foreground, 0.16)
                radius: tile.iconRadius
                // A heading and five lines of text, each placed by hand: a Column and a Repeater cost more to make than the six bars.
                Rectangle { x: 9; y: 11; width: 34; height: 4; color: tile.kindColor }
                Rectangle { x: 9; y: 21; width: 48; height: 2; color: Qt.alpha(theme.foreground, 0.3) }
                Rectangle { x: 9; y: 29; width: 48; height: 2; color: Qt.alpha(theme.foreground, 0.3) }
                Rectangle { x: 9; y: 37; width: 40; height: 2; color: Qt.alpha(theme.foreground, 0.3) }
                Rectangle { x: 9; y: 45; width: 48; height: 2; color: Qt.alpha(theme.foreground, 0.3) }
                Rectangle { x: 9; y: 53; width: 30; height: 2; color: Qt.alpha(theme.foreground, 0.3) }
            }
            Rectangle {
                visible: tile.extension.length > 0
                anchors.left: page.left
                anchors.bottom: page.bottom
                anchors.bottomMargin: 6
                width: badge.implicitWidth + 8
                height: badge.implicitHeight + 2
                color: tile.kindColor
                FolioText { id: badge; anchors.centerIn: parent; text: tile.extension.slice(0, 4); textFormat: Text.PlainText; size: 9; font.bold: true; color: theme.background }
            }
        }
    }

    Component {
        id: waveArt

        // Decorative, not the real waveform: bars seeded from the name so each
        // file keeps its own shape. Album art replaces it when the file has some.
        Rectangle {
            color: theme.lighterBackground
            radius: tile.iconRadius
            Row {
                anchors.centerIn: parent
                spacing: 3
                Repeater {
                    model: 18
                    Rectangle {
                        required property int index
                        anchors.verticalCenter: parent.verticalCenter
                        width: 3
                        height: {
                            let h = 0
                            for (let i = 0; i < tile.name.length; ++i) h = (h * 31 + tile.name.charCodeAt(i) + index * 7) % 997
                            return 10 + (h % 40)
                        }
                        color: tile.kindColor
                    }
                }
            }
            FolioText { anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 5; text: tile.extension; textFormat: Text.PlainText; size: 9; color: theme.secondaryForeground }
        }
    }

    Component {
        id: reelArt

        Rectangle {
            color: theme.darkBackground
            radius: tile.iconRadius
            Glyph { anchors.centerIn: parent; name: "play"; size: 22; color: tile.kindColor }
        }
    }
}
