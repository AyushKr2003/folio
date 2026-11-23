import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import "Format.js" as Format

// The large look at one file (Space in Folio, or `folio preview FILE`):
// pictures at full size, video and audio playing, PDFs to scroll through,
// text to read. Arrows step through the folder, Enter opens the file in its
// app, Space or Escape closes.
ApplicationWindow {
    id: win
    // Never bigger than the screen it opens on: smaller screens keep a margin
    // all around, the bar included.
    width: Math.min(1100, Math.round(Screen.width * 0.85))
    height: Math.min(760, Math.round(Screen.height * 0.85))
    minimumWidth: 480
    minimumHeight: 360
    visible: true
    title: previewer.item.name || qsTr("Files")
    color: theme.darkBackground

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.foreground: theme.foreground
    Material.background: theme.background
    Material.roundedScale: theme.radius

    // Right-to-left languages mirror the window, its menus and tooltips
    // included: they live in the window's overlay, outside its other items.
    // The Translator sets the application's layout direction.
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    property bool helpVisible: false

    readonly property bool rtl: Qt.application.layoutDirection === Qt.RightToLeft
    readonly property var item: previewer.item
    readonly property string viewer: previewer.viewer
    readonly property bool external: viewer === "video" || viewer === "audio" || viewer === "pdf"
    // Whatever is showing; zooming, scrolling and playback keys go to it when
    // it knows how.
    readonly property var view: external ? externalView.item : inlineView.item

    function call(name, arg) {
        if (win.view && typeof win.view[name] === "function")
            win.view[name](arg)
    }

    // Opened in its app, the file needs no preview: the window closes, as Quick Look's does.
    Connections {
        target: previewer
        function onOpened() { Qt.quit() }
    }

    Shortcut { sequences: ["Space", "Escape", "Q"]; context: Qt.ApplicationShortcut; onActivated: win.helpVisible ? win.helpVisible = false : Qt.quit() }
    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    // The folder's files run right to left when mirrored, so Left is next.
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; onActivated: win.rtl ? previewer.next() : previewer.previous() }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; onActivated: win.rtl ? previewer.previous() : previewer.next() }
    Shortcut { sequences: ["Return", "Enter"]; context: Qt.ApplicationShortcut; onActivated: previewer.open() }
    // ↑ and ↓ scroll what's taller than the window (a long text, a zoomed
    // picture, a PDF); anything that fits moves to the item above or below,
    // as the arrows do in the folder.
    function upDown(direction) {
        // A text still being read looks short; stepping past it then would
        // skip a long one. The next press, once it's in, knows which.
        if (previewer.readingText)
            return
        const v = win.view
        const scrolls = v && typeof v.scroll === "function"
            && (v.scrolls !== undefined ? v.scrolls : v.contentHeight > v.height + 1)
        if (scrolls)
            v.scroll(direction)
        else
            previewer.stepRow(direction)
    }
    Shortcut { sequences: ["Up"]; context: Qt.ApplicationShortcut; onActivated: win.upDown(-1) }
    Shortcut { sequences: ["Down"]; context: Qt.ApplicationShortcut; onActivated: win.upDown(1) }
    Shortcut { sequences: [StandardKey.MoveToPreviousPage]; context: Qt.ApplicationShortcut; onActivated: win.call("page", -1) }
    Shortcut { sequences: [StandardKey.MoveToNextPage]; context: Qt.ApplicationShortcut; onActivated: win.call("page", 1) }
    Shortcut { sequences: [StandardKey.ZoomIn, "Ctrl+="]; context: Qt.ApplicationShortcut; onActivated: win.call("zoomBy", 1.25) }
    Shortcut { sequences: [StandardKey.ZoomOut]; context: Qt.ApplicationShortcut; onActivated: win.call("zoomBy", 0.8) }
    Shortcut { sequence: "Ctrl+0"; context: Qt.ApplicationShortcut; onActivated: win.call("fit") }
    Shortcut { sequence: "K"; context: Qt.ApplicationShortcut; onActivated: win.call("toggle") }
    Shortcut { sequence: "Shift+Left"; context: Qt.ApplicationShortcut; onActivated: win.call("seek", -5) }
    Shortcut { sequence: "Shift+Right"; context: Qt.ApplicationShortcut; onActivated: win.call("seek", 5) }

    Item {
        anchors.fill: parent

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // What it is, where it sits in the folder, and the way out.
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: 60
                color: theme.background

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 24
                    anchors.rightMargin: 12
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        // Aligned explicitly, so that they're mirrored: left
                        // as they are, a Latin name would stay on the left.
                        FolioText {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignLeft
                            text: win.item.name || ""
                            textFormat: Text.PlainText
                            color: theme.brightForeground
                            size: 15
                            font.weight: Font.DemiBold
                            elide: Text.ElideMiddle
                        }
                        FolioText {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignLeft
                            text: Format.summary(win.item, previewer.type)
                            color: theme.secondaryForeground
                            size: 12
                            elide: Text.ElideRight
                        }
                    }

                    FolioText {
                        visible: previewer.count > 1
                        text: qsTr("%1 of %2").arg((previewer.index + 1).toLocaleString(Qt.locale(), "f", 0))
                                              .arg(previewer.count.toLocaleString(Qt.locale(), "f", 0))
                        color: theme.secondaryForeground
                        size: 12
                    }
                    // Mirrored, Next sits left of Previous; each points away
                    // from the other, at the key that does the same.
                    IconButton {
                        glyph: win.rtl ? "forward" : "back"
                        text: qsTr("Previous (%1)").arg(win.rtl ? "→" : "←")
                        enabled: previewer.index > 0
                        onClicked: previewer.previous()
                    }
                    IconButton {
                        glyph: win.rtl ? "back" : "forward"
                        text: qsTr("Next (%1)").arg(win.rtl ? "←" : "→")
                        enabled: previewer.index + 1 < previewer.count
                        onClicked: previewer.next()
                    }
                    IconButton { glyph: "open"; text: qsTr("Open (⏎)"); onClicked: previewer.open() }
                    IconButton { glyph: "close"; text: qsTr("Close"); onClicked: Qt.quit() }
                }
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Qt.alpha(theme.foreground, 0.12) }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Loader {
                    id: inlineView
                    anchors.fill: parent
                    active: !win.external
                    sourceComponent: win.viewer === "image" || win.viewer === "animated" ? imageView
                                   : win.viewer === "text" || win.viewer === "markdown" || win.viewer === "archive" ? textView
                                   : win.viewer === "folder" ? folderView
                                   : otherView
                }

                // Media and PDFs come from optional Qt modules; if one is
                // missing, the window says which package to install.
                Loader {
                    id: externalView
                    anchors.fill: parent
                    active: win.external
                    source: win.viewer === "pdf" ? "qrc:/FolioUI/PdfPreview.qml" : win.external ? "qrc:/FolioUI/MediaPreview.qml" : ""
                }

                FolioText {
                    anchors.centerIn: parent
                    width: parent.width - 96
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    visible: externalView.status === Loader.Error
                    text: win.viewer === "pdf" ? qsTr("Previewing PDFs needs Qt PDF, from the qt6-webengine package.")
                                               : qsTr("Playing video and audio needs Qt Multimedia, from the qt6-multimedia package.")
                    color: theme.foreground
                }
            }
        }

        // The mouse's back and forward thumb buttons step through the folder.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.BackButton | Qt.ForwardButton
            onPressed: mouse => mouse.button === Qt.BackButton ? previewer.previous() : previewer.next()
        }

        Loader {
            id: helpLoader
            anchors.fill: parent
            active: win.helpVisible
            visible: active
            source: "HelpOverlay.qml"
            onLoaded: {
                item.groups = Qt.binding(() => [{ title: qsTr("Preview"), shortcuts: [
                    [qsTr("Previous / next file"), (win.rtl ? [["→"], ["←"]] : [["←"], ["→"]]).concat([["↑"], ["↓"]])],
                    [qsTr("Open in its app"), [["⏎"]]],
                    [qsTr("Scroll"), [["↑", "↓"], ["Page Up"], ["Page Down"]]],
                    [qsTr("Zoom in / out"), [["Ctrl", "="], ["Ctrl", "-"]]],
                    [qsTr("Fit to the window"), [["Ctrl", "0"]]],
                    [qsTr("Play or pause"), [["K"]]],
                    [qsTr("Back / ahead 5 seconds"), [["Shift", "←"], ["Shift", "→"]]],
                    [qsTr("Close"), [["Space"], ["Escape"]]]
                ]}])
                item.forceActiveFocus()
            }
        }
        Connections {
            target: helpLoader.item
            function onDismissed() { win.helpVisible = false }
        }
    }

    // Pictures fit the window (never enlarged past their own size until
    // asked); Ctrl+scroll or Ctrl+= zooms, double-click flips between fit and
    // actual size, and a zoomed picture drags around.
    Component {
        id: imageView

        Flickable {
            id: flick
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            readonly property bool animated: win.viewer === "animated"
            readonly property var source: animated ? moving : still
            readonly property bool canZoom: animated || still.status === Image.Ready
            readonly property size natural: source.status === Image.Ready ? Qt.size(source.implicitWidth, source.implicitHeight)
                                            : quick.visible ? Qt.size(quick.implicitWidth, quick.implicitHeight) : Qt.size(1, 1)
            readonly property real fitScale: Math.min(1, (width - 48) / natural.width, (height - 48) / natural.height)
            property real zoom: fitScale

            onFitScaleChanged: if (!zoomed) zoom = fitScale
            property bool zoomed: false

            function zoomBy(factor) {
                if (!canZoom)
                    return
                zoomed = true
                zoom = Math.max(0.05, Math.min(16, zoom * factor))
            }
            function fit() { zoomed = false; zoom = fitScale }
            function scroll(direction) { contentY = Math.max(0, Math.min(contentHeight - height, contentY + direction * 80)) }
            function loadStill() {
                headStart.stop()
                still.source = ""
                const item = previewer.item
                quick.source = previewer.viewer === "image" && item.path && !item.mime.startsWith("image/svg")
                               && !/@[2-9]x\.[^.]+$/.test(item.path)
                               ? Format.cachedPreview(item.path, item.modified, win.width, win.height) : ""
                if (quick.status === Image.Loading)
                    headStart.start()
                else
                    showStill()
            }
            function showStill() {
                headStart.stop()
                if (previewer.viewer === "image")
                    still.source = previewer.url
            }

            contentWidth: Math.max(width, natural.width * zoom)
            contentHeight: Math.max(height, natural.height * zoom)

            Component.onCompleted: loadStill()
            Connections {
                target: previewer
                function onChanged() {
                    flick.loadStill()
                    flick.fit()
                }
            }

            // Qt delivers both images on one reader thread. Give a warm thumb
            // a chance to arrive before the full decode occupies that thread.
            Timer {
                id: headStart
                interval: 50
                onTriggered: flick.showStill()
            }

            Image {
                id: quick
                objectName: "previewThumbnail"
                anchors.fill: still
                asynchronous: true
                cache: false
                fillMode: Image.PreserveAspectFit
                visible: !flick.animated && status === Image.Ready
                         && still.status !== Image.Ready && still.status !== Image.Error
                onStatusChanged: if (headStart.running && (status === Image.Ready || status === Image.Error)) flick.showStill()
            }
            Image {
                id: still
                // Share the in-flight decode across scale reloads. The hidden
                // child uses the same window/DPR and releases its pixels at Ready.
                property Image keeper: Image {
                    parent: still
                    visible: false
                    asynchronous: true
                    autoTransform: true
                    source: still.status === Image.Loading ? still.source : ""
                }
                visible: !flick.animated
                width: flick.natural.width * flick.zoom
                height: flick.natural.height * flick.zoom
                x: (flick.contentWidth - width) / 2
                y: (flick.contentHeight - height) / 2
                asynchronous: true
                autoTransform: true
                smooth: true
                mipmap: true
                onStatusChanged: if (status === Image.Ready || status === Image.Error) quick.source = ""
            }
            AnimatedImage {
                id: moving
                visible: flick.animated
                width: flick.natural.width * flick.zoom
                height: flick.natural.height * flick.zoom
                x: (flick.contentWidth - width) / 2
                y: (flick.contentHeight - height) / 2
                smooth: true
                source: flick.animated ? previewer.url : ""
            }

            TapHandler {
                enabled: flick.canZoom
                onDoubleTapped: flick.zoomed && Math.abs(flick.zoom - 1) < 0.01 ? flick.fit() : (flick.zoomed = true, flick.zoom = 1)
            }
            WheelHandler {
                acceptedModifiers: Qt.ControlModifier
                onWheel: event => flick.zoomBy(event.angleDelta.y > 0 ? 1.1 : 1 / 1.1)
            }

            FolioText {
                anchors.centerIn: parent
                visible: flick.source.status === Image.Error
                text: qsTr("Couldn't read this picture.")
                color: theme.foreground
            }

            ScrollBar.vertical: FolioScrollBar { }
            ScrollBar.horizontal: FolioScrollBar { }
        }
    }

    // Text to read, Markdown rendered, an archive's names; selectable, never editable.
    Component {
        id: textView

        Flickable {
            id: flick
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            contentWidth: width
            contentHeight: body.implicitHeight + 72

            function scroll(direction) { contentY = Math.max(0, Math.min(contentHeight - height, contentY + direction * 60)) }
            function page(direction) { contentY = Math.max(0, Math.min(contentHeight - height, contentY + direction * (height - 60))) }

            Connections {
                target: previewer
                function onChanged() { flick.contentY = 0 }
            }

            TextEdit {
                id: body
                width: Math.min(flick.width - 96, win.viewer === "markdown" ? 820 : 1200)
                x: (flick.width - width) / 2
                y: 36
                readOnly: true
                selectByMouse: true
                activeFocusOnPress: false
                wrapMode: TextEdit.Wrap
                textFormat: win.viewer === "markdown" ? TextEdit.MarkdownText : TextEdit.PlainText
                text: previewer.text
                color: theme.foreground
                selectionColor: theme.accent
                selectedTextColor: theme.accentForeground
                font.family: theme.fontFamily
                font.pixelSize: Math.round(13 * theme.textScale)
            }

            ScrollBar.vertical: FolioScrollBar { }
        }
    }

    // A folder: what's inside, at a glance.
    Component {
        id: folderView

        ColumnLayout {
            spacing: 18
            Item { Layout.fillHeight: true }
            Row {
                Layout.alignment: Qt.AlignHCenter
                spacing: 16
                visible: (win.item.peek || []).length > 0
                Repeater {
                    model: win.item.peek || []
                    Image {
                        required property string modelData
                        width: 220
                        height: 165
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        source: Format.thumb(modelData, "image/*", 256)
                    }
                }
            }
            Glyph {
                Layout.alignment: Qt.AlignHCenter
                visible: (win.item.peek || []).length === 0
                name: "folder"
                size: 120
                stroke: 1.1
                color: theme.accent
            }
            FolioText {
                Layout.alignment: Qt.AlignHCenter
                text: Format.count(win.item.itemCount)
                color: theme.secondaryForeground
            }
            Item { Layout.fillHeight: true }
        }
    }

    // Anything without a viewer of its own: the largest thumbnail the
    // installed thumbnailers can make (HEIC, office files, fonts…), or the
    // file's type and a pointer to its app.
    Component {
        id: otherView

        Item {
            Image {
                id: thumb
                anchors.fill: parent
                anchors.margins: 32
                fillMode: Image.PreserveAspectFit
                asynchronous: true
                source: win.item.path ? Format.thumb(win.item.path, win.item.mime, 1024, win.item.modified) : ""
                visible: status === Image.Ready
            }
            ColumnLayout {
                anchors.centerIn: parent
                visible: !thumb.visible
                spacing: 14
                Glyph {
                    Layout.alignment: Qt.AlignHCenter
                    name: "file"
                    size: 120
                    stroke: 1.1
                    color: theme.secondaryForeground
                }
                FolioText {
                    Layout.alignment: Qt.AlignHCenter
                    text: win.item.extension || ""
                    textFormat: Text.PlainText
                    color: theme.accent
                    font.weight: Font.DemiBold
                }
                FolioText {
                    Layout.alignment: Qt.AlignHCenter
                    text: thumb.status === Image.Loading ? qsTr("Making a preview…") : qsTr("No preview for this type. Press ⏎ to open it.")
                    color: theme.secondaryForeground
                }
            }
        }
    }
}
