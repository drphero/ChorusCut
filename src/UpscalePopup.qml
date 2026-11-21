import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Upscale a take, or the cut's chosen takes that have no upscaled version yet. Each take is
// re-drawn whole (handles included) with the prompt and pictures it was made with, and the result
// is kept with the take: Render uses it, the viewer when switched to Upscaled.
Popup {
    id: up
    objectName: "upscalePopup"
    focus: true
    modal: true
    closePolicy: Popup.CloseOnEscape
    padding: 20
    anchors.centerIn: Overlay.overlay
    readonly property real roomW: Overlay.overlay ? Overlay.overlay.width : 1200
    readonly property real roomH: Overlay.overlay ? Overlay.overlay.height : 800
    width: Math.min(700, roomW - 40)
    height: Math.min(860, roomH - 40)
    background: Rectangle { color: "#1d2930"; radius: 6; border.color: "#34434a" }

    property bool cut: false            // the cut's takes, or one take
    property int take: 0
    property var cutList: []
    property var picked: ({})           // cut: take id -> ticked
    property string workflow: ""
    property var values: ({})
    property bool fixedSeed: false
    property string seedText: ""
    property string note: ""
    signal workflowsRequested()

    readonly property var chosenTakes: cut ? cutList.filter(t => picked[t.take]).map(t => t.take) : (take ? [take] : [])
    readonly property var opts: (editor.plan.revision, editor.workflows.list, editor.generator.choices,
                                 chosenTakes.length ? editor.generator.upscaleOptions(chosenTakes[0], workflow) : ({pictures: [], workflows: []}))
    readonly property var shotInfo: (editor.plan.revision, opts.shot ? editor.plan.shot(opts.shot) : ({}))
    // The size first: it is the setting that matters most here.
    readonly property var settings: (editor.workflows.list, workflow ? Array.from(editor.workflows.settings(workflow))
                                         .sort((a, b) => (b.label === "Megapixels") - (a.label === "Megapixels")) : [])
    readonly property bool hasSeed: (editor.workflows.list, workflow ? editor.workflows.hasSlot(workflow, "seed") : false)
    readonly property real megapixels: {
        const s = settings.find(x => x.label === "Megapixels")
        return s ? parseFloat(values[s.key]) || 0 : 0
    }
    readonly property real totalSeconds: cut ? cutList.filter(t => picked[t.take]).reduce((a, t) => a + t.seconds, 0) : 0
    readonly property string why: {
        if (cut && !cutList.length) return "Every take in the cut already has an upscaled version."
        if (!chosenTakes.length) return "Tick the takes to upscale."
        if (!cut && opts.why) return opts.why
        if (!opts.workflow) return opts.why || ""
        return ""
    }

    // The size the H3 latent upscaler makes: the scale that gives that many megapixels, rounded
    // to a tenth, then rounded up in steps of 32 (1216 × 672 at 2 MP came out 1952 × 1088).
    function estimate(w, h) {
        if (!w || !h || !megapixels) return ""
        const k = Math.round(Math.sqrt(megapixels * 1e6 / (w * h)) * 10) / 10
        return Math.ceil(w / 32 * k - 1e-6) * 32 + " × " + Math.ceil(h / 32 * k - 1e-6) * 32
    }
    // GPU time, from one measurement: an 8 s take at 2 MP took 16 min on an RTX 4090.
    function gpuMinutes(seconds) { return seconds * 2 * Math.max(0.25, megapixels / 2) }
    function duration(minutes) {
        const m = Math.round(minutes)
        return m < 60 ? m + " min" : Math.floor(m / 60) + " h " + (m % 60 ? m % 60 + " min" : "")
    }
    function start(takeId) { cut = false; take = takeId; begin() }
    function startCut() {
        cut = true
        take = 0
        cutList = Array.from(editor.generator.cutToUpscale())
        const p = {}
        for (const t of cutList) p[t.take] = true
        picked = p
        begin()
    }
    function begin() {
        note = ""
        fixedSeed = false
        seedText = ""
        const first = chosenTakes.length ? chosenTakes[0] : 0
        const o = first ? editor.generator.upscaleOptions(first, "") : ({workflows: []})
        const list = Array.from(o.workflows || [])
        const found = list.find(w => w.id === o.workflow) || list.find(w => w.isDefault) || list[0]
        workflow = found ? found.id : ""
        loadValues()
        open()
    }
    onWorkflowChanged: loadValues()
    function loadValues() {
        const v = {}
        for (const s of editor.workflows.settings(workflow)) v[s.key] = s.value
        values = v
        if (workflow) editor.generator.fetchChoices(workflow)
    }
    function shown(v) { return typeof v === "number" ? String(Math.round(v * 10000) / 10000) : typeof v === "boolean" ? (v ? "on" : "off") : String(v) }
    function setValue(key, value) { const v = Object.assign({}, values); v[key] = value; values = v }
    function randomSeed() { return String(Math.floor(Math.random() * 281474976710656)) }
    function go() {
        const options = {workflow: workflow, settings: values, keep: values}
        if (hasSeed && fixedSeed) options.seed = seedText
        note = editor.generator.upscale(chosenTakes, options)
        if (!note) close()
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
    component Label: Text { color: "#8fa0a4"; font.pixelSize: 12; Layout.preferredWidth: 110; elide: Text.ElideRight }
    component Field: TextField {
        id: tf
        implicitHeight: 28
        selectByMouse: true
        color: enabled ? "#edf1ef" : "#6f8085"; font.pixelSize: 13
        leftPadding: 8; rightPadding: 8
        background: Rectangle { radius: 4; color: "#162027"; border.color: tf.activeFocus ? "#87dec7" : "#34434a" }
    }

    contentItem: ColumnLayout {
        spacing: 10
        ColumnLayout {
            spacing: 2
            Layout.fillWidth: true
            Text { text: up.cut ? "Upscale the cut" : "Upscale"; color: "#8fa0a4"; font.pixelSize: 12 }
            Text {
                objectName: "upscaleTitle"
                text: up.cut ? up.cutList.length + (up.cutList.length === 1 ? " chosen take" : " chosen takes") + " without an upscaled version"
                             : "Shot " + (up.shotInfo.number || "") + " · take " + (up.opts.number || "") + (up.opts.lipSync ? " · sung, the song goes with it" : "")
                color: "#edf1ef"; font.pixelSize: 16; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true
            }
        }
        ScrollView {
            id: body
            Layout.fillWidth: true; Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            Component.onCompleted: if (contentItem.boundsBehavior !== undefined) contentItem.boundsBehavior = Flickable.StopAtBounds
            ColumnLayout {
                width: body.availableWidth - 12
                spacing: 12
                RowLayout {
                    Layout.fillWidth: true; spacing: 10
                    visible: (up.opts.workflows || []).length > 1
                    Label { text: "Workflow" }
                    PlanCombo {
                        objectName: "upscaleWorkflow"
                        allowUnset: false
                        options: (up.opts.workflows || []).map(w => ({value: w.id, label: w.name + (w.isDefault ? "  ·  default" : "")}))
                        value: up.workflow
                        onPicked: v => up.workflow = v
                    }
                }
                // One take: what it comes out at, and what it is re-drawn with.
                ColumnLayout {
                    visible: !up.cut
                    Layout.fillWidth: true; spacing: 8
                    Text {
                        objectName: "upscaleSize"
                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#edf1ef"; font.pixelSize: 13
                        text: (up.opts.width || 0) + " × " + (up.opts.height || 0) + (up.estimate(up.opts.width, up.opts.height) ? "  →  about " + up.estimate(up.opts.width, up.opts.height) : "")
                              + " · " + (up.opts.frames || 0) + " frames, handles included"
                              + (up.megapixels && up.opts.frames ? " · about " + up.duration(up.gpuMinutes(up.opts.frames / 24)) + " on an RTX 4090" : "")
                    }
                    Text {
                        visible: !!up.opts.upscaled
                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#e7b75f"; font.pixelSize: 12
                        text: "This take already has an upscaled version; a new one replaces it (undo brings it back)."
                    }
                    Text { text: "Re-drawn as it was made"; color: "#edf1ef"; font.pixelSize: 12; font.weight: Font.DemiBold }
                    Flow {
                        Layout.fillWidth: true; spacing: 6
                        Repeater {
                            model: up.opts.pictures || []
                            PictureTile {
                                required property var modelData
                                required property int index
                                picture: modelData
                                order: index
                                size: 56; dimmed: 1
                                label: modelData.label
                                detail: "Sent as " + modelData.label + ", as when the take was made."
                            }
                        }
                    }
                    Text {
                        visible: (up.opts.gone || 0) > 0
                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#e7b75f"; font.pixelSize: 11
                        text: up.opts.gone + (up.opts.gone === 1 ? " picture it was made with is gone and" : " pictures it was made with are gone and") + " won't be sent."
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: Math.min(150, promptText.implicitHeight + 16)
                        radius: 4; color: "#162027"; border.color: "#2c3a41"; clip: true
                        Text {
                            id: promptText
                            objectName: "upscalePrompt"
                            x: 8; y: 8; width: parent.width - 16
                            wrapMode: Text.Wrap; color: "#adb8ba"; font.pixelSize: 12
                            text: up.opts.prompt || ""
                        }
                    }
                }
                // The cut: its chosen takes not yet upscaled.
                ColumnLayout {
                    visible: up.cut
                    Layout.fillWidth: true; spacing: 4
                    Repeater {
                        model: up.cutList
                        RowLayout {
                            id: cutRow
                            required property var modelData
                            spacing: 6
                            CheckBox {
                                objectName: "upscaleCutTake_" + cutRow.modelData.take
                                focusPolicy: Qt.NoFocus
                                checked: !!up.picked[cutRow.modelData.take]
                                onToggled: { const p = Object.assign({}, up.picked); p[cutRow.modelData.take] = checked; up.picked = p }
                            }
                            Text {
                                text: "Shot " + cutRow.modelData.shotNumber + " · take " + cutRow.modelData.number + " · " + cutRow.modelData.seconds.toFixed(1) + " s"
                                color: "#edf1ef"; font.pixelSize: 13
                            }
                        }
                    }
                    Text {
                        visible: up.cutList.length > 0
                        color: "#8fa0a4"; font.pixelSize: 12
                        text: up.chosenTakes.length + " of " + up.cutList.length + " · " + up.totalSeconds.toFixed(1) + " s of video in all, handles included"
                              + (up.megapixels ? " · about " + up.duration(up.gpuMinutes(up.totalSeconds)) + " on an RTX 4090" : "")
                    }
                }
                RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    visible: up.workflow !== ""
                    Label { text: "Seed" }
                    Small { objectName: "upscaleSeedRandom"; text: "Random"; active: !up.fixedSeed; enabled: up.hasSeed; onClicked: up.fixedSeed = false }
                    Small { objectName: "upscaleSeedFixed"; text: "Fixed"; active: up.fixedSeed; enabled: up.hasSeed
                            onClicked: { up.fixedSeed = true; if (!up.seedText) up.seedText = up.randomSeed() } }
                    Field {
                        objectName: "upscaleSeed"
                        Layout.fillWidth: true
                        enabled: up.hasSeed && up.fixedSeed
                        font.family: "Menlo"
                        text: up.fixedSeed ? up.seedText : ""
                        placeholderText: up.hasSeed ? (up.fixedSeed ? "0" : "a new one each time") : "no seed input"
                        placeholderTextColor: "#6f8085"
                        validator: RegularExpressionValidator { regularExpression: /[0-9]{0,16}/ }
                        onTextEdited: up.seedText = text
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 6
                    visible: up.workflow !== "" && up.settings.length > 0
                    Text { text: "SETTINGS"; color: "#6f8085"; font.pixelSize: 11; font.letterSpacing: 0.6; Layout.topMargin: 4 }
                    Repeater {
                        model: up.settings
                        RowLayout {
                            id: row
                            required property var modelData
                            readonly property var value: up.values[modelData.key]
                            readonly property var choices: editor.generator.choices[modelData.classType + "/" + modelData.input] || []
                            objectName: "upscaleSetting_" + modelData.node + "_" + modelData.input
                            Layout.fillWidth: true; spacing: 8
                            Label { text: row.modelData.label }
                            Field {
                                objectName: "upscaleSettingField"
                                visible: row.modelData.type !== "switch" && row.choices.length === 0
                                Layout.fillWidth: true
                                text: row.value === undefined ? "" : up.shown(row.value)
                                onTextEdited: up.setValue(row.modelData.key, text)
                            }
                            PlanCombo {
                                visible: row.modelData.type === "text" && row.choices.length > 0
                                allowUnset: false
                                options: {
                                    const list = row.choices.map(c => ({value: c, label: c}))
                                    if (row.value !== undefined && row.choices.indexOf(row.value) < 0) list.unshift({value: row.value, label: row.value})
                                    return list
                                }
                                value: row.value
                                onPicked: v => up.setValue(row.modelData.key, v)
                            }
                            CheckBox {
                                visible: row.modelData.type === "switch"
                                Layout.fillWidth: true
                                checked: row.value === true
                                onToggled: up.setValue(row.modelData.key, checked)
                                focusPolicy: Qt.NoFocus
                            }
                        }
                    }
                }
            }
        }
        Text {
            objectName: "upscaleWhy"
            visible: (up.note || up.why).length > 0
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: up.note || up.why; color: "#e7b75f"; font.pixelSize: 12
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            Text {
                text: "Workflows…"; color: wfMouse.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 12
                MouseArea { id: wfMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: { up.close(); up.workflowsRequested() } }
            }
            // Whether this take will show as it generates (Show takes as they generate, in ComfyUI).
            Text {
                id: liveLine
                objectName: "upscaleLive"
                Layout.fillWidth: true; Layout.leftMargin: 10
                elide: Text.ElideRight; font.pixelSize: 11
                text: editor.comfy.previewsOn ? "Live preview on: click the take on its way to watch it" : editor.comfy.previewsWanted ? "Live preview unavailable (point here for why)" : "Live preview off (ComfyUI settings)"
                color: editor.comfy.previewsOn ? "#87dec7" : "#8fa0a4"
                MouseArea { id: liveMouse; anchors.fill: parent; hoverEnabled: true }
                ToolTip.visible: liveMouse.containsMouse; ToolTip.delay: 300
                ToolTip.text: editor.comfy.previewsOn ? "The take plays on its tile in the shot as it generates; click it to watch it large, and stop it if it's wrong." : editor.comfy.previewText
            }
            Small { objectName: "upscaleCancel"; text: "Cancel"; onClicked: up.close() }
            Small {
                objectName: "upscaleGo"
                active: true
                enabled: up.why === "" && (!up.fixedSeed || !up.hasSeed || up.seedText !== "")
                text: up.chosenTakes.length > 1 ? "Upscale " + up.chosenTakes.length + " takes" : "Upscale"
                onClicked: up.go()
            }
        }
    }
}
