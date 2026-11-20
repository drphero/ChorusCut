import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Generate takes for a shot with options: the workflow for its route (a take from reference
// pictures, or one singing to the song), the shot's video prompt (editable here; Save keeps it on
// the shot), the pictures it sends (picked in the inspector), the handles each side, the seed,
// how many, and the workflow's adjustable settings. The length is the shot plus its handles,
// rounded up onto the workflow's frame grid (H3: 5 + 17n frames). For a lip-sync shot the song
// under that span goes with it, so the take is locked to the song.
Popup {
    id: tp
    objectName: "takePopup"
    focus: true
    modal: true
    closePolicy: Popup.CloseOnEscape
    padding: 20
    anchors.centerIn: Overlay.overlay
    // Wide windows get the prompt down the left at full height (H3 prompts run long) and the
    // options in their own column; narrow ones stack them. Each part scrolls on its own.
    readonly property real roomW: Overlay.overlay ? Overlay.overlay.width : 1200
    readonly property real roomH: Overlay.overlay ? Overlay.overlay.height : 800
    readonly property bool wide: roomW >= 980
    width: wide ? Math.min(1280, roomW - 60) : Math.min(620, roomW - 40)
    height: wide ? Math.min(940, roomH - 40) : Math.min(implicitHeight, roomH - 40)
    background: Rectangle { color: "#1d2930"; radius: 6; border.color: "#34434a" }

    property int shot: 0
    property string workflow: ""
    property string prompt: ""
    property real handles: 0.5
    property bool fixedSeed: false
    property string seedText: ""
    property int count: 1
    property var values: ({})
    property var touched: ({})
    property var prefillSettings: null
    property string note: ""
    // The pictures to send, in order (reference ids); the shot keeps them when they differ from
    // its framing's picks.
    property var picks: []
    property bool picksTouched: false
    property bool allPictures: false
    signal workflowsRequested()
    signal askRequested(int shot)

    readonly property var topts: (editor.plan.revision, editor.workflows.list, editor.generator.choices,
                                  shot ? editor.generator.takeOptions(shot, workflow, handles) : ({workflows: [], pictures: []}))
    readonly property var shotInfo: (editor.plan.revision, shot ? editor.plan.shot(shot) : ({}))
    readonly property var settings: (editor.workflows.list, workflow ? editor.workflows.settings(workflow) : [])
    readonly property bool hasSeed: (editor.workflows.list, workflow ? editor.workflows.hasSlot(workflow, "seed") : false)
    readonly property string why: {
        if (!shot) return ""
        const w = topts.why || ""
        if (w.startsWith("Write the shot's video prompt") && prompt.trim()) return ""
        if (!w && !prompt.trim()) return "Write the shot's video prompt first, or ask your agent to."
        return w
    }

    function start(shotId, prefill) {
        shot = shotId
        const p = prefill || {}
        const s = editor.plan.shot(shotId)
        const route = (s.fields || {}).route || ""
        prompt = ((s.fields || {}).prompts || {})[route] || ""
        handles = p.handles !== undefined ? p.handles : 0.5
        prefillSettings = p.settings || null
        fixedSeed = p.seed !== undefined && p.seed !== null
        seedText = fixedSeed ? String(p.seed) : ""
        count = 1
        note = ""
        picks = Array.from(editor.plan.shotPictures(shotId).picked).map(r => r.id)
        picksTouched = false
        allPictures = false
        const opts = editor.generator.takeOptions(shotId, "", handles)
        const list = opts.workflows || []
        // Asked for (Try again), else the shot's own (kept by Save), else the default.
        const found = list.find(w => w.id === p.workflow) || list.find(w => w.id === opts.workflow) || list.find(w => w.isDefault) || list[0]
        const id = found ? found.id : ""
        if (id === workflow) loadValues(); else workflow = id
        open()
    }
    onWorkflowChanged: loadValues()
    function shown(v) { return typeof v === "number" ? String(Math.round(v * 10000) / 10000) : typeof v === "boolean" ? (v ? "on" : "off") : String(v) }
    function loadValues() {
        const v = {}
        for (const s of editor.workflows.settings(workflow))
            v[s.key] = prefillSettings && prefillSettings[s.key] !== undefined ? prefillSettings[s.key] : s.value
        values = v
        touched = {}
        applyAutoAspect()
        if (workflow) editor.generator.fetchChoices(workflow)
    }
    // An aspect ratio nobody chose shows the video's own.
    function isAspect(s) { return s.input === "aspect_ratio" }
    function aspectIsAuto(s) {
        return isAspect(s) && !touched[s.key] && shown(s.value) === shown(s.original) && !(prefillSettings && prefillSettings[s.key] !== undefined)
    }
    function applyAutoAspect() {
        if (!workflow) return
        let v = null
        for (const s of editor.workflows.settings(workflow)) {
            if (!aspectIsAuto(s)) continue
            const auto = editor.generator.autoAspect("shot", workflow, s.key)
            if (auto && values[s.key] !== auto) { v = v || Object.assign({}, values); v[s.key] = auto }
        }
        if (v) values = v
    }
    Connections { target: editor.generator; function onChoicesChanged() { if (tp.opened) tp.applyAutoAspect() } }
    function setValue(key, value) {
        const v = Object.assign({}, values); v[key] = value; values = v
        const t = Object.assign({}, touched); t[key] = true; touched = t
    }
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
    readonly property int most: topts.maxPictures || 9
    function label(n) { return workflow ? editor.generator.pictureLabel(workflow, n) : "<Picture " + n + ">" }
    function togglePick(id) {
        const list = picks.slice()
        const i = list.indexOf(id)
        if (i >= 0) list.splice(i, 1)
        else if (list.length < most) list.push(id)
        else { note = "This workflow takes " + most + " pictures. Click one to take it out first."; return }
        note = ""
        picks = list
        picksTouched = true
    }
    function backToFraming() {
        editor.plan.run({op: "set", kind: "shot", id: shot, fields: {pictures: []}})
        picks = Array.from(editor.plan.shotPictures(shot).picked).map(r => r.id)
        picksTouched = false
    }
    // Pictures to pick from: who is in frame (their looks first) and where, every picture of each
    // (alternatives too); then, on request, everything else in the plan. [{title, pictures}]
    readonly property var groups: {
        editor.plan.references
        const plan = editor.plan
        const out = []
        if (!shot) return out
        const f = (shotInfo.fields || {})
        const seen = {}
        const add = (title, kind, id) => {
            if (seen[kind + id]) return
            seen[kind + id] = true
            const pics = plan.referencesFor(kind, id).filter(r => !r.missing)
            if (pics.length) out.push({title: title, pictures: pics})
        }
        const nameOf = (kind, id) => { const it = plan.item(kind, id); return it.fields ? (it.fields.name || kind) : kind }
        // Lists from C++ are array-like, not JS arrays.
        const who = f.who && typeof f.who === "object" && f.who.length !== undefined ? Array.from(f.who) : []
        for (const w of who) {
            if (w.look) add(nameOf("character", w.character) + " · " + nameOf("look", w.look), "look", w.look)
            add(nameOf("character", w.character), "character", w.character)
        }
        if (f.where) add(nameOf("location", f.where), "location", f.where)
        const props = f.props && typeof f.props === "object" && f.props.length !== undefined ? Array.from(f.props) : []
        for (const p of props) add(nameOf("prop", p), "prop", p)
        // Pictures picked earlier from elsewhere stay in view.
        for (const id of picks) { const r = plan.item("reference", id); if (r.owner && !seen[r.owner + r.ownerId]) add(nameOf(r.owner, r.ownerId), r.owner, r.ownerId) }
        if (allPictures) {
            for (const c of plan.characters) {
                for (const l of plan.looks) if (l.character === c.id) add((c.fields.name || "Character") + " · " + (l.fields.name || "Look"), "look", l.id)
                add(c.fields.name || "Character", "character", c.id)
            }
            for (const l of plan.locations) add(l.fields.name || "Location", "location", l.id)
            for (const p of plan.props) add(p.fields.name || "Prop", "prop", p.id)
        }
        return out
    }
    function randomSeed() { return String(Math.floor(Math.random() * 281474976710656)) }
    // The prompt is the shot's: it stays with the shot for its route.
    function keep() {
        if (workflow) {
            const w = editor.generator.keepSettings(workflow, keptValues())
            if (w) { note = w; return false }
        }
        const s = editor.plan.shot(shot)
        const route = (s.fields || {}).route || ""
        const all = Object.assign({}, (s.fields || {}).prompts || {})
        const fields = {}
        if ((all[route] || "") !== prompt) {
            if (prompt.trim()) all[route] = prompt; else delete all[route]
            fields.prompts = all
        }
        // The pictures picked here become the shot's own, in this order.
        if (picksTouched) fields.pictures = picks
        // The workflow chosen here stays with the shot; the library's default is kept as "follow
        // the default", so changing the default later still reaches this shot.
        if (workflow) {
            const use = topts.use || ""
            const keepAs = use && workflow === editor.workflows.forUse(use) ? "" : workflow
            if (((s.fields || {}).workflow || "") !== keepAs) fields.workflow = keepAs
        }
        if (Object.keys(fields).length) editor.plan.run({op: "set", kind: "shot", id: shot, fields: fields})
        picksTouched = false
        return true
    }
    function go() {
        if (!keep()) return
        const options = {workflow: workflow, count: count, settings: values, aspectChosen: aspectChosen, handles: handles, prompt: prompt}
        if (hasSeed && fixedSeed) options.seed = seedText
        note = editor.generator.generateTake(shot, options)
        if (!note) close()
    }
    function save() { if (keep()) close() }
    function askAgent() { const id = shot; if (!keep()) return; close(); askRequested(id) }

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

    Shortcut { enabled: tp.opened; sequences: ["Ctrl+Return", "Ctrl+Enter"]; onActivated: if (takeGo.enabled) tp.go() }

    contentItem: ColumnLayout {
        spacing: 10
        ColumnLayout {
            spacing: 2
            Layout.fillWidth: true
            Text { text: tp.topts.lipSync ? "Generate a take, singing to the song" : "Generate a take"; color: "#8fa0a4"; font.pixelSize: 12 }
            Text {
                text: "Shot " + (tp.shotInfo.number || "") + (tp.shotInfo.label ? " · " + tp.shotInfo.label.split(" · ").slice(1).join(" · ") : "")
                color: "#edf1ef"; font.pixelSize: 16; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true
            }
        }
        GridLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            columns: tp.wide ? 2 : 1
            columnSpacing: 18; rowSpacing: 12
            // The prompt: the shot's for its route.
            ColumnLayout {
                id: promptPane
                Layout.fillWidth: true
                Layout.fillHeight: tp.wide
                Layout.preferredWidth: tp.wide ? (tp.availableWidth - 18) * 0.58 : -1
                Layout.preferredHeight: tp.wide ? -1 : 240
                Layout.minimumHeight: 160
                spacing: 4
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "Prompt"; color: "#edf1ef"; font.pixelSize: 13; font.weight: Font.DemiBold }
                    Text {
                        objectName: "takePromptWords"
                        readonly property int words: { const t = tp.prompt.trim(); return t ? t.split(/\s+/).length : 0 }
                        text: words ? words + (words === 1 ? " word" : " words") : ""
                        color: "#6f8085"; font.pixelSize: 11; Layout.leftMargin: 6
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        objectName: "takeAsk"
                        text: tp.prompt.trim() ? "Ask your agent to improve it…" : "Ask your agent to write it…"
                        color: takeAskMouse.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 11
                        MouseArea { id: takeAskMouse; anchors.fill: parent; anchors.margins: -3; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                    onClicked: tp.askAgent() }
                    }
                }
                ScrollView {
                    id: promptScroll
                    objectName: "takePromptScroll"
                    Layout.fillWidth: true; Layout.fillHeight: true
                    contentWidth: availableWidth
                    clip: true
                    ScrollBar.vertical.policy: ScrollBar.AsNeeded
                    background: Rectangle { radius: 4; color: "#162027"; border.color: promptText.activeFocus ? "#87dec7" : "#34434a" }
                    // No rubber-banding at the ends: a long prompt scrolls and stops.
                    Component.onCompleted: if (contentItem.boundsBehavior !== undefined) { contentItem.boundsBehavior = Flickable.StopAtBounds; contentItem.flickDeceleration = 4000 }
                    TextArea {
                        id: promptText
                        objectName: "takePrompt"
                        width: promptScroll.availableWidth
                        text: tp.prompt
                        wrapMode: TextEdit.Wrap; selectByMouse: true
                        color: "#edf1ef"; font.pixelSize: 13
                        leftPadding: 10; rightPadding: 14; topPadding: 8; bottomPadding: 8
                        background: null
                        placeholderText: "What happens, shot by shot and second by second, naming the pictures as " + ((tp.topts.pictures || [])[0] || {label: "<Picture 1>"}).label
                        placeholderTextColor: "#6f8085"
                        onTextChanged: if (text !== tp.prompt) tp.prompt = text
                    }
                }
                Text { text: "⌘Return generates · Save keeps it on the shot"; color: "#6f8085"; font.pixelSize: 10 }
            }
        ScrollView {
            id: body
            objectName: "takeOptions"
            Layout.fillWidth: true; Layout.fillHeight: true
            Layout.preferredWidth: tp.wide ? (tp.availableWidth - 18) * 0.42 : -1
            Layout.minimumWidth: tp.wide ? 380 : 0
            Layout.preferredHeight: tp.wide ? -1 : bodyColumn.implicitHeight
            contentWidth: availableWidth
            clip: true
            Component.onCompleted: if (contentItem.boundsBehavior !== undefined) contentItem.boundsBehavior = Flickable.StopAtBounds
            ColumnLayout {
                id: bodyColumn
                width: body.availableWidth - 12
                spacing: 12
                RowLayout {
                    Layout.fillWidth: true; spacing: 10
                    visible: (tp.topts.workflows || []).length > 0
                    Label { text: "Workflow" }
                    PlanCombo {
                        objectName: "takeWorkflow"
                        allowUnset: false
                        options: (tp.topts.workflows || []).map(w => ({value: w.id, label: w.name + (w.isDefault ? "  ·  default" : "")}))
                        value: tp.workflow
                        onPicked: v => tp.workflow = v
                    }
                }
                // The pictures it sends, in the order clicked, as the prompt names them.
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 4
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: "Pictures"; color: "#edf1ef"; font.pixelSize: 13; font.weight: Font.DemiBold; Layout.fillWidth: true }
                        Text {
                            objectName: "takePickCount"
                            text: tp.picks.length + " of " + tp.most
                            color: tp.picks.length ? "#87dec7" : "#e7b75f"; font.pixelSize: 12
                        }
                    }
                    Text {
                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#8fa0a4"; font.pixelSize: 11
                        text: "Click pictures in the order the prompt names them: " + tp.label(1) + (tp.most > 1 ? ", " + tp.label(2) : "") + "…"
                    }
                    Repeater {
                        model: tp.groups
                        ColumnLayout {
                            id: group
                            required property var modelData
                            Layout.fillWidth: true; spacing: 2
                            Text { text: group.modelData.title; color: "#8fa0a4"; font.pixelSize: 11 }
                            Flow {
                                Layout.fillWidth: true; spacing: 6
                                Repeater {
                                    model: group.modelData.pictures
                                    Item {
                                        id: pic
                                        required property var modelData
                                        readonly property int order: tp.picks.indexOf(modelData.id)
                                        objectName: "takePick_" + modelData.id
                                        width: 56; height: 74
                                        Rectangle {
                                            width: 56; height: 56; radius: 4; clip: true; color: "#0c1216"
                                            border.color: pic.order >= 0 ? "#87dec7" : "#34434a"; border.width: pic.order >= 0 ? 2 : 1
                                            Image { anchors.fill: parent; anchors.margins: 2; source: pic.modelData.url || ""; fillMode: Image.PreserveAspectCrop
                                                    asynchronous: true; sourceSize: Qt.size(112, 112); opacity: pic.order >= 0 ? 1 : 0.55 }
                                            Rectangle { visible: pic.order >= 0; x: 3; y: 3; width: 16; height: 16; radius: 8; color: "#87dec7"
                                                        Text { anchors.centerIn: parent; text: pic.order + 1; color: "#122328"; font.pixelSize: 10 } }
                                        }
                                        Text { y: 58; width: 56; elide: Text.ElideRight; font.pixelSize: 9
                                               text: pic.order >= 0 ? tp.label(pic.order + 1) : pic.modelData.viewName + (pic.modelData.approved ? "" : " ·alt")
                                               color: pic.order >= 0 ? "#87dec7" : "#6f8085" }
                                        MouseArea { id: picMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: tp.togglePick(pic.modelData.id) }
                                        ToolTip.visible: picMouse.containsMouse; ToolTip.delay: 400
                                        ToolTip.text: pic.modelData.viewName + (pic.modelData.approved ? " (chosen)" : " (alternative)")
                                    }
                                }
                            }
                        }
                    }
                    Text {
                        visible: tp.groups.length === 0
                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#6f8085"; font.pixelSize: 11
                        text: "No pictures yet: choose who and where in the inspector, or pick from the rest of the plan."
                    }
                    RowLayout {
                        spacing: 14
                        Text {
                            objectName: "takeMorePictures"
                            text: tp.allPictures ? "Only who and where" : "More pictures from the plan…"
                            color: morePics.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 11
                            MouseArea { id: morePics; anchors.fill: parent; anchors.margins: -3; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                        onClicked: tp.allPictures = !tp.allPictures }
                        }
                        Text {
                            objectName: "takePicksReset"
                            visible: tp.shot !== 0 && ((tp.shotInfo.fields || {}).pictures || []).length > 0
                            text: "Back to the framing's picks"
                            color: resetPicks.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 11
                            MouseArea { id: resetPicks; anchors.fill: parent; anchors.margins: -3; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                        onClicked: tp.backToFraming() }
                        }
                    }
                }
                // Length: the shot, its handles, and the grid.
                RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    Label { text: "Handles" }
                    Small { objectName: "takeFewerHandles"; text: "−"; enabled: tp.handles > 0; onClicked: tp.handles = Math.max(0, Math.round((tp.handles - 0.25) * 100) / 100) }
                    Text { objectName: "takeHandles"; text: tp.handles.toFixed(2) + " s"; color: "#edf1ef"; font.pixelSize: 13; Layout.preferredWidth: 52; horizontalAlignment: Text.AlignHCenter }
                    Small { objectName: "takeMoreHandles"; text: "+"; enabled: tp.handles < 2; onClicked: tp.handles = Math.min(2, tp.handles + 0.25) }
                    Text { text: "each side, to trim later"; color: "#6f8085"; font.pixelSize: 11 }
                }
                RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    Label { text: "Length" }
                    Text {
                        objectName: "takeLength"
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        text: tp.topts.frames ? tp.topts.frames + " frames · " + Number(tp.topts.seconds).toFixed(2) + " s"
                                                + (tp.topts.lipSync ? " of the song, locked" : "") : "—"
                        color: "#edf1ef"; font.pixelSize: 13
                    }
                }
                // The seed.
                RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    visible: tp.workflow !== ""
                    Label { text: "Seed" }
                    Small { objectName: "takeSeedRandom"; text: "Random"; active: !tp.fixedSeed; enabled: tp.hasSeed; onClicked: tp.fixedSeed = false }
                    Small { objectName: "takeSeedFixed"; text: "Fixed"; active: tp.fixedSeed; enabled: tp.hasSeed
                            onClicked: { tp.fixedSeed = true; if (!tp.seedText) tp.seedText = tp.randomSeed() } }
                    Field {
                        objectName: "takeSeed"
                        Layout.fillWidth: true
                        enabled: tp.hasSeed && tp.fixedSeed
                        font.family: "Menlo"
                        text: tp.fixedSeed ? tp.seedText : ""
                        placeholderText: tp.hasSeed ? (tp.fixedSeed ? "0" : "a new one each time") : "no seed input"
                        placeholderTextColor: "#6f8085"
                        validator: RegularExpressionValidator { regularExpression: /[0-9]{0,16}/ }
                        onTextEdited: tp.seedText = text
                    }
                }
                RowLayout {
                    spacing: 8
                    visible: tp.workflow !== ""
                    Label { text: "Takes" }
                    Small { objectName: "takeFewer"; text: "−"; enabled: tp.count > 1; onClicked: tp.count-- }
                    Text { objectName: "takeCount"; text: tp.count; color: "#edf1ef"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter; Layout.preferredWidth: 22 }
                    Small { objectName: "takeMore"; text: "+"; enabled: tp.count < 8; onClicked: tp.count++ }
                }
                // The workflow's adjustable settings.
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 6
                    visible: tp.workflow !== ""
                    Text { text: "SETTINGS"; color: "#6f8085"; font.pixelSize: 11; font.letterSpacing: 0.6; Layout.topMargin: 4 }
                    Repeater {
                        model: tp.settings
                        RowLayout {
                            id: row
                            required property var modelData
                            readonly property var value: tp.values[modelData.key]
                            readonly property var choices: editor.generator.choices[modelData.classType + "/" + modelData.input] || []
                            readonly property bool changed: value !== undefined && tp.shown(value) !== tp.shown(modelData.original)
                            objectName: "takeSetting_" + modelData.node + "_" + modelData.input
                            Layout.fillWidth: true; spacing: 8
                            Label { text: row.modelData.label }
                            Field {
                                objectName: "takeSettingField"
                                visible: row.modelData.type !== "switch" && row.choices.length === 0
                                Layout.fillWidth: true
                                text: row.value === undefined ? "" : tp.shown(row.value)
                                validator: row.modelData.type === "number" ? numberCheck : null
                                onTextEdited: tp.setValue(row.modelData.key, text)
                                RegularExpressionValidator { id: numberCheck; regularExpression: /-?[0-9]*\.?[0-9]*/ }
                            }
                            PlanCombo {
                                objectName: "takeSettingChoice"
                                visible: row.modelData.type === "text" && row.choices.length > 0
                                allowUnset: false
                                options: {
                                    const list = row.choices.map(c => ({value: c, label: c}))
                                    if (row.value !== undefined && row.choices.indexOf(row.value) < 0) list.unshift({value: row.value, label: row.value})
                                    return list
                                }
                                value: row.value
                                onPicked: v => tp.setValue(row.modelData.key, v)
                            }
                            CheckBox {
                                id: box
                                visible: row.modelData.type === "switch"
                                Layout.fillWidth: true
                                checked: row.value === true
                                onToggled: tp.setValue(row.modelData.key, checked)
                                focusPolicy: Qt.NoFocus
                            }
                            Text {
                                visible: (tp.touched, tp.values, tp.aspectIsAuto(row.modelData))
                                text: "automatic"; color: "#6f8085"; font.pixelSize: 11
                            }
                            Text {
                                visible: row.changed && !tp.aspectIsAuto(row.modelData)
                                text: "Reset"; color: resetMouse.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 11
                                MouseArea { id: resetMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                            onClicked: tp.setValue(row.modelData.key, row.modelData.original) }
                            }
                        }
                    }
                }
            }
        }
        }
        Text {
            objectName: "takeWhy"
            visible: (tp.note || tp.why).length > 0
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: tp.note || tp.why; color: "#e7b75f"; font.pixelSize: 12
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            Text {
                text: "Workflows…"; color: wfMouse.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 12
                MouseArea { id: wfMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: { tp.close(); tp.workflowsRequested() } }
            }
            Item { Layout.fillWidth: true }
            Small { objectName: "takeCancel"; text: "Cancel"; onClicked: tp.close() }
            Small { objectName: "takeSave"; text: "Save"; onClicked: tp.save()
                    ToolTip.visible: hovered; ToolTip.delay: 400; ToolTip.text: "Keep the prompt on the shot and the settings with the workflow, without generating" }
            Small {
                id: takeGo
                objectName: "takeGo"
                active: true
                enabled: tp.why === "" && (!tp.fixedSeed || !tp.hasSeed || tp.seedText !== "")
                text: tp.count > 1 ? "Generate " + tp.count : "Generate"
                onClicked: tp.go()
            }
        }
    }
}
