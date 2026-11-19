import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One planned field: its label, who wrote it (a dot marks an agent's value) and its tools (ask,
// the large editor, lock), which stay out of sight until the pointer is on the field so a page of
// fields reads as its content. The editor goes inside; it calls set(value) to write through the
// plan's command layer, which records the change as one undo step.
ColumnLayout {
    id: pf
    property string kind
    property int itemId: 1
    property string field
    property string label
    property var item: null                   // the row: {fields, prov}
    property bool required: false
    property string hint: ""
    readonly property var value: item && item.fields ? item.fields[field] : undefined
    readonly property var prov: item && item.prov ? item.prov[field] : undefined
    // Lists from C++ are array-like but not JS arrays, so test their length.
    readonly property bool empty: value === undefined || value === null || value === "" || (typeof value === "object" && value.length === 0)
    readonly property var list: value !== undefined && value !== null && typeof value === "object" && value.length !== undefined ? Array.from(value) : []
    // A button in the header that asks for a bigger editor (see PromptEditor).
    property bool expandable: false
    signal expandRequested()
    default property alias content: holder.data
    Layout.fillWidth: true
    spacing: 3
    HoverHandler { id: hover }
    // Tools show while pointed at (they stay clickable, only faded out otherwise).
    readonly property bool showTools: hover.hovered
    readonly property bool locked: prov !== undefined && prov.locked === true

    function set(next) {
        const fields = {}
        fields[field] = next
        return editor.plan.run({op: "set", kind: kind, id: itemId, fields: fields}).ok
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        // The hint is the label's tooltip, so it never moves the page.
        Text {
            text: pf.label + (pf.required ? " *" : "") + (pf.hint.length ? "  ⓘ" : "")
            color: pf.required && pf.empty ? "#e0a24a" : "#adb8ba"
            font.pixelSize: 11
            MouseArea { id: labelMouse; anchors.fill: parent; hoverEnabled: true; enabled: pf.hint.length > 0 }
            ToolTip.visible: labelMouse.containsMouse && pf.hint.length > 0; ToolTip.delay: 300
            ToolTip.text: pf.hint
        }
        // Written by an agent.
        Rectangle {
            objectName: "agentDot_" + pf.kind + "_" + pf.field
            visible: pf.prov !== undefined && pf.prov.by === "agent"
            width: 6; height: 6; radius: 3; color: "#8a9bd6"
            MouseArea { id: dotMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true }
            ToolTip.visible: dotMouse.containsMouse; ToolTip.delay: 300; ToolTip.text: "Written by your agent"
        }
        Item { Layout.fillWidth: true }
        // Open requests to the agent about this field; click to see them.
        Rectangle {
            id: asked
            objectName: "asked_" + pf.kind + "_" + pf.field
            readonly property var threads: (editor.requests.list, editor.requests.about(pf.kind, pf.itemId, pf.field))
            readonly property bool yourTurn: threads.some(t => t.turn === "user")
            visible: threads.length > 0
            implicitWidth: askedText.implicitWidth + 10; implicitHeight: 14; radius: 3
            color: yourTurn ? "#4a3a1c" : "#1f3a35"
            Text { id: askedText; anchors.centerIn: parent; text: asked.yourTurn ? "agent replied" : "asked"; color: asked.yourTurn ? "#e7b75f" : "#87dec7"; font.pixelSize: 9 }
            MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: pf.Window.window.showRequests(asked.threads[0].id) }
        }
        Text {
            objectName: "ask_" + pf.kind + "_" + pf.field
            opacity: pf.showTools ? 1 : 0
            text: "ask"
            color: askMouse.containsMouse ? "#adb8ba" : "#5d6c71"
            font.pixelSize: 9
            MouseArea { id: askMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: pf.Window.window.askAgent(pf.kind, pf.itemId, pf.field, pf.label) }
            ToolTip.visible: askMouse.containsMouse; ToolTip.delay: 500; ToolTip.text: "Ask your agent to change this, or ask it about it"
        }
        Text {
            objectName: "expand_" + pf.kind + "_" + pf.field
            visible: pf.expandable
            opacity: pf.showTools ? 1 : 0
            text: "⤢ Editor"
            color: expandMouse.containsMouse ? "#9fe3d0" : "#7f9a9f"
            font.pixelSize: 10
            MouseArea { id: expandMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: pf.expandRequested() }
            ToolTip.visible: expandMouse.containsMouse; ToolTip.delay: 500; ToolTip.text: "Read and edit it in a large editor, with what it should follow"
        }
        Text {
            objectName: "lock_" + pf.kind + "_" + pf.field
            readonly property bool locked: pf.locked
            opacity: pf.showTools || locked ? 1 : 0
            text: locked ? "locked" : "lock"
            color: locked ? "#87dec7" : lockMouse.containsMouse ? "#adb8ba" : "#5d6c71"
            font.pixelSize: 9
            MouseArea {
                id: lockMouse
                anchors.fill: parent; anchors.margins: -4
                hoverEnabled: true
                onClicked: editor.plan.run({op: "lock", kind: pf.kind, id: pf.itemId, field: pf.field, locked: !parent.locked})
            }
            ToolTip.visible: lockMouse.containsMouse
            ToolTip.delay: 500
            ToolTip.text: locked ? "Agent passes leave this field alone. Click to unlock." : "Lock this field against agent passes"
        }
    }
    ColumnLayout { id: holder; Layout.fillWidth: true; spacing: 0 }

}
