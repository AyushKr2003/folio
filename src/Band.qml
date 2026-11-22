import QtQuick

// Rubber-band selection: press on empty space and drag a rectangle over
// items to select them (with Alt, add them to what's selected). A click on
// empty space without dragging clears the selection. Lives inside a view's
// content, so the rectangle scrolls with it.
MouseArea {
    id: band

    // The GridView or ListView, and the area each item takes in it.
    required property var view
    required property var rectOf

    property point start
    property point end
    property bool dragging: false
    property var before: []

    z: -1
    width: view.width
    height: Math.max(view.height, view.contentHeight)
    acceptedButtons: Qt.LeftButton

    function hit(a, b) {
        return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height
    }

    onPressed: (mouse) => {
        // Presses on an item belong to the item (select, drag it away);
        // the padding around a tile is empty space.
        const i = view.indexAt(mouse.x, mouse.y)
        if (i >= 0 && hit(rectOf(i), Qt.rect(mouse.x, mouse.y, 0, 0))) {
            mouse.accepted = false
            return
        }
        view.forceActiveFocus()
        start = Qt.point(mouse.x, mouse.y)
        end = start
        dragging = false
        const adding = (mouse.modifiers & Qt.AltModifier) !== 0
        before = []
        if (adding)
            for (let i = 0; i < view.count; ++i)
                if (backend.folder.isSelected(i))
                    before.push(i)
    }
    onPositionChanged: (mouse) => {
        end = Qt.point(mouse.x, mouse.y)
        if (!dragging && Math.abs(end.x - start.x) + Math.abs(end.y - start.y) < 6)
            return
        dragging = true
        const area = Qt.rect(Math.min(start.x, end.x), Math.min(start.y, end.y),
                             Math.abs(end.x - start.x), Math.abs(end.y - start.y))
        const rows = before.slice()
        // An item already in `before` may go in twice; selectRows takes
        // each once.
        for (let i = 0; i < view.count; ++i)
            if (hit(rectOf(i), area))
                rows.push(i)
        backend.folder.selectRows(rows)
        // Dragging past the top or bottom edge scrolls.
        const onScreen = mouse.y - view.contentY
        if (onScreen > view.height - 24)
            view.contentY = Math.min(view.contentHeight - view.height, view.contentY + 16)
        else if (onScreen < 24)
            view.contentY = Math.max(0, view.contentY - 16)
    }
    onReleased: {
        // A click on empty space lets go of the cursor too, so keys that act
        // on the cursor's item (Delete, F2, Return) have nothing to act on.
        if (!dragging) {
            backend.folder.clearSelection()
            view.letGo()
        }
        dragging = false
    }

    Rectangle {
        visible: band.dragging
        x: Math.min(band.start.x, band.end.x)
        y: Math.min(band.start.y, band.end.y)
        width: Math.abs(band.end.x - band.start.x)
        height: Math.abs(band.end.y - band.start.y)
        color: Qt.alpha(theme.accent, 0.12)
        border.width: 1
        border.color: theme.accent
    }
}
