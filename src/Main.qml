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
    Component.onCompleted: { restoreWindow(); if (!editor.ready) showManager(); editor.comfy.testConnection() }

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
    onClosing: saveWindow()
    function showManager() { managerOpen = true; manager.show() }

    // The single selection shared by the timeline and the inspector.
    property var selection: null
    property var lines: editor.lyrics.lines
    readonly property bool typing: managerOpen || lyricsEditor.visible || comfySheet.visible || (activeFocusItem !== null && activeFocusItem.cursorPosition !== undefined)

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
                text: "Chorus Cut aligns lyrics, and later generates shots, through ComfyUI: on this computer, on another machine, or on a rented GPU. Install the Chorus Cut node pack in that ComfyUI."
            }
            TextField { id: comfyUrl; objectName: "comfyUrl"; Layout.fillWidth: true; placeholderText: "http://127.0.0.1:8188"; selectByMouse: true
                        Keys.onReturnPressed: comfySheet.connectNow() }
            TextField { id: comfyToken; objectName: "comfyToken"; Layout.fillWidth: true; echoMode: TextInput.Password; selectByMouse: true
                        placeholderText: editor.comfy.hasToken ? "Access token saved (type to replace)" : "Access token, if the server needs one (optional)"
                        Keys.onReturnPressed: comfySheet.connectNow() }
            Label { objectName: "comfyStatus"; Layout.fillWidth: true; wrapMode: Text.Wrap; visible: text.length > 0; text: editor.comfy.connectionText
                    color: editor.comfy.connection === "ready" ? win.mint : editor.comfy.connection === "checking" ? win.quiet : "#e7b75f" }
            RowLayout {
                Action { visible: editor.comfy.hasToken; text: "Forget token"; onClicked: editor.comfy.configure(comfyUrl.text, "", true) }
                Item { Layout.fillWidth: true }
                Action { objectName: "comfyConnect"; text: "Connect"; onClicked: comfySheet.connectNow() }
                Action { text: "Done"; active: true; onClicked: comfySheet.close() }
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
            Item { Layout.fillWidth: true }
            Action { text: "Undo"; enabled: editor.canUndo; onClicked: editor.undo(); ToolTip.text: editor.undoText ? "Undo " + editor.undoText.toLowerCase() : "" }
            Action { text: "Redo"; enabled: editor.canRedo; onClicked: editor.redo(); ToolTip.text: editor.redoText ? "Redo " + editor.redoText.toLowerCase() : "" }
            Action { text: "Export timeline…"; enabled: editor.ready; onClicked: { save.selectedFile = editor.defaultExport; save.open() } }
        }

        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 160; Layout.maximumHeight: win.height * 0.42
            spacing: 8
            Rectangle {
                Layout.fillWidth: true; Layout.fillHeight: true
                color: "#0c1216"; radius: 5; clip: true
                VideoMonitor { objectName: "monitor"; anchors.fill: parent; anchors.margins: 4; visible: editor.sourceLength > 0 }
                // Lyric preview: the line under the playhead, words lighting up as they are sung.
                Item {
                    id: preview
                    anchors.left: parent.left; anchors.right: parent.right; anchors.margins: 24
                    anchors.verticalCenter: editor.sourceLength > 0 ? undefined : parent.verticalCenter
                    anchors.bottom: editor.sourceLength > 0 ? parent.bottom : undefined
                    anchors.bottomMargin: 24
                    height: previewText.implicitHeight
                    readonly property real ms: 1000 * editor.frame / editor.fps
                    readonly property var line: {
                        let found = null
                        for (const l of win.lines) if (l.start <= ms + 300) found = l
                        return found && ms <= found.end + 1500 ? found : null
                    }
                    Text {
                        id: previewText
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                        textFormat: Text.RichText
                        font.pixelSize: editor.sourceLength > 0 ? 22 : 30
                        style: Text.Outline; styleColor: "#0c1216"
                        text: {
                            const l = preview.line
                            if (!l) return win.lines.length || editor.sourceLength > 0 ? "" : "<span style='color:#6f8085'>Add lyrics with “Lyrics…”</span>"
                            return l.words.map(w => "<span style='color:" + (w.start <= preview.ms ? "#87dec7" : "#edf1ef") + "'>"
                                                    + w.word + "</span>").join(" ")
                        }
                    }
                }
            }
            Inspector {
                id: inspector
                Layout.preferredWidth: 320; Layout.fillHeight: true
                selection: win.selection
                onZoomTo: (a, b) => timeline.zoomTo(a, b)
                onNextFlagged: win.nextFlagged()
                onSelect: s => win.select(s)
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Action { objectName: "playPause"; enabled: editor.ready; text: editor.playing ? "Pause" : "Play"; active: true; onClicked: editor.togglePlay() }
            Action { text: "Play selection"; enabled: win.selection !== null; onClicked: editor.playRange(win.selection.start, win.selection.end) }
            Action { text: "Loop"; active: editor.looping; onClicked: editor.looping = !editor.looping }
            Action { text: "−1 frame"; onClicked: editor.seek(editor.frame-1) }
            Action { text: "+1 frame"; onClicked: editor.seek(editor.frame+1) }
            LabelText { text: win.tc(editor.frame); font.family: "Menlo"; font.pixelSize: 18; color: win.mint; Layout.leftMargin: 8 }
            LabelText { text: "/ " + win.tc(editor.length) + "  ·  " + Number(editor.fps.toFixed(3)) + " fps"; color: win.quiet; font.pixelSize: 12 }
            Item { Layout.fillWidth: true }
            Action { text: "−"; onClicked: timeline.zoomAround(1 / 1.5, timeline.px(timeline.playheadSeconds)); ToolTip.text: "Zoom out (⌘−)" }
            Action { text: "Fit"; onClicked: timeline.fit(); ToolTip.text: "Show the whole song (⌘0)" }
            Action { text: "+"; onClicked: timeline.zoomAround(1.5, timeline.px(timeline.playheadSeconds)); ToolTip.text: "Zoom in (⌘=)" }
            Action { text: editor.monitorMuted ? "Unmute" : "Mute"; Layout.leftMargin: 10; onClicked: editor.monitorMuted = !editor.monitorMuted }
            Slider {
                Layout.preferredWidth: 90
                from: -60; to: 0; stepSize: 1
                value: editor.monitorDb
                Accessible.name: "Monitor gain in decibels"
                onMoved: editor.monitorDb = Math.round(value)
            }
            LabelText { text: editor.monitorDb + " dB"; font.pixelSize: 12; color: win.quiet }
        }

        Timeline {
            id: timeline
            objectName: "timeline"
            Layout.fillWidth: true; Layout.fillHeight: true; Layout.minimumHeight: 240
            selection: win.selection
            onSelected: s => win.select(s)
            // Empty state: a song without aligned lyrics.
            Rectangle {
                visible: editor.ready && win.lines.length === 0
                anchors.centerIn: parent; width: 420; height: 110; radius: 8; color: "#1d2930"; border.color: "#34434a"
                ColumnLayout {
                    anchors.centerIn: parent
                    LabelText { text: editor.lyrics.text.length ? "The lyrics are not aligned yet." : "Start by adding the lyrics."; Layout.alignment: Qt.AlignHCenter }
                    Action { text: "Lyrics…"; active: true; Layout.alignment: Qt.AlignHCenter; onClicked: lyricsEditor.open() }
                }
            }
        }

        LabelText {
            Layout.fillWidth: true; elide: Text.ElideMiddle; color: win.quiet; font.pixelSize: 12
            text: editor.status || (editor.comfy.busy ? editor.comfy.jobText : "") || editor.lyrics.message ||
                  "Drag lyrics to move them, drag edges to trim. Pinch or ⌘-scroll to zoom; scroll to pan."
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
