import QtQuick
import QtQuick.Controls

// The folder as a list, and a tree, as in the Finder: name, size and date
// (the date, then the size, step aside in a narrow window), and an arrow on
// each folder that opens it in place, its items listed under it a level
// in. No headings: the sort menu and S sort it.
FileListView {
    id: list

    function select(row, keep) { selecting.select(row, keep) }
    // A click on empty space lets go of the cursor; the pane then keeps it
    // let go of through a re-read or a switch of view.
    signal cursorLetGo()
    function letGo() { selecting.letGo(); cursorLetGo() }
    function focusView() { forceActiveFocus() }
    // The preview's ↑ and ↓: the row above or below.
    function stepRow(direction) { direction < 0 ? decrementCurrentIndex() : incrementCurrentIndex() }
    // Where the keyboard's menu (Alt+Return) opens: beside the cursor's item.
    function menuAnchor() { return currentItem }

    // Rows grow with the pane's size step, as the grid's tiles do: their
    // height and picture, not the name and details.
    property real zoom: 1
    readonly property int rowHeight: Math.round(32 * zoom)
    // The rows reflow, so bring the cursor's row back into view.
    onRowHeightChanged: Qt.callLater(() => { if (currentIndex >= 0) { forceLayout(); positionViewAtIndex(currentIndex, ListView.Contain) } })

    Selecting { id: selecting; view: list; pageStep: Math.max(1, Math.floor(list.height / list.rowHeight) - 1) }
    onCurrentIndexChanged: selecting.cursorMoved()
    Keys.onPressed: (event) => selecting.page(event) || selecting.extendBy(event, event.key === Qt.Key_Up ? -1 : event.key === Qt.Key_Down ? 1 : 0)

    Band {
        view: list
        rectOf: (i) => Qt.rect(0, i * list.rowHeight, list.width, list.rowHeight)
        rangeFor: (area) => [
            Math.max(0, Math.floor(area.y / list.rowHeight)),
            Math.min(list.count - 1, Math.floor((area.y + area.height) / list.rowHeight))
        ]
    }

    // → opens the selected folder in place, and on one open already steps
    // to its first item; ← closes it, or steps out to the folder the item
    // is in, and from the top goes up a folder. Where nothing opens in
    // place (a search, Recents, the trash), → goes into the folder. Mirrored,
    // the other way round, with the reading direction. ⏎ or a double click
    // goes into a folder anywhere.
    function deeper() {
        const item = backend.folder.get(currentIndex)
        if (!item.isDir)
            return
        if (!backend.folder.expandable)
            backend.navigate(item.path)
        else if (!item.expanded)
            backend.folder.setExpanded(currentIndex, true)
        else if (currentIndex + 1 < count && backend.folder.get(currentIndex + 1).depth > item.depth)
            currentIndex = currentIndex + 1
    }
    function shallower() {
        const item = backend.folder.get(currentIndex)
        if (item.expanded)
            backend.folder.setExpanded(currentIndex, false)
        else if (item.depth > 0)
            currentIndex = backend.folder.parentRow(currentIndex)
        else
            backend.goUp()
    }
    Keys.onRightPressed: LayoutMirroring.enabled ? shallower() : deeper()
    Keys.onLeftPressed: LayoutMirroring.enabled ? deeper() : shallower()
    // A folder that couldn't be opened in place says why.
    Connections {
        target: backend.folder
        function onExpandFailed(message) { backend.showMessage(message) }
    }
    // Open folders close on going to another view; coming back, the list
    // starts closed.
    Component.onDestruction: backend.folder.collapseAll()

    // Right-click on an item (selecting it) or on empty space.
    signal contextMenu(var item, string folder, point at)
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: (point) => list.contextMenu(selecting.menuAt(point), backend.path, point.scenePosition)
    }

    // Rows scrolled out are given to the files scrolled in rather than made
    // anew: scrolling a long list costs a fraction of what it did.
    reuseItems: true
    focus: true
    keyNavigationEnabled: true
    // No cursor of its own to start with: the pane puts it back where the
    // last view had it, and one on the first item would select that instead.
    currentIndex: -1
    highlightFollowsCurrentItem: false
    model: backend.folder

    delegate: FileRow {
        width: list.width
        zoom: list.zoom
        detailed: true
        tree: backend.folder.expandable
        current: selected
        onToggled: backend.folder.setExpanded(index, !expanded)
        onClicked: (modifiers) => { selecting.click(index, modifiers); list.forceActiveFocus() }
        onActivated: backend.open(path)
    }

    FolioText {
        anchors.centerIn: parent
        // A search says it's looking until it finds something, or is done.
        readonly property bool searching: backend.folder.loading && backend.folder.query.length > 0
        visible: list.count === 0 && (!backend.folder.loading || searching) && backend.folder.error.length === 0
        text: searching ? qsTr("Searching…")
            : backend.folder.query ? qsTr("Nothing matches “%1”").arg(backend.folder.query)
            : plugins.views[backend.path] ? plugins.views[backend.path].empty : qsTr("This folder is empty")
        textFormat: Text.PlainText
        color: theme.secondaryForeground
    }
}
