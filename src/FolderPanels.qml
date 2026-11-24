import QtQuick
import QtQuick.Controls
import Folio

// Three panels, as in yazi or ranger: the folder above this one, the
// current folder, and a look at the selected item (what's inside a folder,
// or the file's details), filling the width with no sideways scrolling.
// → or l goes into the selected folder (or opens the file), ← or h back
// out, and j / k move down and up.
Item {
    id: view

    property alias currentIndex: here.currentIndex
    // The pane whose selected file the preview panel describes.
    property var detailsPane: null
    readonly property var details: preview.item

    function select(row, keep) { selecting.select(row, keep) }
    signal cursorLetGo()
    function letGo() { selecting.letGo(); cursorLetGo() }

    Selecting { id: selecting; view: here; pageStep: Math.max(1, Math.floor(here.height / 32) - 1) }
    function focusView() { here.forceActiveFocus() }
    // The preview's ↑ and ↓: the row above or below in the current panel.
    function stepRow(direction) { direction < 0 ? here.decrementCurrentIndex() : here.incrementCurrentIndex() }
    function menuAnchor() { return here.currentItem }

    // Every panel has a menu: on an item there, or on the panel's folder.
    signal contextMenu(var item, string folder, point at)

    readonly property var crumbs: backend.crumbs
    // The folder above this one, and this one's name in it, from the path
    // itself rather than the breadcrumbs, which start at ~ or a drive: ~
    // shows /home beside it, as yazi does. Only / and the views that aren't
    // folders (Recents, a plugin's, the trash) have none.
    readonly property string shownPath: backend.path
    readonly property bool hasParent: shownPath.startsWith("/") && shownPath.length > 1
                                      && !(crumbs.length === 1 && fileOps.inTrash(shownPath))
    readonly property string parentPath: {
        if (!hasParent)
            return ""
        const cut = shownPath.replace(/\/+$/, "").lastIndexOf("/")
        return cut <= 0 ? "/" : shownPath.slice(0, cut)
    }
    readonly property string hereName: hasParent ? shownPath.replace(/\/+$/, "").split("/").pop() : ""
    readonly property var selected: here.currentIndex >= 0 ? (backend.folder.count, backend.folder.get(here.currentIndex)) : ({})

    // The three panels share the width equally (less the two separators);
    // the preview takes what rounding leaves.
    readonly property int parentWidth: Math.floor((width - 2) / 3)
    readonly property int hereWidth: parentWidth

    function enter() {
        if (!selected.path)
            return
        if (selected.isDir)
            backend.navigate(selected.path)
        else
            backend.open(selected.path)
    }

    component Separator: Rectangle { width: 1; height: parent ? parent.height : 0; color: Qt.alpha(theme.foreground, 0.1) }
    // Files dropped in a panel land in the folder it shows.
    component PanelDrop: FolderDrop { parent: items; anchors.fill: parent; z: -1 }

    Row {
        id: row
        anchors.fill: parent

        // The folder above, with this one marked.
        FileListView {
            id: above
            width: view.parentWidth
            height: row.height
            model: FolderModel {
                id: aboveFolder
                path: view.parentPath
                showHidden: backend.folder.showHidden
                sortMode: { backend.sortsRevision; return backend.sortOf(aboveFolder.path) }
                filter: backend.folder.filter
                onLoaded: above.revealHere()
            }
            function revealHere() { positionViewAtIndex(Math.max(0, aboveFolder.indexOf(view.hereName)), ListView.Contain) }
            Connections {
                target: view
                function onHereNameChanged() { above.revealHere() }
            }
            Connections {
                target: translator
                function onChanged() { aboveFolder.reload() }
            }
            PanelDrop { items: above; folder: view.parentPath; enabled: view.parentPath.length > 0 }
            TapHandler {
                acceptedButtons: Qt.RightButton
                enabled: view.parentPath.length > 0
                onTapped: (point) => {
                    const index = above.indexAt(point.position.x, point.position.y)
                    view.contextMenu(index >= 0 ? above.model.get(index) : null, view.parentPath, point.scenePosition)
                }
            }
            delegate: FileRow {
                width: above.width
                chevron: true
                trail: name === view.hereName
                onClicked: isDir ? backend.navigate(path) : backend.showItem(path)
                onActivated: backend.open(path)
            }
        }
        Separator { }

        // The folder being shown: its selection is the window's.
        FileListView {
            id: here
            function letGo() { view.letGo() }
            width: view.hereWidth
            height: row.height
            focus: true
            keyNavigationEnabled: true
            // No cursor of its own to start with: the pane puts it back
            // where the last view had it.
            currentIndex: -1
            highlightFollowsCurrentItem: false
            model: backend.folder

            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: (point) => view.contextMenu(selecting.menuAt(point), backend.path, point.scenePosition)
            }
            Band {
                view: here
                rectOf: (i) => Qt.rect(0, i * 32, here.width, 32)
                rangeFor: (area) => [
                    Math.max(0, Math.floor(area.y / 32)),
                    Math.min(here.count - 1, Math.floor((area.y + area.height) / 32))
                ]
            }
            PanelDrop { items: here; folder: backend.path }

            onCurrentIndexChanged: selecting.cursorMoved()
            Keys.onPressed: (event) => {
                // yazi's keys, without modifiers so Ctrl+H and the like
                // keep their meaning.
                if (event.modifiers === Qt.NoModifier || event.modifiers === Qt.KeypadModifier) {
                    const into = view.LayoutMirroring.enabled ? Qt.Key_H : Qt.Key_L
                    const out = view.LayoutMirroring.enabled ? Qt.Key_L : Qt.Key_H
                    if (event.key === Qt.Key_J) { here.incrementCurrentIndex(); event.accepted = true; return }
                    if (event.key === Qt.Key_K) { here.decrementCurrentIndex(); event.accepted = true; return }
                    if (event.key === into) { view.enter(); event.accepted = true; return }
                    if (event.key === out) { backend.goUp(); event.accepted = true; return }
                }
                selecting.page(event) || selecting.extendBy(event, event.key === Qt.Key_Up ? -1 : event.key === Qt.Key_Down ? 1 : 0)
            }
            Keys.onRightPressed: view.LayoutMirroring.enabled ? backend.goUp() : view.enter()
            Keys.onLeftPressed: view.LayoutMirroring.enabled ? view.enter() : backend.goUp()

            delegate: FileRow {
                width: here.width
                chevron: true
                current: selected
                onClicked: (modifiers) => { selecting.click(index, modifiers); here.forceActiveFocus() }
                onActivated: backend.open(path)
            }

            FolioText {
                x: LayoutMirroring.enabled ? here.width - width - 46 : 46
                y: 8
                readonly property bool searching: backend.folder.loading && backend.folder.query.length > 0
                visible: here.count === 0 && (!backend.folder.loading || searching) && backend.folder.error.length === 0
                text: searching ? qsTr("Searching…") : backend.folder.query ? qsTr("No matches") : qsTr("No items")
                color: theme.secondaryForeground
            }
        }
        Separator { }

        // The preview: what's inside the selected folder, or the selected
        // file's details. Clicking inside a folder goes there.
        Item {
            id: previewArea
            width: Math.max(0, row.width - view.parentWidth - view.hereWidth - 2)
            height: row.height

            FileListView {
                id: inside
                anchors.fill: parent
                visible: view.selected.isDir === true
                TapHandler {
                    acceptedButtons: Qt.RightButton
                    onTapped: (point) => {
                        const index = inside.indexAt(point.position.x, point.position.y)
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
                PanelDrop { items: inside; folder: view.selected.path ?? ""; enabled: inside.visible }
                delegate: FileRow {
                    width: inside.width
                    chevron: true
                    onClicked: backend.showItem(path)
                    onActivated: backend.open(path)
                }
                FolioText {
                    x: LayoutMirroring.enabled ? inside.width - width - 46 : 46
                    y: 8
                    visible: inside.count === 0 && !inside.model.loading
                    text: qsTr("No items")
                    color: theme.secondaryForeground
                }
            }

            // Loaded by URL so PreviewPane isn't compiled until a file is
            // selected.
            Loader {
                id: preview
                anchors.fill: parent
                active: !!view.detailsPane && view.selected.path !== undefined && view.selected.isDir !== true
                visible: active
                source: "PreviewPane.qml"
                onLoaded: {
                    item.folderPane = Qt.binding(() => view.detailsPane)
                    // The facts and the Open button as narrow as the grid's
                    // details pane has them, not stretched across the panel.
                    item.textWidth = 300
                }
            }
            Connections {
                target: preview.item
                function onFinished() { view.focusView() }
            }
        }
    }
}
