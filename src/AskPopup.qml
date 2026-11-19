import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// "Ask the agent…": a request about one field or item (or the whole plan) for the user's agent.
// It is kept in the project's requests and, when Claude Code is connected with Chorus Cut as a
// channel, reaches it at once.
Popup {
    id: ask
    objectName: "askPopup"
    modal: true
    padding: 20
    anchors.centerIn: Overlay.overlay
    width: Math.min(560, (Overlay.overlay ? Overlay.overlay.width : 560) - 60)
    background: Rectangle { color: "#1d2930"; radius: 6; border.color: "#34434a" }

    property string kind
    property int itemId
    property string field
    property string label
    property string currentValue: ""
    signal sent(int thread)

    function start(kind, id, field, label, value) {
        ask.kind = kind; ask.itemId = id; ask.field = field; ask.label = label
        currentValue = value === undefined || value === null ? "" : (typeof value === "object" ? JSON.stringify(value) : String(value))
        open()
    }
    function send() {
        const thread = editor.requests.ask(kind, itemId, field, label, text.text)
        if (!thread) return
        close()
        sent(thread)
    }
    onOpened: { text.text = ""; text.forceActiveFocus() }

    contentItem: ColumnLayout {
        spacing: 10
        Text { text: "Ask your agent"; color: "#8fa0a4"; font.pixelSize: 12 }
        Text { objectName: "askLabel"; text: ask.label; color: "#edf1ef"; font.pixelSize: 16; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
        Text {
            visible: ask.currentValue.length > 0
            Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideRight
            text: "Now: " + ask.currentValue; color: "#8fa0a4"; font.pixelSize: 12
        }
        ScrollView {
            id: askScroll
            Layout.fillWidth: true; Layout.preferredHeight: 120
            contentWidth: availableWidth
            background: Rectangle { radius: 4; color: "#162027"; border.color: text.activeFocus ? "#87dec7" : "#34434a" }
            TextArea {
                id: text
                objectName: "askText"
                width: askScroll.availableWidth
                wrapMode: TextEdit.Wrap; selectByMouse: true
                color: "#edf1ef"; font.pixelSize: 13
                leftPadding: 10; rightPadding: 10; topPadding: 8; bottomPadding: 8
                placeholderText: "e.g. Make this more 80s, or: why a crane move here?"
                placeholderTextColor: "#6f8085"
                background: null
                Keys.onPressed: e => {
                    if ((e.key === Qt.Key_Return || e.key === Qt.Key_Enter) && (e.modifiers & Qt.ControlModifier)) { ask.send(); e.accepted = true }
                    if (e.key === Qt.Key_Escape) { ask.close(); e.accepted = true }
                }
            }
        }
        Text {
            objectName: "askDelivery"
            Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 12
            color: editor.agent.liveClient ? "#87dec7" : "#8fa0a4"
            text: editor.agent.liveClient
                  ? "Claude Code is connected. If it was started with Chorus Cut as a channel, it gets this right away; otherwise it sees it next time it checks."
                  : "No Claude Code is connected right now. The request waits in Plan ▸ Requests; ask your agent to check its Chorus Cut requests."
        }
        RowLayout {
            Layout.fillWidth: true
            Text { text: "⌘Return sends, Esc cancels"; color: "#6f8085"; font.pixelSize: 11; Layout.fillWidth: true }
            Button {
                id: cancelButton
                focusPolicy: Qt.NoFocus
                implicitHeight: 30; leftPadding: 12; rightPadding: 12
                text: "Cancel"
                contentItem: Text { text: cancelButton.text; color: "#edf1ef"; font.pixelSize: 13; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { radius: 5; color: cancelButton.hovered ? "#3b4b52" : "#2b383f" }
                onClicked: ask.close()
            }
            Button {
                id: sendButton
                objectName: "askSend"
                focusPolicy: Qt.NoFocus
                enabled: text.text.trim().length > 0
                implicitHeight: 30; leftPadding: 12; rightPadding: 12
                text: "Send"
                contentItem: Text { text: sendButton.text; color: sendButton.enabled ? "#122328" : "#4b6b66"; font.pixelSize: 13; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { radius: 5; color: sendButton.enabled ? "#87dec7" : "#2b4a44" }
                onClicked: ask.send()
            }
        }
    }
}
