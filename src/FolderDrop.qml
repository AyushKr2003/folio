import QtQuick

// Takes files dropped on a view's empty space into its folder, lighting up
// while they hover. A folder item in the view takes drops on itself first;
// where it refuses one (it's the folder being dropped, or the one the drop
// is already in), this refuses too, rather than catching the drop as it
// falls through and landing it in this folder instead.
DropArea {
    id: area

    property string folder
    // The list or grid whose items lie over this area.
    property Item items

    function takes(drag) {
        const at = area.mapToItem(items.contentItem, drag.x, drag.y)
        const item = items.itemAt(at.x, at.y)
        return !(item && item.isDir) && fileOps.canDrop(drag.urls, folder)
    }

    keys: ["text/uri-list"]
    onEntered: (drag) => { drag.accepted = takes(drag) }
    onPositionChanged: (drag) => { drag.accepted = takes(drag) }
    onDropped: (drop) => {
        if (!takes(drop))
            return
        fileOps.dropInto(drop.urls, folder)
        drop.accept(Qt.MoveAction)
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: 2
        visible: area.containsDrag
        color: Qt.alpha(theme.accent, 0.05)
        border.width: 2
        border.color: theme.accent
        radius: Math.min(6, theme.radius)
    }
}
