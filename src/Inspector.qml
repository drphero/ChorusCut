import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Details and precise controls for whatever is selected on the timeline.
Rectangle {
    id: ins
    color: "#1d2930"
    radius: 5

    property var selection: null
    property var lines: editor.lyrics.lines
    signal zoomTo(int startMs, int endMs)
    signal nextFlagged()
    signal select(var selection)

    readonly property var firstLine: selection && selection.firstRow !== undefined ? lines[selection.firstRow] : null
    readonly property var lastLine: selection && selection.lastRow !== undefined ? lines[selection.lastRow] : null
    readonly property int flaggedInSelection: {
        if (!firstLine || !lastLine) return 0
        let n = 0
        for (let r = selection.firstRow; r <= selection.lastRow; ++r) n += lines[r].flag ? 1 : 0
        return n
    }
    readonly property var word: {
        if (!selection || selection.kind !== "word" || !firstLine) return null
        for (const w of firstLine.words) if (w.index === selection.word) return w
        return null
    }

    function stamp(ms) {
        const s = ms / 1000, m = Math.floor(s / 60)
        return m + ":" + (s - 60 * m).toFixed(2).padStart(5, "0")
    }

    component Small: Button {
        id: b
        focusPolicy: Qt.NoFocus
        implicitHeight: 28; leftPadding: 10; rightPadding: 10
        property bool active: false
        contentItem: Text { text: b.text; color: b.enabled ? (b.active ? "#122328" : "#edf1ef") : "#748084"; font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: 4; color: b.active ? "#87dec7" : b.hovered ? "#3b4b52" : "#2b383f" }
    }
    component Caption: Text { color: "#adb8ba"; font.pixelSize: 11 }
    component MsField: TextField {
        implicitHeight: 24; implicitWidth: 70
        font.pixelSize: 11; font.family: "Menlo"; color: "#edf1ef"
        topPadding: 2; bottomPadding: 2; leftPadding: 6; rightPadding: 6
        background: Rectangle { radius: 3; color: "#162027"; border.color: parent.activeFocus ? "#87dec7" : "#34434a" }
    }
    component Value: Text { color: "#edf1ef"; font.pixelSize: 13; wrapMode: Text.Wrap }

    ScrollView {
        anchors.fill: parent; anchors.margins: 12
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width
            spacing: 8

            // ---------------------------------------------------------- nothing selected
            ColumnLayout {
                visible: ins.selection === null
                Layout.fillWidth: true
                spacing: 6
                Text { text: editor.projectName || "No project"; color: "#edf1ef"; font.pixelSize: 16; font.bold: true }
                Caption { text: ins.lines.length ? ins.lines.length + " sung lines" : "No aligned lyrics yet" }
                Value {
                    Layout.fillWidth: true
                    visible: ins.lines.length > 0
                    text: editor.lyrics.flaggedLines === 0 ? "Every line agrees across the aligners."
                          : editor.lyrics.flaggedLines + (editor.lyrics.flaggedLines === 1 ? " line needs" : " lines need") + " a listen (amber)."
                    color: editor.lyrics.flaggedLines ? "#e0a24a" : "#87dec7"
                }
                Small { visible: editor.lyrics.flaggedLines > 0; text: "Go to next line to check"; onClicked: ins.nextFlagged() }
                Caption {
                    Layout.fillWidth: true; Layout.topMargin: 10; wrapMode: Text.Wrap; lineHeight: 1.25
                    text: "Click a lyric to select it. Drag it to move; drag an edge to trim. Zoom in (pinch, ⌘-scroll or ⌘=) to edit single words.\n\n"
                          + "Space play · Return play selection · L loop · ↑↓ previous/next line · ⌘Z undo"
                }
            }

            // ---------------------------------------------------------- any selection
            ColumnLayout {
                visible: ins.selection !== null
                Layout.fillWidth: true
                spacing: 6
                Caption {
                    text: !ins.selection ? "" : ({line: "Lyric line", lines: "Lyric lines", word: "Word", region: "Section",
                                                  instrumental: "Instrumental", range: "Range", cutaway: "Cutaway"})[ins.selection.kind] || ""
                }
                Value {
                    Layout.fillWidth: true
                    font.pixelSize: 16; font.bold: true
                    text: ins.selection ? (ins.selection.kind === "line" ? ins.firstLine.text : ins.selection.label) : ""
                }
                Caption {
                    text: ins.selection ? ins.stamp(ins.selection.start) + " → " + ins.stamp(ins.selection.end)
                                          + "   (" + ((ins.selection.end - ins.selection.start) / 1000).toFixed(2) + " s)" : ""
                    font.family: "Menlo"
                }
                RowLayout {
                    Small {
                        objectName: "playSelection"
                        text: editor.playing ? "Stop" : "Play"
                        onClicked: editor.playing ? editor.togglePlay() : editor.playRange(ins.selection.start, ins.selection.end)
                    }
                    Small { text: "Loop"; active: editor.looping; onClicked: editor.looping = !editor.looping }
                    Small { text: "Zoom to"; onClicked: ins.zoomTo(ins.selection.start, ins.selection.end) }
                }
                Value {
                    Layout.fillWidth: true
                    visible: ins.flaggedInSelection > 0
                    color: "#e0a24a"; font.pixelSize: 12
                    text: "The aligners disagree here. Listen, adjust if needed, then mark it checked."
                }
                Small {
                    objectName: "markChecked"
                    visible: ins.flaggedInSelection > 0
                    text: "Mark checked"
                    onClicked: editor.lyrics.markChecked(ins.firstLine.first, ins.lastLine.last)
                }
            }

            // ---------------------------------------------------------- single word
            ColumnLayout {
                visible: ins.word !== null
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: [{label: "Start", key: "start"}, {label: "End", key: "end"}]
                    RowLayout {
                        required property var modelData
                        Caption { text: modelData.label; Layout.preferredWidth: 36 }
                        Small { text: "−10"; onClicked: nudge(-10) }
                        Value { text: ins.word ? ins.stamp(ins.word[parent.modelData.key]) : ""; font.family: "Menlo"; Layout.preferredWidth: 72 }
                        Small { text: "+10"; onClicked: nudge(10) }
                        function nudge(ms) {
                            const w = ins.word
                            if (!w) return
                            if (modelData.key === "start") editor.lyrics.correct(w.index, w.start + ms, w.end)
                            else editor.lyrics.correct(w.index, w.start, w.end + ms)
                        }
                    }
                }
            }

            // ---------------------------------------------------------- words of one line
            ColumnLayout {
                visible: ins.selection !== null && ins.selection.kind === "line"
                Layout.fillWidth: true
                spacing: 2
                Caption { text: "Words (ms)"; Layout.topMargin: 6 }
                Repeater {
                    model: ins.selection && ins.selection.kind === "line" && ins.firstLine ? ins.firstLine.words : []
                    RowLayout {
                        required property var modelData
                        spacing: 4
                        Text {
                            text: parent.modelData.word; color: parent.modelData.flag ? "#e0a24a" : "#d2e2de"
                            font.pixelSize: 12; Layout.preferredWidth: 90; elide: Text.ElideRight
                            MouseArea {
                                anchors.fill: parent
                                onClicked: ins.select({kind: "word", word: parent.parent.modelData.index, firstRow: ins.selection.firstRow,
                                                       lastRow: ins.selection.lastRow, start: parent.parent.modelData.start,
                                                       end: parent.parent.modelData.end, label: parent.parent.modelData.word})
                            }
                        }
                        MsField {
                            text: parent.modelData.start; validator: IntValidator { bottom: 0 }
                            onEditingFinished: if (Number(text) !== parent.modelData.start && !editor.lyrics.correct(parent.modelData.index, Number(text), parent.modelData.end)) text = parent.modelData.start
                        }
                        MsField {
                            text: parent.modelData.end; validator: IntValidator { bottom: 1 }
                            onEditingFinished: if (Number(text) !== parent.modelData.end && !editor.lyrics.correct(parent.modelData.index, parent.modelData.start, Number(text))) text = parent.modelData.end
                        }
                    }
                }
            }

            // ---------------------------------------------------------- cutaway
            ColumnLayout {
                visible: ins.selection !== null && ins.selection.kind === "cutaway"
                Layout.fillWidth: true
                property string edge: "out"
                id: cut
                RowLayout {
                    Repeater {
                        model: ["in", "out", "move", "slip"]
                        Small { required property string modelData; text: modelData; active: cut.edge === modelData; onClicked: cut.edge = modelData }
                    }
                }
                RowLayout {
                    Small { text: "−1 frame"; onClicked: editor.change(cut.edge, -1) }
                    Small { text: "+1 frame"; onClicked: editor.change(cut.edge, 1) }
                }
                Caption { text: "Source in: frame " + editor.sourceIn }
            }
        }
    }
}
