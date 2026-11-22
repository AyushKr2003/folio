import QtQuick
import QtQuick.Controls

// The folder as a grid of tiles with real previews.
GridView {
    id: grid

    function select(row, keep) { selecting.select(row, keep) }
    // A click on empty space lets go of the cursor; the pane then keeps it
    // let go of through a re-read or a switch of view.
    signal cursorLetGo()
    function letGo() { selecting.letGo(); cursorLetGo() }
    function focusView() { forceActiveFocus() }
    // The preview's ↑ and ↓: the tile above or below, as the arrow keys move here.
    function stepRow(direction) { direction < 0 ? moveCurrentIndexUp() : moveCurrentIndexDown() }
    // Where the keyboard's menu (Alt+Return) opens: beside the cursor's item.
    function menuAnchor() { return currentItem }

    // A page is the rows in sight, less one, a whole row at a time.
    Selecting { id: selecting; view: grid; pageStep: Math.max(1, Math.floor(grid.height / grid.cellHeight) - 1) * Math.max(1, grid.columns) }
    onCurrentIndexChanged: selecting.cursorMoved()
    Keys.onPressed: (event) => {
        if (selecting.page(event))
            return
        // With no cursor (after a click on empty space), Down starts at the
        // first item, not a row's width on from before it.
        if (event.key === Qt.Key_Down && currentIndex < 0 && count > 0) {
            currentIndex = 0
            event.accepted = true
            return
        }
        // Mirrored, the next item is to the left, as the plain arrows know.
        const ahead = grid.effectiveLayoutDirection === Qt.RightToLeft ? -1 : 1
        selecting.extendBy(event, event.key === Qt.Key_Left ? -ahead : event.key === Qt.Key_Right ? ahead
                                : event.key === Qt.Key_Up ? -columns : event.key === Qt.Key_Down ? columns : 0)
    }

    Band {
        view: grid
        // Mirrored, the first column is on the right, flush with the edge
        // (the columns may not fill the width exactly).
        rectOf: (i) => {
            const c = i % grid.columns
            const x = grid.effectiveLayoutDirection === Qt.RightToLeft ? grid.width - (c + 1) * grid.cellWidth : c * grid.cellWidth
            return Qt.rect(x + 14, Math.floor(i / grid.columns) * grid.cellHeight + 12,
                           grid.cellWidth - 28, grid.cellHeight - 24)
        }
    }

    // Right-click on an item (selecting it) or on empty space.
    signal contextMenu(var item, string folder, point at)
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: (point) => grid.contextMenu(selecting.menuAt(point), backend.path, point.scenePosition)
    }

    clip: true
    focus: true
    keyNavigationEnabled: true
    // No cursor of its own to start with: the pane puts it back where the
    // last view had it, and one on the first item would select that instead.
    currentIndex: -1
    // Pressing and dragging picks up a file (to pin it, or drop it in another
    // app) or draws a band, rather than flicking; the wheel and touchpad
    // still scroll, which turning the view's interactive off would stop too.
    acceptedButtons: Qt.NoButton
    boundsBehavior: Flickable.StopAtBounds
    // No cache buffer, as in FileListView.
    cacheBuffer: 0
    model: backend.folder
    // Tiles grow with the zoom; the name and details under them don't. A
    // narrow pane (one side of a split) caps it at a tile its width holds.
    property real zoom: 1
    readonly property real tileZoom: Math.max(0.5, Math.min(zoom, (width - 40) / 128))
    // The rows reflow, so bring the cursor's tile back into view.
    onTileZoomChanged: Qt.callLater(() => { if (currentIndex >= 0) { forceLayout(); positionViewAtIndex(currentIndex, GridView.Contain) } })
    readonly property int columns: Math.max(1, Math.floor(width / (40 + 128 * tileZoom)))
    cellWidth: Math.floor(width / columns)
    cellHeight: 100 + Math.round(92 * tileZoom)
    highlightFollowsCurrentItem: false
    ScrollBar.vertical: FolioScrollBar { }

    delegate: FileTile {
        width: grid.cellWidth
        height: grid.cellHeight
        zoom: grid.tileZoom
        current: selected
        onClicked: (modifiers) => { selecting.click(index, modifiers); grid.forceActiveFocus() }
        onActivated: backend.open(path)
    }

    FolioText {
        anchors.centerIn: parent
        // A search says it's looking until it finds something, or is done.
        readonly property bool searching: backend.folder.loading && backend.folder.query.length > 0
        visible: grid.count === 0 && (!backend.folder.loading || searching) && backend.folder.error.length === 0
        text: searching ? qsTr("Searching…")
            : backend.folder.query ? qsTr("Nothing matches “%1”").arg(backend.folder.query)
            : plugins.views[backend.path] ? plugins.views[backend.path].empty : qsTr("This folder is empty")
        textFormat: Text.PlainText
        color: theme.secondaryForeground
    }
}
