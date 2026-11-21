import QtQuick
import QtQuick.Controls

// The large view of a picture the pointer rests on in a picker (PictureTile): uncropped, with
// whose it is and which view. It sits in the window's overlay, above any open dialog, only while
// shown, and takes no mouse input, so it never gets in the way of the next click.
Item {
    id: peek
    objectName: "picturePeek"
    property Item tile: null
    property var picture: ({})
    property string detail: ""
    readonly property bool shown: tile !== null
    // The window's overlay, set by Main: while hidden it has no parent, so it can't find it itself.
    property Item room: null
    parent: shown ? room : null
    visible: shown
    z: 100000

    readonly property real aspect: picture && picture.width > 0 && picture.height > 0 ? picture.width / picture.height : 1
    readonly property real longSide: room ? Math.max(160, Math.min(360, Math.min(room.width, room.height) * 0.5)) : 360
    readonly property real imageW: Math.round(aspect >= 1 ? longSide : longSide * aspect)
    readonly property real imageH: Math.round(aspect >= 1 ? longSide / aspect : longSide)
    width: Math.max(imageW, 200) + 16
    height: content.implicitHeight + 16

    Rectangle { anchors.fill: parent; radius: 6; color: "#10181c"; border.color: "#46585f" }
    Column {
        id: content
        x: 8; y: 8; width: peek.width - 16
        spacing: 4
        Item {
            width: parent.width; height: peek.imageH
            Image {
                objectName: "picturePeekImage"
                anchors.centerIn: parent
                width: peek.imageW; height: peek.imageH
                source: peek.picture.url || ""
                fillMode: Image.PreserveAspectFit; asynchronous: true
                sourceSize: Qt.size(peek.imageW * 2, peek.imageH * 2)
            }
        }
        Item { width: 1; height: 2 }
        Text {
            objectName: "picturePeekTitle"
            width: parent.width; wrapMode: Text.Wrap
            text: (peek.picture.whose || "") + (peek.picture.viewName ? " · " + peek.picture.viewName : "")
            color: "#edf1ef"; font.pixelSize: 13; font.weight: Font.DemiBold
        }
        Text {
            width: parent.width; wrapMode: Text.Wrap
            text: (peek.picture.approved ? "Chosen picture" : "Alternative") + (peek.picture.width > 0 ? "  ·  " + peek.picture.width + " × " + peek.picture.height : "")
            color: peek.picture.approved ? "#87dec7" : "#8fa0a4"; font.pixelSize: 11
        }
        Text {
            objectName: "picturePeekDetail"
            visible: peek.detail !== ""
            width: parent.width; wrapMode: Text.Wrap
            text: peek.detail; color: "#acb9ba"; font.pixelSize: 11
        }
    }

    function showFor(item) {
        picture = item.picture || {}
        detail = item.detail
        tile = item
        place()
    }
    function hideFor(item) { if (tile === item) tile = null }
    // Beside the tile, to the right when there is room, else to the left; kept inside the window.
    function place() {
        if (!tile || !room) return
        const p = tile.mapToItem(room, 0, 0)
        let px = p.x + tile.width + 12
        if (px + width > room.width - 8) px = p.x - width - 12
        x = Math.max(8, px)
        y = Math.max(8, Math.min(room.height - height - 8, p.y + tile.size / 2 - height / 2))
    }
    onHeightChanged: place()
}
