import QtQuick
import QtQuick.Layouts

// Renaming (F2): the name with everything but its extension selected, so
// typing replaces the name and keeps the type. Enter renames, Escape leaves
// it alone; a clash with an existing name is said here, never overwritten.
FolioDialog {
    id: dialog

    property var item: null
    property string problem: ""

    title: qsTr("Rename")
    visible: item !== null
    onScrimClicked: close()

    function open(target) {
        problem = ""
        item = target
        // From the path: in search results the name is the path from where
        // the search started ("sub/x.txt").
        const name = target.path.slice(target.path.lastIndexOf("/") + 1)
        field.text = name
        const dot = fileOps.extensionStart(name, target.isDir)
        field.forceActiveFocus()
        field.select(0, dot > 0 ? dot : name.length)
    }
    function close() { item = null }
    function submit() {
        problem = fileOps.rename(item.path, field.text)
        if (problem.length === 0)
            close()
    }

    // On the dialog, not the field, so Escape still cancels after Tab has
    // moved the keyboard to a button.
    Keys.onEscapePressed: dialog.close()

    PlainField {
        id: field
        Layout.fillWidth: true
        onAccepted: dialog.submit()
    }
    // Aligned explicitly, so that it's mirrored: a problem that starts with
    // a file name would otherwise follow the name's script.
    Text {
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignLeft
        visible: dialog.problem.length > 0
        text: dialog.problem
        color: theme.red
        wrapMode: Text.Wrap
    }
    RowLayout {
        Layout.alignment: Qt.AlignRight
        spacing: 10
        PlainButton {
            text: qsTr("Cancel")
            onClicked: dialog.close()
        }
        PlainButton {
            text: qsTr("Rename")
            primary: true
            onClicked: dialog.submit()
        }
    }
}
