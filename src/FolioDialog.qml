import QtQuick
import QtQuick.Layouts

// The frame Rename, Confirm and the mount prompt share: a scrim over the
// pane and a card in its middle, titled, holding the dialog's own fields
// and buttons. A click on the scrim is the dialog's to answer; one on the
// card stays there. Tab and Shift+Tab go round the dialog's own fields and
// buttons, never on to the buttons behind it.
FocusScope {
    id: dialog

    property string title
    property int cardWidth: 520
    property color edge: theme.accent
    property alias spacing: column.spacing
    default property alias content: column.data

    signal scrimClicked()

    // Qt's Tab goes on through the whole window, so the dialog takes Tab
    // while it has the keyboard and steps through the chain itself, passing
    // over whatever lies outside it.
    Shortcut { sequence: "Tab"; enabled: dialog.activeFocus; onActivated: dialog.step(true) }
    Shortcut { sequence: "Shift+Backtab"; enabled: dialog.activeFocus; onActivated: dialog.step(false) }
    function step(forward) {
        let next = Window.activeFocusItem
        do {
            next = next.nextItemInFocusChain(forward)
        } while (!holds(next))
        next.forceActiveFocus(forward ? Qt.TabFocusReason : Qt.BacktabFocusReason)
    }
    function holds(item) {
        for (let i = item; i; i = i.parent)
            if (i === dialog)
                return true
        return false
    }

    Rectangle { anchors.fill: parent; color: Qt.rgba(0, 0, 0, 0.55) }
    MouseArea { anchors.fill: parent; onClicked: dialog.scrimClicked() }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, dialog.cardWidth)
        height: column.implicitHeight + 56
        color: theme.background
        border.width: 1
        border.color: dialog.edge
        radius: theme.radius
        MouseArea { anchors.fill: parent }

        ColumnLayout {
            id: column
            anchors.fill: parent
            anchors.margins: 28
            spacing: 16

            // A title can name a file, and a long name wraps rather than
            // running off the card.
            Text {
                Layout.fillWidth: true
                text: dialog.title
                wrapMode: Text.Wrap
                color: theme.brightForeground
                font.pixelSize: 16
                font.weight: Font.DemiBold
            }
        }
    }
}
