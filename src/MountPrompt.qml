import QtQuick
import QtQuick.Layouts

// GVfs's password request while connecting to another machine whose keys
// aren't set up, or its question about a server it can't vouch for (a key
// seen for the first time, a certificate that can't be verified), put to
// the user in the window, as is an encrypted drive's passphrase. The
// wording comes from GVfs, already translated.
FolioDialog {
    id: prompt

    readonly property var mounter: backend.mounter
    readonly property bool question: mounter.choices.length > 0

    readonly property bool drive: mounter.drive
    title: question ? (drive ? qsTr("Check the drive") : qsTr("Check the server"))
                    : drive ? qsTr("Unlock") : qsTr("Sign in")
    cardWidth: 600
    spacing: 18
    visible: mounter.asking

    // Each request starts fresh, whichever way the last one closed
    // (Connect, Cancel, Escape or GVfs giving up), so a password typed for
    // one machine is never offered to the next, nor kept once it's closed.
    // A question takes the keyboard with none of its answers picked, so Tab
    // goes to them and nothing is answered by a stray Enter.
    onVisibleChanged: {
        password.text = ""
        if (visible && question) {
            forceActiveFocus()
        } else if (visible) {
            user.text = drive ? "" : mounter.user
            password.forceActiveFocus()
        }
    }

    function submit() {
        prompt.mounter.providePassword(user.text, password.text)
    }

    // A click outside the card does nothing: GVfs waits for an answer,
    // which only the buttons or Escape give.
    Shortcut { sequence: "Escape"; enabled: prompt.visible; context: Qt.WindowShortcut; onActivated: prompt.mounter.cancel() }

    // Aligned explicitly, so that it's mirrored: GVfs may word it in
    // another language than Folio's.
    Text {
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignLeft
        text: prompt.mounter.prompt
        color: theme.foreground
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
    }

    PlainField {
        id: user
        // A drive's passphrase has no user to go with it.
        visible: !prompt.question && !prompt.drive
        Layout.fillWidth: true
        placeholderText: qsTr("User")
    }
    PlainField {
        id: password
        visible: !prompt.question
        Layout.fillWidth: true
        placeholderText: prompt.drive ? qsTr("Passphrase") : qsTr("Password")
        echoMode: TextInput.Password
        onAccepted: prompt.submit()
    }

    RowLayout {
        Layout.alignment: Qt.AlignRight
        spacing: 10

        // A question's own answers, none of them the default: going ahead
        // with a server that couldn't be checked takes a deliberate click.
        Repeater {
            model: prompt.mounter.choices
            PlainButton {
                required property string modelData
                required property int index
                text: modelData
                onClicked: prompt.mounter.choose(index)
            }
        }
        PlainButton {
            visible: !prompt.question
            text: qsTr("Cancel")
            onClicked: prompt.mounter.cancel()
        }
        PlainButton {
            visible: !prompt.question
            text: prompt.drive ? qsTr("Unlock") : qsTr("Connect")
            primary: true
            onClicked: prompt.submit()
        }
    }
}
