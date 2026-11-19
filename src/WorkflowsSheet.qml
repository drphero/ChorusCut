import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// The workflow library: the user's ComfyUI workflows and their adapters, shared by every
// project. Import one exported with Workflow ▸ Export (API); Chorus Cut guesses which inputs
// take the prompt, pictures, seed and size, and each guess can be changed here.
Popup {
    id: sheet
    objectName: "workflowsSheet"
    modal: true
    padding: 20
    anchors.centerIn: parent
    width: Math.min(940, parent.width - 60)
    height: Math.min(680, parent.height - 60)
    background: Rectangle { color: "#1d2930"; radius: 6; border.color: "#34434a" }

    readonly property var flows: editor.workflows
    property string selected: ""
    property string note: ""
    property bool allInputs: false
    property bool confirmRemove: false
    readonly property var detail: {
        flows.list
        const d = selected ? flows.get(selected) : null
        return d && d.id ? d : null
    }
    onOpened: { note = ""; if (!selected && flows.list.length) selected = flows.list[0].id }
    onSelectedChanged: { confirmRemove = false; allInputs = false; if (selected) editor.generator.fetchChoices(selected) }

    // The file dialog sets this; tests call it the same way.
    function importUrl(url) {
        const r = flows.importFile(url, "")
        if (r.ok) { selected = r.id; note = "" }
        else note = r.error
    }
    // The slots an input can take, as combo entries.
    readonly property var slotChoices: {
        const out = [{label: "Keep its value", slot: "", index: 0}]
        for (const t of flows.slotTypes) {
            if (t.indexed) for (let i = 0; i < 4; ++i) out.push({label: t.label + " " + (i + 1), slot: t.value, index: i})
            else out.push({label: t.label, slot: t.value, index: 0})
        }
        return out
    }

    component SheetButton: Button {
        id: btn
        property bool active: false
        property bool danger: false
        focusPolicy: Qt.NoFocus
        implicitHeight: 30; leftPadding: 12; rightPadding: 12
        contentItem: Text { text: btn.text; color: btn.active ? "#122328" : btn.danger ? "#f0a08f" : "#edf1ef"; font.pixelSize: 13
                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: btn.active ? "#87dec7" : (btn.hovered ? "#3b4b52" : "#2b383f"); radius: 5 }
    }
    // A dark combo like the plan's, over [{label}] entries.
    component SheetCombo: ComboBox {
        id: sc
        implicitHeight: 28
        textRole: "label"
        contentItem: Text {
            leftPadding: 8; rightPadding: 20
            text: sc.displayText; color: sc.currentIndex <= 0 && sc.dimFirst ? "#8fa0a4" : "#edf1ef"
            font.pixelSize: 13; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
        }
        property bool dimFirst: false
        background: Rectangle { radius: 4; color: sc.hovered ? "#22313a" : "#162027"; border.color: sc.activeFocus ? "#87dec7" : "#34434a" }
        indicator: Text { x: sc.width - width - 8; anchors.verticalCenter: parent.verticalCenter; text: "▾"; color: "#adb8ba" }
        delegate: ItemDelegate {
            required property var modelData
            required property int index
            width: sc.width
            highlighted: sc.highlightedIndex === index
            contentItem: Text { text: modelData.label; color: index === 0 && sc.dimFirst ? "#8fa0a4" : "#edf1ef"; font.pixelSize: 13; elide: Text.ElideRight }
            background: Rectangle { color: highlighted ? "#2f4a52" : "#1d2930" }
        }
        popup.background: Rectangle { color: "#1d2930"; border.color: "#34434a"; radius: 4 }
    }
    component Caption: Text { color: "#8fa0a4"; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }

    FileDialog {
        id: picker
        title: "Import a ComfyUI workflow (Export (API) format)"
        nameFilters: ["ComfyUI workflow (*.json)"]
        onAccepted: sheet.importUrl(selectedFile)
    }

    contentItem: ColumnLayout {
        spacing: 10
        RowLayout {
            Layout.fillWidth: true
            Text { text: "Workflows"; color: "#edf1ef"; font.pixelSize: 17; font.bold: true; Layout.fillWidth: true }
            SheetButton { objectName: "workflowsImport"; text: "Import…"; onClicked: picker.open() }
            SheetButton { objectName: "workflowsDone"; text: "Done"; active: true; onClicked: sheet.close() }
        }
        Caption {
            text: "Your ComfyUI workflows, and which input takes what from Chorus Cut. In ComfyUI, open a workflow and choose Workflow ▸ Export (API), then import that file. They are kept for every project in " + sheet.flows.folder + "."
        }
        Text {
            objectName: "workflowsNote"
            visible: sheet.note.length > 0
            Layout.fillWidth: true; wrapMode: Text.Wrap
            text: sheet.note; color: "#e7b75f"; font.pixelSize: 12
        }

        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            spacing: 14

            // ------------------------------------------------------------ the library
            ColumnLayout {
                Layout.preferredWidth: 250; Layout.fillHeight: true
                spacing: 4
                Text {
                    visible: sheet.flows.list.length === 0
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: "No workflows yet. Import a text-to-image workflow to generate reference pictures from their prompts, and an image-editing one to make new pictures from approved ones."
                    color: "#6f8085"; font.pixelSize: 12
                }
                Repeater {
                    model: sheet.flows.list
                    Button {
                        id: entry
                        required property var modelData
                        required property int index
                        objectName: "workflowEntry" + index
                        Layout.fillWidth: true
                        implicitHeight: 48
                        focusPolicy: Qt.NoFocus
                        readonly property bool current: sheet.selected === modelData.id
                        background: Rectangle { radius: 5; color: entry.current ? "#2f4a52" : entry.hovered ? "#26343b" : "transparent" }
                        contentItem: RowLayout {
                            spacing: 8
                            Rectangle { width: 8; height: 8; radius: 4; color: entry.modelData.ready ? "#87dec7" : "#e0a24a" }
                            ColumnLayout {
                                Layout.fillWidth: true; spacing: 1
                                Text { text: entry.modelData.name; color: "#edf1ef"; font.pixelSize: 13; elide: Text.ElideRight; Layout.fillWidth: true }
                                Text { text: entry.modelData.useLabel + (entry.modelData.isDefault ? " · default" : ""); color: "#8fa0a4"; font.pixelSize: 11; elide: Text.ElideRight; Layout.fillWidth: true }
                            }
                        }
                        onClicked: sheet.selected = modelData.id
                    }
                }
                Item { Layout.fillHeight: true }
            }
            Rectangle { Layout.fillHeight: true; width: 1; color: "#34434a" }

            // ------------------------------------------------------------ one workflow
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true
                spacing: 8
                visible: sheet.detail !== null
                RowLayout {
                    Layout.fillWidth: true; spacing: 10
                    TextField {
                        id: nameField
                        objectName: "workflowName"
                        Layout.fillWidth: true
                        text: sheet.detail ? sheet.detail.name : ""
                        color: "#edf1ef"; font.pixelSize: 14; selectByMouse: true
                        background: Rectangle { color: "#162027"; radius: 4; border.color: nameField.activeFocus ? "#87dec7" : "#34434a" }
                        onEditingFinished: if (sheet.detail && text.trim() && text !== sheet.detail.name) sheet.flows.rename(sheet.selected, text)
                    }
                    SheetCombo {
                        id: useCombo
                        objectName: "workflowUse"
                        Layout.preferredWidth: 280
                        model: sheet.flows.uses
                        currentIndex: sheet.detail ? sheet.flows.uses.findIndex(u => u.value === sheet.detail.use) : -1
                        onActivated: i => sheet.flows.setUse(sheet.selected, sheet.flows.uses[i].value)
                    }
                }
                RowLayout {
                    Layout.fillWidth: true; spacing: 10
                    Caption {
                        text: sheet.detail ? (sheet.detail.isDefault ? "Used for " + sheet.detail.useLabel.toLowerCase() + "." : "Not the default for " + sheet.detail.useLabel.toLowerCase() + ".")
                                             + (sheet.detail.source ? "  Imported from " + sheet.detail.source + "." : "") : ""
                    }
                    SheetButton {
                        objectName: "workflowMakeDefault"
                        visible: sheet.detail !== null && !sheet.detail.isDefault
                        text: "Use by default"
                        onClicked: sheet.flows.setDefault(sheet.detail.use, sheet.selected)
                    }
                    SheetButton {
                        objectName: "workflowRemove"
                        danger: true
                        text: sheet.confirmRemove ? "Remove from the library?" : "Remove"
                        onClicked: {
                            if (!sheet.confirmRemove) { sheet.confirmRemove = true; return }
                            sheet.flows.remove(sheet.selected)
                            sheet.selected = sheet.flows.list.length ? sheet.flows.list[0].id : ""
                        }
                    }
                }
                Repeater {
                    model: sheet.detail ? sheet.detail.problems : []
                    Text { required property var modelData; text: "•  " + modelData; color: "#e7b75f"; font.pixelSize: 12; wrapMode: Text.Wrap; Layout.fillWidth: true }
                }
                Caption {
                    visible: sheet.detail !== null && sheet.detail.ready
                    text: sheet.detail ? "Ready. Results are collected from " + (sheet.detail.outputs.join(", ") || "its outputs") + "." : ""
                    color: "#87dec7"
                }
                // How this model wants to be written to: what it calls its pictures, and notes for the agent.
                RowLayout {
                    Layout.fillWidth: true; spacing: 10
                    visible: sheet.detail !== null
                    Text { text: "Pictures are called"; color: "#8fa0a4"; font.pixelSize: 12 }
                    TextField {
                        id: nameField2
                        objectName: "workflowPictureName"
                        Layout.preferredWidth: 160
                        text: sheet.detail ? sheet.detail.pictureName : ""
                        placeholderText: sheet.detail ? (editor.generator.choices, editor.generator.pictureLabel(sheet.selected, 1)).replace(/1/, "{n}") : ""
                        placeholderTextColor: "#6f8085"
                        color: "#edf1ef"; font.pixelSize: 12; selectByMouse: true
                        background: Rectangle { color: "#162027"; radius: 4; border.color: nameField2.activeFocus ? "#87dec7" : "#34434a" }
                        onEditingFinished: if (sheet.detail && text !== sheet.detail.pictureName) sheet.flows.setConventions(sheet.selected, text, sheet.detail.notes)
                    }
                    Caption { text: "{n} is the number. Empty: as ComfyUI's nodes describe it, else \u201cpicture {n}\u201d." }
                }
                TextField {
                    id: notesField
                    objectName: "workflowNotes"
                    visible: sheet.detail !== null
                    Layout.fillWidth: true
                    text: sheet.detail ? sheet.detail.notes : ""
                    placeholderText: "Notes for your agent on writing for this model, e.g. Write edit instructions, not descriptions; under 150 words"
                    placeholderTextColor: "#6f8085"
                    color: "#edf1ef"; font.pixelSize: 12; selectByMouse: true
                    background: Rectangle { color: "#162027"; radius: 4; border.color: notesField.activeFocus ? "#87dec7" : "#34434a" }
                    onEditingFinished: if (sheet.detail && text !== sheet.detail.notes) sheet.flows.setConventions(sheet.selected, sheet.detail.pictureName, text)
                }
                RowLayout {
                    Layout.fillWidth: true
                    Text { text: "INPUTS"; color: "#6f8085"; font.pixelSize: 11; font.letterSpacing: 0.6; Layout.fillWidth: true }
                    CheckBox {
                        id: allBox
                        objectName: "workflowAllInputs"
                        text: "Show every input"
                        checked: sheet.allInputs
                        onToggled: sheet.allInputs = checked
                        focusPolicy: Qt.NoFocus
                        indicator: Rectangle {
                            x: 0; anchors.verticalCenter: parent.verticalCenter
                            width: 16; height: 16; radius: 3
                            color: allBox.checked ? "#87dec7" : "#162027"; border.color: allBox.checked ? "#87dec7" : "#5c7078"
                            Text { anchors.centerIn: parent; visible: allBox.checked; text: "✓"; color: "#122328"; font.pixelSize: 12 }
                        }
                        contentItem: Text { text: allBox.text; color: "#adb8ba"; font.pixelSize: 12; leftPadding: 22; verticalAlignment: Text.AlignVCenter }
                    }
                }
                ScrollView {
                    id: inputsView
                    Layout.fillWidth: true; Layout.fillHeight: true
                    contentWidth: availableWidth
                    clip: true
                    ColumnLayout {
                        width: inputsView.availableWidth
                        spacing: 4
                        Repeater {
                            model: sheet.detail ? sheet.detail.inputs.filter(i => sheet.allInputs || i.slot !== "") : []
                            RowLayout {
                                id: inputRow
                                required property var modelData
                                objectName: "workflowInput_" + modelData.node + "_" + modelData.input
                                Layout.fillWidth: true; spacing: 10
                                ColumnLayout {
                                    Layout.fillWidth: true; spacing: 0
                                    Text {
                                        Layout.fillWidth: true; elide: Text.ElideRight
                                        text: (inputRow.modelData.title || inputRow.modelData.classType) + "  ·  " + inputRow.modelData.input
                                        color: "#edf1ef"; font.pixelSize: 12
                                    }
                                    Text {
                                        Layout.fillWidth: true; elide: Text.ElideRight
                                        text: "node " + inputRow.modelData.node + "  ·  " + (inputRow.modelData.value === "" ? "empty" : inputRow.modelData.value)
                                        color: "#6f8085"; font.pixelSize: 11
                                    }
                                }
                                SheetCombo {
                                    dimFirst: true
                                    objectName: "workflowSlot_" + inputRow.modelData.node + "_" + inputRow.modelData.input
                                    Layout.preferredWidth: 190
                                    model: sheet.slotChoices
                                    currentIndex: sheet.slotChoices.findIndex(c => c.slot === inputRow.modelData.slot && (c.slot !== "picture" || c.index === inputRow.modelData.index))
                                    onActivated: i => sheet.flows.setSlot(sheet.selected, inputRow.modelData.node, inputRow.modelData.input,
                                                                         sheet.slotChoices[i].slot, sheet.slotChoices[i].index)
                                }
                            }
                        }
                        Caption {
                            visible: sheet.detail !== null && !sheet.allInputs && sheet.detail.inputs.every(i => i.slot === "")
                            text: "Nothing is mapped yet. Show every input and choose which one takes the prompt."
                        }
                    }
                }
            }
        }
    }
}
