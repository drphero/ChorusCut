import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Generate with options: the kind of picture, which workflow, the seed (random, or a fixed one
// to repeat or vary a picture), how many pictures, and the workflow's adjustable settings (the
// inputs marked "Adjust when generating" in Workflows, such as steps, CFG or the sampler). The
// settings chosen here stay with the workflow for the next time, including Generate's quick
// items. Each picture is its own job and arrives waiting for approval.
Popup {
    id: gp
    objectName: "generatePopup"
    focus: true
    modal: true
    padding: 20
    anchors.centerIn: Overlay.overlay
    width: Math.min(560, (Overlay.overlay ? Overlay.overlay.width : 560) - 60)
    height: Math.min(implicitHeight, (Overlay.overlay ? Overlay.overlay.height : 720) - 60)
    background: Rectangle { color: "#1d2930"; radius: 6; border.color: "#34434a" }

    property string owner
    property int ownerId
    property string use: "ref-text"
    property string workflow: ""
    property bool fixedSeed: false
    property string seedText: ""
    property int count: 1
    property var values: ({})          // {key: value} for this workflow's settings
    property var prefillSettings: null
    property string note: ""

    readonly property var opts: (editor.generator.jobs, editor.workflows.list, editor.plan.revision,
                                 owner ? editor.generator.options(owner, ownerId) : ({text: {workflows: []}, edit: {workflows: []}, prompt: ""}))
    readonly property var kind: use === "ref-edit" ? opts.edit : opts.text
    readonly property var flowsForUse: kind && kind.workflows ? kind.workflows : []
    readonly property string why: (editor.workflows.list, editor.plan.revision, editor.plan.references, owner ? editor.generator.whyNot(owner, ownerId, use, workflow) : "")
    readonly property var settings: (editor.workflows.list, workflow ? editor.workflows.settings(workflow) : [])
    readonly property bool hasSeed: (editor.workflows.list, workflow ? editor.workflows.hasSlot(workflow, "seed") : false)
    property string ownerName
    signal workflowsRequested()

    // prefill: {use, workflow, seed, settings: {key: value}}, as Generate again… passes them.
    function start(owner, ownerId, prefill, name) {
        gp.owner = owner; gp.ownerId = ownerId; gp.ownerName = name
        const p = prefill || {}
        const o = editor.generator.options(owner, ownerId)
        use = p.use ? p.use : (!o.text.ready && o.edit.ready ? "ref-edit" : "ref-text")
        prefillSettings = p.settings || null
        fixedSeed = p.seed !== undefined && p.seed !== null
        seedText = fixedSeed ? String(p.seed) : ""
        count = 1
        note = ""
        chooseWorkflow(p.workflow)
        open()
    }
    function chooseWorkflow(wanted) {
        const list = (use === "ref-edit" ? editor.generator.options(owner, ownerId).edit : editor.generator.options(owner, ownerId).text).workflows
        const found = list.find(w => w.id === wanted) || list.find(w => w.isDefault) || list[0]
        const id = found ? found.id : ""
        if (id === workflow) loadValues(); else workflow = id
    }
    onUseChanged: if (opened) { prefillSettings = null; chooseWorkflow(workflow) }
    onWorkflowChanged: loadValues()
    function loadValues() {
        const v = {}
        for (const s of editor.workflows.settings(workflow))
            v[s.key] = prefillSettings && prefillSettings[s.key] !== undefined ? prefillSettings[s.key] : s.value
        values = v
        if (workflow) editor.generator.fetchChoices(workflow)
    }
    function setValue(key, value) { const v = Object.assign({}, values); v[key] = value; values = v }
    function randomSeed() { return String(Math.floor(Math.random() * 281474976710656)) }
    function shown(v) { return typeof v === "number" ? String(Math.round(v * 10000) / 10000) : typeof v === "boolean" ? (v ? "on" : "off") : String(v) }
    function go() {
        const options = {workflow: workflow, count: count, settings: values}
        if (hasSeed && fixedSeed) options.seed = seedText
        note = editor.generator.generateWith(owner, ownerId, use, options)
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
        spacing: 12
        ColumnLayout {
            spacing: 2
            Text { text: "Generate a reference picture"; color: "#8fa0a4"; font.pixelSize: 12 }
            Text { text: gp.ownerName; color: "#edf1ef"; font.pixelSize: 16; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
        }
        // The kind of picture.
        RowLayout {
            spacing: 6
            Small { objectName: "genKindText"; text: "From the picture prompt"; active: gp.use === "ref-text"; onClicked: gp.use = "ref-text" }
            Small { objectName: "genKindEdit"; text: "From approved pictures"; active: gp.use === "ref-edit"; onClicked: gp.use = "ref-edit" }
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 10
            visible: gp.flowsForUse.length > 0
            Label { text: "Workflow" }
            PlanCombo {
                objectName: "genWorkflow"
                allowUnset: false
                options: gp.flowsForUse.map(w => ({value: w.id, label: w.name + (w.isDefault ? "  ·  default" : "")}))
                value: gp.workflow
                onPicked: v => gp.workflow = v
            }
        }
        Text {
            Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideRight
            visible: gp.opts.prompt.length > 0
            text: gp.opts.prompt; color: "#adb8ba"; font.pixelSize: 12; font.italic: true
        }
        // The seed.
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            visible: gp.workflow !== ""
            Label { text: "Seed" }
            Small { objectName: "genSeedRandom"; text: "Random"; active: !gp.fixedSeed; enabled: gp.hasSeed; onClicked: gp.fixedSeed = false }
            Small { objectName: "genSeedFixed"; text: "Fixed"; active: gp.fixedSeed; enabled: gp.hasSeed
                    onClicked: { gp.fixedSeed = true; if (!gp.seedText) gp.seedText = gp.randomSeed() } }
            Field {
                id: seedField
                objectName: "genSeed"
                Layout.fillWidth: true
                enabled: gp.hasSeed && gp.fixedSeed
                font.family: "Menlo"
                text: gp.fixedSeed ? gp.seedText : ""
                placeholderText: gp.hasSeed ? (gp.fixedSeed ? "0" : "a new one each time") : "no seed input"
                placeholderTextColor: "#6f8085"
                validator: RegularExpressionValidator { regularExpression: /[0-9]{0,16}/ }
                onTextEdited: gp.seedText = text
            }
            Small { objectName: "genSeedDice"; text: "🎲"; enabled: gp.hasSeed; onClicked: { gp.fixedSeed = true; gp.seedText = gp.randomSeed() } }
        }
        Text {
            visible: gp.workflow !== "" && !gp.hasSeed
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: "This workflow has no input mapped to the seed. Map one in Workflows to choose it here."
            color: "#6f8085"; font.pixelSize: 11
        }
        // How many.
        RowLayout {
            spacing: 8
            visible: gp.workflow !== ""
            Label { text: "Pictures" }
            Small { objectName: "genFewer"; text: "−"; enabled: gp.count > 1; onClicked: gp.count-- }
            Text { objectName: "genCount"; text: gp.count; color: "#edf1ef"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter; Layout.preferredWidth: 22 }
            Small { objectName: "genMore"; text: "+"; enabled: gp.count < 8; onClicked: gp.count++ }
            Text {
                visible: gp.count > 1
                text: gp.hasSeed && gp.fixedSeed && gp.seedText ? "seeds " + gp.seedText + " to " + (Number(gp.seedText) + gp.count - 1) : "a new seed for each"
                color: "#6f8085"; font.pixelSize: 11
            }
        }
        // The workflow's adjustable settings.
        ColumnLayout {
            Layout.fillWidth: true; spacing: 6
            visible: gp.workflow !== ""
            Text { text: "SETTINGS"; color: "#6f8085"; font.pixelSize: 11; font.letterSpacing: 0.6; Layout.topMargin: 4 }
            Repeater {
                model: gp.settings
                RowLayout {
                    id: row
                    required property var modelData
                    readonly property var value: gp.values[modelData.key]
                    readonly property var choices: editor.generator.choices[modelData.classType + "/" + modelData.input] || []
                    readonly property bool changed: value !== undefined && gp.shown(value) !== gp.shown(modelData.original)
                    objectName: "genSetting_" + modelData.node + "_" + modelData.input
                    Layout.fillWidth: true; spacing: 8
                    Label { text: row.modelData.label }
                    Field {
                        objectName: "genSettingField"
                        visible: row.modelData.type !== "switch" && row.choices.length === 0
                        Layout.fillWidth: true
                        text: row.value === undefined ? "" : gp.shown(row.value)
                        validator: row.modelData.type === "number" ? numberCheck : null
                        // Numbers are kept as typed ("0." while typing 0.5); Chorus Cut reads them when generating.
                        onTextEdited: gp.setValue(row.modelData.key, text)
                        RegularExpressionValidator { id: numberCheck; regularExpression: /-?[0-9]*\.?[0-9]*/ }
                    }
                    PlanCombo {
                        objectName: "genSettingChoice"
                        visible: row.modelData.type === "text" && row.choices.length > 0
                        allowUnset: false
                        options: {
                            const list = row.choices.map(c => ({value: c, label: c}))
                            if (row.value !== undefined && row.choices.indexOf(row.value) < 0) list.unshift({value: row.value, label: row.value})
                            return list
                        }
                        value: row.value
                        onPicked: v => gp.setValue(row.modelData.key, v)
                    }
                    CheckBox {
                        id: box
                        objectName: "genSettingSwitch"
                        visible: row.modelData.type === "switch"
                        Layout.fillWidth: true
                        checked: row.value === true
                        onToggled: gp.setValue(row.modelData.key, checked)
                        focusPolicy: Qt.NoFocus
                        indicator: Rectangle {
                            x: 0; anchors.verticalCenter: parent.verticalCenter
                            width: 16; height: 16; radius: 3
                            color: box.checked ? "#87dec7" : "#162027"; border.color: box.checked ? "#87dec7" : "#5c7078"
                            Text { anchors.centerIn: parent; visible: box.checked; text: "✓"; color: "#122328"; font.pixelSize: 12 }
                        }
                    }
                    Text {
                        objectName: "genSettingReset"
                        visible: row.changed
                        text: "Reset"; color: resetMouse.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 11
                        MouseArea { id: resetMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                    onClicked: gp.setValue(row.modelData.key, row.modelData.original) }
                        ToolTip.visible: resetMouse.containsMouse; ToolTip.delay: 400
                        ToolTip.text: "The workflow's own value: " + gp.shown(row.modelData.original)
                    }
                }
            }
            Text {
                Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#6f8085"; font.pixelSize: 11
                text: (gp.settings.length === 0 ? "This workflow has no adjustable settings yet. " : "")
                      + "To adjust another of its inputs here, open Workflows and set it to “Adjust when generating”. What you choose stays with the workflow."
            }
        }
        Text {
            objectName: "genWhy"
            visible: (gp.note || gp.why).length > 0
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: gp.note || gp.why; color: "#e7b75f"; font.pixelSize: 12
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            Text {
                objectName: "genOpenWorkflows"
                text: "Workflows…"; color: wfMouse.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 12
                MouseArea { id: wfMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: { gp.close(); gp.workflowsRequested() } }
            }
            Item { Layout.fillWidth: true }
            Small { text: "Cancel"; onClicked: gp.close() }
            Small {
                objectName: "genGo"
                active: true
                enabled: gp.why === "" && (!gp.fixedSeed || !gp.hasSeed || gp.seedText !== "")
                text: gp.count > 1 ? "Generate " + gp.count : "Generate"
                onClicked: gp.go()
            }
        }
    }
}
