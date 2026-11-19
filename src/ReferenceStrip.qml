import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// One view's pictures (a character's, look's or location's), in order: the chosen one (what the
// video model gets) and alternatives. Add them with the button or by dropping image files on the
// strip; they are copied into the project. Generate makes new ones through the user's ComfyUI
// workflows, from the prompt or from picked pictures as the view says; they appear here while
// they run and arrive waiting to be chosen. Click a picture to see it large (and choose it there);
// right-click for a menu. Every change is one undo step.
Item {
    id: strip
    property string owner
    property int ownerId
    property int viewId: 0
    property int size: 84
    readonly property var allRefs: (editor.plan.references, editor.plan.referencesFor(owner, ownerId))
    readonly property var refs: viewId ? allRefs.filter(r => r.view === viewId) : allRefs
    readonly property var views: (editor.plan.revision, editor.plan.viewsFor(owner, ownerId))
    // Generation: what this view can make now, and the jobs running for it.
    readonly property var gen: (editor.generator.jobs, editor.workflows.list, editor.plan.revision, viewId ? editor.generator.viewOptions(viewId) : ({ready: false, why: ""}))
    readonly property var pending: editor.generator.jobs.filter(j => j.owner === owner && j.ownerId === ownerId && (!viewId || j.view === viewId))
    property string note: ""
    function generate() {
        note = editor.generator.generateWith(owner, ownerId, "", {view: viewId})
    }
    readonly property var imageTypes: ["png", "jpg", "jpeg", "webp", "heic", "heif", "tif", "tiff", "bmp", "gif"]
    Layout.fillWidth: true
    implicitHeight: flow.implicitHeight + (why.visible ? why.implicitHeight + 4 : 0)

    // Several files at once are one undo step. Files that are not pictures are left out.
    function addFiles(urls) {
        const commands = []
        for (const u of urls) {
            const path = u.toString()
            if (imageTypes.indexOf(path.split(".").pop().toLowerCase()) >= 0)
                commands.push(viewId ? {op: "ref.add", owner: owner, ownerId: ownerId, path: path, view: viewId} : {op: "ref.add", owner: owner, ownerId: ownerId, path: path})
        }
        if (commands.length === 0) return false
        return editor.plan.run(commands.length === 1 ? commands[0]
                               : {op: "batch", label: "Add " + commands.length + " references", commands: commands}).ok
    }

    FileDialog {
        id: picker
        title: "Add reference pictures"
        fileMode: FileDialog.OpenFiles
        nameFilters: ["Images (*.png *.jpg *.jpeg *.webp *.heic *.heif *.tif *.tiff *.bmp *.gif)"]
        onAccepted: strip.addFiles(selectedFiles)
    }

    Rectangle {
        anchors.fill: parent; anchors.margins: -4
        radius: 6
        color: "transparent"
        border.color: drop.containsDrag ? "#87dec7" : "transparent"
        border.width: 2
    }
    DropArea {
        id: drop
        objectName: "refDrop_" + strip.owner
        anchors.fill: parent
        keys: ["text/uri-list"]
        onDropped: d => { if (d.hasUrls && strip.addFiles(d.urls)) d.accept(Qt.CopyAction) }
    }

    Flow {
        id: flow
        width: parent.width
        spacing: 8
        Repeater {
            model: strip.refs
            Item {
                id: thumb
                required property var modelData
                required property int index
                objectName: "ref_" + strip.owner + "_" + index
                width: strip.size; height: strip.size + 16
                Rectangle {
                    width: strip.size; height: strip.size
                    radius: 4; clip: true
                    color: "#0c1216"
                    border.color: thumb.modelData.missing ? "#e07f6f" : thumb.modelData.approved ? "#3f9c84" : strip.refs.some(r => r.approved) ? "#34434a" : "#e0a24a"
                    border.width: thumb.modelData.approved ? 2 : 1
                    Image {
                        anchors.fill: parent; anchors.margins: 1
                        source: thumb.modelData.missing ? "" : thumb.modelData.url
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize: Qt.size(strip.size * 2, strip.size * 2)
                        opacity: thumb.modelData.approved ? 1 : 0.6
                    }
                    Text {
                        visible: thumb.modelData.missing
                        anchors.centerIn: parent; width: parent.width - 8
                        horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
                        text: "Missing"; color: "#f0a08f"; font.pixelSize: 10
                    }
                }
                Text {
                    y: strip.size + 2; width: strip.size
                    elide: Text.ElideRight
                    text: thumb.modelData.approved ? "Chosen" : strip.refs.some(r => r.approved) ? "Alternative" : "To choose"
                    color: thumb.modelData.approved ? "#87dec7" : strip.refs.some(r => r.approved) ? "#6f8085" : "#e0a24a"; font.pixelSize: 10
                }
                MouseArea {
                    id: thumbMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    cursorShape: Qt.PointingHandCursor
                    onClicked: m => {
                        if (m.button === Qt.RightButton) menu.popup()
                        else strip.Window.window.viewReference(strip.owner, strip.ownerId, thumb.modelData.id)
                    }
                }
                ToolTip.visible: thumbMouse.containsMouse && !menu.visible
                ToolTip.delay: 500
                ToolTip.text: thumb.modelData.name + "  ·  " + thumb.modelData.width + "×" + thumb.modelData.height
                              + "\nClick to view large and choose; right-click for options"

                Menu {
                    id: menu
                    objectName: "refMenu"
                    palette.window: "#1d2930"; palette.windowText: "#edf1ef"; palette.text: "#edf1ef"
                    palette.light: "#2f4a52"; palette.highlight: "#2f4a52"; palette.highlightedText: "#edf1ef"
                    palette.mid: "#34434a"; palette.dark: "#87dec7"
                    function set(fields) { editor.plan.run({op: "set", kind: "reference", id: thumb.modelData.id, fields: fields}) }
                    Menu {
                        title: "Role"
                        palette.window: "#1d2930"; palette.windowText: "#edf1ef"; palette.light: "#2f4a52"; palette.mid: "#34434a"; palette.dark: "#87dec7"
                        Repeater {
                            model: editor.plan.options.refRole
                            MenuItem {
                                required property var modelData
                                objectName: "refRole_" + modelData.value
                                text: modelData.label
                                checkable: true
                                checked: thumb.modelData.role === modelData.value
                                onTriggered: menu.set({role: modelData.value})
                            }
                        }
                    }
                    MenuItem {
                        objectName: "refApproved"
                        text: "Chosen for this view"
                        checkable: true
                        checked: thumb.modelData.approved
                        onTriggered: menu.set({approved: !thumb.modelData.approved})
                    }
                    Menu {
                        title: "Move to view"
                        enabled: strip.views.length > 1
                        palette.window: "#1d2930"; palette.windowText: "#edf1ef"; palette.light: "#2f4a52"; palette.mid: "#34434a"; palette.dark: "#87dec7"
                        Repeater {
                            model: strip.views.filter(v => v.id !== thumb.modelData.view)
                            MenuItem {
                                required property var modelData
                                text: modelData.name
                                onTriggered: editor.plan.run({op: "ref.move", id: thumb.modelData.id, view: modelData.id})
                            }
                        }
                    }
                    MenuSeparator {}
                    MenuItem {
                        objectName: "refEarlier"
                        text: "Move earlier"
                        enabled: thumb.index > 0
                        onTriggered: editor.plan.run({op: "ref.move", id: thumb.modelData.id, position: thumb.index - 1})
                    }
                    MenuItem {
                        objectName: "refLater"
                        text: "Move later"
                        enabled: thumb.index < strip.refs.length - 1
                        onTriggered: editor.plan.run({op: "ref.move", id: thumb.modelData.id, position: thumb.index + 1})
                    }
                    MenuSeparator {}
                    MenuItem {
                        objectName: "refRemove"
                        text: "Remove"
                        onTriggered: editor.plan.run({op: "remove", kind: "reference", id: thumb.modelData.id})
                    }
                }
            }
        }
        // The add tile; dropping files anywhere on the strip works too.
        Rectangle {
            objectName: "addRef_" + strip.owner
            width: strip.size; height: strip.size
            radius: 4
            color: addMouse.containsMouse ? "#22313a" : "#162027"
            border.color: "#34434a"
            Column {
                anchors.centerIn: parent
                spacing: 2
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "+"; color: "#87dec7"; font.pixelSize: strip.size > 60 ? 22 : 16 }
                Text { anchors.horizontalCenter: parent.horizontalCenter; visible: strip.size > 60; text: "Add or drop"; color: "#8fa0a4"; font.pixelSize: 10 }
            }
            MouseArea { id: addMouse; anchors.fill: parent; hoverEnabled: true; onClicked: picker.open() }
        }
        // Pictures being generated, and ones that failed.
        Repeater {
            model: strip.pending
            Item {
                id: job
                required property var modelData
                required property int index
                readonly property bool failed: modelData.state === "failed"
                objectName: "genJob_" + strip.owner + "_" + index
                width: strip.size; height: strip.size + 16
                Rectangle {
                    width: strip.size; height: strip.size
                    radius: 4
                    color: jobMouse.containsMouse ? "#22313a" : "#162027"
                    border.color: job.failed ? "#e07f6f" : "#5c7078"
                    border.width: 1
                    Text {
                        anchors.centerIn: parent; width: parent.width - 10
                        horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap; maximumLineCount: 4; elide: Text.ElideRight
                        text: job.failed ? "Failed" : job.modelData.text
                        color: job.failed ? "#f0a08f" : "#adb8ba"; font.pixelSize: 10
                    }
                    // A slow sweep while it runs.
                    Rectangle {
                        visible: !job.failed
                        y: parent.height - 4; height: 2; radius: 1; color: "#87dec7"
                        width: parent.width * 0.3
                        SequentialAnimation on x {
                            running: !job.failed && job.visible; loops: Animation.Infinite
                            NumberAnimation { from: 2; to: strip.size * 0.7 - 2; duration: 900; easing.type: Easing.InOutQuad }
                            NumberAnimation { from: strip.size * 0.7 - 2; to: 2; duration: 900; easing.type: Easing.InOutQuad }
                        }
                    }
                }
                Text {
                    y: strip.size + 2; width: strip.size; elide: Text.ElideRight
                    text: job.failed ? "Click for options" : "Generating"
                    color: job.failed ? "#f0a08f" : "#8fa0a4"; font.pixelSize: 10
                }
                MouseArea { id: jobMouse; anchors.fill: parent; hoverEnabled: true; onClicked: jobMenu.popup() }
                ToolTip.visible: jobMouse.containsMouse && !jobMenu.visible; ToolTip.delay: 300
                ToolTip.text: (job.modelData.workflow ? job.modelData.workflow + ": " : "") + job.modelData.text
                Menu {
                    id: jobMenu
                    objectName: "genJobMenu"
                    palette.window: "#1d2930"; palette.windowText: "#edf1ef"; palette.light: "#2f4a52"; palette.mid: "#34434a"; palette.dark: "#87dec7"
                    MenuItem { objectName: "genRetry"; visible: job.failed; height: visible ? implicitHeight : 0; text: "Try again"; onTriggered: editor.generator.retry(job.modelData.id) }
                    MenuItem { objectName: "genDismiss"; visible: job.failed; height: visible ? implicitHeight : 0; text: "Dismiss"; onTriggered: editor.generator.dismiss(job.modelData.id) }
                    MenuItem { objectName: "genCancel"; visible: !job.failed; height: visible ? implicitHeight : 0; text: "Cancel"; onTriggered: editor.generator.cancel(job.modelData.id) }
                }
            }
        }
        // Generate: from the picture prompt, or from approved pictures and the prompt.
        Rectangle {
            id: genTile
            objectName: "genRef_" + strip.owner
            width: strip.size; height: strip.size
            radius: 4
            color: genMouse.containsMouse ? "#22313a" : "#162027"
            border.color: "#34434a"
            Column {
                anchors.centerIn: parent
                spacing: 2
                Text { anchors.horizontalCenter: parent.horizontalCenter; text: "✦"; color: "#87dec7"; font.pixelSize: strip.size > 60 ? 18 : 14 }
                Text { anchors.horizontalCenter: parent.horizontalCenter; visible: strip.size > 60; text: "Generate"; color: "#8fa0a4"; font.pixelSize: 10 }
            }
            MouseArea { id: genMouse; anchors.fill: parent; hoverEnabled: true; onClicked: { strip.note = ""; genMenu.popup() } }
            ToolTip.visible: genMouse.containsMouse && !genMenu.visible; ToolTip.delay: 500
            ToolTip.text: "Generate a reference picture with ComfyUI"
            Menu {
                id: genMenu
                objectName: "genMenu"
                width: 320
                palette.window: "#1d2930"; palette.windowText: "#edf1ef"; palette.light: "#2f4a52"; palette.mid: "#34434a"; palette.dark: "#87dec7"
                palette.disabled.windowText: "#6f8085"
                MenuItem {
                    objectName: "genQuick"
                    enabled: strip.gen.ready
                    text: (strip.gen.use === "ref-edit" ? "From the picked pictures" : "From the prompt")
                          + (strip.gen.workflowName ? "  ·  " + strip.gen.workflowName : "")
                    onTriggered: strip.generate()
                }
                MenuItem {
                    objectName: "genMenuWhy"
                    visible: !strip.gen.ready && strip.gen.why !== ""
                    height: visible ? implicitHeight : 0
                    enabled: false
                    text: strip.gen.why || ""
                }
                MenuItem {
                    objectName: "genOptions"
                    text: "With options…  (seed, how many, settings)"
                    onTriggered: strip.Window.window.openGenerate(strip.owner, strip.ownerId, {view: strip.viewId})
                }
                MenuSeparator {}
                MenuItem {
                    objectName: "genWorkflows"
                    text: "Workflows…"
                    onTriggered: strip.Window.window.openWorkflows()
                }
            }
        }
    }
    // Why a Generate request was refused, under the strip (only after a click, never on hover).
    Text {
        id: why
        objectName: "genNote_" + strip.owner
        y: flow.height + 4
        width: parent.width; wrapMode: Text.Wrap
        visible: text.length > 0
        text: strip.note
        color: "#e7b75f"; font.pixelSize: 11
    }
}
