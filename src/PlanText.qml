import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Plan text. One line commits on Return or leaving the field; several lines commit on leaving
// the field or ⌘Return. The shown text follows the plan unless the field is being edited.
Item {
    id: pt
    property var value
    property bool multiline: false
    property string placeholder: ""
    property int lines: 3
    signal commit(string text)
    Layout.fillWidth: true
    implicitHeight: multiline ? Math.max(lines * 18 + 12, area.implicitHeight) : 28
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
        anchors.fill: parent
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
}
