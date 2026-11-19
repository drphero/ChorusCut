import QtQuick
import QtQuick.Controls

// A character or location named in the plan, as a small chip that opens its links.
Button {
    id: chip
    property string kind: "character"
    property int itemId: 0
    readonly property bool person: kind === "character"
    focusPolicy: Qt.NoFocus
    implicitHeight: 22
    leftPadding: 8; rightPadding: 8; topPadding: 0; bottomPadding: 0
    contentItem: Text {
        text: chip.text; elide: Text.ElideRight
        color: chip.person ? "#f2c2d8" : "#b5ecdc"; font.pixelSize: 12
        verticalAlignment: Text.AlignVCenter
    }
    background: Rectangle {
        radius: 4
        color: chip.person ? (chip.hovered ? "#5a2f45" : "#40263a") : (chip.hovered ? "#24564a" : "#1c3d36")
    }
    ToolTip.visible: hovered && contentItem.truncated; ToolTip.delay: 400; ToolTip.text: text
}
