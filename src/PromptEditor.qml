import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A large editor for one long prompt (a character's, look's, location's or prop's picture prompt), with
// what it should follow beside it: the item's own description and the concept's look and rules.
// Save (⌘Return) writes it as one undo step, like any field; Cancel (Esc) leaves it as it was.
Popup {
    id: pe
    objectName: "promptEditor"
    modal: true
    padding: 20
    anchors.centerIn: Overlay.overlay
    width: Math.min(1000, (Overlay.overlay ? Overlay.overlay.width : 1000) - 60)
    height: Math.min(720, (Overlay.overlay ? Overlay.overlay.height : 720) - 60)
    closePolicy: Popup.NoAutoClose
    background: Rectangle { color: "#1d2930"; radius: 6; border.color: "#34434a" }

    property string kind
    property int itemId
    property string field: "prompt"
    property string label: "Picture prompt"
    readonly property var plan: editor.plan
    readonly property var item: (plan.revision, kind ? plan.item(kind, itemId) : ({}))
    readonly property var fields: item && item.fields ? item.fields : ({})
    readonly property var prov: item && item.prov && item.prov[field] ? item.prov[field] : null
    readonly property var owner: kind === "look" && item.character ? plan.item("character", item.character) : null
    readonly property string title: {
        if (!item || !item.fields) return ""
        const name = fields.name || (kind === "location" ? "Unnamed location" : kind === "prop" ? "Unnamed prop" : kind === "look" ? "Unnamed look" : "Unnamed character")
        return kind === "look" && owner ? (owner.fields.name || "Unnamed character") + " · " + name : name
    }
    readonly property int words: { const t = area.text.trim(); return t ? t.split(/\s+/).length : 0 }

    function edit(kind, id, field, label) {
        pe.kind = kind; pe.itemId = id; pe.field = field; pe.label = label || "Picture prompt"
        open()
    }
    function save() {
        const before = fields[field] || ""
        if (area.text !== before) {
            const f = {}
            f[field] = area.text
            plan.run({op: "set", kind: kind, id: itemId, fields: f})
        }
        close()
    }
    onOpened: { area.text = fields[field] || ""; area.forceActiveFocus(); area.cursorPosition = area.length }
    // The item went away (removed, or undone): nothing left to edit.
    onItemChanged: if (opened && (!item || !item.fields)) close()

    // What the prompt should follow, as [{label, text}].
    readonly property var context: {
        const out = []
        const add = (label, text) => { if (text !== undefined && text !== null && String(text).trim()) out.push({label: label, text: String(text)}) }
        const opts = plan.options
        const optionLabel = (key, value) => { const o = (opts[key] || []).find(x => x.value === value); return o ? o.label : value }
        if (kind === "character") {
            add("Identity", fields.identity); add("Role", optionLabel("role", fields.role)); add("Notes", fields.notes)
        } else if (kind === "look" && owner) {
            add("Wardrobe and makeup", fields.description)
            add((owner.fields.name || "The character") + "'s identity", owner.fields.identity)
            add("The character's picture prompt", owner.fields.prompt)
        } else if (kind === "location") {
            add("Description", fields.description); add("Time of day", optionLabel("timeOfDay", fields.timeOfDay)); add("Lighting", fields.lighting)
        } else if (kind === "prop") {
            add("Notes", fields.notes)
        }
        const c = plan.concept.fields || {}
        add("Visual style", c.style); add("Palette", c.palette); add("Lighting (concept)", c.lighting)
        add("Era", c.era); add("Lens and format", c.lens); add("Grade", c.grade); add("Aspect ratio", c.aspect)
        if (c.rules && c.rules.length) add("Rules", Array.from(c.rules).map(r => "• " + r).join("\n"))
        return out
    }

    component SheetButton: Button {
        id: btn
        property bool active: false
        focusPolicy: Qt.NoFocus
        implicitHeight: 30; leftPadding: 12; rightPadding: 12
        contentItem: Text { text: btn.text; color: btn.active ? "#122328" : "#edf1ef"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: btn.active ? "#87dec7" : (btn.hovered ? "#3b4b52" : "#2b383f"); radius: 5 }
    }

    contentItem: ColumnLayout {
        spacing: 10
        RowLayout {
            Layout.fillWidth: true; spacing: 10
            ColumnLayout {
                Layout.fillWidth: true; spacing: 1
                Text { text: pe.label; color: "#8fa0a4"; font.pixelSize: 12 }
                Text { objectName: "promptEditorTitle"; text: pe.title; color: "#edf1ef"; font.pixelSize: 17; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
            }
            Rectangle {
                visible: pe.prov !== null && pe.prov.by === "agent"
                implicitWidth: tag.implicitWidth + 10; implicitHeight: 18; radius: 3; color: "#2f3d5c"
                Text { id: tag; anchors.centerIn: parent; text: "written by an agent"; color: "#b6c4ef"; font.pixelSize: 10 }
            }
            Rectangle {
                visible: pe.prov !== null && pe.prov.locked === true
                implicitWidth: lockTag.implicitWidth + 10; implicitHeight: 18; radius: 3; color: "#1f3a35"
                Text { id: lockTag; anchors.centerIn: parent; text: "locked against agents"; color: "#87dec7"; font.pixelSize: 10 }
            }
            SheetButton { objectName: "promptCancel"; text: "Cancel"; onClicked: pe.close() }
            SheetButton { objectName: "promptSave"; text: "Save"; active: true; onClicked: pe.save() }
        }
        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            spacing: 16
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true
                spacing: 6
                ScrollView {
                    id: scroller
                    Layout.fillWidth: true; Layout.fillHeight: true
                    contentWidth: availableWidth
                    background: Rectangle { radius: 4; color: "#162027"; border.color: area.activeFocus ? "#87dec7" : "#34434a" }
                    TextArea {
                        id: area
                        objectName: "promptEditorText"
                        width: scroller.availableWidth
                        wrapMode: TextEdit.Wrap
                        selectByMouse: true
                        color: "#edf1ef"; font.pixelSize: 14
                        leftPadding: 12; rightPadding: 12; topPadding: 10; bottomPadding: 10
                        placeholderText: "Describe only what can be seen. Keep to the style, palette and rules on the right."
                        placeholderTextColor: "#6f8085"
                        background: null
                        Keys.onPressed: e => {
                            if ((e.key === Qt.Key_Return || e.key === Qt.Key_Enter) && (e.modifiers & Qt.ControlModifier)) { pe.save(); e.accepted = true }
                            if (e.key === Qt.Key_Escape) { pe.close(); e.accepted = true }
                        }
                    }
                }
                Text {
                    objectName: "promptEditorCount"
                    text: pe.words + (pe.words === 1 ? " word" : " words") + "  ·  " + area.length + " characters  ·  ⌘Return saves, Esc cancels"
                    color: "#6f8085"; font.pixelSize: 11
                }
            }
            // ------------------------------------------------------------ what it follows
            ScrollView {
                id: side
                Layout.preferredWidth: 320; Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true
                ColumnLayout {
                    objectName: "promptContext"
                    width: side.availableWidth
                    spacing: 10
                    Text { text: "WHAT IT FOLLOWS"; color: "#6f8085"; font.pixelSize: 11; font.letterSpacing: 0.6 }
                    Repeater {
                        model: pe.context
                        ColumnLayout {
                            required property var modelData
                            Layout.fillWidth: true; spacing: 2
                            Text { text: parent.modelData.label; color: "#8fa0a4"; font.pixelSize: 11 }
                            TextEdit {
                                Layout.fillWidth: true
                                text: parent.modelData.text; readOnly: true; selectByMouse: true
                                wrapMode: TextEdit.Wrap; color: "#c9d2d3"; font.pixelSize: 12
                            }
                        }
                    }
                    Text {
                        visible: pe.context.length === 0
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        text: "Nothing to follow yet: fill in the description here and the concept's look and rules."
                        color: "#6f8085"; font.pixelSize: 12
                    }
                }
            }
        }
    }
}
