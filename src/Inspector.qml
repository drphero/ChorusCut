import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "keys.js" as Kbd

// Details and precise controls for whatever is selected on the timeline.
Rectangle {
    id: ins
    color: "#1d2930"
    radius: 5

    property var selection: null
    property var lines: editor.lyrics.lines
    signal zoomTo(int startMs, int endMs)
    signal nextFlagged()
    signal select(var selection)
    // Scrolls the selected shot's takes into view (after the shot's details have laid out).
    function revealTakes() { revealTimer.restart() }
    Timer {
        id: revealTimer
        interval: 80
        onTriggered: {
            if (!takesCaption.visible) return
            const f = scroller.contentItem
            const y = takesCaption.mapToItem(f.contentItem, 0, 0).y
            if (y < f.contentY || y + 110 > f.contentY + f.height)
                f.contentY = Math.max(0, Math.min(f.contentHeight - f.height, y - 40))
        }
    }

    readonly property var firstLine: selection && selection.firstRow !== undefined ? lines[selection.firstRow] : null
    readonly property var lastLine: selection && selection.lastRow !== undefined ? lines[selection.lastRow] : null
    readonly property int flaggedInSelection: {
        if (!firstLine || !lastLine) return 0
        let n = 0
        for (let r = selection.firstRow; r <= selection.lastRow; ++r) n += lines[r].flag ? 1 : 0
        return n
    }
    readonly property var plan: editor.plan
    readonly property var opts: editor.plan.options
    // The selected shot, kept current as the plan changes.
    readonly property var shot: selection && selection.kind === "shot" ? (plan.shots.length, plan.shot(selection.id)) : null
    readonly property bool hasShot: shot !== null && shot.id !== undefined
    readonly property var shotFields: hasShot ? shot.fields : ({})
    // Lyric rows the selection covers, for singers.
    readonly property var rows: {
        if (!selection || selection.firstRow === undefined || selection.kind === "word") return []
        const out = []
        for (let r = selection.firstRow; r <= selection.lastRow; ++r) out.push(r)
        return out
    }
    readonly property var commonSingers: {
        if (rows.length === 0) return []
        const all = plan.singers
        let common = (all[rows[0]] || {singers: []}).singers.slice()
        for (const r of rows) common = common.filter(c => ((all[r] || {singers: []}).singers).indexOf(c) >= 0)
        return common
    }
    function characterName(id) {
        for (const c of plan.characters) if (c.id === id) return c.fields.name
        return "?"
    }
    function setShot(field, value) {
        const fields = {}
        fields[field] = value
        return plan.run({op: "set", kind: "shot", id: shot.id, fields: fields}).ok
    }
    function addShot(track) {
        const a = Math.round(selection.start / 1000 * editor.fps), b = Math.max(a + 1, Math.round(selection.end / 1000 * editor.fps))
        const r = plan.run({op: "shot.add", track: track, start: a, end: b})
        if (r.ok) {
            const s = plan.shot(r.id)
            ins.select({kind: "shot", id: r.id, start: s.startMs, end: s.endMs, label: "Shot " + s.number})
        }
    }

    readonly property var word: {
        if (!selection || selection.kind !== "word" || !firstLine) return null
        for (const w of firstLine.words) if (w.index === selection.word) return w
        return null
    }

    function stamp(ms) {
        const s = ms / 1000, m = Math.floor(s / 60)
        return m + ":" + (s - 60 * m).toFixed(2).padStart(5, "0")
    }

    component Small: Button {
        id: b
        focusPolicy: Qt.NoFocus
        implicitHeight: 28; leftPadding: 10; rightPadding: 10
        property bool active: false
        contentItem: Text { text: b.text; color: b.enabled ? (b.active ? "#122328" : "#edf1ef") : "#748084"; font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: 4; color: b.active ? "#87dec7" : b.hovered ? "#3b4b52" : "#2b383f" }
    }
    component Caption: Text { color: "#adb8ba"; font.pixelSize: 11 }
    component MsField: TextField {
        implicitHeight: 24; implicitWidth: 70
        font.pixelSize: 11; font.family: "Menlo"; color: "#edf1ef"
        topPadding: 2; bottomPadding: 2; leftPadding: 6; rightPadding: 6
        background: Rectangle { radius: 3; color: "#162027"; border.color: parent.activeFocus ? "#87dec7" : "#34434a" }
    }
    component Value: Text { color: "#edf1ef"; font.pixelSize: 13; wrapMode: Text.Wrap }

    ScrollView {
        id: scroller
        anchors.fill: parent; anchors.margins: 12
        contentWidth: availableWidth
        ColumnLayout {
            width: parent.width
            spacing: 8

            // ---------------------------------------------------------- nothing selected
            ColumnLayout {
                visible: ins.selection === null
                Layout.fillWidth: true
                spacing: 6
                Text { text: editor.projectName || "No project"; color: "#edf1ef"; font.pixelSize: 16; font.bold: true }
                Caption { text: ins.lines.length ? ins.lines.length + " sung lines" : "No aligned lyrics yet" }
                Value {
                    Layout.fillWidth: true
                    visible: ins.lines.length > 0
                    text: editor.lyrics.flaggedLines === 0 ? "Every line agrees across the aligners."
                          : editor.lyrics.flaggedLines + (editor.lyrics.flaggedLines === 1 ? " line needs" : " lines need") + " a listen (amber)."
                    color: editor.lyrics.flaggedLines ? "#e0a24a" : "#87dec7"
                }
                Small { visible: editor.lyrics.flaggedLines > 0; text: "Go to next line to check"; onClicked: ins.nextFlagged() }
                // How to select part of the song: nothing on the timeline says so on its own.
                Rectangle {
                    objectName: "selectHint"
                    Layout.fillWidth: true; Layout.topMargin: 10
                    implicitHeight: hintText.implicitHeight + 16
                    radius: 5; color: "#1f2b30"; border.color: "#34434a"
                    Text {
                        id: hintText
                        x: 8; y: 8; width: parent.width - 16
                        wrapMode: Text.Wrap; lineHeight: 1.25; color: "#c3cdce"; font.pixelSize: 12
                        textFormat: Text.StyledText
                        text: "<b>Select part of the song</b><br>"
                              + "Drag across the waveform or any empty space · click a section, shot or lyric (⇧-click for more lines) · "
                              + "press <b>I</b> and <b>O</b> at the playhead · <b>X</b> selects the shot or section under it · Esc clears"
                    }
                }
                Caption {
                    Layout.fillWidth: true; Layout.topMargin: 10; wrapMode: Text.Wrap; lineHeight: 1.25
                    text: ("Click a lyric to select it. Drag it to move; drag an edge to trim. Zoom in (pinch, ⌘-scroll or ⌘=) to edit single words.\n\n"
                          + "Space play · Return play selection · L loop · ↑↓ previous/next line · ⌘Z undo").replace(/⌘/g, Kbd.mod())
                }
            }

            // ---------------------------------------------------------- any selection
            ColumnLayout {
                visible: ins.selection !== null
                Layout.fillWidth: true
                spacing: 6
                Caption {
                    text: !ins.selection ? "" : ins.hasShot ? "Shot on V" + ins.shot.track + (ins.shotFields.type ? " · " + ins.shot.typeLabel : "")
                        : ({line: "Lyric line", lines: "Lyric lines", word: "Word", region: "Section",
                            instrumental: "Instrumental", range: "Range", cutaway: "Cutaway"})[ins.selection.kind] || ""
                }
                Value {
                    Layout.fillWidth: true
                    font.pixelSize: 16; font.bold: true
                    text: ins.selection ? (ins.selection.kind === "line" ? (ins.firstLine ? ins.firstLine.text : "") : ins.hasShot ? "Shot " + ins.shot.number : ins.selection.label) : ""
                }
                Caption {
                    text: ins.selection ? ins.stamp(ins.selection.start) + " → " + ins.stamp(ins.selection.end)
                                          + "   (" + ((ins.selection.end - ins.selection.start) / 1000).toFixed(2) + " s)" : ""
                    font.family: "Menlo"
                }
                RowLayout {
                    Small {
                        objectName: "playSelection"
                        text: editor.playing ? "Stop" : "Play"
                        onClicked: editor.playing ? editor.togglePlay() : editor.playRange(ins.selection.start, ins.selection.end)
                    }
                    Small { text: "Loop"; active: editor.looping; onClicked: editor.looping = !editor.looping }
                    Small { objectName: "inspectorZoomTo"; text: "Zoom to"; onClicked: ins.zoomTo(ins.selection.start, ins.selection.end) }
                }
                Value {
                    Layout.fillWidth: true
                    visible: ins.flaggedInSelection > 0
                    color: "#e0a24a"; font.pixelSize: 12
                    text: "The aligners disagree here. Listen, adjust if needed, then mark it checked."
                }
                // Plan a shot over the selected lines, section or range.
                RowLayout {
                    visible: ins.selection !== null && ["line", "lines", "region", "instrumental", "range"].indexOf(ins.selection.kind) >= 0
                    Caption { text: "Add shot"; Layout.preferredWidth: 60 }
                    Small { objectName: "addShotV1"; text: "on V1"; onClicked: ins.addShot(1) }
                    Small { objectName: "addShotV2"; text: "on V2"; onClicked: ins.addShot(2) }
                }
                Small {
                    objectName: "markChecked"
                    visible: ins.flaggedInSelection > 0
                    text: "Mark checked"
                    onClicked: editor.lyrics.markChecked(ins.firstLine.first, ins.lastLine.last)
                }
            }

            // ---------------------------------------------------------- single word
            ColumnLayout {
                visible: ins.word !== null
                Layout.fillWidth: true
                spacing: 6
                Repeater {
                    model: [{label: "Start", key: "start"}, {label: "End", key: "end"}]
                    RowLayout {
                        required property var modelData
                        Caption { text: modelData.label; Layout.preferredWidth: 36 }
                        Small { text: "−10"; onClicked: nudge(-10) }
                        Value { text: ins.word ? ins.stamp(ins.word[parent.modelData.key]) : ""; font.family: "Menlo"; Layout.preferredWidth: 72 }
                        Small { text: "+10"; onClicked: nudge(10) }
                        function nudge(ms) {
                            const w = ins.word
                            if (!w) return
                            if (modelData.key === "start") editor.lyrics.correct(w.index, w.start + ms, w.end)
                            else editor.lyrics.correct(w.index, w.start, w.end + ms)
                        }
                    }
                }
            }

            // ---------------------------------------------------------- words of one line
            ColumnLayout {
                visible: ins.selection !== null && ins.selection.kind === "line"
                Layout.fillWidth: true
                spacing: 2
                Caption { text: "Words (ms)"; Layout.topMargin: 6 }
                Repeater {
                    model: ins.selection && ins.selection.kind === "line" && ins.firstLine ? ins.firstLine.words : []
                    RowLayout {
                        required property var modelData
                        spacing: 4
                        Text {
                            text: parent.modelData.word; color: parent.modelData.flag ? "#e0a24a" : "#d2e2de"
                            font.pixelSize: 12; Layout.preferredWidth: 90; elide: Text.ElideRight
                            MouseArea {
                                anchors.fill: parent
                                onClicked: ins.select({kind: "word", word: parent.parent.modelData.index, firstRow: ins.selection.firstRow,
                                                       lastRow: ins.selection.lastRow, start: parent.parent.modelData.start,
                                                       end: parent.parent.modelData.end, label: parent.parent.modelData.word})
                            }
                        }
                        MsField {
                            text: parent.modelData.start; validator: IntValidator { bottom: 0 }
                            onEditingFinished: if (Number(text) !== parent.modelData.start && !editor.lyrics.correct(parent.modelData.index, Number(text), parent.modelData.end)) text = parent.modelData.start
                        }
                        MsField {
                            text: parent.modelData.end; validator: IntValidator { bottom: 1 }
                            onEditingFinished: if (Number(text) !== parent.modelData.end && !editor.lyrics.correct(parent.modelData.index, parent.modelData.start, Number(text))) text = parent.modelData.end
                        }
                    }
                }
            }

            // ---------------------------------------------------------- cutaway
            ColumnLayout {
                visible: ins.selection !== null && ins.selection.kind === "cutaway"
                Layout.fillWidth: true
                property string edge: "out"
                id: cut
                RowLayout {
                    Repeater {
                        model: ["in", "out", "move", "slip"]
                        Small { required property string modelData; text: modelData; active: cut.edge === modelData; onClicked: cut.edge = modelData }
                    }
                }
                RowLayout {
                    Small { text: "−1 frame"; onClicked: editor.change(cut.edge, -1) }
                    Small { text: "+1 frame"; onClicked: editor.change(cut.edge, 1) }
                }
                Caption { text: "Source in: frame " + editor.sourceIn }
            }

            // ---------------------------------------------------------- who sings the selected lines
            ColumnLayout {
                visible: ins.rows.length > 0 && !ins.hasShot
                Layout.fillWidth: true
                spacing: 4
                Caption { text: ins.rows.length === 1 ? "Sung by" : "Sung by (" + ins.rows.length + " lines)"; Layout.topMargin: 6 }
                Caption {
                    visible: ins.plan.characters.length === 0
                    Layout.fillWidth: true; wrapMode: Text.Wrap
                    text: "Add the cast in Plan to say who sings each line."
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 4
                    Repeater {
                        model: ins.plan.characters
                        Small {
                            required property var modelData
                            objectName: "singer_" + modelData.fields.name
                            text: modelData.fields.name
                            active: ins.commonSingers.indexOf(modelData.id) >= 0
                            onClicked: {
                                const id = modelData.id
                                const next = active ? ins.commonSingers.filter(c => c !== id) : ins.commonSingers.concat([id])
                                ins.plan.run({op: "singers", rows: ins.rows, singers: next})
                            }
                        }
                    }
                }
            }

            // ---------------------------------------------------------- shot spec
            ColumnLayout {
                id: shotSpec
                visible: ins.hasShot
                Layout.fillWidth: true
                spacing: 8
                property bool more: false
                Caption {
                    text: ins.hasShot ? (ins.shot.end - ins.shot.start) + " frames" + (ins.plan.maxFrames(ins.shotFields.route || "") > 0
                          ? " · route limit " + (ins.plan.maxFrames(ins.shotFields.route) / editor.fps).toFixed(1) + " s" : "") : ""
                }
                // The lyrics this shot covers, and who sings them.
                Repeater {
                    model: ins.hasShot ? ins.plan.linesIn(ins.shot.start, ins.shot.end) : []
                    Text {
                        required property var modelData
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        text: "“" + modelData.text + "”" + (modelData.singers.length ? "  — " + modelData.singers.join(", ") : "")
                        color: "#bcd3cc"; font.pixelSize: 11; font.italic: true
                    }
                }
                // Problems with this shot, with their fixes.
                Repeater {
                    model: ins.hasShot ? ins.plan.checksFor(ins.shot.id) : []
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Rectangle { Layout.alignment: Qt.AlignTop; Layout.topMargin: 4; width: 7; height: 7; radius: 3.5
                                    color: modelData.level === "error" ? "#f0a08f" : modelData.level === "todo" ? "#e0a24a" : "#9fb4d8" }
                        Text { Layout.fillWidth: true; wrapMode: Text.Wrap; text: parent.modelData.message; color: "#edf1ef"; font.pixelSize: 11 }
                        Small {
                            visible: parent.modelData.fix === "splitLines"
                            objectName: "shotSplitLines"
                            text: "Split at lines"
                            onClicked: ins.plan.run({op: "shot.splitLines", id: ins.shot.id})
                        }
                    }
                }
                PlanField {
                    kind: "shot"; itemId: ins.hasShot ? ins.shot.id : 0; field: "type"; label: "Type"; item: ins.shot; required: true
                    PlanCombo { objectName: "shotType"; options: ins.opts.type; value: parent.parent.value; onPicked: v => parent.parent.set(v) }
                }
                PlanField {
                    kind: "shot"; itemId: ins.hasShot ? ins.shot.id : 0; field: "purpose"; label: ins.shotFields.type === "gap" ? "Reason for the gap" : "Purpose"
                    item: ins.shot; required: true
                    PlanText { objectName: "shotPurpose"; multiline: true; lines: 2; value: parent.parent.value; placeholder: "The story beat or musical moment it is for"; onCommit: t => parent.parent.set(t) }
                }
                PlanField {
                    id: whoField
                    visible: ins.shotFields.type !== "gap"
                    kind: "shot"; itemId: ins.hasShot ? ins.shot.id : 0; field: "who"; label: "Who is in frame"; item: ins.shot; required: true
                    readonly property var entries: list
                    Repeater {
                        model: whoField.entries
                        RowLayout {
                            required property var modelData
                            required property int index
                            objectName: "whoEntry" + index
                            Layout.fillWidth: true
                            Text { text: ins.characterName(modelData.character); color: "#edf1ef"; font.pixelSize: 12; Layout.preferredWidth: 80; elide: Text.ElideRight }
                            PlanCombo {
                                unsetLabel: "Any look"
                                options: ins.plan.looks.filter(l => l.character === parent.modelData.character).map(l => ({value: l.id, label: l.fields.name}))
                                value: parent.modelData.look
                                onPicked: v => {
                                    const next = whoField.entries.slice()
                                    const entry = {character: next[parent.index].character}
                                    if (v !== "") entry.look = v
                                    next[parent.index] = entry
                                    whoField.set(next)
                                }
                            }
                            Small { text: "×"; onClicked: whoField.set(whoField.entries.filter((e, i) => i !== parent.index)) }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        PlanCombo {
                            objectName: "shotWhoAdd"
                            visible: whoField.value !== "none"
                            unsetLabel: ins.plan.characters.length ? "Add someone…" : "No cast yet"
                            options: ins.plan.characters.filter(c => !whoField.entries.some(e => e.character === c.id)).map(c => ({value: c.id, label: c.fields.name}))
                            value: ""
                            onPicked: v => { if (v !== "") whoField.set(whoField.entries.concat([{character: v}])) }
                        }
                        Small {
                            objectName: "shotNoOne"
                            text: whoField.value === "none" ? "No one in frame ✓" : "No one"
                            active: whoField.value === "none"
                            onClicked: whoField.set(whoField.value === "none" ? [] : "none")
                        }
                    }
                }
                PlanField {
                    visible: ins.shotFields.type !== "gap"
                    kind: "shot"; itemId: ins.hasShot ? ins.shot.id : 0; field: "where"; label: "Location"; item: ins.shot; required: true
                    PlanCombo {
                        objectName: "shotWhere"
                        options: [{value: "none", label: "No location (abstract, title)"}].concat(ins.plan.locations.map(l => ({value: l.id, label: l.fields.name})))
                        value: parent.parent.value
                        onPicked: v => parent.parent.set(v)
                    }
                }
                // The objects that matter in the shot; their pictures go with the shot's.
                PlanField {
                    id: propsField
                    objectName: "shotPropsField"
                    visible: ins.shotFields.type !== "gap"
                    kind: "shot"; itemId: ins.hasShot ? ins.shot.id : 0; field: "props"; label: "Props"; item: ins.shot
                    readonly property var entries: list
                    Flow {
                        Layout.fillWidth: true; spacing: 4
                        visible: propsField.entries.length > 0
                        Repeater {
                            model: propsField.entries
                            Rectangle {
                                required property var modelData
                                required property int index
                                objectName: "shotProp" + index
                                width: propRow.implicitWidth + 12; height: 24; radius: 4; color: "#45371c"
                                Row {
                                    id: propRow
                                    x: 8; anchors.verticalCenter: parent.verticalCenter; spacing: 6
                                    Text { text: (ins.plan.props.find(p => p.id === parent.parent.modelData) || {fields: {name: "Removed prop"}}).fields.name
                                           color: "#f0d39a"; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
                                    Text {
                                        objectName: "shotPropRemove" + parent.parent.index
                                        text: "×"; color: propX.containsMouse ? "#f0a08f" : "#c9a96a"; font.pixelSize: 14; anchors.verticalCenter: parent.verticalCenter
                                        MouseArea { id: propX; anchors.fill: parent; anchors.margins: -4; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                                    onClicked: { const i = parent.parent.parent.index; propsField.set(propsField.entries.filter((e, k) => k !== i)) } }
                                    }
                                }
                            }
                        }
                    }
                    PlanCombo {
                        objectName: "shotPropAdd"
                        unsetLabel: ins.plan.props.length ? "Add a prop…" : "No props yet (Plan › Props)"
                        options: ins.plan.props.filter(p => propsField.entries.indexOf(p.id) < 0).map(p => ({value: p.id, label: p.fields.name}))
                        value: ""
                        onPicked: v => { if (v !== "") propsField.set(propsField.entries.concat([v])) }
                    }
                }
                RowLayout {
                    visible: ins.shotFields.type !== "gap"
                    Layout.fillWidth: true
                    PlanField {
                        kind: "shot"; itemId: ins.hasShot ? ins.shot.id : 0; field: "framing"; label: "Framing"; item: ins.shot; required: true
                        PlanCombo { objectName: "shotFraming"; options: ins.opts.framing; value: parent.parent.value; onPicked: v => parent.parent.set(v) }
                    }
                    PlanField {
                        kind: "shot"; itemId: ins.hasShot ? ins.shot.id : 0; field: "move"; label: "Camera move"; item: ins.shot; required: true
                        PlanCombo { objectName: "shotMove"; options: ins.opts.move; value: parent.parent.value; onPicked: v => parent.parent.set(v) }
                    }
                }
                PlanField {
                    visible: ins.shotFields.type !== "gap"
                    kind: "shot"; itemId: ins.hasShot ? ins.shot.id : 0; field: "route"; label: "Route"; item: ins.shot; required: true
                    PlanCombo { objectName: "shotRoute"; options: ins.opts.route; value: parent.parent.value; onPicked: v => parent.parent.set(v) }
                }
                // The pictures this shot sends to the video model (nine at most): each view's chosen
                // picture of who, where and the props, picked by the framing; click one to send it or not.
                ColumnLayout {
                    id: shotPics
                    visible: ins.shotFields.type !== "gap" && ins.hasShot
                    Layout.fillWidth: true
                    spacing: 4
                    readonly property var pics: ins.hasShot ? (ins.plan.references, ins.plan.shots, ins.plan.revision, ins.plan.shotPictures(ins.shot.id))
                                                            : ({picked: [], candidates: [], overridden: false, limit: 9})
                    function toggle(id) {
                        const now = pics.picked.map(r => r.id)
                        const i = now.indexOf(id)
                        if (i >= 0) now.splice(i, 1); else now.push(id)
                        ins.plan.run({op: "set", kind: "shot", id: ins.shot.id, fields: {pictures: now.length ? now : []}})
                    }
                    // The count on its own line and the links below it, so the row never
                    // forces the panel wider than it is.
                    Caption {
                        objectName: "shotPicCount"
                        Layout.fillWidth: true; wrapMode: Text.Wrap
                        text: shotPics.pics.candidates.length ? "Pictures for the video model: " + shotPics.pics.picked.length + " of " + shotPics.pics.limit
                                                              : "Pictures: none chosen yet for who, where and props"
                        color: shotPics.pics.picked.length > shotPics.pics.limit ? "#f0a08f" : "#8fa0a4"
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 14
                        Text {
                            objectName: "shotPicPick"
                            text: "Pick…"; color: pickPics.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 11
                            MouseArea { id: pickPics; anchors.fill: parent; anchors.margins: -3; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                        onClicked: ins.Window.window.openTake(ins.shot.id, {}) }
                            ToolTip.visible: pickPics.containsMouse; ToolTip.delay: 400
                            ToolTip.text: "Pick any pictures in the plan, in the order the prompt names them"
                        }
                        Text {
                            objectName: "shotPicReset"
                            visible: shotPics.pics.overridden
                            text: "Back to the framing's picks"; color: resetPics.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 11
                            MouseArea { id: resetPics; anchors.fill: parent; anchors.margins: -3; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                        onClicked: ins.plan.run({op: "set", kind: "shot", id: ins.shot.id, fields: {pictures: []}}) }
                        }
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 4
                        Repeater {
                            model: shotPics.pics.candidates
                            PictureTile {
                                id: cand
                                required property var modelData
                                objectName: "shotRef" + modelData.id
                                picture: modelData
                                // Its place in the prompt: <Picture n>.
                                order: shotPics.pics.picked.findIndex(r => r.id === modelData.id)
                                selected: modelData.picked
                                square: true; size: 48; dimmed: 0.35
                                label: (modelData.extra ? modelData.ownerName + " · " : "") + modelData.viewName
                                labelColor: modelData.picked ? "#adb8ba" : "#5d6c71"
                                detail: (modelData.picked ? "Sent as picture " + (order + 1) : "Left out: click to send it") + (modelData.note ? "\n" + modelData.note : "")
                                onClicked: shotPics.toggle(modelData.id)
                            }
                        }
                    }
                }
                // The video: the prompt for this route (written by the agent or by hand), the takes
                // made so far (the chosen one plays in the cut), and Generate take.
                ColumnLayout {
                    id: shotVideo
                    objectName: "shotVideo"
                    visible: ins.shotFields.type !== "gap" && ins.hasShot
                    Layout.fillWidth: true
                    spacing: 6
                    readonly property string route: ins.shotFields.route || ""
                    readonly property var topts: ins.hasShot ? (ins.plan.revision, editor.workflows.list, editor.generator.jobs, editor.generator.choices,
                                                               editor.generator.takeOptions(ins.shot.id)) : ({})
                    readonly property var takes: ins.hasShot ? (ins.plan.revision, ins.plan.takesFor(ins.shot.id)) : []
                    readonly property var jobs: ins.hasShot ? editor.generator.jobs.filter(j => j.owner === "shot" && j.ownerId === ins.shot.id) : []
                    // Takes not watched yet, and the one the viewer is auditioning.
                    readonly property var freshIds: editor.newTakes.map(n => n.take)
                    function isNew(id) { return freshIds.indexOf(id) >= 0 }
                    readonly property int auditioned: ins.hasShot && editor.audition.shot === ins.shot.id ? editor.audition.take : 0
                    property string note: ""
                    function setPrompt(text) {
                        const all = Object.assign({}, ins.shotFields.prompts || {})
                        if (text.trim()) all[route] = text; else delete all[route]
                        ins.setShot("prompts", all)
                    }
                    RowLayout {
                        Layout.fillWidth: true; Layout.topMargin: 4
                        Text { text: "Video"; color: "#edf1ef"; font.pixelSize: 13; font.weight: Font.DemiBold; Layout.fillWidth: true }
                        Text {
                            objectName: "shotPromptAsk"
                            visible: shotVideo.route !== ""
                            text: "Ask your agent…"; color: promptAsk.containsMouse ? "#9fe3d0" : "#87dec7"; font.pixelSize: 11
                            MouseArea { id: promptAsk; anchors.fill: parent; anchors.margins: -3; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                        onClicked: ins.Window.window.askAgent("shot", ins.shot.id, "prompts", "Video prompt") }
                        }
                    }
                    Caption {
                        visible: shotVideo.route === "" || shotVideo.topts.use === ""
                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#8fa0a4"
                        text: shotVideo.route === "" ? "Choose a route to make takes: H3 lip-sync with the song, or reference-to-video."
                                                     : "Takes are made with H3 lip-sync or reference-to-video for now."
                    }
                    PlanText {
                        id: shotPrompt
                        objectName: "shotPrompt"
                        visible: shotVideo.route !== ""
                        multiline: true; lines: 3; collapsedLines: 4
                        value: (ins.shotFields.prompts || {})[shotVideo.route] || ""
                        placeholder: "The video prompt for " + (ins.opts.route.find(o => o.value === shotVideo.route) || {label: "this route"}).label.toLowerCase()
                                     + ", naming the pictures as " + ((shotVideo.topts.pictures || [])[0] || {label: "<Picture 1>"}).label
                        onCommit: t => shotVideo.setPrompt(t)
                    }
                    // Takes: the chosen one plays in the cut. Click to choose; right-click for more.
                    Caption {
                        id: takesCaption
                        objectName: "shotTakeCount"
                        visible: shotVideo.takes.length > 0 || shotVideo.jobs.length > 0
                        readonly property int fresh: shotVideo.takes.filter(t => shotVideo.isNew(t.id)).length
                        text: shotVideo.takes.length ? "Takes: " + shotVideo.takes.length + (shotVideo.takes.some(t => t.chosen) ? "" : " · none chosen")
                                                       + (fresh ? " · " + fresh + " new" : "") : "Takes"
                    }
                    Flow {
                        Layout.fillWidth: true
                        spacing: 6
                        Repeater {
                            model: shotVideo.takes
                            Item {
                                id: takeTile
                                required property var modelData
                                objectName: "shotTake" + modelData.number
                                width: 74; height: 60
                                readonly property bool watching: shotVideo.auditioned === modelData.id
                                readonly property bool fresh: shotVideo.isNew(modelData.id)
                                Rectangle {
                                    width: 74; height: 44; radius: 3; clip: true; color: "#0c1216"
                                    border.color: takeTile.watching ? "#e7b75f" : takeTile.modelData.chosen ? "#87dec7" : "#34434a"
                                    border.width: takeTile.watching || takeTile.modelData.chosen ? 2 : 1
                                    Image { anchors.fill: parent; anchors.margins: 2; source: takeTile.modelData.thumbUrl || ""; fillMode: Image.PreserveAspectCrop
                                            asynchronous: true; sourceSize: Qt.size(148, 88); opacity: takeTile.modelData.chosen || takeTile.watching || takeTile.fresh ? 1 : 0.6 }
                                    Text { visible: takeTile.modelData.missing; anchors.centerIn: parent; text: "missing"; color: "#f0a08f"; font.pixelSize: 9 }
                                }
                                // Upscaled, or made at the upscaled size.
                                Rectangle {
                                    objectName: "shotTakeHd" + takeTile.modelData.number
                                    visible: !!takeTile.modelData.hd
                                    x: 3; y: 3; width: hdText.implicitWidth + 6; height: 13; radius: 3; color: "#d0101820"; border.color: "#e7d08a"
                                    Text { id: hdText; anchors.centerIn: parent; text: "HD"; color: "#e7d08a"; font.pixelSize: 8; font.weight: Font.Bold }
                                }
                                // Not watched yet.
                                Rectangle {
                                    objectName: "shotTakeNew" + takeTile.modelData.number
                                    visible: takeTile.fresh
                                    x: 74 - width - 3; y: 3; width: newText.implicitWidth + 8; height: 13; radius: 6.5; color: "#87dec7"
                                    Text { id: newText; anchors.centerIn: parent; text: "new"; color: "#122328"; font.pixelSize: 9; font.weight: Font.DemiBold }
                                }
                                Text { y: 46; width: 74; elide: Text.ElideRight; font.pixelSize: 9
                                       text: (takeTile.watching ? "watching" : takeTile.modelData.chosen ? "chosen" : "take " + takeTile.modelData.number)
                                             + (takeTile.modelData.refined ? " · " + takeTile.modelData.refined + "'s face" : takeTile.modelData.lipSync ? " · sync" : "")
                                       color: takeTile.watching ? "#e7b75f" : takeTile.modelData.chosen ? "#87dec7" : "#8fa0a4" }
                                MouseArea {
                                    id: takeMouse
                                    anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onClicked: m => {
                                        if (m.button === Qt.RightButton) { takeMenu.popup(); return }
                                        // Choosing rebuilds this list, so everything else happens first.
                                        const id = takeTile.modelData.id, chosen = takeTile.modelData.chosen, plan = ins.plan
                                        editor.markTakeSeen(id)
                                        if (chosen) editor.endAudition()
                                        editor.seek(ins.shot.start)
                                        if (!chosen) plan.run({op: "set", kind: "take", id: id, fields: {approved: true}})
                                    }
                                }
                                ToolTip.visible: takeMouse.containsMouse; ToolTip.delay: 400
                                ToolTip.text: "Take " + takeTile.modelData.number + " · " + takeTile.modelData.seconds.toFixed(2) + " s"
                                              + (takeTile.modelData.generation && takeTile.modelData.generation.seed !== undefined ? " · seed " + takeTile.modelData.generation.seed : "")
                                              + (takeTile.fresh ? " · new" : "")
                                              + (takeTile.modelData.upscaledUrl ? "\nUpscaled to " + takeTile.modelData.upscaledWidth + " × " + takeTile.modelData.upscaledHeight
                                                 : takeTile.modelData.hd ? "\nMade at " + takeTile.modelData.width + " × " + takeTile.modelData.height + " from an upscaled version" : "")
                                              + (takeTile.modelData.refined ? "\n" + takeTile.modelData.refined + "'s face refined, from take " + takeTile.modelData.refinedFromNumber : "")
                                              + (takeTile.modelData.chosen ? "\nPlays in the cut" : "\nClick to play this one in the cut; right-click to watch it first")
                                Menu {
                                    id: takeMenu
                                    MenuItem { objectName: "takeWatch"; text: "Watch in the viewer"; onTriggered: editor.auditionTake(takeTile.modelData.id, true) }
                                    MenuItem { objectName: "takeChoose"; text: takeTile.modelData.chosen ? "Chosen for this shot" : "Choose for this shot"; enabled: !takeTile.modelData.chosen
                                               onTriggered: ins.plan.run({op: "set", kind: "take", id: takeTile.modelData.id, fields: {approved: true}}) }
                                    MenuItem { objectName: "takeRefine"; text: "Refine faces…"; onTriggered: ins.Window.window.openRefine(takeTile.modelData.id) }
                                    MenuItem { objectName: "takeUpscale"; text: takeTile.modelData.upscaledUrl ? "Upscale again…" : "Upscale…"; onTriggered: ins.Window.window.openUpscale(takeTile.modelData.id) }
                                    MenuItem { objectName: "takeUnupscale"; visible: !!takeTile.modelData.upscaledUrl; height: visible ? implicitHeight : 0; text: "Remove upscaled version"
                                               onTriggered: ins.plan.run({op: "take.unupscale", id: takeTile.modelData.id}) }
                                    MenuItem { objectName: "takeAgain"; visible: !takeTile.modelData.refined; height: visible ? implicitHeight : 0; text: "Generate again with its seed…"
                                               onTriggered: ins.Window.window.openTake(ins.shot.id, {seed: takeTile.modelData.generation.seed, workflow: takeTile.modelData.generation.workflow,
                                                                                                     settings: Object.fromEntries((takeTile.modelData.generation.settings || []).map(x => [x.key, x.value]))}) }
                                    MenuItem { objectName: "takeRemove"; text: "Remove take"; onTriggered: ins.plan.run({op: "remove", kind: "take", id: takeTile.modelData.id}) }
                                }
                            }
                        }
                        // Takes on their way, or failed. With live previews, the latest sampling step
                        // plays on the tile; click to watch it large in the viewer, ■ to stop it.
                        Repeater {
                            model: shotVideo.jobs
                            Item {
                                id: jobTile
                                required property var modelData
                                objectName: "shotTakeJob"
                                width: 74; height: 60
                                readonly property bool failed: modelData.state === "failed"
                                readonly property bool watching: editor.generator.watching === modelData.id
                                readonly property bool hasPreview: !!modelData.preview
                                Rectangle {
                                    width: 74; height: 44; radius: 3; clip: true
                                    color: jobTile.failed ? "#3a2420" : jobTile.hasPreview ? "#0c1216" : "#1f3a35"
                                    border.color: jobTile.watching ? "#e7b75f" : jobTile.failed ? "#a5574a" : "#3f9c84"
                                    border.width: jobTile.watching ? 2 : 1
                                    LivePreview {
                                        objectName: "shotTakeJobPreview"
                                        anchors.fill: parent; anchors.margins: 2
                                        visible: jobTile.hasPreview && !jobTile.failed
                                        source: jobTile.modelData.preview || ""
                                        fps: jobTile.modelData.previewFps || 24
                                        fillMode: Image.PreserveAspectCrop
                                    }
                                    Text { visible: !jobTile.hasPreview || jobTile.failed
                                           anchors.fill: parent; anchors.margins: 4; wrapMode: Text.Wrap; elide: Text.ElideRight; maximumLineCount: 3
                                           text: jobTile.failed ? "Failed" : jobTile.modelData.text; font.pixelSize: 9
                                           color: jobTile.failed ? "#f0a08f" : "#87dec7" }
                                }
                                Text { y: 46; width: 74; elide: Text.ElideRight; font.pixelSize: 9
                                       objectName: "shotTakeJobStep"
                                       text: jobTile.failed ? "failed" : (jobTile.watching ? "watching" : "generating")
                                             + (jobTile.modelData.steps ? " · " + jobTile.modelData.step + "/" + jobTile.modelData.steps : "")
                                       color: jobTile.failed ? "#f0a08f" : jobTile.watching ? "#e7b75f" : "#87dec7" }
                                MouseArea { id: jobMouse; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.LeftButton | Qt.RightButton
                                            cursorShape: jobTile.failed ? Qt.ArrowCursor : Qt.PointingHandCursor
                                            onClicked: m => {
                                                if (m.button === Qt.RightButton || jobTile.failed) { jobMenu.popup(); return }
                                                editor.generator.watch(jobTile.watching ? 0 : jobTile.modelData.id)
                                            } }
                                // Stop, one click away while it generates.
                                Rectangle {
                                    objectName: "shotTakeJobStop"
                                    visible: !jobTile.failed && (jobMouse.containsMouse || stopMouse.containsMouse)
                                    x: 74 - width - 3; y: 3; width: 16; height: 16; radius: 3
                                    color: stopMouse.containsMouse ? "#c0503f" : "#d0101820"; border.color: "#e07f6f"
                                    Rectangle { anchors.centerIn: parent; width: 6; height: 6; color: "#f4d6cf" }
                                    MouseArea { id: stopMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                                onClicked: editor.generator.cancel(jobTile.modelData.id) }
                                }
                                ToolTip.visible: jobMouse.containsMouse && !stopMouse.containsMouse; ToolTip.delay: 400
                                ToolTip.text: jobTile.modelData.text + (jobTile.failed ? "" : jobTile.hasPreview ? (jobTile.watching ? "\nClick to stop watching" : "\nClick to watch it in the viewer") + "; ■ stops it" : "\n■ stops it")
                                Menu {
                                    id: jobMenu
                                    MenuItem { visible: !jobTile.failed && jobTile.modelData.live; height: visible ? implicitHeight : 0; text: jobTile.watching ? "Stop watching" : "Watch in the viewer"
                                               onTriggered: editor.generator.watch(jobTile.watching ? 0 : jobTile.modelData.id) }
                                    MenuItem { visible: jobTile.failed; height: visible ? implicitHeight : 0; text: "Try again"; onTriggered: editor.generator.retry(jobTile.modelData.id) }
                                    MenuItem { visible: jobTile.failed; height: visible ? implicitHeight : 0; text: "Dismiss"; onTriggered: editor.generator.dismiss(jobTile.modelData.id) }
                                    MenuItem { visible: !jobTile.failed; height: visible ? implicitHeight : 0; text: "Cancel"; onTriggered: editor.generator.cancel(jobTile.modelData.id) }
                                }
                            }
                        }
                    }
                    // A refined take, watched or chosen: whose face was found, outlined in green.
                    ColumnLayout {
                        readonly property var shown: {
                            const t = shotVideo.takes
                            return t.find(x => x.id === shotVideo.auditioned && x.facesUrl) || (shotVideo.auditioned ? null : t.find(x => x.chosen && x.facesUrl)) || null
                        }
                        visible: shown !== null
                        Layout.fillWidth: true; spacing: 3
                        Caption { text: parent.shown ? "Take " + parent.shown.number + ": " + parent.shown.refined + "'s face was the one outlined in green" : "" }
                        Image {
                            objectName: "takeFacesFound"
                            Layout.fillWidth: true
                            Layout.preferredHeight: width * (implicitWidth > 0 ? implicitHeight / implicitWidth : 9 / 16)
                            source: parent.shown ? parent.shown.facesUrl : ""
                            fillMode: Image.PreserveAspectFit; asynchronous: true
                        }
                    }
                    RowLayout {
                        visible: shotVideo.route !== ""
                        spacing: 6
                        Small {
                            objectName: "shotGenerateTake"
                            text: "Generate take"
                            enabled: shotVideo.topts.ready === true
                            onClicked: shotVideo.note = editor.generator.generateTake(ins.shot.id, {})
                        }
                        Small {
                            objectName: "shotTakeOptions"
                            text: "With options…"
                            enabled: shotVideo.topts.use !== ""
                            onClicked: ins.Window.window.openTake(ins.shot.id, {})
                        }
                    }
                    Caption {
                        objectName: "shotTakeWhy"
                        visible: text.length > 0
                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#e7b75f"
                        text: shotVideo.note || (shotVideo.route !== "" && shotVideo.topts.use !== "" ? shotVideo.topts.why || "" : "")
                    }
                    Caption {
                        visible: shotVideo.topts.ready === true
                        Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#6f8085"
                        text: (shotVideo.topts.lipSync ? "Sings to the song: " : "") + shotVideo.topts.frames + " frames (" + Number(shotVideo.topts.seconds).toFixed(2)
                              + " s) with " + Number(shotVideo.topts.handles).toFixed(1) + " s handles · " + (shotVideo.topts.pictures || []).length + " pictures · " + shotVideo.topts.workflowName
                    }
                }
                Small {
                    visible: ins.shotFields.type !== "gap"
                    objectName: "shotMore"
                    text: shotSpec.more ? "Fewer details" : "More details (optional)"
                    onClicked: shotSpec.more = !shotSpec.more
                }
                ColumnLayout {
                    visible: shotSpec.more && ins.shotFields.type !== "gap"
                    Layout.fillWidth: true
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        PlanField {
                            kind: "shot"; itemId: ins.hasShot ? ins.shot.id : 0; field: "angle"; label: "Angle"; item: ins.shot
                            PlanCombo { options: ins.opts.angle; value: parent.parent.value; onPicked: v => parent.parent.set(v) }
                        }
                        PlanField {
                            kind: "shot"; itemId: ins.hasShot ? ins.shot.id : 0; field: "timeOfDay"; label: "Time of day"; item: ins.shot
                            PlanCombo { options: ins.opts.timeOfDay; value: parent.parent.value; onPicked: v => parent.parent.set(v) }
                        }
                    }
                    Repeater {
                        model: [{field: "action", label: "Action"}, {field: "emotion", label: "Emotion"}, {field: "composition", label: "Composition"},
                                {field: "lens", label: "Lens feel"}, {field: "notes", label: "Notes"}]
                        PlanField {
                            required property var modelData
                            kind: "shot"; itemId: ins.hasShot ? ins.shot.id : 0; field: modelData.field; label: modelData.label; item: ins.shot
                            PlanText { value: parent.parent.value; onCommit: t => parent.parent.set(t) }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        PlanField {
                            kind: "shot"; itemId: ins.hasShot ? ins.shot.id : 0; field: "energy"; label: "Energy"; item: ins.shot
                            PlanCombo { options: ins.opts.energy; value: parent.parent.value; onPicked: v => parent.parent.set(v) }
                        }
                        PlanField {
                            kind: "shot"; itemId: ins.hasShot ? ins.shot.id : 0; field: "follows"; label: "Follows from"; item: ins.shot
                            PlanCombo {
                                options: ins.plan.shots.filter(s => ins.hasShot && s.id !== ins.shot.id).map(s => ({value: s.id, label: "Shot " + s.number}))
                                value: parent.parent.value
                                onPicked: v => parent.parent.set(v)
                            }
                        }
                    }
                }
                RowLayout {
                    Layout.topMargin: 4
                    Small {
                        objectName: "shotSplitPlayhead"
                        enabled: ins.hasShot && editor.frame > ins.shot.start && editor.frame < ins.shot.end
                        text: "Split at playhead"
                        onClicked: ins.plan.run({op: "shot.split", id: ins.shot.id, at: editor.frame})
                    }
                    Small {
                        objectName: "shotDelete"
                        text: "Delete shot"
                        onClicked: { const id = ins.shot.id; ins.select(null); ins.plan.run({op: "remove", kind: "shot", id: id}) }
                    }
                }
            }
        }
    }
}
