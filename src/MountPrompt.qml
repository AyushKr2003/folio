import QtQuick
import QtQuick.Layouts

// GVfs's password request while connecting to another machine whose keys
// aren't set up, or an encrypted drive's passphrase, put to the user in the
// window. The wording comes from GVfs, already translated.
FolioDialog {
    id: prompt

    readonly property var mounter: backend.mounter

    title: mounter.drive ? qsTr("Unlock") : qsTr("Sign in")
    cardWidth: 600
    spacing: 18
    visible: mounter.asking

    // Each request starts fresh, whichever way the last one closed
    // (Connect, Cancel, Escape or GVfs giving up), so a password typed for
    // one machine is never offered to the next, nor kept once it's closed.
    onVisibleChanged: {
        password.text = ""
        if (visible) {
            user.text = mounter.drive ? "" : mounter.user
            password.forceActiveFocus()
        }
    }

    function submit() {
        prompt.mounter.providePassword(user.text, password.text)
    }

    // A click outside the card does nothing: GVfs waits for an answer,
    // which only the buttons or Escape give.
    Shortcut { sequence: "Escape"; enabled: prompt.visible; context: Qt.WindowShortcut; onActivated: prompt.mounter.cancel() }

    Text {
        Layout.fillWidth: true
        text: prompt.mounter.prompt
        color: theme.foreground
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
    }

    PlainField {
        id: user
        Layout.fillWidth: true
        // A drive's passphrase has no user to go with it.
        visible: !prompt.mounter.drive
        placeholderText: qsTr("User")
    }
    PlainField {
        id: password
        Layout.fillWidth: true
        placeholderText: prompt.mounter.drive ? qsTr("Passphrase") : qsTr("Password")
        echoMode: TextInput.Password
        onAccepted: prompt.submit()
    }

    RowLayout {
        Layout.alignment: Qt.AlignRight
        spacing: 10

        PlainButton {
            text: qsTr("Cancel")
            onClicked: prompt.mounter.cancel()
        }
        PlainButton {
            text: prompt.mounter.drive ? qsTr("Unlock") : qsTr("Connect")
            primary: true
            onClicked: prompt.submit()
        }
    }
}
