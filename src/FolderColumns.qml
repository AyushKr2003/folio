import QtQuick
import QtQuick.Controls
import Folio

// Columns, as in the macOS Finder: the folders on the way here from the
// root (~, another machine, or /), the current folder, and what's inside
// the selected subfolder. → goes into the selected folder, ← back out;
// clicking anywhere to the left jumps there.
Item {
    id: view

    property alias currentIndex: here.currentIndex
    readonly property int columnWidth: 260
    // The pane whose selected file the details column describes.
    property var detailsPane: null
    readonly property var details: detailsColumn.item

    function select(row, keep) { selecting.select(row, keep) }
    signal cursorLetGo()
    function letGo() { selecting.letGo(); cursorLetGo() }

    Selecting { id: selecting; view: here; pageStep: Math.max(1, Math.floor(here.height / 32) - 1) }
    function focusView() { here.forceActiveFocus() }
    function menuAnchor() { return here.currentItem }

    // Every column has a menu: on an item there, or on the column's folder.
    signal contextMenu(var item, string folder, point at)

    readonly property var crumbs: backend.crumbs

    // The folders on the way here, kept as a list model that is only trimmed
    // and extended: moving one folder deeper or back out adds or drops one
    // column, and the rest stay as they are instead of reloading (which
    // flashed).
    ListModel { id: trail }
    // How many of the folders on the way here are scrolled out of sight to
    // the left: a place picked in the sidebar is the first column shown,
    // with the folders above it there to scroll back to. Going up past it
    // shows what's above again.
    property int hidden: 0
    property string startingAt: ""
    function startAt(path) {
        startingAt = path
        if (backend.path === path)
            started()
    }
    function started() {
        startingAt = ""
        hidden = trail.count
        revealEnd()
    }
    function syncTrail() {
        const want = []
        for (let i = 0; i + 1 < crumbs.length; ++i)
            want.push({ path: crumbs[i].path, next: crumbs[i + 1].name })
        let same = 0
        while (same < trail.count && same < want.length && trail.get(same).path === want[same].path)
            ++same
        // Somewhere else altogether (another drive, /) starts from its top.
        if (same === 0)
            hidden = 0
        // Columns that stay may now point at a different next folder.
        for (let i = 0; i < same; ++i)
            if (trail.get(i).next !== want[i].next)
                trail.setProperty(i, "next", want[i].next)
        if (trail.count > same)
            trail.remove(same, trail.count - same)
        for (let i = same; i < want.length; ++i)
            trail.append(want[i])
        hidden = Math.min(hidden, trail.count)
        if (startingAt.length > 0 && backend.path === startingAt)
            started()
    }
    onCrumbsChanged: syncTrail()
    Component.onCompleted: syncTrail()
    readonly property var selected: here.currentIndex >= 0 ? (backend.folder.count, backend.folder.get(here.currentIndex)) : ({})

    // Where the first column shown starts, from the side the columns grow
    // from: past the hidden ones and their separators.
    readonly property int hiddenWidth: hidden * (columnWidth + 1)
    // Keeps the newest columns in view as the trail grows, and the hidden
    // ones out of it: on the left, when mirrored.
    function revealEnd() {
        const from = Math.max(view.hiddenWidth, row.width - flick.width)
        const most = Math.max(0, flick.contentWidth - flick.width)
        flick.contentX = view.LayoutMirroring.enabled ? most - from : from
    }
    LayoutMirroring.onEnabledChanged: revealEnd()
    function enter() {
        if (view.selected.isDir && (insideFolder.loading || insideFolder.count > 0 || insideFolder.error.length > 0))
            backend.navigate(view.selected.path)
    }

    component Separator: Rectangle { width: 1; height: parent ? parent.height : 0; color: Qt.alpha(theme.foreground, 0.1) }
    // Files dropped in a column land in the folder it shows. It lies over
    // the whole column, under its items, rather than in the list's content,
    // which ends at its last item.
    component ColumnDrop: FolderDrop { parent: items; anchors.fill: parent; z: -1 }

    Flickable {
        id: flick
        anchors.fill: parent
        clip: true
        acceptedButtons: Qt.NoButton
        // Room to scroll the hidden columns out of sight even when the rest
        // would fit.
        contentWidth: Math.max(width, row.width, view.hiddenWidth + width)
        contentHeight: height
        boundsBehavior: Flickable.StopAtBounds
        onContentWidthChanged: view.revealEnd()
        onWidthChanged: view.revealEnd()
        ScrollBar.horizontal: FolioScrollBar { }

        Row {
            id: row
            // A Row isn't placed by its Flickable, so it's moved to the right
            // edge by hand when mirrored.
            x: LayoutMirroring.enabled ? parent.width - width : 0
            height: flick.height

            // The folders on the way here, each showing which way the trail goes.
            Repeater {
                model: trail
                Row {
                    id: ancestor
                    required property string path
                    required property string next
                    height: row.height

                    FileListView {
                        id: column
                        width: view.columnWidth
                        height: parent.height
                        model: FolderModel {
                            id: folder
                            path: ancestor.path
                            showHidden: backend.folder.showHidden
                            sortMode: { backend.sortsRevision; return backend.sortOf(ancestor.path) }
                            filter: backend.folder.filter
                            onLoaded: column.revealNext()
                        }
                        // Names sort by the language's alphabet.
                        Connections {
                            target: translator
                            function onChanged() { folder.reload() }
                        }
                        function revealNext() { positionViewAtIndex(Math.max(0, folder.indexOf(ancestor.next)), ListView.Contain) }
                        ColumnDrop { items: column; folder: ancestor.path }
                        TapHandler {
                            acceptedButtons: Qt.RightButton
                            onTapped: (point) => {
                                const index = column.indexAt(point.position.x + column.contentX, point.position.y + column.contentY)
                                view.contextMenu(index >= 0 ? folder.get(index) : null, ancestor.path, point.scenePosition)
                            }
                        }
                        Connections {
                            target: ancestor
                            function onNextChanged() { column.revealNext() }
                        }
                        delegate: FileRow {
                            width: column.width
                            chevron: true
                            trail: name === ancestor.next
                            onClicked: isDir ? backend.navigate(path) : backend.showItem(path)
                            onActivated: backend.open(path)
                        }
                    }
                    Separator { }
                }
            }

            // The folder being shown: its selection is the window's.
            FileListView {
                id: here
                width: view.columnWidth
                height: row.height
                focus: true
                keyNavigationEnabled: true
                // No cursor of its own to start with: the pane puts it back
                // where the last view had it, and one on the first item would
                // select that instead.
                currentIndex: -1
                highlightFollowsCurrentItem: false
                model: backend.folder

                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: (point) => view.contextMenu(selecting.menuAt(point), backend.path, point.scenePosition)
                }
                ColumnDrop { items: here; folder: backend.path }

                onCurrentIndexChanged: selecting.cursorMoved()
                Keys.onPressed: (event) => selecting.page(event) || selecting.extendBy(event, event.key === Qt.Key_Up ? -1 : event.key === Qt.Key_Down ? 1 : 0)
                // Into a folder is the way the columns grow: leftward, when
                // mirrored.
                Keys.onRightPressed: view.LayoutMirroring.enabled ? backend.goUp() : view.enter()
                Keys.onLeftPressed: view.LayoutMirroring.enabled ? view.enter() : backend.goUp()

                delegate: FileRow {
                    width: here.width
                    chevron: true
                    current: selected
                    onClicked: (modifiers) => { selecting.click(index, modifiers); here.forceActiveFocus() }
                    onActivated: backend.open(path)
                }

                // Where the first item would be, lined up with the names.
                Text {
                    x: LayoutMirroring.enabled ? here.width - width - 46 : 46
                    y: 8
                    readonly property bool searching: backend.folder.loading && backend.folder.query.length > 0
                    visible: here.count === 0 && (!backend.folder.loading || searching) && backend.folder.error.length === 0
                    text: searching ? qsTr("Searching…") : backend.folder.query ? qsTr("No matches") : qsTr("No items")
                    color: theme.secondaryForeground
                }
            }
            Separator { }

            // A look inside the selected folder; clicking in it goes there.
            FileListView {
                id: inside
                width: view.columnWidth
                height: row.height
                visible: view.selected.isDir === true
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: (point) => {
                        const index = inside.indexAt(point.position.x + inside.contentX, point.position.y + inside.contentY)
                        view.contextMenu(index >= 0 ? inside.model.get(index) : null, view.selected.path, point.scenePosition)
                    }
                }
                model: FolderModel {
                    id: insideFolder
                    path: view.selected.isDir === true ? view.selected.path : ""
                    showHidden: backend.folder.showHidden
                    sortMode: { backend.sortsRevision; return backend.sortOf(insideFolder.path) }
                    filter: backend.folder.filter
                }
                Connections {
                    target: translator
                    function onChanged() { insideFolder.reload() }
                }
                ColumnDrop { items: inside; folder: view.selected.path ?? "" }
                delegate: FileRow {
                    width: inside.width
                    chevron: true
                    onClicked: backend.showItem(path)
                    onActivated: backend.open(path)
                }

                Text {
                    x: LayoutMirroring.enabled ? inside.width - width - 46 : 46
                    y: 8
                    visible: inside.count === 0 && !inside.model.loading
                    text: qsTr("No items")
                    color: theme.secondaryForeground
                }
            }

            // The selected file's details, where a folder would show what's
            // inside, filling the rest of the width.
            Loader {
                id: detailsColumn
                active: !!view.detailsPane && view.selected.path !== undefined && view.selected.isDir !== true
                visible: active
                sourceComponent: PreviewPane {
                    // All the room right of the columns in sight, and no
                    // narrower than it reads well.
                    width: Math.max(300, flick.width - (trail.count - view.hidden + 1) * (view.columnWidth + 1))
                    height: row.height
                    folderPane: view.detailsPane
                    onFinished: view.focusView()
                }
            }
        }
    }
}
