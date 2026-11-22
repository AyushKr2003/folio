import QtQuick

// How a view selects, shared by the grid, the list and the column view:
// click selects one, Shift+click or Shift+arrows a range from the anchor,
// Alt+click adds or removes one, and moving the cursor with the arrows
// selects what it lands on. The selection itself lives in the folder model.
QtObject {
    // The ListView or GridView whose currentIndex is the cursor.
    required property var view
    property int anchor: -1
    // The anchor's item, so its row is found again when rows come or go
    // above it (a folder opening in the list's tree): it can be on the
    // other side of them from the cursor.
    property string anchorPath: ""
    function setAnchor(row) {
        anchor = row
        anchorPath = row >= 0 ? (backend.folder.get(row).path || "") : ""
    }
    // Set while the cursor is moved on purpose, so moving it doesn't also
    // reset the selection.
    property bool quiet: false

    function moveTo(index) {
        quiet = true
        view.currentIndex = index
        quiet = false
        placed()
    }

    // The item the cursor is on, and its row. Rows coming or going above it
    // (a folder opening in the list's tree) move the cursor's row without
    // moving the cursor: that isn't a move, and leaves the selection alone.
    property string cursorPath: ""
    property int cursorRow: -1
    function placed() {
        cursorPath = view.currentIndex >= 0 ? (backend.folder.get(view.currentIndex).path || "") : ""
        cursorRow = view.currentIndex
    }

    // The cursor moved by the keyboard (or the view itself).
    function cursorMoved() {
        if (quiet)
            return
        const path = view.currentIndex >= 0 ? (backend.folder.get(view.currentIndex).path || "") : ""
        if (path.length > 0 && path === cursorPath) {
            // Still on its item: the anchor stays on its own item too.
            if (anchorPath.length > 0)
                anchor = backend.folder.indexOfPath(anchorPath)
            cursorRow = view.currentIndex
            return
        }
        backend.folder.selectOnly(view.currentIndex)
        setAnchor(view.currentIndex)
        view.positionViewAtIndex(view.currentIndex, ListView.Contain)
        placed()
    }

    function click(index, modifiers) {
        if (index < 0)
            return
        if (modifiers & Qt.ShiftModifier) {
            backend.folder.selectRange(anchor < 0 ? index : anchor, index, (modifiers & Qt.AltModifier) !== 0)
        } else if (modifiers & Qt.AltModifier) {
            backend.folder.toggleSelected(index)
            setAnchor(index)
        } else {
            backend.folder.selectOnly(index)
            setAnchor(index)
        }
        moveTo(index)
    }

    // How many items Page Up and Page Down move by: a screenful, less one
    // row so the one paged from stays in sight.
    property int pageStep: 1

    // Page Up and Page Down move the cursor a screenful, selecting what it
    // lands on; with Shift they extend the selection, as Shift+arrows do.
    // Returns whether the key was one of them.
    function page(event) {
        if (event.key !== Qt.Key_PageUp && event.key !== Qt.Key_PageDown)
            return false
        event.accepted = true
        if (view.count === 0)
            return true
        const from = Math.max(0, view.currentIndex)
        const to = Math.max(0, Math.min(view.count - 1, from + (event.key === Qt.Key_PageUp ? -pageStep : pageStep)))
        if (event.modifiers & Qt.ShiftModifier)
            extendTo(to)
        else
            view.currentIndex = to
        return true
    }

    // Shift+arrow, `step` items on (0 for any other key): extends the
    // selection from the anchor.
    function extendBy(event, step) {
        if (!(event.modifiers & Qt.ShiftModifier) || step === 0)
            return
        extendTo(view.currentIndex + step)
        event.accepted = true
    }

    // Extends from the anchor to where the cursor goes.
    function extendTo(index) {
        if (index < 0 || index >= view.count)
            return
        // With no cursor (let go of by a click on empty space), the range
        // starts at the first item, where a step from no cursor starts.
        if (anchor < 0)
            setAnchor(Math.max(0, view.currentIndex))
        moveTo(index)
        backend.folder.selectRange(anchor, index, false)
        view.positionViewAtIndex(index, ListView.Contain)
    }

    // No cursor, as a click on empty space leaves it. What's selected (by
    // Ctrl+A or the rubber band since) stays selected.
    function letGo() {
        moveTo(-1)
        setAnchor(-1)
    }

    // Puts the cursor on `row` (or, for -1, the first item). `keep` leaves a
    // selection of several alone, for a folder that was only re-read.
    function select(row, keep) {
        const index = row >= 0 ? row : (view.count > 0 ? 0 : -1)
        moveTo(index)
        if (!(keep && backend.folder.selectedCount > 0)) {
            backend.folder.selectOnly(index)
            setAnchor(index)
        }
        if (index >= 0)
            view.positionViewAtIndex(index, ListView.Contain)
    }

    // Right-click on an item outside the selection selects just it, so
    // the menu acts on what was clicked. Returns the item, or null for
    // empty space.
    // The handler is on the view's content (a Flickable puts it there), so
    // its point is in content coordinates already: adding the scroll again
    // picked an item as far further on as the view was scrolled (#129).
    function menuAt(point) {
        const index = view.indexAt(point.position.x, point.position.y)
        if (index >= 0 && !backend.folder.isSelected(index))
            click(index, 0)
        return index >= 0 ? backend.folder.get(index) : null
    }
}
