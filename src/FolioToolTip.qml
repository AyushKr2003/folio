import QtQuick
import QtQuick.Controls

// A hover message that follows the active theme. Material's ToolTip forces a
// dark gray surface and inherits the app's foreground as its text, so on a
// light theme the words nearly vanish. Paint it with colors from the palette
// instead: a surface a step from the pane, and an ink that reads on it.
ToolTip {
    id: tip

    delay: 600
    // Like the toolbar's old tooltips: shown at once, without a fade.
    enter: null
    exit: null

    font.family: theme.fontFamily
    font.pixelSize: Math.round(13 * theme.textScale)
    padding: 7
    horizontalPadding: 11

    contentItem: Text {
        text: tip.text
        font: tip.font
        wrapMode: Text.Wrap
        color: theme.tooltipForeground
    }

    background: Rectangle {
        color: theme.tooltipBackground
        border.width: 1
        border.color: Qt.alpha(theme.foreground, 0.12)
        radius: Math.min(4, theme.radius)
    }
}
