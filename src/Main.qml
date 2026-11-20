import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Chorus

ApplicationWindow {
    id: win
    // Shown by restoreWindow(), at the size and place it had last time.
    visible: false
    title: editor.ready ? editor.projectName + " — Chorus Cut" : "Chorus Cut"
    width: 1280; height: 820; minimumWidth: 900; minimumHeight: 640
    color: "#162027"
    readonly property color ink: "#edf1ef"
    readonly property color quiet: "#adb8ba"
    readonly property color mint: "#87dec7"

    // The Project Manager covers the editor at launch when no project is open, and on ⌘O.
    property bool managerOpen: false
    Component.onCompleted: { editor.releaseFocusOnClick(win); restoreWindow(); if (!editor.ready) showManager(); editor.comfy.testConnection() }

    // ---------------------------------------------------------------- window size and place
    Settings {
        id: windowState
        category: "window"
        property int x: -1
        property int y: -1
        property int width: 1280
        property int height: 820
        property bool maximized: false
    }
    function restoreWindow() {
        const w = Math.max(minimumWidth, windowState.width), h = Math.max(minimumHeight, windowState.height)
        // Only reuse the old position while part of the window is still on a connected screen.
        let onScreen = false
        for (const sc of Qt.application.screens)
            onScreen = onScreen || windowState.x + w > sc.virtualX + 40 && windowState.x < sc.virtualX + sc.width - 40 &&
                         windowState.y + 40 > sc.virtualY && windowState.y < sc.virtualY + sc.height - 40
        width = w; height = h
        if (windowState.x !== -1 && onScreen) { x = windowState.x; y = windowState.y }
        if (windowState.maximized) showMaximized(); else show()
    }
    function saveWindow() {
        if (visibility === Window.Windowed) {
            windowState.x = x; windowState.y = y; windowState.width = width; windowState.height = height
            windowState.maximized = false
        } else if (visibility === Window.Maximized) windowState.maximized = true
    }
    // Save shortly after the user stops moving or resizing, so a crash or force-quit keeps it too.
    Timer { id: saveSoon; interval: 400; onTriggered: win.saveWindow() }
    onXChanged: saveSoon.restart()
    onYChanged: saveSoon.restart()
    onWidthChanged: saveSoon.restart()
    onHeightChanged: saveSoon.restart()
    onVisibilityChanged: saveSoon.restart()
    onClosing: { saveWindow(); if (viewerWindow.visible) { const keep = viewerState.popped; viewerWindow.close(); viewerState.popped = keep } }
    function showManager() { managerOpen = true; manager.show() }

    // ---------------------------------------------------------------- viewer size and pop-out
    Settings {
        id: viewerState
        category: "viewer"
        property int height: 330
        property int panelWidth: 340
        property bool popped: false
    }
    property bool viewerPopped: false
    function popOutViewer() { viewerPopped = true; viewerState.popped = true; viewerWindow.showAgain() }
    ViewerWindow {
        id: viewerWindow
        lines: win.lines
        onClosedByUser: { win.viewerPopped = false; viewerState.popped = false }
    }
    // A viewer left popped out comes back popped out, once a project is open.
    Connections {
        target: editor
        function onProjectChanged() { if (editor.ready && viewerState.popped && !win.viewerPopped) win.popOutViewer() }
    }

    // Edit (timeline) or Plan (concept, cast, locations, arc); and, in Edit, the right-hand panel.
    property string workspace: "edit"
    property string sidePanel: "inspector"

    // The single selection shared by the timeline and the inspector.
    property var selection: null
    property var lines: editor.lyrics.lines
    readonly property bool typing: managerOpen || lyricsEditor.visible || comfySheet.visible || agentSheet.visible || workflowsSheet.visible || refViewer.visible || generatePopup.visible || takePopup.visible || (activeFocusItem !== null && activeFocusItem.cursorPosition !== undefined)

    function select(next) { selection = next }
    function selectRow(row) {
        if (row < 0 || row >= lines.length) return
        const l = lines[row]
        select({kind: "line", firstRow: row, lastRow: row, start: l.start, end: l.end, label: l.text})
        editor.seekLyric(l.start)
        timeline.reveal(l.start / 1000)
    }
    function currentRow() {
        const ms = 1000 * editor.frame / editor.fps
        let row = -1
        for (let i = 0; i < lines.length; ++i) if (lines[i].start <= ms + 1) row = i
        return row
    }
    function nextFlagged() {
        const from = selection && selection.lastRow !== undefined ? selection.lastRow + 1 : 0
        for (let k = 0; k < lines.length; ++k) {
            const row = (from + k) % lines.length
            if (lines[row].flag) { selectRow(row); timeline.zoomTo(lines[row].start - 2000, lines[row].end + 2000); return }
        }
    }
    function tc(frame) {
        const nominal = Math.round(editor.fps)
        const seconds = Math.floor(frame / nominal)
        return [Math.floor(seconds/3600), Math.floor(seconds/60)%60, seconds%60, frame%nominal]
            .map(n => String(n).padStart(2, "0")).join(":")
    }
    Connections {
        target: editor
        function onProjectChanged() { win.selection = null; timeline.fit() }
    }
    Connections {
        target: editor.plan
        // A selected shot follows edits and undo, and goes away when the shot does.
        function onChanged() {
            const s = win.selection
            if (!s || s.kind !== "shot") return
            const shot = editor.plan.shot(s.id)
            win.selection = shot.id === undefined ? null : Object.assign({}, s, {start: shot.startMs, end: shot.endMs, label: "Shot " + shot.number})
        }
    }
    // Opens a shot in Edit: selected, with the playhead at its start and the timeline showing it.
    function showShot(id) {
        const s = editor.plan.shot(id)
        if (s.id === undefined) return
        workspace = "edit"
        sidePanel = "inspector"
        timeline.selectShot(id)
        editor.seekLyric(s.startMs)
        timeline.reveal(s.startMs / 1000)
    }
    // Opens what a job was for: a shot in Edit, or a character's, look's or location's card.
    function showJob(owner, ownerId) {
        if (owner === "shot") showShot(ownerId)
        else if (owner) { workspace = "plan"; planWorkspace.reveal(owner, ownerId) }
    }
    Connections {
        target: editor.notifier
        function onActivated(owner, ownerId) { win.raise(); win.requestActivate(); win.showJob(owner, ownerId); editor.notifier.dismissLatest() }
    }
    // Opens what a plan check is about: its shot, its span of the song, or its Plan page.
    function showCheck(c) {
        if (c.code === "lyrics") { workspace = "edit"; lyricsEditor.open(); return }
        // A reference to-do is fixed on the character's, look's or location's card.
        if (c.owner) { workspace = "plan"; planWorkspace.reveal(c.owner.kind, c.owner.id); return }
        if (c.gate === "G2" && c.shots.length === 0) {
            workspace = "plan"
            planWorkspace.page = c.code === "concept" ? "concept" : "arc"
            return
        }
        workspace = "edit"
        if (c.shots.length > 0) timeline.selectShot(c.shots[0])
        else if (c.end > c.start) {
            const row = lines.findIndex(l => l.start === c.start)
            if (row >= 0) selectRow(row)
            else select({kind: "range", start: c.start, end: c.end, label: "Range"})
        }
        if (c.end > c.start) timeline.zoomTo(c.start - 1500, c.end + 1500)
    }
    Connections {
        target: editor.lyrics
        // Keep the selection's times current after an edit or undo.
        function onChanged() {
            const s = win.selection
            if (!s || s.firstRow === undefined) return
            const ls = editor.lyrics.lines
            if (s.lastRow >= ls.length) { win.selection = null; return }
            if (s.kind === "word") {
                for (const w of ls[s.firstRow].words) if (w.index === s.word)
                    win.selection = Object.assign({}, s, {start: w.start, end: w.end})
            } else if (s.kind === "line" || s.kind === "lines") {
                win.selection = Object.assign({}, s, {start: ls[s.firstRow].start, end: ls[s.lastRow].end})
            }
        }
    }

    component Action: Button {
        id: btn
        property bool active: false
        focusPolicy: Qt.NoFocus
        implicitHeight: 32; leftPadding: 12; rightPadding: 12
        contentItem: Text { text: btn.text; color: btn.enabled ? (btn.active ? "#122328" : win.ink) : "#748084"; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: btn.active ? win.mint : (btn.hovered ? "#3b4b52" : "#2b383f"); radius: 5 }
        ToolTip.visible: hovered && ToolTip.text.length > 0
        ToolTip.delay: 500
    }
    component LabelText: Text { color: win.ink; font.pixelSize: 14; verticalAlignment: Text.AlignVCenter }
    component Tab: Button {
        id: tab
        property bool active: false
        focusPolicy: Qt.NoFocus
        implicitHeight: 26; leftPadding: 12; rightPadding: 12
        contentItem: Text { text: tab.text; color: tab.active ? "#122328" : win.ink; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: tab.active ? win.mint : (tab.hovered ? "#3b4b52" : "#22303a"); radius: 4 }
    }

    // ---------------------------------------------------------------- shortcuts
    Shortcut { enabled: !lyricsEditor.visible && !win.managerOpen; sequences: [StandardKey.Open]; onActivated: win.showManager() }
    Shortcut { enabled: editor.ready && !win.typing; sequence: "Space"; onActivated: editor.togglePlay() }
    Shortcut { enabled: editor.ready && !win.typing; sequences: ["Return", "Enter"]
               onActivated: if (win.selection) editor.playRange(win.selection.start, win.selection.end) }
    Shortcut { enabled: editor.ready && !win.typing; sequence: "L"; onActivated: editor.looping = !editor.looping }
    Shortcut { enabled: editor.ready && !win.typing; sequence: "Left"; onActivated: editor.seek(editor.frame-1) }
    Shortcut { enabled: editor.ready && !win.typing; sequence: "Right"; onActivated: editor.seek(editor.frame+1) }
    Shortcut { enabled: editor.ready && !win.typing; sequence: "Shift+Left"; onActivated: editor.seek(editor.frame-Math.round(editor.fps)) }
    Shortcut { enabled: editor.ready && !win.typing; sequence: "Shift+Right"; onActivated: editor.seek(editor.frame+Math.round(editor.fps)) }
    Shortcut { enabled: editor.ready && !win.typing; sequence: "Up"
               onActivated: win.selectRow(win.selection && win.selection.firstRow !== undefined ? win.selection.firstRow - 1 : win.currentRow()) }
    Shortcut { enabled: editor.ready && !win.typing; sequence: "Down"
               onActivated: win.selectRow(win.selection && win.selection.lastRow !== undefined ? win.selection.lastRow + 1 : win.currentRow() + 1) }
    Shortcut { enabled: editor.ready && !win.typing; sequence: "Escape"; onActivated: win.selection = null }
    Shortcut { enabled: editor.ready && !win.typing; sequences: [StandardKey.ZoomIn, "Ctrl+="]; onActivated: timeline.zoomAround(1.5, timeline.px(timeline.playheadSeconds)) }
    Shortcut { enabled: editor.ready && !win.typing; sequences: [StandardKey.ZoomOut]; onActivated: timeline.zoomAround(1 / 1.5, timeline.px(timeline.playheadSeconds)) }
    Shortcut { enabled: editor.ready && !win.typing; sequence: "Ctrl+0"; onActivated: timeline.fit() }
    Shortcut { enabled: editor.ready && !win.typing; sequence: "Z"; onActivated: if (win.selection) timeline.zoomTo(win.selection.start, win.selection.end) }
    Shortcut { enabled: editor.ready && !win.typing && win.selection !== null && win.selection.kind === "shot"
               sequences: [StandardKey.Delete, "Backspace"]
               onActivated: { const id = win.selection.id; win.selection = null; editor.plan.run({op: "remove", kind: "shot", id: id}) } }
    // ⌘B splits the selected shot at the playhead, as in most editors.
    Shortcut { enabled: editor.ready && !win.typing && win.selection !== null && win.selection.kind === "shot"; sequence: "Ctrl+B"
               onActivated: editor.plan.run({op: "shot.split", id: win.selection.id, at: editor.frame}) }
    Shortcut { enabled: editor.ready && !win.managerOpen; sequence: "Ctrl+1"; onActivated: win.workspace = "edit" }
    Shortcut { enabled: editor.ready && !win.managerOpen; sequence: "Ctrl+2"; onActivated: win.workspace = "plan" }
    Shortcut { enabled: editor.ready && !win.typing; sequences: [StandardKey.Undo]; onActivated: editor.undo() }
    Shortcut { enabled: editor.ready && !win.typing; sequences: [StandardKey.Redo]; onActivated: editor.redo() }

    // ---------------------------------------------------------------- dialogs
    FileDialog { id: save; fileMode: FileDialog.SaveFile; nameFilters: ["Final Cut Pro 7 XML (*.xml)"]; onAccepted: editor.exportTimeline(selectedFile) }
    FileDialog {
        id: importDialog
        title: "Import GPU alignment"
        nameFilters: ["Alignment JSON (*.json)"]
        onAccepted: if (editor.lyrics.importAlignment(selectedFile)) lyricsEditor.close()
    }

    // Where Chorus Cut runs its GPU jobs: ComfyUI on this computer or on a rented GPU.
    Popup {
        id: comfySheet
        objectName: "comfySheet"
        anchors.centerIn: parent
        width: Math.min(520, win.width - 80)
        modal: true
        padding: 20
        background: Rectangle { color: "#1d2930"; radius: 6; border.color: "#34434a" }
        onOpened: { comfyUrl.text = editor.comfy.url; comfyToken.text = ""; comfyUrl.forceActiveFocus() }
        function connectNow() { editor.comfy.configure(comfyUrl.text, comfyToken.text, false); comfyToken.text = ""; editor.comfy.testConnection() }
        contentItem: ColumnLayout {
            spacing: 10
            LabelText { text: "ComfyUI"; font.pixelSize: 17; font.bold: true }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap; color: win.quiet
                text: "Chorus Cut aligns lyrics and generates pictures through ComfyUI: on this computer, on another machine, or on a rented GPU. Install the Chorus Cut node pack there to align lyrics."
            }
            TextField { id: comfyUrl; objectName: "comfyUrl"; Layout.fillWidth: true; placeholderText: "http://127.0.0.1:8188"; selectByMouse: true
                        Keys.onReturnPressed: comfySheet.connectNow() }
            TextField { id: comfyToken; objectName: "comfyToken"; Layout.fillWidth: true; echoMode: TextInput.Password; selectByMouse: true
                        placeholderText: editor.comfy.hasToken ? "Access token saved (type to replace)" : "Access token, if the server needs one (optional)"
                        Keys.onReturnPressed: comfySheet.connectNow() }
            Label { Layout.fillWidth: true; wrapMode: Text.Wrap; visible: text.length > 0; text: editor.comfy.tokenNote; color: win.quiet; font.pixelSize: 12 }
            Label { objectName: "comfyStatus"; Layout.fillWidth: true; wrapMode: Text.Wrap; visible: text.length > 0; text: editor.comfy.connectionText
                    color: editor.comfy.connection === "ready" ? win.mint : editor.comfy.connection === "checking" ? win.quiet : "#e7b75f" }
            RowLayout {
                Action { visible: editor.comfy.hasToken; text: "Forget token"; onClicked: editor.comfy.configure(comfyUrl.text, "", true) }
                Action { objectName: "comfyWorkflows"; text: "Workflows…"; onClicked: win.openWorkflows() }
                Item { Layout.fillWidth: true }
                Action { objectName: "comfyConnect"; text: "Connect"; onClicked: comfySheet.connectNow() }
                Action { text: "Done"; active: true; onClicked: comfySheet.close() }
            }
        }
    }

    // Asking the agent about a field, an item or the whole plan.
    AskPopup { id: askPopup; onSent: thread => { win.workspace = "plan"; planWorkspace.showRequests(thread) } }
    // What a field or item is called, for a request: "Cherry · Picture prompt".
    function aboutName(kind, id) {
        const plan = editor.plan
        if (kind === "concept") return "Concept"
        if (kind === "arc") return (plan.arc[id] || {}).label || "Section"
        if (kind === "shot") return "Shot " + plan.shot(id).number
        if (kind === "view") { const v = plan.item("view", id); return v.owner ? aboutName(v.owner, v.ownerId) + " · " + ((v.fields || {}).name || "View") : "View" }
        const item = plan.item(kind, id)
        const name = item.fields && item.fields.name ? item.fields.name : "Unnamed " + kind
        if (kind === "look") { const c = plan.item("character", item.character); return ((c.fields && c.fields.name) || "Unnamed character") + " · " + name }
        return name
    }
    function askAgent(kind, id, field, fieldLabel) {
        if (!editor.ready) return
        let value
        if (kind && field) {
            const fields = kind === "concept" ? editor.plan.concept.fields : kind === "arc" ? (editor.plan.arc[id] || {}).fields : (editor.plan.item(kind, id).fields || {})
            value = fields ? fields[field] : undefined
        }
        const label = kind ? aboutName(kind, id) + (fieldLabel ? " · " + fieldLabel : "") : "The whole plan"
        askPopup.start(kind, id, field, label, value)
    }
    function showRequests(thread) { workspace = "plan"; planWorkspace.showRequests(thread) }

    // The ComfyUI workflows Chorus Cut generates with, and their adapters.
    WorkflowsSheet { id: workflowsSheet }
    function openWorkflows() { comfySheet.close(); workflowsSheet.open() }
    // A reference picture, large; and Generate with a seed, a count and the workflow's settings.
    ReferenceViewer { id: refViewer; onGenerateAgain: (owner, ownerId, prefill) => win.openGenerate(owner, ownerId, prefill) }
    function viewReference(owner, ownerId, index) { refViewer.show(owner, ownerId, index, aboutName(owner, ownerId)) }
    GeneratePopup { id: generatePopup; onWorkflowsRequested: win.openWorkflows(); onAskRequested: (view, label) => win.askAgent("view", view, "framing", label) }
    function openGenerate(owner, ownerId, prefill) { if (editor.ready) generatePopup.start(owner, ownerId, prefill, aboutName(owner, ownerId)) }
    // A shot's takes with options.
    TakePopup { id: takePopup; onWorkflowsRequested: win.openWorkflows(); onAskRequested: shot => win.askAgent("shot", shot, "prompts", "Video prompt") }
    function openTake(shot, prefill) { if (editor.ready) takePopup.start(shot, prefill) }

    // Connecting an agent: Claude Code, Codex or any MCP client runs choruscut-mcp, which talks to this app.
    Popup {
        id: agentSheet
        objectName: "agentSheet"
        anchors.centerIn: parent
        width: Math.min(640, win.width - 80)
        modal: true
        padding: 20
        background: Rectangle { color: "#1d2930"; radius: 6; border.color: "#34434a" }
        component CommandRow: RowLayout {
            id: cmdRow
            property string label
            property string command
            Layout.fillWidth: true
            spacing: 8
            Text { text: cmdRow.label; color: win.quiet; font.pixelSize: 12; Layout.preferredWidth: 86 }
            TextField {
                Layout.fillWidth: true
                readOnly: true; selectByMouse: true
                text: cmdRow.command
                color: win.ink; font.family: "Menlo"; font.pixelSize: 11
                onTextChanged: cursorPosition = 0
                Component.onCompleted: cursorPosition = 0
                background: Rectangle { color: "#162027"; radius: 4; border.color: "#34434a" }
            }
            Action { text: "Copy"; onClicked: editor.agent.copy(cmdRow.command) }
        }
        contentItem: ColumnLayout {
            spacing: 10
            LabelText { text: "Agent"; font.pixelSize: 17; font.bold: true }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap; color: win.quiet
                text: "Plan with your own Claude Code or Codex. It works on the project open here: you see every change as it lands, each pass is one step you can undo, and fields you wrote or locked are left alone. Run one of these once in a terminal to add Chorus Cut to your agent:"
            }
            CommandRow { label: "Claude Code"; command: editor.agent.claudeCommand }
            CommandRow { label: "Codex"; command: editor.agent.codexCommand }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap; color: win.quiet; font.pixelSize: 12
                text: "To send requests from the app straight into a Claude Code session (“ask” on any field), start Claude Code like this. Channels are a Claude Code research preview; it asks you to confirm at startup, and Team or Enterprise plans need an owner to turn channels on. Without it, requests wait in Plan ▸ Requests until your agent checks them."
            }
            CommandRow { objectName: "channelCommand"; label: "Live requests"; command: editor.agent.channelCommand }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap; color: win.quiet; font.pixelSize: 12
                text: "Other MCP clients can run the same program over stdio: " + editor.agent.shimPath + "\n\nThen just ask your agent to plan the video in Chorus Cut. In Claude Code, /mcp__ChorusCut__plan starts it too."
            }
            Label {
                objectName: "agentStatus"
                Layout.fillWidth: true; wrapMode: Text.Wrap
                text: editor.agent.note || (editor.agent.clients.length ? "Connected: " + editor.agent.clients.join(", ") : "No agent connected right now.")
                color: editor.agent.note ? "#e7b75f" : editor.agent.clients.length ? win.mint : win.quiet
            }
            RowLayout {
                Item { Layout.fillWidth: true }
                Action { objectName: "agentDone"; text: "Done"; active: true; onClicked: agentSheet.close() }
            }
        }
    }

    // Lyric text: paste, save, align locally or import a GPU alignment.
    Popup {
        id: lyricsEditor
        objectName: "lyricsEditor"
        readonly property bool working: editor.lyrics.busy || editor.comfy.busy
        anchors.centerIn: parent
        width: Math.min(620, win.width - 80); height: Math.min(640, win.height - 80)
        modal: true
        closePolicy: editor.lyrics.busy ? Popup.NoAutoClose : Popup.CloseOnEscape
        onOpened: draft.text = editor.lyrics.text
        background: Rectangle { color: "#1d2930"; radius: 6; border.color: "#34434a" }
        contentItem: ColumnLayout {
            spacing: 8
            LabelText { text: "Lyrics"; font.pixelSize: 17; font.bold: true }
            Label {
                Layout.fillWidth: true; wrapMode: Text.Wrap; color: win.quiet
                text: "Paste the lyrics as sung, repeating each chorus, with section names like [Verse 1] on their own lines. Changing the text clears the current timings."
            }
            ScrollView {
                Layout.fillWidth: true; Layout.fillHeight: true
                TextArea {
                    id: draft; objectName: "lyricDraft"
                    readOnly: editor.lyrics.busy; selectByMouse: true; wrapMode: TextEdit.Wrap
                    color: win.ink; font.pixelSize: 15; placeholderTextColor: "#acb9ba"
                    placeholderText: "Paste the lyrics in performance order…"
                    background: Rectangle { color: "#162027"; radius: 4 }
                }
            }
            Label { Layout.fillWidth: true; visible: text.length > 0 && !editor.comfy.busy; text: editor.lyrics.message; color: win.mint; wrapMode: Text.Wrap }
            Label { Layout.fillWidth: true; visible: text.length > 0; text: editor.comfy.jobText; color: win.quiet; wrapMode: Text.Wrap; objectName: "comfyJobText" }
            // Secondary actions above; the main path (language, Align, Done) below.
            RowLayout {
                Layout.fillWidth: true
                Action { text: "Import alignment…"; enabled: draft.text === editor.lyrics.text && draft.text.length > 0 && !lyricsEditor.working; onClicked: importDialog.open()
                         ToolTip.text: "Load a saved alignment JSON for these exact lyrics" }
                Action { objectName: "alignWords"; text: "Basic offline align"; enabled: draft.text.trim().length > 0 && !lyricsEditor.working
                         ToolTip.text: "Older single-model aligner that runs without ComfyUI; less accurate"
                         onClicked: { if (editor.lyrics.saveText(draft.text)) editor.lyrics.align() } }
                Item { Layout.fillWidth: true }
                Action { text: "Save lyrics"; enabled: draft.text !== editor.lyrics.text && !lyricsEditor.working; onClicked: editor.lyrics.saveText(draft.text) }
            }
            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                Action { visible: lyricsEditor.working; text: "Cancel alignment"; onClicked: { editor.lyrics.cancel(); editor.comfy.cancel() } }
                ComboBox {
                    id: alignLanguage
                    objectName: "alignLanguage"
                    implicitHeight: 32; implicitWidth: 120
                    enabled: !lyricsEditor.working
                    textRole: "label"; valueRole: "code"
                    model: [{label: "English", code: "en"}, {label: "French", code: "fr"}, {label: "German", code: "de"}, {label: "Spanish", code: "es"}]
                    Component.onCompleted: currentIndex = Math.max(0, indexOfValue(editor.comfy.language))
                    Accessible.name: "Language of the lyrics"
                    contentItem: Text { leftPadding: 10; text: alignLanguage.displayText; color: win.ink; font.pixelSize: 13; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { color: alignLanguage.hovered ? "#3b4b52" : "#2b383f"; radius: 5 }
                    indicator: Text { x: alignLanguage.width - width - 10; anchors.verticalCenter: parent.verticalCenter; text: "▾"; color: win.quiet }
                }
                Action { objectName: "alignComfy"; text: "Align"; active: true
                         enabled: draft.text.trim().length > 0 && !lyricsEditor.working
                         ToolTip.text: "Align every word on ComfyUI (" + editor.comfy.url + ")"
                         onClicked: { if (editor.lyrics.saveText(draft.text)) editor.comfy.alignLyrics(alignLanguage.currentValue) } }
                Action { text: "Done"; enabled: !lyricsEditor.working; onClicked: lyricsEditor.close() }
            }
        }
        Connections {
            target: editor.lyrics
            function onFinished(success) { if (success) lyricsEditor.close() }
        }
        Connections {
            target: editor.comfy
            function onFinished(success) { if (success) lyricsEditor.close() }
        }
    }

    // ---------------------------------------------------------------- layout
    ColumnLayout {
        anchors.fill: parent; anchors.margins: 14; spacing: 8

        RowLayout {
            Layout.fillWidth: true
            LabelText { text: "Chorus Cut"; font.pixelSize: 22; font.weight: Font.DemiBold; Layout.rightMargin: 6 }
            Action { objectName: "openManager"; text: "Projects"; onClicked: win.showManager(); ToolTip.text: "Project Manager (⌘O)" }
            Action { objectName: "openLyrics"; text: "Lyrics…"; enabled: editor.ready; onClicked: lyricsEditor.open() }
            Action {
                objectName: "comfyButton"
                text: "ComfyUI"
                leftPadding: 26
                onClicked: comfySheet.open()
                ToolTip.text: editor.comfy.connectionText || editor.comfy.url
                Rectangle {
                    width: 8; height: 8; radius: 4; x: 11; anchors.verticalCenter: parent.verticalCenter
                    color: editor.comfy.connection === "ready" ? win.mint : editor.comfy.connection === "missingNode" ? "#e7b75f"
                         : editor.comfy.connection === "unreachable" ? "#f0a08f" : "#748084"
                }
            }
            Action {
                objectName: "agentButton"
                text: editor.requests.waitingOnUser ? "Agent · " + editor.requests.waitingOnUser : "Agent"
                leftPadding: 26
                onClicked: editor.requests.waitingOnUser ? win.showRequests(0) : agentSheet.open()
                ToolTip.text: (editor.requests.waitingOnUser ? editor.requests.waitingOnUser + " waiting for your answer. " : "")
                              + (editor.agent.clients.length ? "Connected: " + editor.agent.clients.join(", ") : "Plan with Claude Code or Codex")
                Rectangle {
                    width: 8; height: 8; radius: 4; x: 11; anchors.verticalCenter: parent.verticalCenter
                    color: editor.agent.clients.length ? win.mint : editor.agent.note ? "#e7b75f" : "#748084"
                }
            }
            Action {
                id: jobsButton
                objectName: "jobsButton"
                readonly property int running: editor.generator.jobs.filter(j => j.state !== "failed").length
                readonly property int failed: editor.generator.jobs.length - running
                text: running ? "Generating · " + running : failed ? "Jobs · " + failed + " failed" : "Jobs"
                leftPadding: running || failed ? 26 : 12
                onClicked: jobsPopup.opened ? jobsPopup.close() : jobsPopup.open()
                ToolTip.text: "What is generating, and how to be told when it finishes"
                Rectangle {
                    visible: jobsButton.running || jobsButton.failed
                    width: 8; height: 8; radius: 4; x: 11; anchors.verticalCenter: parent.verticalCenter
                    color: jobsButton.running ? "#e7b75f" : "#e07f6f"
                    SequentialAnimation on opacity { running: jobsButton.running > 0; loops: Animation.Infinite
                        NumberAnimation { to: 0.3; duration: 700 } NumberAnimation { to: 1; duration: 700 } }
                }
                JobsPopup {
                    id: jobsPopup
                    y: jobsButton.height + 6
                    onShowRequested: (owner, ownerId) => win.showJob(owner, ownerId)
                }
            }
            RowLayout {
                Layout.leftMargin: 18
                spacing: 2
                Tab { objectName: "workspaceEdit"; text: "Edit"; active: win.workspace === "edit"; enabled: editor.ready; onClicked: win.workspace = "edit"
                      ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Timeline, shots and lyrics (⌘1)" }
                Tab { objectName: "workspacePlan"; text: "Plan"; active: win.workspace === "plan"; enabled: editor.ready; onClicked: win.workspace = "plan"
                      ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Concept, look, cast, locations and arc (⌘2)" }
            }
            Item { Layout.fillWidth: true }
            Action { text: "Undo"; enabled: editor.canUndo; onClicked: editor.undo(); ToolTip.text: editor.undoText ? "Undo " + editor.undoText.toLowerCase() : "" }
            Action { text: "Redo"; enabled: editor.canRedo; onClicked: editor.redo(); ToolTip.text: editor.redoText ? "Redo " + editor.redoText.toLowerCase() : "" }
            Action { text: "Export timeline…"; enabled: editor.ready; onClicked: { save.selectedFile = editor.defaultExport; save.open() } }
        }

        PlanWorkspace {
            id: planWorkspace
            objectName: "planWorkspace"
            visible: win.workspace === "plan"
            Layout.fillWidth: true; Layout.fillHeight: true
            onShowCheck: c => win.showCheck(c)
            onShowShot: id => win.showShot(id)
            shotColors: timeline.shotColors
        }

        // Edit: viewer, transport and timeline, with the inspector or checks down the right. The
        // dividers between them drag like any editor's; the viewer can also pop out to a window.
        SplitView {
            id: editSplit
            objectName: "editSplit"
            visible: win.workspace === "edit"
            Layout.fillWidth: true; Layout.fillHeight: true
            orientation: Qt.Horizontal
            handle: Rectangle {
                objectName: "panelDivider"
                implicitWidth: 8
                color: SplitHandle.pressed || SplitHandle.hovered ? "#3e6b75" : "transparent"
                Rectangle { anchors.centerIn: parent; width: 2; height: 28; radius: 1; color: "#3a4950" }
            }
            SplitView {
                id: editColumn
                objectName: "editColumn"
                SplitView.fillWidth: true; SplitView.minimumWidth: 520
                orientation: Qt.Vertical
                handle: Rectangle {
                    objectName: "viewerDivider"
                    implicitHeight: 8
                    color: SplitHandle.pressed || SplitHandle.hovered ? "#3e6b75" : "transparent"
                    Rectangle { anchors.centerIn: parent; width: 28; height: 2; radius: 1; color: "#3a4950" }
                }
                ColumnLayout {
                    id: viewerPane
                    objectName: "viewerPane"
                    // Popped out, only the transport stays here and the timeline takes the room.
                    SplitView.preferredHeight: viewerState.height
                    SplitView.minimumHeight: win.viewerPopped ? transport.implicitHeight : 120 + transport.implicitHeight
                    SplitView.maximumHeight: win.viewerPopped ? transport.implicitHeight : Math.max(160, editColumn.height - 160)
                    onHeightChanged: if (!win.viewerPopped && editColumn.height > 0 && visible) viewerSave.restart()
                    Timer { id: viewerSave; interval: 300; onTriggered: viewerState.height = Math.round(viewerPane.height) }
                    spacing: 8
                    MonitorView {
                        id: inlineViewer
                        visible: !win.viewerPopped
                        Layout.fillWidth: true; Layout.fillHeight: true
                        radius: 5
                        monitorName: "monitor"
                        lines: win.lines
                        lyricScale: Math.max(1, Math.min(1.8, height / 420))
                    }
                    RowLayout {
                        id: transport
                        Layout.fillWidth: true
                        Action { objectName: "playPause"; enabled: editor.ready; text: editor.playing ? "Pause" : "Play"; active: true; onClicked: editor.togglePlay() }
                        // Narrow windows drop the controls that have keyboard shortcuts.
                        Action { visible: editColumn.width >= 820; text: "Play selection"; enabled: win.selection !== null; onClicked: editor.playRange(win.selection.start, win.selection.end) }
                        Action { text: "Loop"; active: editor.looping; onClicked: editor.looping = !editor.looping }
                        Action { visible: editColumn.width >= 1080; text: "−1 frame"; onClicked: editor.seek(editor.frame-1) }
                        Action { visible: editColumn.width >= 1080; text: "+1 frame"; onClicked: editor.seek(editor.frame+1) }
                        LabelText { text: win.tc(editor.frame); font.family: "Menlo"; font.pixelSize: 18; color: win.mint; Layout.leftMargin: 8 }
                        LabelText { visible: editColumn.width >= 700; text: "/ " + win.tc(editor.length) + "  ·  " + Number(editor.fps.toFixed(3)) + " fps"; color: win.quiet; font.pixelSize: 12 }
                        Item { Layout.fillWidth: true }
                        Action {
                            objectName: "viewerPopOut"
                            text: win.viewerPopped ? "Viewer back" : "Pop out"
                            active: win.viewerPopped
                            onClicked: win.viewerPopped ? viewerWindow.close() : win.popOutViewer()
                            ToolTip.text: win.viewerPopped ? "Put the viewer back on this page" : "Show the viewer in its own window, for a second screen or full screen"
                        }
                        Action { text: "−"; onClicked: timeline.zoomAround(1 / 1.5, timeline.px(timeline.playheadSeconds)); ToolTip.text: "Zoom out (⌘−)" }
                        Action { text: "Fit"; onClicked: timeline.fit(); ToolTip.text: "Show the whole song (⌘0)" }
                        Action { text: "+"; onClicked: timeline.zoomAround(1.5, timeline.px(timeline.playheadSeconds)); ToolTip.text: "Zoom in (⌘=)" }
                        Action { visible: editColumn.width >= 620; text: editor.monitorMuted ? "Unmute" : "Mute"; Layout.leftMargin: 10; onClicked: editor.monitorMuted = !editor.monitorMuted }
                        Slider {
                            visible: editColumn.width >= 620
                            Layout.preferredWidth: 90
                            from: -60; to: 0; stepSize: 1
                            value: editor.monitorDb
                            Accessible.name: "Monitor gain in decibels"
                            onMoved: editor.monitorDb = Math.round(value)
                        }
                        LabelText { visible: editColumn.width >= 620; text: editor.monitorDb + " dB"; font.pixelSize: 12; color: win.quiet }
                    }
                }
                Timeline {
                    id: timeline
                    objectName: "timeline"
                    SplitView.fillHeight: true; SplitView.minimumHeight: 160
                    selection: win.selection
                    onSelected: s => win.select(s)
                    // Empty state: a song without aligned lyrics.
                    Rectangle {
                        visible: editor.ready && win.lines.length === 0
                        anchors.centerIn: parent; width: 420; height: 110; radius: 8; color: "#1d2930"; border.color: "#34434a"
                        ColumnLayout {
                            anchors.centerIn: parent
                            LabelText { text: editor.comfy.busy ? editor.comfy.jobText : editor.lyrics.text.length ? "The lyrics are not aligned yet." : "Start by adding the lyrics."
                                        Layout.alignment: Qt.AlignHCenter; Layout.maximumWidth: 380; wrapMode: Text.Wrap; horizontalAlignment: Text.AlignHCenter }
                            Action { text: "Lyrics…"; active: true; visible: !editor.comfy.busy; Layout.alignment: Qt.AlignHCenter; onClicked: lyricsEditor.open() }
                        }
                    }
                }
            }
            ColumnLayout {
                id: sidePanelColumn
                objectName: "sidePanel"
                SplitView.preferredWidth: viewerState.panelWidth; SplitView.minimumWidth: 340; SplitView.maximumWidth: 640
                onWidthChanged: if (width >= 340 && visible) panelSave.restart()
                Timer { id: panelSave; interval: 300; onTriggered: viewerState.panelWidth = Math.round(sidePanelColumn.width) }
                spacing: 6
                RowLayout {
                    spacing: 2
                    Tab { objectName: "panelInspector"; text: "Inspector"; active: win.sidePanel === "inspector"; onClicked: win.sidePanel = "inspector" }
                    Tab {
                        objectName: "panelChecks"
                        text: "Checks" + (editor.plan.errorCount ? " · " + editor.plan.errorCount : "")
                        active: win.sidePanel === "checks"
                        onClicked: win.sidePanel = "checks"
                    }
                }
                Inspector {
                    id: inspector
                    visible: win.sidePanel === "inspector"
                    Layout.fillWidth: true; Layout.fillHeight: true
                    selection: win.selection
                    onZoomTo: (a, b) => timeline.zoomTo(a, b)
                    onNextFlagged: win.nextFlagged()
                    onSelect: s => win.select(s)
                }
                Rectangle {
                    visible: win.sidePanel === "checks"
                    Layout.fillWidth: true; Layout.fillHeight: true
                    color: "#1d2930"; radius: 5
                    Checks {
                        anchors.fill: parent; anchors.margins: 10
                        compact: true
                        onShowCheck: c => win.showCheck(c)
                    }
                }
            }
        }

        LabelText {
            Layout.fillWidth: true; elide: Text.ElideMiddle; color: win.quiet; font.pixelSize: 12
            text: editor.status || (editor.comfy.busy ? editor.comfy.jobText : "") || editor.plan.message || editor.lyrics.message ||
                  "Drag lyrics and shots to move them, drag edges to trim. Double-click V1 or V2 to plan a shot. Pinch or ⌘-scroll to zoom."
        }
    }

    // After an agent's pass: what it did, and a one-click undo while it is the latest step.
    Rectangle {
        id: passBanner
        objectName: "agentPassBanner"
        property string label: ""
        property int changes: 0
        property string client: ""
        visible: label.length > 0 && !win.managerOpen
        z: 50
        anchors.horizontalCenter: parent.horizontalCenter
        y: 52
        width: passRow.implicitWidth + 24; height: 36
        radius: 6; color: "#24364a"; border.color: "#5c7fa8"
        Connections {
            target: editor.agent
            function onPassApplied(label, changes, client) { passBanner.label = label; passBanner.changes = changes; passBanner.client = client }
        }
        RowLayout {
            id: passRow
            anchors.centerIn: parent
            spacing: 10
            Text {
                text: (passBanner.client ? passBanner.client + " · " : "") + passBanner.label.replace(/^Agent: /, "")
                      + " — " + passBanner.changes + (passBanner.changes === 1 ? " change" : " changes")
                color: win.ink; font.pixelSize: 13
            }
            Tab {
                objectName: "agentPassUndo"
                text: "Undo"
                enabled: editor.undoText === passBanner.label
                onClicked: { editor.undo(); passBanner.label = "" }
            }
            Tab { objectName: "agentPassDismiss"; text: "Dismiss"; onClicked: passBanner.label = "" }
        }
    }

    // When a take or pictures arrive (or fail): a banner at the bottom right, gone after a while.
    Rectangle {
        id: noticeBanner
        objectName: "noticeBanner"
        readonly property var n: editor.notifier.latest
        visible: n.title !== undefined && !win.managerOpen
        z: 50
        anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 18
        width: Math.min(380, noticeColumn.implicitWidth + 28); height: noticeColumn.implicitHeight + 20
        radius: 6; color: n.ok === false ? "#3a2622" : "#203530"; border.color: n.ok === false ? "#b4544a" : "#3f9c84"
        onNChanged: if (visible) noticeHide.restart()
        Timer { id: noticeHide; interval: 12000; onTriggered: if (!noticeHover.hovered) editor.notifier.dismissLatest(); else restart() }
        HoverHandler { id: noticeHover }
        ColumnLayout {
            id: noticeColumn
            x: 14; y: 10; width: noticeBanner.width - 28
            spacing: 6
            Text { objectName: "noticeTitle"; text: noticeBanner.n.title || ""; color: win.ink; font.pixelSize: 13; font.weight: Font.DemiBold
                   Layout.fillWidth: true; elide: Text.ElideRight }
            Text { text: noticeBanner.n.body || ""; color: win.quiet; font.pixelSize: 12; Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideRight }
            RowLayout {
                spacing: 6
                Item { Layout.fillWidth: true }
                Tab { objectName: "noticeShow"; visible: (noticeBanner.n.owner || "") !== ""; text: "Show"
                      onClicked: { const n = noticeBanner.n; editor.notifier.dismissLatest(); win.showJob(n.owner, n.ownerId) } }
                Tab { objectName: "noticeDismiss"; text: "Dismiss"; onClicked: editor.notifier.dismissLatest() }
            }
        }
    }

    ProjectManager {
        id: manager
        objectName: "projectManager"
        anchors.fill: parent
        z: 100
        visible: win.managerOpen
        focus: visible
        onFinished: { win.managerOpen = false; win.contentItem.forceActiveFocus() }
    }
}
