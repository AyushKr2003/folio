import QtQuick

// How a view selects, shared by the grid, the list and the column view:
// click selects one, Shift+click or Shift+arrows a range from the anchor,
// Alt+click adds or removes one, and moving the cursor with the arrows
// selects what it lands on. The selection itself lives in the folder model.
QtObject {
    // The ListView or GridView whose currentIndex is the cursor.
    required property var view
    property int anchor: -1
    // Set while the cursor is moved on purpose, so moving it doesn't also
    // reset the selection.
    property bool quiet: false

    function moveTo(index) {
        quiet = true
        view.currentIndex = index
        quiet = false
    }

    // The cursor moved by the keyboard (or the view itself).
    function cursorMoved() {
        if (quiet)
            return
        backend.folder.selectOnly(view.currentIndex)
        anchor = view.currentIndex
        view.positionViewAtIndex(view.currentIndex, ListView.Contain)
    }

    function click(index, modifiers) {
        if (index < 0)
            return
        if (modifiers & Qt.ShiftModifier) {
            backend.folder.selectRange(anchor < 0 ? index : anchor, index, (modifiers & Qt.AltModifier) !== 0)
        } else if (modifiers & Qt.AltModifier) {
            backend.folder.toggleSelected(index)
            anchor = index
        } else {
            backend.folder.selectOnly(index)
            anchor = index
        }
        moveTo(index)
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
        if (anchor < 0)
            anchor = view.currentIndex
        moveTo(index)
        backend.folder.selectRange(anchor, index, false)
        view.positionViewAtIndex(index, ListView.Contain)
    }

    // No cursor, as a click on empty space leaves it. What's selected (by
    // Ctrl+A or the rubber band since) stays selected.
    function letGo() {
        moveTo(-1)
        anchor = -1
    }

    // Puts the cursor on `row` (or, for -1, the first item). `keep` leaves a
    // selection of several alone, for a folder that was only re-read.
    function select(row, keep) {
        const index = row >= 0 ? row : (view.count > 0 ? 0 : -1)
        moveTo(index)
        if (!(keep && backend.folder.selectedCount > 0)) {
            backend.folder.selectOnly(index)
            anchor = index
        }
        if (index >= 0)
            view.positionViewAtIndex(index, ListView.Contain)
    }

    // Right-click on an item outside the selection selects just it, so
    // the menu acts on what was clicked. Returns the item, or null for
    // empty space.
    function menuAt(point) {
        const index = view.indexAt(point.position.x + view.contentX, point.position.y + view.contentY)
        if (index >= 0 && !backend.folder.isSelected(index))
            click(index, 0)
        return index >= 0 ? backend.folder.get(index) : null
    }
}
