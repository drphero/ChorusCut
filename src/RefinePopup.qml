import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Refine faces on a take: tick whose face to redraw. Each ticked person is one pass, in the order
// listed, each refining the take the pass before it makes. A person is found by a picture of
// their face and redrawn towards their own pictures only, with their own prompt (kept on the
// character for next time). A sung take sends the song under it; any other take silence.
Popup {
    id: rp
    objectName: "refinePopup"
    focus: true
    modal: true
    closePolicy: Popup.CloseOnEscape
    padding: 20
    anchors.centerIn: Overlay.overlay
    readonly property real roomW: Overlay.overlay ? Overlay.overlay.width : 1200
    readonly property real roomH: Overlay.overlay ? Overlay.overlay.height : 800
    width: Math.min(780, roomW - 40)
    height: Math.min(940, roomH - 40)
    background: Rectangle { color: "#1d2930"; radius: 6; border.color: "#34434a" }

    property int take: 0
    property string workflow: ""
    // character id -> {on, face, pictures: [reference ids], prompt, promptTouched}
    property var chosen: ({})
    property var values: ({})
    property bool fixedSeed: false
    property string seedText: ""
    property string note: ""
    signal workflowsRequested()

    readonly property var ropts: (editor.plan.revision, editor.workflows.list, editor.generator.choices,
                                  take ? editor.generator.refineOptions(take, workflow) : ({people: [], workflows: []}))
    readonly property var people: Array.from(ropts.people || [])
    readonly property var takeInfo: (editor.plan.revision, take ? Array.from(editor.plan.takesFor(editor.plan.item("take", take).shot || 0)).find(t => t.id === take) || ({}) : ({}))
    readonly property var shotInfo: (editor.plan.revision, ropts.shot ? editor.plan.shot(ropts.shot) : ({}))
    // The strength first: it is the setting that matters most here.
    readonly property var settings: (editor.workflows.list, workflow ? Array.from(editor.workflows.settings(workflow))
                                         .sort((a, b) => (b.classType === "H3FaceRefineStrength") - (a.classType === "H3FaceRefineStrength")) : [])
    readonly property bool hasSeed: (editor.workflows.list, workflow ? editor.workflows.hasSlot(workflow, "seed") : false)
    readonly property int most: ropts.maxPictures || 3
    readonly property var ticked: people.filter(p => chosen[p.character] && chosen[p.character].on)
    readonly property string why: {
        if (ropts.why) return ropts.why
        if (!ticked.length) return "Tick whose face to refine."
        for (const p of ticked) if (!chosen[p.character].face) return "Choose a picture to find " + p.name + "'s face by."
        return ""
    }

    function label(n) { return workflow ? editor.generator.pictureLabel(workflow, n) : "<Picture " + n + ">" }
    // "<Picture 1> and <Picture 2> show Lena." until the user writes their own.
    function defaultPrompt(name, count) {
        if (!count) return name + "."
        const labels = []
        for (let i = 1; i <= count; ++i) labels.push(label(i))
        const list = labels.length > 1 ? labels.slice(0, -1).join(", ") + " and " + labels[labels.length - 1] : labels[0]
        return list + (count > 1 ? " show " : " shows ") + name + "."
    }
    function start(takeId) {
        take = takeId
        note = ""
        fixedSeed = false
        seedText = ""
        const opts = editor.generator.refineOptions(takeId, "")
        const list = Array.from(opts.workflows || [])
        const found = list.find(w => w.id === opts.workflow) || list.find(w => w.isDefault) || list[0]
        workflow = found ? found.id : ""
        const c = {}
        const ps = Array.from(opts.people || [])
        for (const p of ps) {
            const pics = Array.from(p.pictures || [])
            // With one person in the shot, it's them; with several, the choice is deliberate.
            c[p.character] = {on: ps.length === 1 && !p.why, face: p.face, pictures: pics,
                              prompt: p.prompt || defaultPrompt(p.name, pics.length), promptTouched: !!p.prompt}
        }
        chosen = c
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
    function setPerson(id, patch) {
        const c = Object.assign({}, chosen)
        c[id] = Object.assign({}, c[id], patch)
        chosen = c
    }
    function togglePicture(person, ref) {
        const was = chosen[person.character]
        const list = was.pictures.slice()
        const i = list.indexOf(ref)
        if (i >= 0) list.splice(i, 1)
        else if (list.length < most) list.push(ref)
        else { note = "This workflow takes " + most + " pictures. Click one to take it out first."; return }
        note = ""
        const patch = {pictures: list}
        if (!was.promptTouched) patch.prompt = defaultPrompt(person.name, list.length)
        setPerson(person.character, patch)
    }
    function randomSeed() { return String(Math.floor(Math.random() * 281474976710656)) }
    function go() {
        const passes = []
        for (const p of ticked) {
            const c = chosen[p.character]
            passes.push({character: p.character, face: c.face, pictures: c.pictures, prompt: c.prompt})
            // A prompt written here stays with the person for their next refine.
            if (c.promptTouched && c.prompt.trim() !== (p.prompt || "").trim())
                editor.plan.run({op: "set", kind: "character", id: p.character, fields: {facePrompt: c.prompt.trim()}})
        }
        const options = {workflow: workflow, settings: values, keep: values}
        if (hasSeed && fixedSeed) options.seed = seedText
        note = editor.generator.refineFaces(take, passes, options)
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
            Text { text: "Refine faces"; color: "#8fa0a4"; font.pixelSize: 12 }
            Text {
                text: "Shot " + (rp.shotInfo.number || "") + " · take " + (rp.takeInfo.number || "")
                      + (rp.takeInfo.refined ? " (" + rp.takeInfo.refined + "'s face refined)" : "") + (rp.ropts.lipSync ? " · sung, the song goes with it" : "")
                color: "#edf1ef"; font.pixelSize: 16; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true
            }
        }
        ScrollView {
            id: body
            objectName: "refineBody"
            Layout.fillWidth: true; Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true
            Component.onCompleted: if (contentItem.boundsBehavior !== undefined) contentItem.boundsBehavior = Flickable.StopAtBounds
            ColumnLayout {
                width: body.availableWidth - 12
                spacing: 12
                RowLayout {
                    Layout.fillWidth: true; spacing: 10
                    visible: (rp.ropts.workflows || []).length > 1
                    Label { text: "Workflow" }
                    PlanCombo {
                        objectName: "refineWorkflow"
                        allowUnset: false
                        options: (rp.ropts.workflows || []).map(w => ({value: w.id, label: w.name + (w.isDefault ? "  ·  default" : "")}))
                        value: rp.workflow
                        onPicked: v => rp.workflow = v
                    }
                }
                Text {
                    Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#8fa0a4"; font.pixelSize: 12
                    text: "Tick whose face to redraw. Each is its own pass, in this order, each on the take the pass before it makes, so every step is a take you can compare."
                }
                Repeater {
                    model: rp.people
                    Rectangle {
                        id: card
                        required property var modelData
                        required property int index
                        readonly property var c: rp.chosen[modelData.character] || ({on: false, face: 0, pictures: [], prompt: ""})
                        objectName: "refineCard_" + modelData.character
                        Layout.fillWidth: true
                        implicitHeight: cardColumn.implicitHeight + 20
                        radius: 5; color: "#162027"; border.color: card.c.on ? "#3f9c84" : "#2c3a41"
                        ColumnLayout {
                            id: cardColumn
                            x: 10; y: 10; width: parent.width - 20
                            spacing: 8
                            RowLayout {
                                spacing: 6
                                CheckBox {
                                    objectName: "refinePerson_" + card.modelData.character
                                    focusPolicy: Qt.NoFocus
                                    enabled: !card.modelData.why
                                    checked: card.c.on
                                    onToggled: rp.setPerson(card.modelData.character, {on: checked})
                                }
                                Text { text: card.modelData.name; color: "#edf1ef"; font.pixelSize: 14; font.weight: Font.DemiBold }
                                Text {
                                    visible: card.c.on && rp.ticked.length > 1
                                    text: "pass " + (rp.ticked.findIndex(p => p.character === card.modelData.character) + 1)
                                    color: "#87dec7"; font.pixelSize: 11
                                }
                            }
                            Text { visible: !!card.modelData.why; text: card.modelData.why || ""; color: "#e7b75f"; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
                            ColumnLayout {
                                visible: card.c.on
                                Layout.fillWidth: true; spacing: 8
                                Text { text: "Find their face by"; color: "#edf1ef"; font.pixelSize: 12; font.weight: Font.DemiBold }
                                Flow {
                                    Layout.fillWidth: true; spacing: 6
                                    Repeater {
                                        model: card.modelData.faces
                                        PictureTile {
                                            required property var modelData
                                            objectName: "refineFace_" + modelData.id
                                            picture: modelData
                                            order: card.c.face === modelData.id ? 0 : -1
                                            size: 72; dimmed: 0.5
                                            label: modelData.viewName + (card.c.face === modelData.id ? " · finds them" : "")
                                            detail: "The face that looks most like this one is the one redrawn."
                                            onClicked: rp.setPerson(card.modelData.character, {face: modelData.id})
                                        }
                                    }
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { text: "Pictures for the video model"; color: "#edf1ef"; font.pixelSize: 12; font.weight: Font.DemiBold; Layout.fillWidth: true }
                                    Text { text: card.c.pictures.length + " of " + rp.most; color: card.c.pictures.length ? "#87dec7" : "#e7b75f"; font.pixelSize: 12 }
                                }
                                Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#6f8085"; font.pixelSize: 11
                                       text: "Only theirs: their face, their look. Click in the order the prompt names them." }
                                Flow {
                                    Layout.fillWidth: true; spacing: 6
                                    Repeater {
                                        model: Array.from(card.modelData.available || []).filter(r => !r.missing)
                                        PictureTile {
                                            required property var modelData
                                            objectName: "refinePick_" + modelData.id
                                            picture: modelData
                                            order: card.c.pictures.indexOf(modelData.id)
                                            size: 72; dimmed: 0.5
                                            label: order >= 0 ? rp.label(order + 1) : modelData.viewName + (modelData.approved ? "" : " · alt")
                                            detail: order >= 0 ? "Sent as " + rp.label(order + 1) + ". Click to take it out." : "Click to send it too."
                                            onClicked: rp.togglePicture(card.modelData, modelData.id)
                                        }
                                    }
                                }
                                Text { text: "Prompt"; color: "#edf1ef"; font.pixelSize: 12; font.weight: Font.DemiBold }
                                TextArea {
                                    objectName: "refinePrompt_" + card.modelData.character
                                    Layout.fillWidth: true
                                    Layout.minimumHeight: 64
                                    text: card.c.prompt
                                    wrapMode: TextEdit.Wrap; selectByMouse: true
                                    color: "#edf1ef"; font.pixelSize: 13
                                    placeholderText: "Who this is, naming the pictures as " + rp.label(1) + "… Only this person: no places or props."
                                    placeholderTextColor: "#6f8085"
                                    background: Rectangle { radius: 4; color: "#0f181d"; border.color: parent.activeFocus ? "#87dec7" : "#34434a" }
                                    onTextChanged: if (text !== card.c.prompt) rp.setPerson(card.modelData.character, {prompt: text, promptTouched: true})
                                }
                                Text { text: "Kept with " + card.modelData.name + " for their next refine."; color: "#6f8085"; font.pixelSize: 10 }
                            }
                        }
                    }
                }
                // The seed.
                RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    visible: rp.workflow !== ""
                    Label { text: "Seed" }
                    Small { objectName: "refineSeedRandom"; text: "Random"; active: !rp.fixedSeed; enabled: rp.hasSeed; onClicked: rp.fixedSeed = false }
                    Small { objectName: "refineSeedFixed"; text: "Fixed"; active: rp.fixedSeed; enabled: rp.hasSeed
                            onClicked: { rp.fixedSeed = true; if (!rp.seedText) rp.seedText = rp.randomSeed() } }
                    Field {
                        objectName: "refineSeed"
                        Layout.fillWidth: true
                        enabled: rp.hasSeed && rp.fixedSeed
                        font.family: "Menlo"
                        text: rp.fixedSeed ? rp.seedText : ""
                        placeholderText: rp.hasSeed ? (rp.fixedSeed ? "0" : "a new one each time") : "no seed input"
                        placeholderTextColor: "#6f8085"
                        validator: RegularExpressionValidator { regularExpression: /[0-9]{0,16}/ }
                        onTextEdited: rp.seedText = text
                    }
                }
                // The workflow's adjustable settings: the strength above all.
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 6
                    visible: rp.workflow !== "" && rp.settings.length > 0
                    Text { text: "SETTINGS"; color: "#6f8085"; font.pixelSize: 11; font.letterSpacing: 0.6; Layout.topMargin: 4 }
                    Repeater {
                        model: rp.settings
                        RowLayout {
                            id: row
                            required property var modelData
                            readonly property var value: rp.values[modelData.key]
                            readonly property var choices: editor.generator.choices[modelData.classType + "/" + modelData.input] || []
                            objectName: "refineSetting_" + modelData.node + "_" + modelData.input
                            Layout.fillWidth: true; spacing: 8
                            Label { text: row.modelData.label }
                            Field {
                                visible: row.modelData.type !== "switch" && row.choices.length === 0
                                Layout.fillWidth: true
                                text: row.value === undefined ? "" : rp.shown(row.value)
                                onTextEdited: rp.setValue(row.modelData.key, text)
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
                                onPicked: v => rp.setValue(row.modelData.key, v)
                            }
                            CheckBox {
                                visible: row.modelData.type === "switch"
                                Layout.fillWidth: true
                                checked: row.value === true
                                onToggled: rp.setValue(row.modelData.key, checked)
                                focusPolicy: Qt.NoFocus
                            }
                        }
                    }
                }
            }
        }
        Text {
            objectName: "refineWhy"
            visible: (rp.note || rp.why).length > 0
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: rp.note || rp.why; color: "#e7b75f"; font.pixelSize: 12
        }
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            Text {
                text: "Workflows…"; color: wfMouse.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 12
                MouseArea { id: wfMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                            onClicked: { rp.close(); rp.workflowsRequested() } }
            }
            Item { Layout.fillWidth: true }
            Small { objectName: "refineCancel"; text: "Cancel"; onClicked: rp.close() }
            Small {
                objectName: "refineGo"
                active: true
                enabled: rp.why === "" && (!rp.fixedSeed || !rp.hasSeed || rp.seedText !== "")
                text: rp.ticked.length > 1 ? "Refine " + rp.ticked.length + " faces" : rp.ticked.length === 1 ? "Refine " + rp.ticked[0].name + "'s face" : "Refine"
                onClicked: rp.go()
            }
        }
    }
}
