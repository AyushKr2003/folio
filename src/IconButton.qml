import QtQuick
import QtQuick.Controls

// A square toolbar button holding one glyph. `text` is its accessible name
// and tooltip, never drawn.
AbstractButton {
    id: button

    property string glyph
    property bool active: false

    implicitWidth: 36
    implicitHeight: 36
    focusPolicy: Qt.NoFocus
    hoverEnabled: true

    Accessible.name: text
    // Its own tooltip, painted in the theme's colors.
    FolioToolTip {
        visible: button.hovered && button.text.length > 0
        text: button.text
    }

    background: Rectangle {
        radius: Math.min(4, theme.radius)
        color: button.active ? Qt.alpha(theme.accent, 0.16)
             : button.hovered && button.enabled ? Qt.alpha(theme.foreground, 0.08)
             : "transparent"
    }

    contentItem: Item {
        Glyph {
            anchors.centerIn: parent
            name: button.glyph
            color: !button.enabled ? Qt.alpha(theme.foreground, 0.35)
                 : button.active ? theme.brightForeground : theme.foreground
        }
    }
}
