import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Plan text. One line commits on Return or leaving the field; several lines commit on leaving
// the field or ⌘Return. The shown text follows the plan unless the field is being edited.
// With collapsedLines set, long text shows that many lines, fading out, and "Show all" until the
// field is clicked into; it opens to its full length while being edited.
Item {
    id: pt
    property var value
    property bool multiline: false
    property string placeholder: ""
    property int lines: 3
    property int collapsedLines: 0
    signal commit(string text)
    Layout.fillWidth: true
    readonly property real fullHeight: Math.max(lines * 18 + 12, area.implicitHeight)
    readonly property real collapsedHeight: Math.max(lines, collapsedLines) * 18 + 12
    readonly property bool collapsed: multiline && collapsedLines > 0 && !area.activeFocus && area.implicitHeight > collapsedHeight + 4
    readonly property int words: { const t = shown.trim(); return t ? t.split(/\s+/).length : 0 }
    implicitHeight: multiline ? (collapsed ? collapsedHeight + more.implicitHeight + 2 : fullHeight) : 28
    readonly property string shown: value === undefined || value === null ? "" : String(value)
    readonly property Item input: multiline ? area : line
    function sync() { if (!input.activeFocus) input.text = shown }
    onShownChanged: sync()
    Component.onCompleted: { line.text = shown; area.text = shown }
    function done() { if (input.text !== shown) commit(input.text) }

    TextField {
        id: line
        visible: !pt.multiline
        anchors.fill: parent
        selectByMouse: true
        color: "#edf1ef"; font.pixelSize: 13
        placeholderText: pt.placeholder; placeholderTextColor: "#6f8085"
        leftPadding: 8; rightPadding: 8
        background: Rectangle { radius: 4; color: "#162027"; border.color: line.activeFocus ? "#87dec7" : "#34434a" }
        onEditingFinished: pt.done()
        Keys.onEscapePressed: { text = pt.shown; focus = false }
    }
    TextArea {
        id: area
        visible: pt.multiline
        width: parent.width
        height: pt.collapsed ? pt.collapsedHeight : parent.height
        clip: pt.collapsed
        selectByMouse: true
        wrapMode: TextEdit.Wrap
        color: "#edf1ef"; font.pixelSize: 13
        placeholderText: pt.placeholder; placeholderTextColor: "#6f8085"
        leftPadding: 8; rightPadding: 8; topPadding: 6; bottomPadding: 6
        background: Rectangle { radius: 4; color: "#162027"; border.color: area.activeFocus ? "#87dec7" : "#34434a" }
        onActiveFocusChanged: if (!activeFocus) pt.done()
        Keys.onPressed: e => {
            if ((e.key === Qt.Key_Return || e.key === Qt.Key_Enter) && (e.modifiers & Qt.ControlModifier)) { pt.done(); e.accepted = true }
            if (e.key === Qt.Key_Escape) { text = pt.shown; focus = false; e.accepted = true }
        }
    }
    // The end of a folded field fades out; the line below opens it.
    Rectangle {
        visible: pt.collapsed
        x: 1; width: area.width - 2; y: pt.collapsedHeight - 25; height: 24
        gradient: Gradient { GradientStop { position: 0; color: "#00162027" } GradientStop { position: 1; color: "#162027" } }
    }
    Text {
        id: more
        objectName: "showAll"
        visible: pt.collapsed
        y: pt.collapsedHeight + 2
        text: "Show all"
        color: moreMouse.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 11
        MouseArea {
            id: moreMouse
            anchors.fill: parent; anchors.margins: -3
            hoverEnabled: true; cursorShape: Qt.PointingHandCursor
            onClicked: { area.forceActiveFocus(); area.cursorPosition = area.length }
        }
    }
}
