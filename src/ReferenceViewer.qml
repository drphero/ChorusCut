import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One reference picture, large: a character's, look's or location's pictures one at a time
// (← and → step through them), with what can be done to it beside it: approve, role, order,
// remove. A generated picture shows what it was made with (workflow, seed, settings, prompt),
// and Generate again… opens the Generate dialog with those, the seed fixed.
// Click the picture to see it at full size and drag to look around; click again to fit.
Popup {
    id: viewer
    objectName: "refViewer"
    modal: true
    padding: 0
    x: 24; y: 24
    width: (Overlay.overlay ? Overlay.overlay.width : 1200) - 48
    height: (Overlay.overlay ? Overlay.overlay.height : 800) - 48
    background: Rectangle { color: "#0f171b"; radius: 8; border.color: "#34434a" }
    Overlay.modal: Rectangle { color: "#c8060a0c" }

    property string owner
    property int ownerId
    property int index: 0
    property bool zoomed: false
    property bool confirmRemove: false
    readonly property var refs: (editor.plan.references, owner ? editor.plan.referencesFor(owner, ownerId) : [])
    readonly property var ref: refs.length ? refs[Math.max(0, Math.min(index, refs.length - 1))] : null
    readonly property var gen: ref && ref.generation ? ref.generation : null
    property string ownerName
    // Generate again…: the window opens the Generate dialog with these.
    signal generateAgain(string owner, int ownerId, var prefill)

    function show(owner, ownerId, index, name) {
        viewer.owner = owner; viewer.ownerId = ownerId; viewer.index = index; viewer.ownerName = name
        open()
    }
    function step(d) {
        if (refs.length < 2) return
        index = (index + d + refs.length) % refs.length
    }
    onIndexChanged: { zoomed = false; confirmRemove = false }
    onOpened: { zoomed = false; confirmRemove = false; keys.forceActiveFocus() }
    // Everything removed (or undone away): nothing left to show.
    onRefsChanged: if (opened && refs.length === 0) close()
    function set(fields) { editor.plan.run({op: "set", kind: "reference", id: ref.id, fields: fields}) }
    function copy(text) { copier.text = text; copier.selectAll(); copier.copy() }
    function shown(v) { return typeof v === "number" ? String(Math.round(v * 10000) / 10000) : typeof v === "boolean" ? (v ? "on" : "off") : String(v) }

    TextEdit { id: copier; visible: false }

    component Small: Button {
        id: sb
        property bool active: false
        property bool danger: false
        focusPolicy: Qt.NoFocus
        implicitHeight: 28; leftPadding: 10; rightPadding: 10
        contentItem: Text { text: sb.text; color: !sb.enabled ? "#5c7078" : sb.active ? "#122328" : sb.danger ? "#f0a08f" : "#edf1ef"; font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: 4; color: sb.active ? (sb.hovered ? "#9fe3d0" : "#87dec7") : sb.hovered && sb.enabled ? "#3b4b52" : "#2b383f" }
    }
    component Label: Text { color: "#6f8085"; font.pixelSize: 11; font.letterSpacing: 0.5 }

    contentItem: Item {
        id: keys
        focus: true
        Keys.onLeftPressed: viewer.step(-1)
        Keys.onRightPressed: viewer.step(1)
        Keys.onEscapePressed: viewer.close()
        Keys.onSpacePressed: viewer.close()

        RowLayout {
            anchors.fill: parent
            spacing: 0

            // ------------------------------------------------------------ the picture
            Item {
                Layout.fillWidth: true; Layout.fillHeight: true
                clip: true
                Flickable {
                    id: flick
                    objectName: "refViewerPicture"
                    anchors.fill: parent; anchors.margins: 16
                    contentWidth: viewer.zoomed ? Math.max(width, big.implicitWidth) : width
                    contentHeight: viewer.zoomed ? Math.max(height, big.implicitHeight) : height
                    interactive: viewer.zoomed
                    boundsBehavior: Flickable.StopAtBounds
                    Image {
                        id: big
                        width: flick.contentWidth; height: flick.contentHeight
                        source: viewer.ref && !viewer.ref.missing ? viewer.ref.url : ""
                        fillMode: viewer.zoomed ? Image.Pad : Image.PreserveAspectFit
                        asynchronous: true
                        smooth: true; mipmap: true
                        autoTransform: true
                        // Opening the full size centres it.
                        onStatusChanged: if (status === Image.Ready && viewer.zoomed) Qt.callLater(viewer.centre)
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: viewer.zoomed ? Qt.OpenHandCursor : Qt.PointingHandCursor
                        enabled: !!viewer.ref && !viewer.ref.missing
                        onClicked: { viewer.zoomed = !viewer.zoomed; if (viewer.zoomed) Qt.callLater(viewer.centre) }
                    }
                }
                Text {
                    anchors.centerIn: parent
                    visible: !!viewer.ref && viewer.ref.missing
                    text: "This picture's file is missing."; color: "#f0a08f"; font.pixelSize: 14
                }
                BusyIndicator { anchors.centerIn: parent; running: big.status === Image.Loading; visible: running }
                // Previous and next, at the sides.
                Repeater {
                    model: viewer.refs.length > 1 ? [-1, 1] : []
                    Rectangle {
                        required property int modelData
                        objectName: modelData < 0 ? "refViewerPrev" : "refViewerNext"
                        anchors.verticalCenter: parent.verticalCenter
                        x: modelData < 0 ? 12 : parent.width - width - 12
                        width: 40; height: 64; radius: 6
                        color: arrowMouse.containsMouse ? "#cc2b383f" : "#801d2930"
                        Text { anchors.centerIn: parent; text: parent.modelData < 0 ? "‹" : "›"; color: "#edf1ef"; font.pixelSize: 30 }
                        MouseArea { id: arrowMouse; anchors.fill: parent; hoverEnabled: true; onClicked: viewer.step(parent.modelData) }
                    }
                }
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter; y: parent.height - height - 14
                    visible: viewer.refs.length > 0
                    width: counter.implicitWidth + 16; height: 22; radius: 11; color: "#b31d2930"
                    Text {
                        id: counter
                        objectName: "refViewerCounter"
                        anchors.centerIn: parent
                        text: (Math.min(viewer.index, viewer.refs.length - 1) + 1) + " of " + viewer.refs.length
                              + (viewer.zoomed ? "  ·  full size" : "") + "   ← →"
                        color: "#adb8ba"; font.pixelSize: 11
                    }
                }
            }
            Rectangle { Layout.fillHeight: true; width: 1; color: "#26343b" }

            // ------------------------------------------------------------ about it
            ScrollView {
                id: side
                Layout.preferredWidth: 320; Layout.fillHeight: true
                contentWidth: availableWidth
                clip: true
                ColumnLayout {
                    x: 16; width: side.availableWidth - 32
                    spacing: 10
                    Item { height: 6 }
                    RowLayout {
                        Layout.fillWidth: true
                        Text { text: viewer.ownerName; color: "#9fe3d0"; font.pixelSize: 15; font.weight: Font.DemiBold; elide: Text.ElideRight; Layout.fillWidth: true }
                        Small { objectName: "refViewerClose"; text: "Close"; onClicked: viewer.close() }
                    }
                    Text {
                        visible: !!viewer.ref
                        Layout.fillWidth: true; wrapMode: Text.WrapAnywhere; maximumLineCount: 2; elide: Text.ElideRight
                        text: viewer.ref ? viewer.ref.name + "\n" + viewer.ref.width + " × " + viewer.ref.height : ""
                        color: "#8fa0a4"; font.pixelSize: 12
                    }
                    // Approval first: it is what a new picture is waiting for.
                    RowLayout {
                        Layout.fillWidth: true; spacing: 8
                        visible: !!viewer.ref
                        Rectangle {
                            implicitWidth: stateText.implicitWidth + 12; implicitHeight: 20; radius: 3
                            color: viewer.ref && viewer.ref.approved ? "#1f3a35" : "#4a3a1c"
                            Text {
                                id: stateText
                                objectName: "refViewerState"
                                anchors.centerIn: parent; font.pixelSize: 11
                                color: viewer.ref && viewer.ref.approved ? "#87dec7" : "#e7b75f"
                                text: viewer.ref ? (viewer.ref.approved ? "Approved" : "To approve") : ""
                            }
                        }
                        Item { Layout.fillWidth: true }
                        Small {
                            objectName: "refViewerApprove"
                            active: viewer.ref && !viewer.ref.approved
                            text: viewer.ref && viewer.ref.approved ? "Unapprove" : "Approve"
                            onClicked: viewer.set({approved: !viewer.ref.approved})
                        }
                    }
                    Label { text: "ROLE" }
                    PlanCombo {
                        objectName: "refViewerRole"
                        allowUnset: false
                        options: editor.plan.options.refRole
                        value: viewer.ref ? viewer.ref.role : ""
                        onPicked: v => viewer.set({role: v})
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 6
                        Small { objectName: "refViewerEarlier"; text: "Move earlier"; enabled: viewer.index > 0
                                onClicked: { editor.plan.run({op: "ref.move", id: viewer.ref.id, position: viewer.index - 1}); viewer.index-- } }
                        Small { objectName: "refViewerLater"; text: "Move later"; enabled: viewer.index < viewer.refs.length - 1
                                onClicked: { editor.plan.run({op: "ref.move", id: viewer.ref.id, position: viewer.index + 1}); viewer.index++ } }
                        Item { Layout.fillWidth: true }
                        Small {
                            objectName: "refViewerRemove"
                            danger: true
                            text: viewer.confirmRemove ? "Remove it?" : "Remove"
                            onClicked: {
                                if (!viewer.confirmRemove) { viewer.confirmRemove = true; return }
                                const at = viewer.index
                                editor.plan.run({op: "remove", kind: "reference", id: viewer.ref.id})
                                viewer.index = Math.max(0, Math.min(at, viewer.refs.length - 1))
                                viewer.confirmRemove = false
                            }
                        }
                    }
                    Rectangle { Layout.fillWidth: true; height: 1; color: "#26343b"; Layout.topMargin: 4 }

                    // What a generated picture was made with.
                    ColumnLayout {
                        Layout.fillWidth: true; spacing: 8
                        visible: viewer.gen !== null
                        Label { text: "GENERATED" }
                        Text {
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                            text: viewer.gen ? (viewer.gen.use === "ref-edit" ? "From approved pictures" : "From the picture prompt") + " with " + viewer.gen.workflowName : ""
                            color: "#edf1ef"; font.pixelSize: 12
                        }
                        RowLayout {
                            Layout.fillWidth: true; spacing: 8
                            visible: viewer.gen !== null && viewer.gen.seed !== undefined
                            Text { text: "Seed"; color: "#8fa0a4"; font.pixelSize: 12; Layout.preferredWidth: 92 }
                            TextEdit {
                                objectName: "refViewerSeed"
                                Layout.fillWidth: true; readOnly: true; selectByMouse: true
                                text: viewer.gen && viewer.gen.seed !== undefined ? String(viewer.gen.seed) : ""
                                color: "#edf1ef"; font.pixelSize: 12; font.family: "Menlo"
                            }
                            Small { objectName: "refViewerCopySeed"; text: "Copy"; onClicked: viewer.copy(String(viewer.gen.seed)) }
                        }
                        Repeater {
                            model: viewer.gen && viewer.gen.settings ? viewer.gen.settings : []
                            RowLayout {
                                required property var modelData
                                Layout.fillWidth: true; spacing: 8
                                Text { text: parent.modelData.label; color: "#8fa0a4"; font.pixelSize: 12; Layout.preferredWidth: 92; elide: Text.ElideRight }
                                Text { text: viewer.shown(parent.modelData.value); color: "#edf1ef"; font.pixelSize: 12; Layout.fillWidth: true; elide: Text.ElideRight }
                            }
                        }
                        Text { text: "Prompt"; color: "#8fa0a4"; font.pixelSize: 12 }
                        TextEdit {
                            objectName: "refViewerPrompt"
                            Layout.fillWidth: true; readOnly: true; selectByMouse: true; wrapMode: TextEdit.Wrap
                            text: viewer.gen ? viewer.gen.prompt : ""
                            color: "#d5dcdb"; font.pixelSize: 12
                        }
                        Small {
                            objectName: "refViewerAgain"
                            Layout.topMargin: 4
                            text: "Generate again…"
                            onClicked: {
                                const settings = {}
                                for (const s of (viewer.gen.settings || [])) settings[s.key] = s.value
                                const o = viewer.owner, id = viewer.ownerId
                                const prefill = {use: viewer.gen.use, workflow: viewer.gen.workflow, settings: settings}
                                if (viewer.gen.seed !== undefined) prefill.seed = viewer.gen.seed
                                viewer.close()
                                viewer.generateAgain(o, id, prefill)
                            }
                        }
                        Text {
                            Layout.fillWidth: true; wrapMode: Text.Wrap
                            text: "Opens Generate with this workflow, seed and settings. Keep the seed and change the prompt or a setting to see what that does to the same picture."
                            color: "#6f8085"; font.pixelSize: 11
                        }
                    }
                    Text {
                        visible: viewer.gen === null && !!viewer.ref
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        text: viewer.ref ? (viewer.ref.source === "agent" ? "Added by your agent." : "Added from a file.") : ""
                        color: "#6f8085"; font.pixelSize: 12
                    }
                    Item { height: 12 }
                }
            }
        }
    }
    function centre() {
        flick.contentX = Math.max(0, (flick.contentWidth - flick.width) / 2)
        flick.contentY = Math.max(0, (flick.contentHeight - flick.height) / 2)
    }
}
