import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtCore

// The window: one bar across the top, under it the sidebar, one folder pane
// or two side by side (Ctrl+Alt+N splits, Ctrl+W closes the right one),
// and the details pane. Each pane's path sits in the bar above its files;
// the sidebar and the details follow the pane used last.
ApplicationWindow {
    id: win
    width: 1280
    height: 800
    minimumWidth: 720
    minimumHeight: 460
    visible: true
    // The backend's title (Home, Trash) is translated in C++, which
    // retranslating QML doesn't redo, so the language is read here too.
    title: {
        translator.language
        return activePane && activePane.paneBackend.title.length > 0 ? activePane.paneBackend.title : qsTr("Files")
    }
    color: theme.background

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.foreground: theme.foreground
    Material.background: theme.background
    Material.roundedScale: theme.radius

    // Right-to-left languages mirror the window, its menus and tooltips
    // included: they live in the window's overlay, outside its other items.
    // The Translator sets the application's layout direction.
    LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true

    // Hyprland sizes a new window only once it's on screen: the first frames
    // are drawn at the size asked for above (or the compositor's first
    // guess), then the real size arrives and everything moves. The contents
    // stay hidden until that size is in, or until it's clear none is coming,
    // so they first appear where they'll stay. Until then they take no
    // clicks or keys either, so nothing acts on files no one can see yet.
    property bool settled: false
    onSettledChanged: activePane.focusView()
    property bool drawn: false
    onFrameSwapped: if (!drawn) { drawn = true; settleTimer.start() }
    onWidthChanged: if (drawn) settled = true
    onHeightChanged: if (drawn) settled = true
    Timer { id: settleTimer; interval: 150; onTriggered: win.settled = true }

    property bool helpVisible: false
    // The shortcuts take the keyboard while they're shown, and give it back
    // to the files.
    onHelpVisibleChanged: helpVisible ? help.forceActiveFocus() : activePane.focusView()
    property bool previewVisible: true
    // Columns on a first launch; after that, whichever view was used last.
    property string viewMode: "columns"
    property real zoom: 1

    // The pane keys, the sidebar and the details go to.
    property var activePane: leftPane
    // The right pane while the window is split.
    property var rightPane: null
    readonly property bool split: rightPane !== null
    // Whether keys are going into a text field. The right pane can go before
    // the window does when it closes, leaving activePane null.
    readonly property bool typing: !!activePane && activePane.typing
    // The rightmost pane's path bar reaches over the details column.
    readonly property int detailsWidth: details.visible ? details.width : 0
    // Whether the window is wide enough for the details column at all.
    readonly property bool detailsFit: width >= (split ? 1400 : 960)

    // The process works in the folder the window used last shows, in the
    // pane used last, for Omarchy's terminal key to open there.
    onActiveChanged: if (active && activePane) activePane.paneBackend.keepWorkingDirectory()
    onActivePaneChanged: if (active && activePane) activePane.paneBackend.keepWorkingDirectory()

    // Remembers the view between launches (~/.config/folio/folio.conf).
    Settings {
        property alias viewMode: win.viewMode
        property alias zoom: win.zoom
    }

    // Ctrl+Alt+N: a second pane on the right, starting where the active
    // one is. Pressed again, it moves the keyboard there.
    function openSplit() {
        if (!rightPane) {
            rightPane = windows.openSplit(win, activePane.paneBackend.path)
            if (!rightPane)
                return
            rightPane.parent = splitHost
            rightPane.anchors.fill = splitHost
            rightPane.closable = true
            rightPane.viewMode = activePane.viewMode
            rightPane.zoom = activePane.zoom
            rightPane.barExtension = Qt.binding(() => win.detailsWidth)
            rightPane.closeRequested.connect(closeSplit)
        }
        rightPane.focusView()
    }
    function closeSplit() {
        if (!rightPane)
            return
        activePane = leftPane
        rightPane = null
        windows.closeSplit(win)
        leftPane.focusView()
    }

    Shortcut { sequence: "?"; enabled: !win.typing || win.helpVisible; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Escape"; enabled: win.helpVisible; onActivated: win.helpVisible = false }
    Shortcut { sequence: "Q"; enabled: !win.typing; onActivated: win.close() }
    Shortcut { sequence: "Ctrl+Alt+N"; enabled: !win.typing; onActivated: win.openSplit() }
    Shortcut { sequence: "Ctrl+W"; enabled: win.split; onActivated: win.closeSplit() }
    Shortcut { sequence: "Ctrl+I"; onActivated: win.previewVisible = !win.previewVisible }
    // Tab moves the keyboard: sidebar, left pane, right pane, the details
    // (while there's an app to pick there), and around. Shift+Tab goes back.
    Shortcut { sequence: "Tab"; enabled: !win.typing; onActivated: win.moveKeyboard(1) }
    Shortcut { sequence: "Shift+Backtab"; enabled: !win.typing; onActivated: win.moveKeyboard(-1) }
    function moveKeyboard(step) {
        const stops = [sidebar, leftPane].concat(split ? [rightPane] : [], details.takesKeyboard ? [details] : [])
        const here = sidebar.activeFocus ? sidebar : details.hasKeyboard ? details : activePane
        const next = stops[(stops.indexOf(here) + step + stops.length) % stops.length]
        if (next === sidebar)
            sidebar.focusSidebar()
        else if (next === details)
            details.focusKeyboard()
        else
            next.focusView()
    }

    Item {
        anchors.fill: parent
        enabled: win.settled
        opacity: win.settled ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 100 } }

        // The bar across the top; the panes' path bars draw over it.
        readonly property int barHeight: 60
        Rectangle {
            width: parent.width
            height: parent.barHeight
            color: theme.background
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Qt.alpha(theme.foreground, 0.12) }
        }

        RowLayout {
            anchors.fill: parent
            spacing: 0

            Sidebar {
                id: sidebar
                target: win.activePane ? win.activePane.paneBackend : backend
                onFinished: win.activePane.focusView()
                onHelpRequested: win.helpVisible = !win.helpVisible
                onFormatRequested: (disk) => win.activePane.formatDrive(disk)
                Layout.topMargin: parent.parent.barHeight
                Layout.preferredWidth: 190
                Layout.fillHeight: true
            }
            Rectangle { Layout.topMargin: parent.parent.barHeight; Layout.fillHeight: true; implicitWidth: 1; color: Qt.alpha(theme.foreground, 0.12) }

            FolderPane {
                id: leftPane
                barExtension: win.split ? 0 : win.detailsWidth
                barLeft: sidebar.width + 1
                Layout.fillWidth: true
                Layout.fillHeight: true
                focus: true
            }

            // openSplit() puts the right pane of a split here. Between the
            // panes the divider runs up through the top bar, so each path
            // reads as belonging to the files below it.
            Rectangle { visible: win.split; Layout.fillHeight: true; implicitWidth: 1; color: Qt.alpha(theme.foreground, 0.12) }
            Item {
                id: splitHost
                visible: win.split
                Layout.fillWidth: true
                Layout.fillHeight: true
            }

            PreviewPane {
                id: details
                visible: win.previewVisible && win.detailsFit
                Layout.topMargin: parent.parent.barHeight
                Layout.preferredWidth: 300
                Layout.fillHeight: true
                folderPane: win.activePane
                onFinished: win.activePane.focusView()
            }
        }

        HelpOverlay {
            id: help
            anchors.fill: parent
            visible: win.helpVisible
            onDismissed: win.helpVisible = false
        }
    }
}
