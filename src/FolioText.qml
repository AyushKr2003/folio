import QtQuick

// Text in the system's fixed-width font (Omarchy's pick), followed live:
// `size` is its pixel size at Omarchy's default text size, and the system's
// size scales it (Theme.textScale).
Text {
    property real size: 13
    font.family: theme.fontFamily
    font.pixelSize: Math.round(size * theme.textScale)
}
