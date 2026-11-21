import QtQuick

// A picture to pick: at its own shape (or cropped square where there is little room), with its
// place in the order. Resting the pointer on it shows it large beside it (PicturePeek), so
// pictures that look alike at this size can be told apart before clicking.
Item {
    id: tile
    required property var picture       // a reference as Plan exports it: url, width, height, whose, viewName, approved
    property int order: -1               // place in the order picked, or -1
    property bool selected: order >= 0
    property real size: 96               // height of the picture
    property bool square: false
    property string label: ""
    property int labelLines: 1
    property color labelColor: selected ? "#87dec7" : "#8fa0a4"
    property real dimmed: 0.6            // opacity when not picked
    property string detail: ""           // an extra line in the large view
    property real minWidth: 0            // room for a longer label; the picture stays centred
    signal clicked()

    readonly property real aspect: picture && picture.width > 0 && picture.height > 0 ? picture.width / picture.height : 1
    readonly property real frameW: square ? size : Math.round(Math.max(size * 0.6, Math.min(size * 1.8, size * aspect)))
    readonly property bool hovered: mouse.containsMouse
    readonly property var peek: Window.window ? Window.window.peek : null
    width: Math.max(frameW, minWidth)
    height: size + (label ? 3 + labelLines * Math.ceil(caption.font.pixelSize * 1.3) : 0)

    Rectangle {
        x: Math.round((tile.width - tile.frameW) / 2)
        width: tile.frameW; height: tile.size; radius: 4; clip: true; color: "#0c1216"
        border.color: tile.selected ? "#87dec7" : tile.hovered ? "#6b8088" : "#34434a"
        border.width: tile.selected ? 2 : 1
        Image {
            anchors.fill: parent; anchors.margins: 2
            source: tile.picture ? tile.picture.url || "" : ""
            fillMode: Image.PreserveAspectCrop; asynchronous: true
            sourceSize: Qt.size(Math.ceil(tile.frameW) * 2, Math.ceil(tile.size) * 2)
            opacity: tile.selected || tile.hovered ? 1 : tile.dimmed
        }
        Rectangle {
            readonly property real d: tile.size >= 72 ? 20 : 14
            visible: tile.order >= 0
            x: 3; y: 3; width: d; height: d; radius: d / 2; color: "#87dec7"
            Text { anchors.centerIn: parent; text: tile.order + 1; color: "#122328"; font.pixelSize: parent.d >= 20 ? 11 : 9; font.bold: parent.d >= 20 }
        }
    }
    Text {
        id: caption
        visible: tile.label !== ""
        y: tile.size + 3; width: tile.width
        horizontalAlignment: tile.width > tile.frameW ? Text.AlignHCenter : Text.AlignLeft
        text: tile.label; color: tile.labelColor
        font.pixelSize: tile.size >= 72 ? 11 : 9
        wrapMode: tile.labelLines > 1 ? Text.Wrap : Text.NoWrap
        maximumLineCount: tile.labelLines; elide: Text.ElideRight
    }
    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: tile.clicked()
        onContainsMouseChanged: {
            if (containsMouse) peekDelay.restart()
            else { peekDelay.stop(); if (tile.peek) tile.peek.hideFor(tile) }
        }
    }
    Timer { id: peekDelay; interval: 350; onTriggered: if (tile.hovered && tile.visible && tile.peek) tile.peek.showFor(tile) }
    // Keep the large view in step when the picture or its place in the order changes under it.
    onDetailChanged: if (peek && peek.tile === tile) peek.showFor(tile)
    onVisibleChanged: if (!visible && peek) peek.hideFor(tile)
    Component.onDestruction: if (peek) peek.hideFor(tile)
}
