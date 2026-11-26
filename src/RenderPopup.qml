import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Render the cut to an MP4: the whole song or the selection, at the takes' own size or a standard
// one, with pictures of another shape filled, fitted or stretched, at a chosen CRF. The render
// runs in the background; closing this leaves it running, and the top bar shows its progress.
Popup {
    id: rp
    objectName: "renderPopup"
    focus: true
    modal: true
    closePolicy: Popup.CloseOnEscape
    padding: 20
    anchors.centerIn: Overlay.overlay
    width: Math.min(600, (Overlay.overlay ? Overlay.overlay.width : 600) - 60)
    height: Math.min(implicitHeight, (Overlay.overlay ? Overlay.overlay.height : 760) - 40)
    background: Rectangle { color: "#1d2930"; radius: 6; border.color: "#34434a" }

    property var selection: null       // the timeline's, when opened
    property string part: "song"       // "song" or "selection"
    property string size: "native"     // "native", "custom" or "0", "1"… into opts.sizes
    property string customW: ""
    property string customH: ""
    property string fit: "fill"
    property string crfText: "19"
    property string path: ""
    property bool pathTouched: false
    signal upscaleCutRequested()
    property var opts: ({native: {width: 0, height: 0}, sizes: [], missing: [], shots: 0, aspect: ""})

    readonly property bool hasSelection: selection !== null && selection !== undefined && selection.end > selection.start
    readonly property string partLabel: part === "selection" && hasSelection ? selection.label : ""
    readonly property int outW: size === "native" ? opts.native.width : size === "custom" ? parseInt(customW) || 0 : (opts.sizes[parseInt(size)] || {width: 0}).width
    readonly property int outH: size === "native" ? opts.native.height : size === "custom" ? parseInt(customH) || 0 : (opts.sizes[parseInt(size)] || {height: 0}).height
    readonly property int crf: /^\d+$/.test(crfText.trim()) ? parseInt(crfText) : -1
    readonly property string why: {
        if (outW < 16 || outH < 16 || outW > 7680 || outH > 4320) return "Choose a size from 16 × 16 to 7680 × 4320."
        if (outW % 2 || outH % 2) return "H.264 needs an even width and height."
        if (crf < 0 || crf > 51) return "CRF runs from 0 to 51."
        if (!path.trim()) return "Choose where to save the video."
        return ""
    }

    function span(ms) {
        const t = ms / 1000
        return t < 60 ? t.toFixed(1) + " s" : Math.floor(t / 60) + ":" + (t % 60).toFixed(1).padStart(4, "0")
    }
    function start(sel) {
        selection = sel
        opts = editor.renderer.options()
        part = hasSelection ? "selection" : "song"
        if (size === "custom" && !customW) size = "native"
        if (size !== "native" && size !== "custom" && parseInt(size) >= opts.sizes.length) size = "native"
        crfText = String(editor.renderer.crf)
        pathTouched = false
        path = editor.renderer.defaultPath(partLabel)
        open()
    }
    // The label is worked out here: partLabel's binding may not have caught up yet.
    onPartChanged: if (!pathTouched && opened) path = editor.renderer.defaultPath(part === "selection" && hasSelection ? selection.label : "")
    function go() {
        if (why || editor.renderer.busy) return
        const s = part === "selection" && hasSelection
        editor.renderer.start({startMs: s ? selection.start : -1, endMs: s ? selection.end : -1, width: outW, height: outH,
                               fit: fit, crf: crf, path: path.trim()})
    }

    component Small: Button {
        id: sb
        property bool active: false
        focusPolicy: Qt.NoFocus
        implicitHeight: 28; leftPadding: 10; rightPadding: 10
        contentItem: Text { text: sb.text; color: !sb.enabled ? "#5c7078" : sb.active ? "#122328" : "#edf1ef"; font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: 4; color: sb.active ? (sb.enabled ? (sb.hovered ? "#9fe3d0" : "#87dec7") : "#2b4a44") : sb.hovered && sb.enabled ? "#3b4b52" : "#2b383f" }
    }
    component Heading: Text { color: "#edf1ef"; font.pixelSize: 13; font.weight: Font.DemiBold }
    component Note: Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#8fa0a4"; font.pixelSize: 11 }
    component Field: TextField {
        id: tf
        implicitHeight: 28
        selectByMouse: true
        color: enabled ? "#edf1ef" : "#6f8085"; font.pixelSize: 13
        leftPadding: 8; rightPadding: 8
        background: Rectangle { radius: 4; color: "#162027"; border.color: tf.activeFocus ? "#87dec7" : "#34434a" }
    }

    FileDialog {
        id: saveAs
        fileMode: FileDialog.SaveFile
        nameFilters: ["MP4 video (*.mp4)"]
        onAccepted: { rp.path = selectedFile.toString().replace(/^file:\/\//, "").replace(/%20/g, " "); rp.pathTouched = true }
    }

    contentItem: ColumnLayout {
        spacing: 14
        ColumnLayout {
            spacing: 2
            Text { text: "Render"; color: "#edf1ef"; font.pixelSize: 17; font.bold: true }
            Note { text: "H.264 video with the song as AAC audio, in an MP4. Takes play silent, as in the viewer; the track switches don't apply." }
        }

        ScrollView {
            id: body
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.preferredHeight: column.implicitHeight
            contentWidth: availableWidth
            clip: true
            enabled: !editor.renderer.busy
            ColumnLayout {
                id: column
                width: body.availableWidth
                spacing: 14

                // What part of the song.
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 6
                    Heading { text: "Part" }
                    Flow {
                        Layout.fillWidth: true; spacing: 6
                        Small { objectName: "renderPartSong"; text: "Whole song · " + rp.span(editor.length / editor.fps * 1000); active: rp.part === "song"; onClicked: rp.part = "song" }
                        Small {
                            objectName: "renderPartSelection"
                            enabled: rp.hasSelection
                            text: rp.hasSelection ? "Selection · " + rp.selection.label + " · " + rp.span(rp.selection.end - rp.selection.start) : "Selection"
                            active: rp.part === "selection"
                            onClicked: rp.part = "selection"
                        }
                    }
                    Note {
                        visible: !rp.hasSelection
                        text: "To render part of the song, close this and select it first: drag across the waveform, click a section or shot, or press I and O."
                    }
                }

                // Size and how pictures of another shape fill it.
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 6
                    Heading { text: "Size" }
                    Flow {
                        Layout.fillWidth: true; spacing: 6
                        Small { objectName: "renderSizeNative"; text: "Native · " + rp.opts.native.width + " × " + rp.opts.native.height; active: rp.size === "native"; onClicked: rp.size = "native" }
                        Repeater {
                            model: rp.opts.sizes
                            Small {
                                required property var modelData
                                required property int index
                                objectName: "renderSize" + modelData.width + "x" + modelData.height
                                text: modelData.label; active: rp.size === String(index); onClicked: rp.size = String(index)
                            }
                        }
                        Small {
                            objectName: "renderSizeCustom"; text: "Custom"; active: rp.size === "custom"
                            onClicked: { if (!rp.customW) { rp.customW = String(rp.outW); rp.customH = String(rp.outH) } rp.size = "custom" }
                        }
                    }
                    RowLayout {
                        visible: rp.size === "custom"
                        spacing: 6
                        Field { objectName: "renderWidth"; Layout.preferredWidth: 80; text: rp.customW; onTextEdited: rp.customW = text; validator: IntValidator { bottom: 0; top: 7680 } }
                        Text { text: "×"; color: "#8fa0a4"; font.pixelSize: 13 }
                        Field { objectName: "renderHeight"; Layout.preferredWidth: 80; text: rp.customH; onTextEdited: rp.customH = text; validator: IntValidator { bottom: 0; top: 4320 } }
                    }
                    Note {
                        text: rp.size === "native" ? "The largest take's own size" + (rp.opts.native.width % 32 === 0 && rp.opts.native.height % 32 === 0 ? " (H3 makes sizes in steps of 32)" : "") + ": those takes aren't scaled at all."
                              : rp.opts.aspect ? "Standard sizes for " + rp.opts.aspect + "." : ""
                    }
                    Flow {
                        Layout.fillWidth: true; spacing: 6; Layout.topMargin: 4
                        Small { objectName: "renderFitFill"; text: "Fill"; active: rp.fit === "fill"; onClicked: rp.fit = "fill" }
                        Small { objectName: "renderFitFit"; text: "Fit"; active: rp.fit === "fit"; onClicked: rp.fit = "fit" }
                        Small { objectName: "renderFitStretch"; text: "Stretch"; active: rp.fit === "stretch"; onClicked: rp.fit = "stretch" }
                    }
                    Note {
                        text: rp.fit === "fill" ? "Pictures of a different shape are scaled to cover the frame and their edges cropped: no bars, nothing squashed. A few pixels go when the shapes nearly match."
                            : rp.fit === "fit" ? "Pictures of a different shape are scaled to fit whole, with black bars where they don't reach. Nothing is lost."
                            : "Pictures of a different shape are scaled to the frame exactly, each axis on its own. Invisible for small differences; big ones look squashed."
                    }
                }

                // Quality.
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 6
                    Heading { text: "Quality" }
                    RowLayout {
                        spacing: 6
                        Small { objectName: "renderCrf16"; text: "Visually lossless · 16"; active: rp.crf === 16; onClicked: rp.crfText = "16" }
                        Small { objectName: "renderCrf19"; text: "High · 19"; active: rp.crf === 19; onClicked: rp.crfText = "19" }
                        Small { objectName: "renderCrf23"; text: "Smaller · 23"; active: rp.crf === 23; onClicked: rp.crfText = "23" }
                        Text { text: "CRF"; color: "#8fa0a4"; font.pixelSize: 12; Layout.leftMargin: 6 }
                        Field { objectName: "renderCrf"; Layout.preferredWidth: 52; text: rp.crfText; onTextEdited: rp.crfText = text; validator: IntValidator { bottom: 0; top: 51 } }
                    }
                    Note { text: "x264's constant rate factor, as in ComfyUI's Video Combine: lower is better and bigger, 0 is lossless." }
                }

                // Shots with no take yet.
                Rectangle {
                    objectName: "renderMissing"
                    visible: rp.opts.missing.length > 0
                    Layout.fillWidth: true
                    implicitHeight: missingText.implicitHeight + 16
                    radius: 5; color: "#2c2a1d"; border.color: "#5a5230"
                    Text {
                        id: missingText
                        x: 8; y: 8; width: parent.width - 16; wrapMode: Text.Wrap; font.pixelSize: 12; color: "#e7d08a"
                        text: (rp.opts.missing.length === 1 ? "Shot " + rp.opts.missing[0] + " has" : rp.opts.missing.length + " shots have") + " no take yet"
                              + (rp.opts.missing.length > 1 && rp.opts.missing.length <= 8 ? " (" + rp.opts.missing.join(", ") + ")" : "")
                              + ". " + (rp.opts.missing.length === 1 ? "It renders" : "They render") + " as a card with the shot's number, who and where, and its purpose."
                    }
                }

                // How many shots render from upscaled versions; the rest are drafts scaled up.
                RowLayout {
                    objectName: "renderUpscaled"
                    visible: (rp.opts.withTakes || 0) > 0
                    Layout.fillWidth: true; spacing: 8
                    Text {
                        Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 12
                        color: rp.opts.upscaled === rp.opts.withTakes ? "#87dec7" : "#adb8ba"
                        text: "Upscaled: " + rp.opts.upscaled + " of " + rp.opts.withTakes + (rp.opts.withTakes === 1 ? " shot" : " shots")
                              + (rp.opts.upscaled < rp.opts.withTakes ? ". The rest render from their drafts, scaled up." : ".")
                    }
                    Small {
                        objectName: "renderUpscaleCut"
                        visible: rp.opts.upscaled < rp.opts.withTakes
                        text: "Upscale the cut…"
                        onClicked: { rp.close(); rp.upscaleCutRequested() }
                    }
                }

                // Where.
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 6
                    Heading { text: "Save as" }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        Field { objectName: "renderPath"; Layout.fillWidth: true; text: rp.path; onTextEdited: { rp.path = text; rp.pathTouched = true } }
                        Small {
                            text: "Choose…"
                            onClicked: { saveAs.selectedFile = "file://" + rp.path; saveAs.open() }
                        }
                    }
                }
            }
        }

        // Progress, or what happened last, or why it can't start: always the same space, so the
        // dialog never grows and moves its buttons when a render starts or finishes.
        ColumnLayout {
            Layout.fillWidth: true; spacing: 4
            Rectangle {
                objectName: "renderProgress"
                opacity: editor.renderer.busy ? 1 : 0
                Layout.fillWidth: true; implicitHeight: 6; radius: 3; color: "#162027"
                Rectangle { width: parent.width * editor.renderer.progress; height: parent.height; radius: 3; color: "#87dec7" }
            }
            Text {
                objectName: "renderMessage"
                Layout.fillWidth: true; Layout.preferredHeight: 2 * Math.ceil(font.pixelSize * 1.35)
                wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight; font.pixelSize: 12
                verticalAlignment: Text.AlignTop
                readonly property bool saved: editor.renderer.file !== "" && editor.renderer.message.startsWith("Saved")
                text: editor.renderer.busy ? editor.renderer.message + "  " + Math.round(editor.renderer.progress * 100) + "%"
                      : rp.why !== "" ? rp.why : editor.renderer.message
                color: editor.renderer.busy ? "#c3cdce" : rp.why === "" && saved ? "#87dec7" : "#e7b75f"
            }
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            Small {
                objectName: "renderReveal"
                visible: !editor.renderer.busy && editor.renderer.file !== "" && editor.renderer.message.startsWith("Saved")
                text: library.revealLabel; onClicked: editor.renderer.reveal()
            }
            Item { Layout.fillWidth: true }
            Small { objectName: "renderCancel"; visible: editor.renderer.busy; text: "Cancel render"; onClicked: editor.renderer.cancel() }
            Small { objectName: "renderClose"; text: "Close"; onClicked: rp.close() }
            // Always here, so Close and Render never move when a render starts or finishes.
            Small {
                objectName: "renderGo"
                enabled: !editor.renderer.busy && rp.why === ""
                active: true
                text: editor.renderer.busy ? "Rendering…" : "Render"
                onClicked: rp.go()
            }
        }
    }
}
