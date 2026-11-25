import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "keys.js" as Kbd

// A character's, look's or location's pictures, one tile per view (Face, Three-quarter, Full
// body, Wide, a turnaround sheet…): the view's chosen picture, its name and a word on where it
// stands. Clicking a tile opens that view underneath (and clicking it again closes it): its framing, what it is made from, its
// pictures (the chosen one goes to the video model) and Generate. "+ view" adds one of the usual
// views or your own. One view is open at a time, so a card shows pictures first and the workings
// only on request. Every change is one undo step through the plan's commands.
ColumnLayout {
    id: rv
    property string owner
    property int ownerId
    property int size: 84            // picture size in the open view's strip
    property int tileWidth: size >= 80 ? 104 : 84
    readonly property int tileHeight: Math.round(tileWidth * 1.27)
    spacing: 10
    Layout.fillWidth: true

    readonly property var views: (editor.plan.revision, editor.plan.viewsFor(owner, ownerId))
    readonly property var names: views.map(v => v.name.toLowerCase())
    readonly property var jobs: editor.generator.jobs.filter(j => j.owner === owner && j.ownerId === ownerId)
    property int openId: 0
    // No view is open until its tile is clicked, so a card shows its pictures and nothing more.
    readonly property int openIndex: views.findIndex(v => v.id === openId)
    readonly property var open: openIndex >= 0 ? views[openIndex] : null
    property bool editingFraming: false
    onOpenIndexChanged: editingFraming = false

    // What else people usually want of this kind; one click adds the view and opens it.
    readonly property var suggested: {
        const all = owner === "character" ? [
            {name: "Three-quarter", framing: "Turned 45 degrees to the left, face and shoulders, plain neutral grey backdrop, soft even light. Keep the face exactly the same.", role: "face", from: "pictures"},
            {name: "Profile", framing: "Side profile facing left, head and shoulders, plain neutral grey backdrop, soft even light. Keep the face exactly the same.", role: "face", from: "pictures"},
            {name: "Full body", framing: "Full body, head to shoes, standing front-on, plain neutral grey backdrop, soft even light. Keep the face exactly the same.", role: "body", from: "pictures"},
            {name: "Turnaround sheet", framing: "Character turnaround sheet: the same person four times side by side, front, three-quarter, side and back, full body, plain white background, even light.", role: "sheet", from: "pictures"}]
          : owner === "look" ? [
            {name: "Back", framing: "The same person and outfit seen from behind, full body, plain neutral grey backdrop, soft even light.", role: "look", from: "pictures"},
            {name: "Face", framing: "Face and shoulders, front view, plain neutral grey backdrop, soft even light, with this look's hair and makeup.", role: "face", from: "pictures"},
            {name: "Detail", framing: "Close-up of the outfit's most distinctive detail, plain backdrop, soft even light.", role: "detail", from: "pictures"},
            {name: "Outfit sheet", framing: "Outfit sheet: the same person in this look front, side and back, full body, side by side, plain white background, even light.", role: "sheet", from: "pictures"}]
          : [
            {name: "Reverse angle", framing: "The same place seen from the opposite direction, wide, eye level, empty of people.", role: "setting", from: "pictures"},
            {name: "Detail", framing: "Close-up of the place's most distinctive fixed detail, empty of people.", role: "detail", from: "pictures"},
            {name: "Angles sheet", framing: "The same place from three angles side by side: wide front, reverse and a high angle, empty of people.", role: "sheet", from: "pictures"}]
        return all.filter(s => names.indexOf(s.name.toLowerCase()) < 0)
    }
    function addView(spec) {
        const r = editor.plan.run({op: "add", kind: "view", owner: owner, ownerId: ownerId,
                                   fields: {name: spec.name, framing: spec.framing, role: spec.role, from: spec.from}})
        if (r.ok) openId = r.id
        return r
    }
    property var composing: null   // the sheet view whose panels are being ticked
    property var ticked: []

    // ---------------------------------------------------------------- the tiles
    Flow {
        Layout.fillWidth: true
        spacing: 10
        // By count, so tiles survive edits; rebuilt only when a view is added or removed.
        Repeater {
            model: rv.views.length
            Item {
                id: tile
                required property int index
                readonly property var view: rv.views[index] || ({id: 0, name: "", pictures: [], chosen: {}, from: "prompt", fields: {}})
                readonly property bool isOpen: rv.openIndex === index
                readonly property var running: rv.jobs.filter(j => j.view === view.id)
                readonly property bool failed: running.some(j => j.state === "failed")
                readonly property bool busy: running.some(j => j.state !== "failed")
                readonly property bool hasChoice: !!view.chosen.id
                objectName: "viewTile_" + rv.owner + "_" + index
                width: rv.tileWidth; height: rv.tileHeight + 34
                Rectangle {
                    width: rv.tileWidth; height: rv.tileHeight
                    radius: 6; clip: true
                    color: tile.hasChoice ? "#0c1216" : "#162027"
                    border.width: tile.hasChoice ? 2 : 1
                    border.color: tile.failed ? "#e07f6f" : tile.hasChoice ? "#3f9c84" : tile.view.pictures.length ? "#e0a24a" : "#5c7078"
                    Image {
                        anchors.fill: parent; anchors.margins: 2
                        visible: tile.hasChoice
                        source: tile.hasChoice && !tile.view.chosen.missing ? tile.view.chosen.url : ""
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize: Qt.size(rv.tileWidth * 2, rv.tileHeight * 2)
                    }
                    Column {
                        visible: !tile.hasChoice
                        anchors.centerIn: parent
                        spacing: 4
                        Text { anchors.horizontalCenter: parent.horizontalCenter; text: tile.view.pictures.length ? "◐" : "✦"
                               color: tile.view.pictures.length ? "#e0a24a" : "#87dec7"; font.pixelSize: 18 }
                        Text { anchors.horizontalCenter: parent.horizontalCenter; font.pixelSize: 11
                               text: tile.view.pictures.length ? "Choose one" : "Generate"
                               color: tile.view.pictures.length ? "#e0a24a" : "#8fa0a4" }
                    }
                    // A slow sweep while one of its pictures is being made.
                    Rectangle {
                        visible: tile.busy
                        y: parent.height - 5; height: 3; radius: 1; color: "#87dec7"
                        width: parent.width * 0.3
                        SequentialAnimation on x {
                            running: tile.busy && tile.visible; loops: Animation.Infinite
                            NumberAnimation { from: 3; to: rv.tileWidth * 0.7 - 3; duration: 900; easing.type: Easing.InOutQuad }
                            NumberAnimation { from: rv.tileWidth * 0.7 - 3; to: 3; duration: 900; easing.type: Easing.InOutQuad }
                        }
                    }
                }
                // The open view is marked under its tile.
                Rectangle { visible: tile.isOpen; x: 0; y: rv.tileHeight + 2; width: rv.tileWidth; height: 2; radius: 1; color: "#87dec7" }
                Text {
                    y: rv.tileHeight + 6; width: rv.tileWidth; elide: Text.ElideRight
                    text: tile.view.name; color: tile.isOpen ? "#edf1ef" : "#d5dcdb"; font.pixelSize: 12
                }
                Text {
                    y: rv.tileHeight + 21; width: rv.tileWidth; elide: Text.ElideRight
                    color: tile.failed ? "#f0a08f" : "#6f8085"; font.pixelSize: 10
                    text: tile.failed ? "failed" : tile.busy ? "generating…"
                          : tile.hasChoice ? "chosen" + (tile.view.pictures.length > 1 ? " · " + (tile.view.pictures.length - 1) + " more" : "")
                          : tile.view.pictures.length ? tile.view.pictures.length + " to choose from"
                          : tile.view.from === "pictures" ? "edited from pictures" : "from the appearance"
                }
                MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: rv.openId = tile.isOpen ? 0 : tile.view.id }
            }
        }
        // Add a view: the usual ones for this kind, or your own.
        Item {
            width: rv.tileWidth; height: rv.tileHeight + 34
            Rectangle {
                objectName: "addViewTile_" + rv.owner
                width: rv.tileWidth; height: rv.tileHeight; radius: 6
                color: addMouse.containsMouse ? "#1a262d" : "transparent"
                border.color: "#34434a"
                Text { anchors.centerIn: parent; text: "+ view"; color: "#8fa0a4"; font.pixelSize: 12 }
                MouseArea { id: addMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: addMenu.popup(parent, 0, parent.height + 4) }
                ToolTip.visible: addMouse.containsMouse; ToolTip.delay: 600
                ToolTip.text: "Another way of seeing it: an angle, the back, a detail, or a sheet with several in one picture"
                Menu {
                    id: addMenu
                    objectName: "addViewMenu_" + rv.owner
                    palette.window: "#1d2930"; palette.windowText: "#edf1ef"; palette.light: "#2f4a52"; palette.mid: "#34434a"; palette.dark: "#87dec7"
                    Repeater {
                        model: rv.suggested
                        MenuItem {
                            required property var modelData
                            objectName: "suggestView_" + rv.owner + "_" + modelData.name
                            text: modelData.name
                            onTriggered: rv.addView(modelData)
                        }
                    }
                    MenuSeparator { visible: rv.suggested.length > 0 }
                    MenuItem {
                        objectName: "addOwnView_" + rv.owner
                        text: "Your own…"
                        onTriggered: rv.addView({name: "View " + (rv.views.length + 1), framing: "", role: "other", from: rv.views.length ? "pictures" : "prompt"})
                    }
                }
            }
        }
    }

    // ---------------------------------------------------------------- the open view
    Rectangle {
        id: panel
        objectName: "viewPanel_" + rv.owner
        visible: rv.open !== null
        Layout.fillWidth: true
        implicitHeight: panelBody.implicitHeight + 24
        radius: 6; color: "#152027"; border.color: "#26343b"
        readonly property var view: rv.open || ({id: 0, name: "", fields: {}, pictures: [], chosen: {}, from: "prompt", role: "", missingInputs: 0})
        readonly property var inputs: (editor.plan.references, (view.fields.inputs || []).map(i => editor.plan.item("reference", i)).filter(r => r.id))
        ColumnLayout {
            id: panelBody
            x: 12; y: 12; width: parent.width - 24
            spacing: 8
            RowLayout {
                Layout.fillWidth: true; spacing: 8
                PlanText {
                    objectName: "viewName_" + rv.owner + "_" + rv.openIndex
                    Layout.preferredWidth: 180; Layout.fillWidth: false
                    value: panel.view.name
                    onCommit: t => { if (t.trim()) editor.plan.run({op: "set", kind: "view", id: panel.view.id, fields: {name: t}}) }
                }
                // Drawn from the appearance, or edited from picked pictures: click to switch.
                Rectangle {
                    objectName: "viewFrom_" + rv.owner + "_" + rv.openIndex
                    implicitWidth: fromText.implicitWidth + 12; implicitHeight: 20; radius: 3
                    color: fromMouse.containsMouse ? "#2c3858" : "#232c45"
                    Text { id: fromText; anchors.centerIn: parent; font.pixelSize: 11; color: "#b6c4ef"
                           text: panel.view.role === "sheet" && (panel.view.fields.panels || []).length ? "composed from " + panel.view.fields.panels.length + " views"
                                 : panel.view.from === "pictures" ? "edited from pictures" : "drawn from the appearance" }
                    MouseArea { id: fromMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: editor.plan.run({op: "set", kind: "view", id: panel.view.id,
                                                            fields: {from: panel.view.from === "pictures" ? "prompt" : "pictures"}}) }
                    ToolTip.visible: fromMouse.containsMouse; ToolTip.delay: 400
                    ToolTip.text: panel.view.from === "pictures" ? "Click to draw it from the appearance instead" : "Click to edit it from pictures you pick instead"
                }
                Item { Layout.fillWidth: true }
                Text {
                    objectName: "viewAsk_" + rv.owner + "_" + rv.openIndex
                    text: "Ask"; color: askMouse.containsMouse ? "#adb8ba" : "#6f8085"; font.pixelSize: 11
                    MouseArea { id: askMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: rv.Window.window.askAgent("view", panel.view.id, "framing", panel.view.from === "pictures" ? "Instruction" : "Framing") }
                }
                Text {
                    objectName: "viewRemove_" + rv.owner + "_" + rv.openIndex
                    text: "Remove view"; color: rmMouse.containsMouse ? "#f0a08f" : "#6f8085"; font.pixelSize: 11
                    MouseArea { id: rmMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: editor.plan.run({op: "remove", kind: "view", id: panel.view.id}) }
                    ToolTip.visible: rmMouse.containsMouse; ToolTip.delay: 400
                    ToolTip.text: panel.view.pictures.length ? "Removes it and its " + panel.view.pictures.length + " pictures (" + Kbd.mod() + "Z brings them back)" : "Remove this view"
                }
            }
            // The framing (or instruction), read as a sentence; "edit" opens it.
            RowLayout {
                visible: !rv.editingFraming
                Layout.fillWidth: true; spacing: 6
                Text {
                    objectName: "viewFramingText_" + rv.owner + "_" + rv.openIndex
                    Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideRight
                    font.pixelSize: 12
                    text: panel.view.fields.framing || (panel.view.from === "pictures" ? "No instruction yet: say what to make from the picked pictures." : "No framing yet: angle, distance, backdrop and light.")
                    color: panel.view.fields.framing ? "#adb8ba" : "#6f8085"
                }
                Text {
                    objectName: "viewFramingEdit_" + rv.owner + "_" + rv.openIndex
                    Layout.alignment: Qt.AlignTop
                    text: "edit"; color: editMouse.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 11
                    MouseArea { id: editMouse; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: { rv.editingFraming = true; Qt.callLater(() => framingEditor.input.forceActiveFocus()) } }
                }
            }
            PlanText {
                id: framingEditor
                objectName: "viewFraming_" + rv.owner + "_" + rv.openIndex
                visible: rv.editingFraming
                multiline: true; lines: 2
                value: panel.view.fields.framing
                placeholder: panel.view.from === "pictures" ? "What to make from the picked pictures: angle, distance, backdrop; what to keep and change"
                                                           : "Angle, distance, backdrop and light"
                onCommit: t => editor.plan.run({op: "set", kind: "view", id: panel.view.id, fields: {framing: t}})
                Connections { target: framingEditor.input; function onActiveFocusChanged() { if (!framingEditor.input.activeFocus) rv.editingFraming = false } }
            }
            // The pictures it is made from, in order (Generate ▸ With options… to change them).
            RowLayout {
                visible: panel.view.from === "pictures" && panel.view.role !== "sheet" || panel.view.role === "sheet" && panel.inputs.length > 0
                spacing: 4
                Text { text: "Made from"; color: "#8fa0a4"; font.pixelSize: 11; rightPadding: 4 }
                Repeater {
                    model: panel.inputs
                    Rectangle {
                        required property var modelData
                        required property int index
                        width: 30; height: 30; radius: 3; clip: true; color: "#0c1216"; border.color: "#5c7078"
                        Image { anchors.fill: parent; anchors.margins: 1; source: parent.modelData.url || ""; fillMode: Image.PreserveAspectCrop
                                asynchronous: true; sourceSize: Qt.size(60, 60) }
                        Rectangle { width: 13; height: 13; radius: 6; color: "#87dec7"; x: 1; y: 1
                                    Text { anchors.centerIn: parent; text: parent.parent.index + 1; color: "#122328"; font.pixelSize: 8 } }
                    }
                }
                Text {
                    objectName: "viewPick_" + rv.owner + "_" + rv.openIndex
                    text: panel.inputs.length ? "change" : "pick pictures…"
                    color: pickMouse.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 11; leftPadding: 4
                    MouseArea { id: pickMouse; anchors.fill: parent; anchors.margins: -3; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: rv.Window.window.openGenerate(rv.owner, rv.ownerId, {view: panel.view.id}) }
                }
                Text { visible: panel.view.missingInputs > 0; text: "a picked picture was removed"; color: "#e7b75f"; font.pixelSize: 10; leftPadding: 6 }
            }
            // A sheet can be laid out from other views' chosen pictures, here, exactly.
            ColumnLayout {
                visible: panel.view.role === "sheet"
                Layout.fillWidth: true; spacing: 4
                Text {
                    objectName: "viewCompose_" + rv.owner + "_" + rv.openIndex
                    text: rv.composing === panel.view.id ? "Cancel composing" : "Compose from views…"
                    color: compMouse.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 11
                    MouseArea { id: compMouse; anchors.fill: parent; anchors.margins: -3; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (rv.composing === panel.view.id) { rv.composing = null; return }
                                    rv.ticked = (panel.view.fields.panels || []).slice()
                                    if (!rv.ticked.length) rv.ticked = rv.views.filter(v => v.role !== "sheet" && v.chosen.id).map(v => v.id)
                                    rv.composing = panel.view.id
                                } }
                }
                ColumnLayout {
                    visible: rv.composing === panel.view.id
                    Layout.fillWidth: true; spacing: 4
                    Text { text: "Lay these views' chosen pictures side by side, no labels:"; color: "#adb8ba"; font.pixelSize: 11 }
                    Flow {
                        Layout.fillWidth: true; spacing: 6
                        Repeater {
                            model: rv.views.filter(v => v.id !== panel.view.id)
                            Rectangle {
                                id: tick
                                required property var modelData
                                readonly property bool on: rv.ticked.indexOf(modelData.id) >= 0
                                readonly property bool usable: !!modelData.chosen.id
                                objectName: "composeTick_" + modelData.name
                                implicitWidth: tickText.implicitWidth + 16; implicitHeight: 22; radius: 11
                                color: on ? "#1f3a35" : "#26323a"; opacity: usable ? 1 : 0.5
                                Text { id: tickText; anchors.centerIn: parent; font.pixelSize: 11; color: tick.on ? "#87dec7" : "#8fa0a4"
                                       text: (tick.on ? "✓ " : "") + tick.modelData.name + (tick.usable ? "" : " (nothing chosen)") }
                                MouseArea { anchors.fill: parent; enabled: tick.usable; cursorShape: Qt.PointingHandCursor
                                            onClicked: { const t = rv.ticked.slice(); const i = t.indexOf(tick.modelData.id)
                                                         if (i >= 0) t.splice(i, 1); else t.push(tick.modelData.id)
                                                         rv.ticked = rv.views.map(v => v.id).filter(id => t.indexOf(id) >= 0) } }
                            }
                        }
                        Rectangle {
                            objectName: "composeGo"
                            implicitWidth: goText.implicitWidth + 16; implicitHeight: 22; radius: 4
                            color: rv.ticked.length >= 2 ? "#87dec7" : "#2b4a44"
                            Text { id: goText; anchors.centerIn: parent; text: "Compose"; font.pixelSize: 11; color: rv.ticked.length >= 2 ? "#122328" : "#4b6b66" }
                            MouseArea { anchors.fill: parent; enabled: rv.ticked.length >= 2; cursorShape: Qt.PointingHandCursor
                                        onClicked: { const why = editor.generator.composeSheet(panel.view.id, rv.ticked)
                                                     composeNote.text = why; if (!why) rv.composing = null } }
                        }
                    }
                    Text { id: composeNote; objectName: "composeNote"; visible: text.length > 0; color: "#e7b75f"; font.pixelSize: 11; wrapMode: Text.Wrap; Layout.fillWidth: true }
                }
            }
            ReferenceStrip {
                objectName: "viewRefs_" + rv.owner + "_" + rv.openIndex
                owner: rv.owner; ownerId: rv.ownerId; viewId: panel.view.id; size: rv.size >= 80 ? 72 : 56
            }
        }
    }
}
