import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A choice from the plan's options, with "—" for not set.
ComboBox {
    id: pc
    property var options: []                  // [{value, label}]
    property var value
    property string unsetLabel: "—"
    signal picked(var value)
    Layout.fillWidth: true
    implicitHeight: 28
    focusPolicy: Qt.StrongFocus
    readonly property var entries: [{value: "", label: unsetLabel}].concat(options)
    model: entries
    textRole: "label"
    valueRole: "value"
    currentIndex: {
        const v = value === undefined || value === null ? "" : value
        for (let i = 0; i < entries.length; ++i) if (entries[i].value === v) return i
        return 0
    }
    onActivated: i => picked(entries[i].value)
    contentItem: Text {
        leftPadding: 8; rightPadding: 20
        text: pc.displayText
        color: pc.currentIndex === 0 ? "#6f8085" : "#edf1ef"
        font.pixelSize: 13; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
    }
    background: Rectangle { radius: 4; color: pc.hovered ? "#22313a" : "#162027"; border.color: pc.activeFocus ? "#87dec7" : "#34434a" }
    indicator: Text { x: pc.width - width - 8; anchors.verticalCenter: parent.verticalCenter; text: "▾"; color: "#adb8ba" }
    delegate: ItemDelegate {
        required property var modelData
        required property int index
        width: pc.width
        highlighted: pc.highlightedIndex === index
        contentItem: Text { text: modelData.label; color: index === 0 ? "#8fa0a4" : "#edf1ef"; font.pixelSize: 13; elide: Text.ElideRight }
        background: Rectangle { color: highlighted ? "#2f4a52" : "#1d2930" }
    }
    popup.background: Rectangle { color: "#1d2930"; border.color: "#34434a"; radius: 4 }
}
