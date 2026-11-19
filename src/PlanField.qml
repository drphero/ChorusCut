import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One planned field: its label, who wrote it (an agent's value is tagged) and a lock that keeps
// agent passes away from it. The editor goes inside; it calls set(value) to write through the
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
    default property alias content: holder.data
    Layout.fillWidth: true
    spacing: 3

    function set(next) {
        const fields = {}
        fields[field] = next
        return editor.plan.run({op: "set", kind: kind, id: itemId, fields: fields}).ok
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 6
        Text {
            text: pf.label + (pf.required ? " *" : "")
            color: pf.required && pf.empty ? "#e0a24a" : "#adb8ba"
            font.pixelSize: 11
        }
        Item { Layout.fillWidth: true }
        Rectangle {
            visible: pf.prov !== undefined && pf.prov.by === "agent"
            implicitWidth: agentText.implicitWidth + 8; implicitHeight: 14; radius: 3; color: "#2f3d5c"
            Text { id: agentText; anchors.centerIn: parent; text: "agent"; color: "#b6c4ef"; font.pixelSize: 9 }
        }
        Text {
            objectName: "lock_" + pf.kind + "_" + pf.field
            readonly property bool locked: pf.prov !== undefined && pf.prov.locked === true
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
    Text {
        visible: pf.hint.length > 0
        Layout.fillWidth: true; wrapMode: Text.Wrap
        text: pf.hint; color: "#6f8085"; font.pixelSize: 10
    }
}
