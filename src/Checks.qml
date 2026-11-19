import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Plan checks, recomputed after every edit. Errors are wrong, to-dos are unfinished; both hold
// back batch rendering (any single shot can still be tried). Warnings are advice.
Rectangle {
    id: ck
    color: "transparent"
    property bool compact: false
    signal showCheck(var check)
    readonly property var plan: editor.plan
    readonly property var ordered: {
        // Errors first, then to-dos, then advice; otherwise in the plan's own order (song, concept, shots).
        const rank = {error: 0, todo: 1, warning: 2}
        return plan.checks.map((c, i) => ({c: c, i: i})).sort((a, b) => rank[a.c.level] - rank[b.c.level] || a.i - b.i).map(x => x.c)
    }
    readonly property var levelColor: ({error: "#f0a08f", todo: "#e0a24a", warning: "#9fb4d8"})

    component Small: Button {
        id: sb
        focusPolicy: Qt.NoFocus
        implicitHeight: 24; leftPadding: 8; rightPadding: 8
        contentItem: Text { text: sb.text; color: "#edf1ef"; font.pixelSize: 11; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: 4; color: sb.hovered ? "#3b4b52" : "#2b383f" }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8
        Text {
            visible: !ck.compact
            text: "Checks"; color: "#edf1ef"; font.pixelSize: 18; font.weight: Font.DemiBold
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: ck.compact ? 8 : 14
            Repeater {
                model: ck.plan.gates
                RowLayout {
                    required property var modelData
                    spacing: 5
                    Rectangle { width: 8; height: 8; radius: 4; color: parent.modelData.pass ? "#87dec7" : "#e0a24a" }
                    Text { text: parent.modelData.gate + " " + parent.modelData.label; color: "#adb8ba"; font.pixelSize: 11 }
                }
            }
        }
        Text {
            visible: ck.ordered.length === 0
            text: "Everything planned so far checks out."; color: "#87dec7"; font.pixelSize: 12
        }
        ListView {
            id: list
            objectName: "checksList"
            Layout.fillWidth: true; Layout.fillHeight: true
            clip: true
            spacing: 4
            model: ck.ordered
            ScrollBar.vertical: ScrollBar {}
            delegate: Rectangle {
                id: row
                required property var modelData
                required property int index
                objectName: "check_" + modelData.code + "_" + index
                width: list.width - 6
                height: body.implicitHeight + 12
                radius: 4
                color: rowMouse.containsMouse ? "#22313a" : "#18242b"
                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    onClicked: ck.showCheck(row.modelData)
                }
                RowLayout {
                    id: body
                    x: 8; y: 6; width: parent.width - 16
                    spacing: 8
                    Rectangle { Layout.alignment: Qt.AlignTop; Layout.topMargin: 4; width: 7; height: 7; radius: 3.5; color: ck.levelColor[row.modelData.level] }
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 2
                        Text {
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                            text: row.modelData.message; color: "#edf1ef"; font.pixelSize: 12
                        }
                        Text {
                            text: ({error: "Error", todo: "To do", warning: "Advice"})[row.modelData.level] + " · " + row.modelData.gate
                            color: "#6f8085"; font.pixelSize: 10
                        }
                    }
                    Small {
                        objectName: "fix_" + row.modelData.fix
                        visible: row.modelData.fix.length > 0
                        Layout.alignment: Qt.AlignTop
                        text: row.modelData.fix === "splitLines" ? "Split at lines" : row.modelData.fix === "markCurrent" ? "Mark current" : ""
                        onClicked: {
                            const c = row.modelData
                            if (c.fix === "splitLines") ck.plan.run({op: "shot.splitLines", id: c.shots[0]})
                            else if (c.fix === "markCurrent") ck.plan.run({op: "shot.markCurrent", ids: c.shots})
                        }
                    }
                }
            }
        }
    }
}
