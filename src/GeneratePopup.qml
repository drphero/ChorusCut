import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Generate a view's pictures with options: from the prompt or from pictures the user picks (in
// order, up to what the workflow takes; the workflow decides the limit, not the model), the
// instruction (the view's framing, editable for this time or saved as the view's), which
// workflow, the seed (random, or fixed to repeat or vary a picture), how many pictures, and the
// workflow's adjustable settings (inputs marked "Adjust when generating" in Workflows). Settings
// stay with the workflow, picked pictures with the view. Each picture is its own job and arrives
// waiting to be chosen. Save keeps the picks and the instruction on the view without generating;
// Ask saves them too, so the agent sees what was picked, and asks it to write the instruction.
// Settings changed here stay with the workflow whichever way it is left except Cancel. An aspect
// ratio nobody chose shows the automatic one (3:4 for people, the video's for places) and is not
// kept, so the next kind of picture gets its own.
// Clicking outside doesn't close it, so picks are never dropped by a stray click.
Popup {
    id: gp
    objectName: "generatePopup"
    focus: true
    modal: true
    closePolicy: Popup.CloseOnEscape
    padding: 20
    anchors.centerIn: Overlay.overlay
    width: Math.min(620, (Overlay.overlay ? Overlay.overlay.width : 620) - 60)
    height: Math.min(implicitHeight, (Overlay.overlay ? Overlay.overlay.height : 720) - 40)
    background: Rectangle { color: "#1d2930"; radius: 6; border.color: "#34434a" }

    property string owner
    property int ownerId
    property string use: "ref-text"
    property string workflow: ""
    property bool fixedSeed: false
    property string seedText: ""
    property int count: 1
    property var values: ({})          // {key: value} for this workflow's settings
    property var touched: ({})         // keys the user changed here
    property var prefillSettings: null
    property string note: ""
    property int view: 0
    property var inputs: []            // reference ids to edit from, in order
    property string instruction: ""
    property bool saveFraming: false
    property bool allPictures: false

    readonly property var opts: (editor.generator.jobs, editor.workflows.list, editor.plan.revision,
                                 owner ? editor.generator.options(owner, ownerId) : ({text: {workflows: []}, edit: {workflows: []}, prompt: ""}))
    readonly property var vopts: (editor.generator.jobs, editor.generator.choices, editor.workflows.list, editor.plan.revision, editor.plan.references,
                                  view ? editor.generator.viewOptions(view, use, workflow) : ({workflows: [], pictures: {min: 0, max: 0, known: false}, subject: ""}))
    readonly property var flowsForUse: vopts.workflows || []
    readonly property bool editing: use === "ref-edit"
    readonly property int most: vopts.pictures ? vopts.pictures.max : 0
    readonly property int least: vopts.pictures && vopts.pictures.known ? vopts.pictures.min : 0
    readonly property string why: {
        const base = (editor.workflows.list, editor.plan.revision, owner ? editor.generator.whyNot(owner, ownerId, use, workflow) : "")
        if (base) return base
        if (!editing) return !instruction.trim() && !vopts.subject ? "Write a picture prompt or an instruction first." : ""
        if (!inputs.length) return "Pick the pictures to make it from."
        if (inputs.length > most) return "This workflow takes at most " + most + (most === 1 ? " picture." : " pictures.")
        if (inputs.length < least) return "This workflow needs at least " + least + (least === 1 ? " picture." : " pictures.")
        return ""
    }
    function label(n) { return workflow ? editor.generator.pictureLabel(workflow, n) : "picture " + n }
    // Pictures to pick from: this item's views first, then what it belongs with, then (on
    // request) everything else in the plan. [{title, pictures}]
    readonly property var groups: {
        editor.plan.references
        const plan = editor.plan
        const out = []
        const add = (title, kind, id) => {
            const pics = plan.referencesFor(kind, id).filter(r => !r.missing)
            if (pics.length) out.push({title: title, pictures: pics})
        }
        const nameOf = (kind, id) => { const it = plan.item(kind, id); return it.fields ? (it.fields.name || kind) : kind }
        if (!owner) return out
        const seen = {}
        const mark = (kind, id) => { seen[kind + id] = true }
        add("This " + owner, owner, ownerId); mark(owner, ownerId)
        if (owner === "look") {
            const c = plan.item("look", ownerId).character
            add(nameOf("character", c), "character", c); mark("character", c)
            for (const l of plan.looks) if (l.character === c && l.id !== ownerId) { add(nameOf("character", c) + " · " + (l.fields.name || "Look"), "look", l.id); mark("look", l.id) }
        } else if (owner === "character") {
            for (const l of plan.looks) if (l.character === ownerId) { add(nameOf("character", ownerId) + " · " + (l.fields.name || "Look"), "look", l.id); mark("look", l.id) }
        }
        if (allPictures) {
            for (const c of plan.characters) if (!seen["character" + c.id]) add(c.fields.name || "Character", "character", c.id)
            for (const l of plan.looks) if (!seen["look" + l.id]) add(nameOf("character", l.character) + " · " + (l.fields.name || "Look"), "look", l.id)
            for (const l of plan.locations) if (!seen["location" + l.id]) add(l.fields.name || "Location", "location", l.id)
            for (const p of plan.props) if (!seen["prop" + p.id]) add(p.fields.name || "Prop", "prop", p.id)
        }
        return out
    }
    function togglePick(id) {
        const list = inputs.slice()
        const i = list.indexOf(id)
        if (i >= 0) list.splice(i, 1)
        else if (list.length < most) list.push(id)
        else { note = "This workflow takes " + most + (most === 1 ? " picture" : " pictures") + ". Click one to take it out first."; return }
        note = ""
        inputs = list
    }
    readonly property var settings: (editor.workflows.list, workflow ? editor.workflows.settings(workflow) : [])
    readonly property bool hasSeed: (editor.workflows.list, workflow ? editor.workflows.hasSlot(workflow, "seed") : false)
    property string ownerName
    signal workflowsRequested()
    signal askRequested(int view, string fieldLabel)

    // prefill: {view, use, workflow, seed, settings: {key: value}, instruction, inputs}, as the
    // strip and Generate again… pass them.
    function start(owner, ownerId, prefill, name) {
        gp.owner = owner; gp.ownerId = ownerId; gp.ownerName = name
        const p = prefill || {}
        const views = editor.plan.viewsFor(owner, ownerId)
        view = p.view ? p.view : (views.length ? views[0].id : 0)
        const v = views.find(x => x.id === view) || {fields: {}, from: "prompt"}
        if (view) gp.ownerName = name + " · " + v.name
        use = p.use ? p.use : (v.from === "pictures" ? "ref-edit" : "ref-text")
        const vo = view ? editor.generator.viewOptions(view, use, p.workflow || "") : {inputs: []}
        inputs = p.inputs && p.inputs.length ? Array.from(p.inputs) : Array.from(vo.inputs || [])
        instruction = p.instruction !== undefined && p.instruction !== null ? p.instruction : (v.fields.framing || "")
        saveFraming = false
        allPictures = false
        prefillSettings = p.settings || null
        fixedSeed = p.seed !== undefined && p.seed !== null
        seedText = fixedSeed ? String(p.seed) : ""
        count = 1
        note = ""
        chooseWorkflow(p.workflow)
        open()
    }
    function chooseWorkflow(wanted) {
        const list = view ? editor.generator.viewOptions(view, use, "").workflows
                          : (use === "ref-edit" ? editor.generator.options(owner, ownerId).edit : editor.generator.options(owner, ownerId).text).workflows
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
        touched = {}
        applyAutoAspect()
        if (workflow) editor.generator.fetchChoices(workflow)
    }
    // An aspect ratio nobody chose shows what will be used: 3:4 for people, the video's for places.
    function isAspect(s) { return s.input === "aspect_ratio" }
    function aspectIsAuto(s) {
        return isAspect(s) && !touched[s.key] && gp.shown(s.value) === gp.shown(s.original) && !(prefillSettings && prefillSettings[s.key] !== undefined)
    }
    function applyAutoAspect() {
        if (!workflow || !owner) return
        let v = null
        for (const s of editor.workflows.settings(workflow)) {
            if (!aspectIsAuto(s)) continue
            const auto = editor.generator.autoAspect(owner, workflow, s.key)
            if (auto && values[s.key] !== auto) { v = v || Object.assign({}, values); v[s.key] = auto }
        }
        if (v) values = v
    }
    Connections { target: editor.generator; function onChoicesChanged() { if (gp.opened) gp.applyAutoAspect() } }
    function setValue(key, value) {
        const v = Object.assign({}, values); v[key] = value; values = v
        const t = Object.assign({}, touched); t[key] = true; touched = t
    }
    // What stays with the workflow: every setting, except an aspect ratio left to choose itself.
    function keptValues() {
        const out = {}
        for (const s of editor.workflows.settings(workflow))
            if (values[s.key] !== undefined && (!isAspect(s) || touched[s.key])) out[s.key] = values[s.key]
        return out
    }
    readonly property bool aspectChosen: {
        for (const s of settings) if (isAspect(s) && (touched[s.key] || (prefillSettings && prefillSettings[s.key] !== undefined))) return true
        return false
    }
    function randomSeed() { return String(Math.floor(Math.random() * 281474976710656)) }
    function shown(v) { return typeof v === "number" ? String(Math.round(v * 10000) / 10000) : typeof v === "boolean" ? (v ? "on" : "off") : String(v) }
    function go() {
        const options = {workflow: workflow, count: count, settings: values, keep: keptValues(), aspectChosen: aspectChosen, view: view, instruction: instruction}
        if (editing) options.inputs = inputs
        if (hasSeed && fixedSeed) options.seed = seedText
        note = editor.generator.generateWith(owner, ownerId, use, options)
        if (note) return
        // The picked pictures stay with the view; the instruction too when asked.
        keep(saveFraming)
        close()
    }
    // Writes the picks (and, with the instruction, how the view is made) to the view as one undo step.
    function keep(withInstruction) {
        if (workflow) {
            const why = editor.generator.keepSettings(workflow, keptValues())
            if (why) { note = why; return false }
        }
        if (!view) return true
        const v = editor.plan.item("view", view).fields || {}
        const fields = {}
        if (editing && JSON.stringify(v.inputs || []) !== JSON.stringify(inputs)) fields.inputs = inputs
        if (withInstruction && (v.framing || "") !== instruction) fields.framing = instruction
        if (withInstruction && (v.from || "prompt") !== (editing ? "pictures" : "prompt")) fields.from = editing ? "pictures" : "prompt"
        if (Object.keys(fields).length) editor.plan.run({op: "set", kind: "view", id: view, fields: fields})
        return true
    }
    function save() { if (keep(true)) close() }
    function askAgent() {
        const id = view, label = editing ? "Instruction" : "Framing"
        if (!keep(true)) return
        close()
        askRequested(id, label)
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
        // Everything but the reason and the buttons scrolls when the window is short.
        ScrollView {
            id: body
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.preferredHeight: bodyColumn.implicitHeight
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                id: bodyColumn
                width: body.availableWidth
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
                    Small { objectName: "genKindEdit"; text: "From pictures you pick"; active: gp.use === "ref-edit"; onClicked: gp.use = "ref-edit" }
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
                // Made from: exactly the pictures picked, in the order clicked.
                ColumnLayout {
                    visible: gp.editing && gp.workflow !== ""
                    Layout.fillWidth: true; spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: "Made from"; color: "#edf1ef"; font.pixelSize: 13; font.weight: Font.DemiBold; Layout.fillWidth: true }
                        Text {
                            objectName: "genPickCount"
                            text: gp.inputs.length + " of " + gp.most + (gp.most === 1 ? " picture" : " pictures") + (gp.least > 1 ? " · at least " + gp.least : "")
                            color: gp.inputs.length > gp.most || gp.inputs.length < gp.least ? "#e7b75f" : "#87dec7"; font.pixelSize: 12
                        }
                    }
                    Text {
                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#8fa0a4"; font.pixelSize: 11
                        text: "Click pictures in order. The instruction can name them as " + gp.label(1) + (gp.most > 1 ? ", " + gp.label(2) : "") + "…"
                    }
                    // What it is made from, in order, as the instruction names them.
                    Rectangle {
                        objectName: "genSending"
                        visible: gp.inputs.length > 0
                        Layout.fillWidth: true
                        implicitHeight: sendingFlow.implicitHeight + 16
                        radius: 5; color: "#162027"; border.color: "#2c3a41"
                        Flow {
                            id: sendingFlow
                            x: 8; y: 8; width: parent.width - 16
                            spacing: 10
                            Repeater {
                                model: (editor.plan.revision, gp.inputs.map(id => editor.plan.item("reference", id)).filter(r => r.id !== undefined))
                                PictureTile {
                                    required property var modelData
                                    required property int index
                                    objectName: "genSending_" + modelData.id
                                    picture: modelData
                                    order: index
                                    size: 72; labelLines: 2; minWidth: 104
                                    label: gp.label(index + 1) + "\n" + (modelData.whose || "") + " · " + modelData.viewName
                                    detail: "Made from it as " + gp.label(index + 1) + ". Click to take it out."
                                    onClicked: gp.togglePick(modelData.id)
                                }
                            }
                        }
                    }
                    ScrollView {
                        id: pickScroll
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.min(pickColumn.implicitHeight, 300)
                        contentWidth: availableWidth
                        clip: true
                        ColumnLayout {
                            id: pickColumn
                            width: pickScroll.availableWidth
                            spacing: 4
                            Repeater {
                                model: gp.groups
                                ColumnLayout {
                                    id: group
                                    required property var modelData
                                    Layout.fillWidth: true; spacing: 2
                                    Text { text: group.modelData.title; color: "#8fa0a4"; font.pixelSize: 11 }
                                    Flow {
                                        Layout.fillWidth: true; spacing: 6
                                        Repeater {
                                            model: group.modelData.pictures
                                            PictureTile {
                                                id: pick
                                                required property var modelData
                                                objectName: "genPick_" + modelData.id
                                                picture: modelData
                                                order: gp.inputs.indexOf(modelData.id)
                                                size: 84
                                                label: modelData.viewName + (modelData.approved ? "" : " · alt")
                                                labelColor: order >= 0 || modelData.approved ? "#87dec7" : "#6f8085"
                                                detail: order >= 0 ? "Made from it as " + gp.label(order + 1) + ". Click to take it out." : "Click to make it from this too."
                                                onClicked: gp.togglePick(modelData.id)
                                            }
                                        }
                                    }
                                }
                            }
                            Text {
                                visible: gp.groups.length === 0
                                text: "No pictures yet. Generate this item's first view from the prompt, or add pictures."
                                color: "#6f8085"; font.pixelSize: 11
                            }
                        }
                    }
                    Text {
                        objectName: "genMorePictures"
                        text: gp.allPictures ? "Only this and what it belongs with" : "More pictures from the plan…"
                        color: moreMouse.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 11
                        MouseArea { id: moreMouse; anchors.fill: parent; anchors.margins: -3; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                    onClicked: gp.allPictures = !gp.allPictures }
                    }
                }
                // The instruction (or framing): the view's own, editable for this time.
                ColumnLayout {
                    visible: gp.view !== 0
                    Layout.fillWidth: true; spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: gp.editing ? "Instruction" : "Framing"; color: "#edf1ef"; font.pixelSize: 13; font.weight: Font.DemiBold; Layout.fillWidth: true }
                        Text {
                            objectName: "genAsk"
                            text: gp.instruction.trim() ? "Ask your agent to improve it…" : "Ask your agent to write it…"
                            color: genAskMouse.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 11
                            MouseArea { id: genAskMouse; anchors.fill: parent; anchors.margins: -3; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                        onClicked: gp.askAgent() }
                            ToolTip.visible: genAskMouse.containsMouse; ToolTip.delay: 400
                            ToolTip.text: gp.editing ? "Saves the picked pictures and this instruction to the view, so your agent can look at them, then asks it"
                                                     : "Saves this framing to the view, then asks your agent about it"
                        }
                    }
                    ScrollView {
                        id: insScroll
                        Layout.fillWidth: true; Layout.preferredHeight: 64
                        contentWidth: availableWidth
                        background: Rectangle { radius: 4; color: "#162027"; border.color: insText.activeFocus ? "#87dec7" : "#34434a" }
                        TextArea {
                            id: insText
                            objectName: "genInstruction"
                            width: insScroll.availableWidth
                            text: gp.instruction
                            wrapMode: TextEdit.Wrap; selectByMouse: true
                            color: "#edf1ef"; font.pixelSize: 12
                            leftPadding: 8; rightPadding: 8; topPadding: 6; bottomPadding: 6
                            background: null
                            placeholderText: gp.editing ? "e.g. The woman from " + gp.label(1) + ", turned 45 degrees to her left, face and shoulders. Keep her face exactly the same."
                                                        : "Angle, distance, backdrop and light"
                            placeholderTextColor: "#6f8085"
                            onTextChanged: if (text !== gp.instruction) gp.instruction = text
                        }
                    }
                    RowLayout {
                        spacing: 8
                        CheckBox {
                            id: saveBox
                            objectName: "genSaveFraming"
                            checked: gp.saveFraming
                            onToggled: gp.saveFraming = checked
                            focusPolicy: Qt.NoFocus
                            indicator: Rectangle {
                                x: 0; anchors.verticalCenter: parent.verticalCenter
                                width: 14; height: 14; radius: 3
                                color: saveBox.checked ? "#87dec7" : "#162027"; border.color: saveBox.checked ? "#87dec7" : "#5c7078"
                                Text { anchors.centerIn: parent; visible: saveBox.checked; text: "✓"; color: "#122328"; font.pixelSize: 10 }
                            }
                            contentItem: Text { text: "Save as this view's " + (gp.editing ? "instruction" : "framing"); color: "#adb8ba"; font.pixelSize: 12; leftPadding: 20; verticalAlignment: Text.AlignVCenter }
                        }
                    }
                    Text {
                        Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 2; elide: Text.ElideRight
                        visible: gp.vopts.subject !== undefined && gp.vopts.subject.length > 0
                        text: "Then the subject: " + gp.vopts.subject; color: "#6f8085"; font.pixelSize: 11; font.italic: true
                    }
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
                                // Numbers are kept as typed ("0." while typing 0.5); ChorusCut reads them when generating.
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
                                objectName: "genSettingAuto"
                                readonly property bool auto: (gp.touched, gp.values, gp.aspectIsAuto(row.modelData))
                                visible: auto
                                text: "automatic"; color: "#6f8085"; font.pixelSize: 11
                                MouseArea { id: autoMouse; anchors.fill: parent; hoverEnabled: true }
                                ToolTip.visible: autoMouse.containsMouse; ToolTip.delay: 300
                                ToolTip.text: "Chosen for this kind of picture: 3:4 for people and looks, the video's aspect for places. Pick another to choose it yourself."
                            }
                            Text {
                                objectName: "genSettingReset"
                                visible: row.changed && !gp.aspectIsAuto(row.modelData)
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
            Small { objectName: "genCancel"; text: "Cancel"; onClicked: gp.close() }
            Small {
                id: saveButton
                objectName: "genSave"
                visible: gp.view !== 0
                text: "Save"
                onClicked: gp.save()
                ToolTip.visible: hovered; ToolTip.delay: 400
                ToolTip.text: (gp.editing ? "Keep the picked pictures and the instruction on this view" : "Keep the framing on this view") + ", and the settings with the workflow, without generating"
            }
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
