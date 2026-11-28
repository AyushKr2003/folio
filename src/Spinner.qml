import QtQuick
import QtQuick.Shapes

// A small turning arc, for work under way with no end in sight yet (a
// search going through the folders below).
Item {
    id: spinner
    property real size: 14
    property color color: theme.secondaryForeground
    readonly property real effectiveSize: Math.round(size * theme.textScale)
    implicitWidth: effectiveSize
    implicitHeight: effectiveSize

    Shape {
        anchors.fill: parent
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeColor: spinner.color
            strokeWidth: 1.6
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathAngleArc {
                centerX: spinner.effectiveSize / 2
                centerY: spinner.effectiveSize / 2
                radiusX: spinner.effectiveSize / 2 - 1
                radiusY: spinner.effectiveSize / 2 - 1
                startAngle: 0
                sweepAngle: 270
            }
        }
        RotationAnimator on rotation {
            running: spinner.visible
            from: 0
            to: 360
            duration: 900
            loops: Animation.Infinite
        }
    }
}
