import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The conversations with the agent about this plan: what the user asked (from "ask" on a field,
// a picked item or the whole plan) and what the agent asked back with ask_user, each with its
// messages, whose turn it is, a reply box, and Done.
ScrollView {
    id: page
    required property var ws
    contentWidth: availableWidth
    clip: true
    property int highlight: 0
    readonly property var threads: editor.requests.list

    function revealThread(id) {
        highlight = id
        Qt.callLater(() => {
            for (let i = 0; i < cards.count; ++i) {
                const card = cards.itemAt(i)
                if (card && card.thread.id === id) {
                    const flick = page.contentItem
                    flick.contentY = Math.max(0, Math.min(card.y - 12, flick.contentHeight - flick.height))
                }
            }
        })
    }
    // Opens what a thread is about.
    function open(about) {
        if (!about || !about.kind) return
        if (about.kind === "concept") ws.page = "concept"
        else if (about.kind === "arc") ws.reveal("section", about.id)
        else if (about.kind === "shot") ws.showShot(about.id)
        else ws.reveal(about.kind, about.id)
    }

    component Small: Button {
        id: sb
        property bool active: false
        property bool danger: false
        focusPolicy: Qt.NoFocus
        implicitHeight: 28; leftPadding: 10; rightPadding: 10
        contentItem: Text { text: sb.text; color: sb.active ? "#122328" : sb.danger ? "#f0a08f" : "#edf1ef"; font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: 4; color: sb.active ? (sb.hovered ? "#9fe3d0" : "#87dec7") : sb.hovered ? "#3b4b52" : "#2b383f" }
    }

    ColumnLayout {
        width: Math.min(page.availableWidth - 16, 760)
        spacing: 14
        RowLayout {
            Layout.fillWidth: true
            Text { text: "Requests"; color: "#edf1ef"; font.pixelSize: 18; font.weight: Font.DemiBold; Layout.fillWidth: true }
            Small { objectName: "askWholePlan"; text: "Ask about the whole plan…"; onClicked: page.Window.window.askAgent("", 0, "", "") }
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#8fa0a4"; font.pixelSize: 12
            text: "What you asked your agent (with “ask” on any field) and what it asked you. Requests are kept with the project. "
                  + (editor.agent.liveClient ? "Claude Code is connected; started with Chorus Cut as a channel, it gets each request at once."
                                             : "No Claude Code is connected: requests wait here until your agent checks them.")
                  + " To have requests arrive live, start Claude Code with: " + editor.agent.channelCommand
        }
        Text {
            visible: page.threads.length === 0
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#6f8085"; font.pixelSize: 12
            text: "Nothing asked yet."
        }
        Repeater {
            id: cards
            model: page.threads
            Rectangle {
                id: card
                required property var modelData
                required property int index
                readonly property var thread: modelData
                readonly property var messages: modelData.messages || []
                readonly property var last: messages.length ? messages[messages.length - 1] : null
                readonly property bool done: modelData.turn === "done"
                objectName: "thread" + modelData.id
                Layout.fillWidth: true
                implicitHeight: body.implicitHeight + 24
                radius: 6; color: "#18242b"
                border.color: page.highlight === modelData.id ? "#87dec7" : modelData.turn === "user" ? "#6b5a2c" : "#26343b"
                opacity: done ? 0.7 : 1
                ColumnLayout {
                    id: body
                    x: 12; y: 12; width: parent.width - 24
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        Text {
                            text: card.thread.label; color: "#9fe3d0"; font.pixelSize: 14; font.weight: Font.DemiBold
                            Layout.fillWidth: true; elide: Text.ElideRight
                            font.underline: aboutMouse.containsMouse && card.thread.about && card.thread.about.kind !== undefined
                            MouseArea { id: aboutMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: card.thread.about && card.thread.about.kind ? Qt.PointingHandCursor : Qt.ArrowCursor
                                        onClicked: page.open(card.thread.about) }
                        }
                        Rectangle {
                            implicitWidth: stateText.implicitWidth + 12; implicitHeight: 18; radius: 3
                            color: card.done ? "#26343b" : card.thread.turn === "user" ? "#4a3a1c" : "#1f3a35"
                            Text {
                                id: stateText
                                objectName: "threadState"
                                anchors.centerIn: parent; font.pixelSize: 10
                                color: card.done ? "#8fa0a4" : card.thread.turn === "user" ? "#e7b75f" : "#87dec7"
                                text: card.done ? "Done" : card.thread.turn === "user" ? "Your turn"
                                    : "Waiting for the agent" + (card.thread.seen ? " · seen" : card.thread.sent ? " · sent live" : "")
                            }
                        }
                    }
                    Repeater {
                        model: card.messages
                        ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true; spacing: 1
                            Text { text: parent.modelData.from === "agent" ? "Agent" : "You"; color: parent.modelData.from === "agent" ? "#b6c4ef" : "#8fa0a4"; font.pixelSize: 11 }
                            TextEdit {
                                Layout.fillWidth: true; readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
                                text: parent.modelData.text; color: "#edf1ef"; font.pixelSize: 13
                            }
                        }
                    }
                    // An agent's question with answers to click.
                    Flow {
                        visible: !card.done && card.thread.turn === "user" && card.last && card.last.options !== undefined
                        Layout.fillWidth: true; spacing: 6
                        Repeater {
                            model: card.last && card.last.options ? card.last.options : []
                            Small {
                                required property var modelData
                                required property int index
                                objectName: "option" + index
                                text: modelData
                                onClicked: editor.requests.reply(card.thread.id, modelData)
                            }
                        }
                    }
                    RowLayout {
                        visible: !card.done
                        Layout.fillWidth: true; spacing: 8
                        TextField {
                            id: replyField
                            objectName: "replyText"
                            Layout.fillWidth: true
                            selectByMouse: true
                            color: "#edf1ef"; font.pixelSize: 13
                            placeholderText: card.thread.turn === "user" ? "Reply…" : "Add to it…"
                            placeholderTextColor: "#6f8085"
                            background: Rectangle { radius: 4; color: "#162027"; border.color: replyField.activeFocus ? "#87dec7" : "#34434a" }
                            onAccepted: if (text.trim() && editor.requests.reply(card.thread.id, text)) text = ""
                        }
                        Small { objectName: "replySend"; text: "Send"; active: true; enabled: replyField.text.trim().length > 0
                                onClicked: if (editor.requests.reply(card.thread.id, replyField.text)) replyField.text = "" }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        Item { Layout.fillWidth: true }
                        Small { objectName: "threadDone"; visible: !card.done; text: "Mark done"; onClicked: editor.requests.close(card.thread.id) }
                        Small { objectName: "threadReopen"; visible: card.done; text: "Reopen"; onClicked: editor.requests.reopen(card.thread.id) }
                        Small { objectName: "threadDelete"; danger: true; text: "Delete"; onClicked: editor.requests.remove(card.thread.id) }
                    }
                }
            }
        }
        Item { height: 12 }
    }
}
