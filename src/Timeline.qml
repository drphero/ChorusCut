import QtQuick
import QtQuick.Controls
import QtCore
import Chorus

// NLE-style timeline: ruler, song sections, picture lanes, lyric clips and the master waveform.
// Lyric clips can be moved and trimmed directly; at higher zoom every word becomes a clip.
// All edits go through the C++ lyric commands, so they share validation, saving and undo.
Rectangle {
    id: tl
    color: "#121b20"
    radius: 5
    clip: true

    // ---- view state
    property real pps: 20                    // pixels per second
    property real viewStart: 0               // seconds at the left edge of the lanes
    readonly property real header: 104
    readonly property real viewWidth: Math.max(1, width - header)
    readonly property real duration: editor.ready ? editor.length / editor.fps : 1
    readonly property real visibleSeconds: viewWidth / pps
    readonly property real minPps: viewWidth / Math.max(1, duration)
    readonly property real maxPps: 800
    readonly property real playheadSeconds: editor.frame / editor.fps

    // ---- data
    property var lines: editor.lyrics.lines
    property var regions: editor.lyrics.regions
    property var instrumental: editor.lyrics.instrumental
    property var shots: editor.plan.shots
    property var coverage: editor.plan.coverage
    readonly property bool hasPicture: editor.sourceLength > 0

    // ---- selection (owned by Main.qml)
    property var selection: null
    signal selected(var selection)

    // ---- in-progress drag preview: words [dragFirst, dragLast] shift by these deltas (ms)
    property int dragFirst: -1
    property int dragLast: -2
    property int dragStartDelta: 0
    property int dragEndDelta: 0

    // ---- in-progress shot drag: {id, track, start, end} in frames, or id -1
    property var shotDrag: ({id: -1})

    // ---- track geometry: each track has its own height, resized from its header.
    readonly property int rulerH: 26
    readonly property var trackDefaults: ({sectionH: 24, v2H: 40, v1H: 40, lyricH: 56, audioH: 160})
    readonly property var trackMinimum: ({sectionH: 18, v2H: 22, v1H: 22, lyricH: 30, audioH: 40})
    readonly property int trackMaximum: 480
    property int sectionH: trackDefaults.sectionH
    property int v2H: trackDefaults.v2H
    property int v1H: trackDefaults.v1H
    property int lyricH: trackDefaults.lyricH
    property int audioH: trackDefaults.audioH
    property real scrollY: 0                 // vertical scroll of the tracks under the fixed ruler
    // V1 and V2 always show: planned shots live there before any footage exists.
    readonly property int contentH: sectionH + v2H + v1H + lyricH + audioH
    readonly property int trackViewH: Math.max(1, height - rulerH - 12)
    readonly property int sectionY: rulerH - scrollY
    readonly property int v2Y: sectionY + sectionH
    readonly property int v1Y: v2Y + v2H
    readonly property int lyricY: v1Y + v1H
    readonly property int audioY: lyricY + lyricH
    // Lyric clips switch between two views on their own. Zoomed out, the line's text wraps
    // to fill the clip. Once most of a line's words are wide enough to read and grab, the
    // clip shows a one-row title strip with word clips below (the track must fit both rows).
    FontMetrics { id: lineFont; font.pixelSize: 12 }
    FontMetrics { id: wordFont; font.pixelSize: 11 }
    function wordsFit(words) {
        if (lyricH < 46 || words.length === 0) return false
        // A word counts only when it shows in full; an elided "res…" is not readable.
        let fit = 0
        for (const w of words)
            if ((w.end - w.start) / 1000 * pps >= wordFont.advanceWidth(w.word) + 6) ++fit
        return fit >= 0.5 * words.length
    }
    function widestWord(words) {
        let widest = 0
        for (const w of words) widest = Math.max(widest, lineFont.advanceWidth(w.word))
        return widest
    }

    function clampScroll() { scrollY = Math.max(0, Math.min(scrollY, Math.max(0, contentH - trackViewH))) }
    function resizeTrack(name, value) {
        tl[name] = Math.max(trackMinimum[name], Math.min(trackMaximum, Math.round(value)))
        clampScroll()
    }
    onContentHChanged: { clampScroll(); Qt.callLater(syncBar) }
    onHeightChanged: clampScroll()
    onScrollYChanged: Qt.callLater(syncBar)

    // Track heights are remembered between sessions.
    Settings {
        category: "timeline"
        property alias sectionHeight: tl.sectionH
        property alias cutawayHeight: tl.v2H
        property alias performanceHeight: tl.v1H
        property alias lyricsHeight: tl.lyricH
        property alias masterHeight: tl.audioH
    }

    function px(seconds) { return (seconds - viewStart) * pps }
    function sec(x) { return viewStart + x / pps }
    function clampView() {
        if (!isFinite(viewStart)) viewStart = 0   // never let a bad delta leave the view unusable
        if (!isFinite(pps) || pps <= 0) pps = minPps
        viewStart = Math.max(0, Math.min(viewStart, Math.max(0, duration - visibleSeconds)))
    }
    function zoomAround(factor, x) {
        const anchor = sec(x)
        pps = Math.max(minPps, Math.min(maxPps, pps * factor))
        viewStart = anchor - x / pps
        clampView()
    }
    function fit() { pps = minPps; viewStart = 0 }
    // Two-finger swipes (either way) and the mouse wheel pan; ⌘-scroll zooms around the pointer;
    // ⌥-scroll moves the tracks up and down. On macOS Qt reports ⌘ as Control and ⌥ as Alt.
    // Pixel deltas come from trackpads, angle deltas (120 per notch) from mouse wheels.
    function wheel(angle, pixel, modifiers, x) {
        if (modifiers & Qt.ControlModifier) {
            zoomAround(Math.pow(1.0018, angle.y || pixel.y * 4), x)
            return
        }
        const dx = pixel.x || angle.x / 4, dy = pixel.y || angle.y / 4
        if (modifiers & Qt.AltModifier) { scrollY -= dy; clampScroll(); return }
        viewStart -= (Math.abs(dx) > Math.abs(dy) ? dx : dy) / pps
        clampView()
    }
    function zoomTo(startMs, endMs) {
        const span = Math.max(0.5, (endMs - startMs) / 1000)
        pps = Math.max(minPps, Math.min(maxPps, viewWidth * 0.8 / span))
        viewStart = startMs / 1000 - 0.1 * visibleSeconds
        clampView()
    }
    function reveal(seconds) {
        if (seconds < viewStart || seconds > viewStart + visibleSeconds) {
            viewStart = seconds - 0.2 * visibleSeconds
            clampView()
        }
    }
    function clock(seconds, fine) {
        const m = Math.floor(seconds / 60), s = seconds - 60 * m
        const whole = Math.floor(s)
        let text = m + ":" + String(whole).padStart(2, "0")
        if (fine) text += "." + String(Math.floor((s - whole) * 10))
        return text
    }
    // ---- shots: snapping and free space
    // Shot edges snap within 8 px to lyric lines, sections, other shots, the playhead and the
    // song's ends; to words as well once they are wide enough to aim at.
    function snapTargets(exceptId) {
        const t = [0, duration * 1000, playheadSeconds * 1000]
        for (const l of lines) {
            t.push(l.start, l.end)
            if (pps >= 120) for (const w of l.words) t.push(w.start, w.end)
        }
        for (const r of regions) t.push(r.start, r.end)
        for (const r of instrumental) t.push(r.start, r.end)
        for (const s of shots) if (s.id !== exceptId) t.push(s.startMs, s.endMs)
        return t
    }
    // Returns the snapped frame and how far (px) it moved, or null when nothing is close.
    function snapFrame(frame, targets) {
        const ms = frame / editor.fps * 1000
        let best = null, bestPx = 8
        for (const t of targets) {
            const d = Math.abs(t - ms) / 1000 * pps
            if (d < bestPx) { bestPx = d; best = t }
        }
        return best === null ? null : {frame: Math.round(best / 1000 * editor.fps), px: bestPx}
    }
    // The free frames around [start, end) on a track, or null if another shot is in the way.
    function freeAround(track, start, end, exceptId) {
        let lo = 0, hi = editor.length
        for (const s of shots) {
            if (s.id === exceptId || s.track !== track) continue
            if (s.end <= start) lo = Math.max(lo, s.end)
            else if (s.start >= end) hi = Math.min(hi, s.start)
            else return null
        }
        return {lo: lo, hi: hi}
    }
    function trackAt(y) { return y >= v2Y && y < v2Y + v2H ? 2 : y >= v1Y && y < v1Y + v1H ? 1 : 0 }
    // Double-clicking an empty track plans a shot over the lyric line there, or over the
    // pause between lines, kept inside the free space.
    function shotSpanAt(track, ms) {
        let a = 0, b = duration * 1000
        for (const l of lines) {
            if (l.start <= ms && ms < l.end) { a = l.start; b = l.end; break }
            if (l.end <= ms) a = l.end
            else if (l.start > ms) { b = l.start; break }
        }
        let start = Math.round(a / 1000 * editor.fps), end = Math.max(start + 1, Math.round(b / 1000 * editor.fps))
        const f = Math.round(ms / 1000 * editor.fps)
        const free = freeAround(track, f, f + 1, -1)
        if (!free) return null
        start = Math.max(start, free.lo); end = Math.min(end, free.hi)
        return end > start ? {start: start, end: end} : null
    }
    function addShotAt(track, ms) {
        const span = shotSpanAt(track, ms)
        if (!span) return
        const r = editor.plan.run({op: "shot.add", track: track, start: span.start, end: span.end})
        if (r.ok) selectShot(r.id)
    }
    function selectShot(id) {
        const s = editor.plan.shot(id)
        if (s.id !== undefined) selected({kind: "shot", id: id, start: s.startMs, end: s.endMs, label: "Shot " + s.number})
    }
    readonly property var shotColors: ({
        lipsync: ["#24505a", "#6fc3d2"], performance: ["#2a4652", "#7fb0c0"], story: ["#363c66", "#959ce6"],
        broll: ["#47395c", "#b99ade"], insert: ["#56482b", "#dcb66c"], abstract: ["#28534a", "#79cdb5"],
        transition: ["#3c4347", "#9aa5aa"], title: ["#3c4347", "#9aa5aa"], gap: ["#1b2429", "#56656c"], "": ["#2e3a40", "#7a8a90"]
    })

    function snapMs(ms) {
        // Edges snap to the playhead within 6 px.
        const head = playheadSeconds * 1000
        return Math.abs(ms - head) / 1000 * pps < 6 ? Math.round(head) : ms
    }
    onWidthChanged: { if (pps < minPps) pps = minPps; clampView() }
    // The scrollbar follows the view; it is set directly to avoid a binding loop with its own clamping.
    function syncBar() {
        if (!hbar.pressed) {
            hbar.size = Math.min(1, visibleSeconds / duration)
            hbar.position = viewStart / duration
        }
        if (!vbar.pressed) {
            vbar.size = Math.min(1, trackViewH / Math.max(1, contentH))
            vbar.position = scrollY / Math.max(1, contentH)
        }
    }
    onViewStartChanged: Qt.callLater(syncBar)
    onVisibleSecondsChanged: Qt.callLater(syncBar)
    onDurationChanged: fit()
    Component.onCompleted: fit()

    Connections {
        target: editor
        function onCursorChanged() {
            if (!editor.playing) return
            const t = tl.playheadSeconds
            if (t > tl.viewStart + tl.visibleSeconds * 0.92 || t < tl.viewStart) {
                tl.viewStart = t - tl.visibleSeconds * 0.08
                tl.clampView()
            }
        }
    }

    // ---------------------------------------------------------------- track headers
    // Drag the divider at the bottom of a header to resize that track; double-click it to reset.
    Item {
        id: headerArea
        x: 0; y: tl.rulerH; width: tl.header; height: tl.trackViewH
        clip: true
        Repeater {
            // A static model: rebuilding it while a divider is dragged would end the drag.
            model: [
                {label: "Sections", key: "sectionH"},
                {label: "V2", key: "v2H"},
                {label: "V1", key: "v1H"},
                {label: "Lyrics", key: "lyricH"},
                {label: "A1  Master", key: "audioH"}
            ]
            Rectangle {
                id: trackHeader
                required property var modelData
                objectName: "trackHeader_" + modelData.key
                readonly property var tops: ({sectionH: tl.sectionY, v2H: tl.v2Y, v1H: tl.v1Y, lyricH: tl.lyricY, audioH: tl.audioY})
                y: tops[modelData.key] - tl.rulerH
                width: tl.header; height: tl[modelData.key]
                color: "#18242b"
                Text {
                    anchors.verticalCenter: parent.verticalCenter; x: 10
                    width: parent.width - 14; elide: Text.ElideRight
                    // Footage from an imported pair sits under the planned shots.
                    text: trackHeader.modelData.label + (!tl.hasPicture ? "" : trackHeader.modelData.key === "v2H" ? "  Cutaway"
                          : trackHeader.modelData.key === "v1H" ? "  Performance" : "")
                    color: "#adb8ba"; font.pixelSize: 11
                }
                Rectangle {
                    id: divider
                    objectName: "trackResize_" + trackHeader.modelData.key
                    // A 10 px grab area along the header's bottom edge (6 px proved hard to hit).
                    anchors.bottom: parent.bottom; width: parent.width; height: 10
                    color: dividerMouse.containsMouse || dividerMouse.pressed ? "#3e6b75" : "transparent"
                    Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: "#0e161a" }
                    MouseArea {
                        id: dividerMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.SizeVerCursor
                        property real startY: 0
                        property int startH: 0
                        onPressed: m => { startY = mapToItem(tl, m.x, m.y).y; startH = tl[trackHeader.modelData.key] }
                        onPositionChanged: m => {
                            if (pressed) tl.resizeTrack(trackHeader.modelData.key, startH + mapToItem(tl, m.x, m.y).y - startY)
                        }
                        onDoubleClicked: tl.resizeTrack(trackHeader.modelData.key, tl.trackDefaults[trackHeader.modelData.key])
                    }
                }
            }
        }
        WheelHandler {
            // Scrolling over the headers moves the tracks up and down.
            onWheel: e => { tl.scrollY -= e.pixelDelta.y || e.angleDelta.y / 4; tl.clampScroll() }
        }
    }
    Rectangle {
        x: 0; y: 0; width: tl.header; height: tl.rulerH; color: "#18242b"
        Text {
            anchors.verticalCenter: parent.verticalCenter; x: 10
            text: tl.clock(tl.playheadSeconds, true); color: "#87dec7"; font.family: "Menlo"; font.pixelSize: 12
        }
    }

    // ---------------------------------------------------------------- lanes area
    Item {
        id: lanes
        objectName: "timelineLanes"
        x: tl.header; y: 0; width: tl.viewWidth; height: tl.height
        clip: true

        // Empty-space interaction: click seeks, drag selects a range.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            property real pressX: 0
            property bool ranging: false
            onPressed: m => { pressX = m.x; ranging = false; editor.seekLyric(Math.round(tl.sec(m.x) * 1000)) }
            onPositionChanged: m => {
                if (!pressed) return
                if (m.y < tl.rulerH && !ranging) { editor.seekLyric(Math.round(tl.sec(m.x) * 1000)); return }
                if (Math.abs(m.x - pressX) > 4) ranging = true
                if (ranging) {
                    const a = Math.max(0, Math.min(tl.sec(pressX), tl.sec(m.x))), b = Math.min(tl.duration, Math.max(tl.sec(pressX), tl.sec(m.x)))
                    tl.selected({kind: "range", start: Math.round(a * 1000), end: Math.round(b * 1000), label: "Range"})
                }
            }
            property bool planned: false   // a double-click planned a shot; its release keeps the selection
            onReleased: m => { if (!ranging && !planned && m.y >= tl.rulerH) tl.selected(null); planned = false }
            // Scrolling anywhere over the lanes, including over clips (they pass wheel events on).
            // A MouseArea sees both axes; a WheelHandler only reacts to the vertical one by default,
            // which left two-finger sideways swipes doing nothing.
            onWheel: w => tl.wheel(w.angleDelta, w.pixelDelta, w.modifiers, w.x)
            onDoubleClicked: m => {
                const track = tl.trackAt(m.y)
                if (track && m.y >= tl.rulerH) { planned = true; tl.addShotAt(track, tl.sec(m.x) * 1000) }
            }
        }

        PinchHandler {
            target: null
            property real last: 1
            onActiveChanged: last = 1
            onActiveScaleChanged: { tl.zoomAround(activeScale / last, centroid.position.x); last = activeScale }
        }

        // ---- grid lines (under the clips)
        Repeater {
            model: ticks.model
            Rectangle {
                required property int index
                readonly property real t: ticks.first + index * ticks.step
                visible: t >= 0 && t <= tl.duration
                x: tl.px(t); y: tl.rulerH; width: 1; height: lanes.height - tl.rulerH; color: "#1f2c33"
            }
        }

        // ---- sections lane: [Heading] sections and detected instrumental parts
        Repeater {
            model: tl.regions
            Rectangle {
                required property var modelData
                x: tl.px(modelData.start / 1000); y: tl.sectionY + 2
                width: Math.max(2, (modelData.end - modelData.start) / 1000 * tl.pps); height: tl.sectionH - 4
                visible: x + width > 0 && x < lanes.width
                radius: 3; color: "#24414a"; border.color: "#3e6b75"
                Text {
                    anchors.fill: parent; anchors.leftMargin: 6; verticalAlignment: Text.AlignVCenter
                    text: parent.modelData.label; color: "#9fe3d0"; font.pixelSize: 11; elide: Text.ElideRight
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: tl.selected({kind: "region", start: parent.modelData.start, end: parent.modelData.end,
                                            label: parent.modelData.label, firstRow: parent.modelData.firstRow,
                                            lastRow: parent.modelData.lastRow})
                    onDoubleClicked: tl.zoomTo(parent.modelData.start, parent.modelData.end)
                }
            }
        }
        Repeater {
            model: tl.instrumental
            Rectangle {
                required property var modelData
                readonly property string name: "♪ " + (modelData.kind === "intro" ? "Instrumental intro" : modelData.kind === "outro" ? "Instrumental outro" : "Instrumental")
                x: tl.px(modelData.start / 1000); y: tl.sectionY + 2
                width: Math.max(2, (modelData.end - modelData.start) / 1000 * tl.pps); height: tl.sectionH - 4
                visible: x + width > 0 && x < lanes.width
                radius: 3; color: "#2b2f3a"; border.color: "#5a6178"
                Text {
                    anchors.fill: parent; anchors.leftMargin: 6; verticalAlignment: Text.AlignVCenter
                    text: parent.name; color: "#b9bfd6"; font.pixelSize: 11; font.italic: true; elide: Text.ElideRight
                }
                MouseArea {
                    anchors.fill: parent
                    onClicked: tl.selected({kind: "instrumental", start: parent.modelData.start, end: parent.modelData.end, label: parent.name})
                    onDoubleClicked: tl.zoomTo(parent.modelData.start, parent.modelData.end)
                }
            }
        }

        // ---- picture lanes: planned shots, over any imported footage
        Rectangle { y: tl.v2Y; width: parent.width; height: tl.v2H; color: "#141e24" }
        Rectangle { y: tl.v1Y; width: parent.width; height: tl.v1H; color: "#131c22" }
        Rectangle { y: tl.v1Y; width: parent.width; height: 1; color: "#0e161a" }
        Rectangle {
            visible: tl.hasPicture
            x: tl.px(0); y: tl.v1Y + 2; width: editor.baseLength / editor.fps * tl.pps; height: tl.v1H - 4
            radius: 4; color: "#2a4b59"; border.color: "#557d8c"; clip: true
            Text { x: 8; anchors.verticalCenter: parent.verticalCenter; text: editor.performanceName; color: "#edf1ef"; font.pixelSize: 11 }
        }
        Rectangle {
            id: cutaway
            objectName: "cutawayClip"
            visible: tl.hasPicture
            property int delta: 0
            property string mode: ""
            readonly property int inF: editor.cutIn + ((mode === "in" || mode === "move") ? delta : 0)
            readonly property int outF: editor.cutOut + ((mode === "out" || mode === "move") ? delta : 0)
            x: tl.px(inF / editor.fps); y: tl.v2Y + 2
            width: Math.max(4, (outF - inF) / editor.fps * tl.pps); height: tl.v2H - 4
            radius: 4; clip: true
            color: "#593d37"
            border.color: tl.selection && tl.selection.kind === "cutaway" ? "#ecedc9" : "#edab83"
            border.width: tl.selection && tl.selection.kind === "cutaway" ? 2 : 1
            Text { x: 8; anchors.verticalCenter: parent.verticalCenter; text: editor.cutawayName; color: "#edf1ef"; font.pixelSize: 11 }
            MouseArea {
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: mouseX <= 8 || mouseX >= width - 8 ? Qt.SizeHorCursor : Qt.OpenHandCursor
                property real first
                onPressed: m => {
                    first = mapToItem(lanes, m.x, m.y).x
                    cutaway.mode = m.x <= 8 ? "in" : m.x >= width - 8 ? "out" : "move"
                    tl.selected({kind: "cutaway", start: Math.round(editor.cutIn / editor.fps * 1000),
                                 end: Math.round(editor.cutOut / editor.fps * 1000), label: "Cutaway"})
                }
                onPositionChanged: m => {
                    if (!pressed) return
                    const d = Math.round((mapToItem(lanes, m.x, m.y).x - first) / tl.pps * editor.fps)
                    const a = editor.cutIn, b = editor.cutOut, s = editor.sourceIn
                    const lo = cutaway.mode === "in" ? Math.max(-a, -s) : cutaway.mode === "out" ? a + 1 - b : -a
                    const hi = cutaway.mode === "in" ? b - a - 1 : cutaway.mode === "out" ? Math.min(editor.length - b, editor.sourceLength - s - (b - a)) : editor.length - b
                    cutaway.delta = Math.max(lo, Math.min(hi, d))
                }
                onReleased: { const d = cutaway.delta; cutaway.delta = 0; if (d) editor.change(cutaway.mode, d); cutaway.mode = "" }
                onCanceled: { cutaway.delta = 0; cutaway.mode = "" }
            }
        }

        Text {
            visible: tl.shots.length === 0 && editor.ready
            x: 10; y: tl.v1Y + (tl.v1H - height) / 2
            text: "Double-click V1 or V2 to plan a shot over a lyric line, or select lines and use Add shot."
            color: "#56656c"; font.pixelSize: 11
        }
        Repeater {
            model: tl.shots
            Rectangle {
                id: shotCard
                required property var modelData
                objectName: "shotCard" + modelData.id
                readonly property bool dragging: tl.shotDrag.id === modelData.id
                readonly property int track: dragging ? tl.shotDrag.track : modelData.track
                readonly property int sf: dragging ? tl.shotDrag.start : modelData.start
                readonly property int ef: dragging ? tl.shotDrag.end : modelData.end
                readonly property bool chosen: tl.selection !== null && tl.selection.kind === "shot" && tl.selection.id === modelData.id
                readonly property var colors: tl.shotColors[modelData.fields.type || ""] || tl.shotColors[""]
                x: tl.px(sf / editor.fps); y: (track === 2 ? tl.v2Y : tl.v1Y) + 3
                width: Math.max(3, (ef - sf) / editor.fps * tl.pps); height: (track === 2 ? tl.v2H : tl.v1H) - 6
                visible: x + width > -2 && x < lanes.width + 2
                z: dragging ? 3 : 1
                radius: 4; clip: true
                color: colors[0]
                opacity: dragging ? 0.85 : 1
                border.color: chosen ? "#ecedc9" : modelData.problems ? "#e07f6f" : colors[1]
                border.width: chosen ? 2 : 1
                // The chosen take, as a strip of its still behind the label; and takes on their way.
                readonly property int makingTakes: editor.generator.jobs.filter(j => j.owner === "shot" && j.ownerId === modelData.id && j.state !== "failed").length
                Row {
                    visible: (shotCard.modelData.chosenThumb || "") !== ""
                    anchors.fill: parent; anchors.margins: 1
                    clip: true
                    opacity: 0.55
                    Repeater {
                        model: shotCard.modelData.chosenThumb ? Math.ceil(shotCard.width / Math.max(20, shotCard.height * 16 / 9)) : 0
                        Image { width: Math.max(20, shotCard.height * 16 / 9); height: shotCard.height - 2; source: shotCard.modelData.chosenThumb
                                fillMode: Image.PreserveAspectCrop; asynchronous: true; sourceSize: Qt.size(160, 90) }
                    }
                }
                Text {
                    objectName: "shotTakes" + shotCard.modelData.id
                    visible: (shotCard.modelData.takes > 0 || shotCard.makingTakes > 0) && shotCard.width > 60 && shotCard.height >= 32
                    anchors.right: parent.right; anchors.rightMargin: 5; anchors.bottom: parent.bottom; anchors.bottomMargin: 3
                    text: shotCard.makingTakes > 0 ? "generating " + shotCard.makingTakes : shotCard.modelData.takes + (shotCard.modelData.takes === 1 ? " take" : " takes")
                    color: shotCard.makingTakes > 0 ? "#e7b75f" : "#ecedc9"; font.pixelSize: 9
                }
                Column {
                    x: 6; y: 3; width: parent.width - 12 - (badge.visible ? badge.width + 4 : 0)
                    spacing: 1
                    Text {
                        width: parent.width; elide: Text.ElideRight
                        text: shotCard.modelData.label + (shotCard.modelData.stale ? "  ↻" : "")
                        color: "#edf1ef"; font.pixelSize: 11; font.weight: Font.DemiBold
                    }
                    Text {
                        visible: shotCard.height >= 32
                        width: parent.width; elide: Text.ElideRight
                        text: shotCard.modelData.fields.purpose || shotCard.modelData.typeLabel || "Unplanned shot"
                        color: "#b9c6c8"; font.pixelSize: 10
                    }
                }
                Rectangle {
                    id: badge
                    visible: shotCard.modelData.problems > 0 && shotCard.width > 30
                    anchors.right: parent.right; anchors.rightMargin: 4; y: 4
                    width: 15; height: 15; radius: 7.5; color: "#b4544a"
                    Text { anchors.centerIn: parent; text: shotCard.modelData.problems; color: "#fff4f0"; font.pixelSize: 9; font.bold: true }
                }
                MouseArea {
                    id: shotMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    property string mode: ""
                    property real pressX: 0
                    property bool moved: false
                    property var targets: []
                    property var free: null
                    cursorShape: mouseX <= 6 || mouseX >= width - 6 ? Qt.SizeHorCursor : Qt.OpenHandCursor
                    function lanePoint(m) { return mapToItem(lanes, m.x, m.y) }
                    onPressed: m => {
                        pressX = lanePoint(m).x; moved = false
                        mode = m.x <= 6 ? "in" : m.x >= width - 6 ? "out" : "move"
                        targets = tl.snapTargets(shotCard.modelData.id)
                        free = tl.freeAround(shotCard.modelData.track, shotCard.modelData.start, shotCard.modelData.end, shotCard.modelData.id)
                    }
                    function preview(p) {
                        const s0 = shotCard.modelData.start, e0 = shotCard.modelData.end, len = e0 - s0
                        const d = Math.round((p.x - pressX) / tl.pps * editor.fps)
                        let track = shotCard.modelData.track, start = s0, end = e0
                        if (mode === "move") {
                            start = s0 + d
                            const a = tl.snapFrame(start, targets), b = tl.snapFrame(start + len, targets)
                            if (a && (!b || a.px <= b.px)) start = a.frame
                            else if (b) start = b.frame - len
                            start = Math.max(0, Math.min(editor.length - len, start))
                            // Moving onto the other track works where that track is free.
                            const over = tl.trackAt(p.y)
                            if (over && over !== track && tl.freeAround(over, start, start + len, shotCard.modelData.id)) track = over
                            else start = Math.max(free.lo, Math.min(free.hi - len, start))
                            end = start + len
                        } else if (mode === "in") {
                            const a = tl.snapFrame(s0 + d, targets)
                            start = Math.max(free.lo, Math.min(e0 - 1, a ? a.frame : s0 + d))
                        } else {
                            const b = tl.snapFrame(e0 + d, targets)
                            end = Math.min(free.hi, Math.max(s0 + 1, b ? b.frame : e0 + d))
                        }
                        tl.shotDrag = {id: shotCard.modelData.id, track: track, start: start, end: end}
                    }
                    onPositionChanged: m => {
                        if (!pressed) return
                        const p = lanePoint(m)
                        if (!moved && Math.abs(p.x - pressX) < 3 && tl.trackAt(p.y) === shotCard.modelData.track) return
                        moved = true
                        preview(p)
                    }
                    onReleased: m => {
                        // Commit after clearing the preview: the edit rebuilds the cards.
                        const timeline = tl, plan = editor.plan, id = shotCard.modelData.id
                        if (moved) preview(lanePoint(m))
                        const d = timeline.shotDrag, wasMoved = moved
                        timeline.shotDrag = {id: -1}
                        if (wasMoved) {
                            const r = plan.run({op: "shot.retime", id: id, track: d.track, start: d.start, end: d.end})
                            if (r.ok) timeline.selectShot(id)
                        } else {
                            timeline.selectShot(id)
                        }
                    }
                    onCanceled: tl.shotDrag = {id: -1}
                    onDoubleClicked: editor.playRange(shotCard.modelData.startMs, shotCard.modelData.endMs)
                }
            }
        }

        // ---- lyric lane
        Rectangle { y: tl.lyricY; width: parent.width; height: tl.lyricH; color: "#152027" }
        Repeater {
            model: tl.lines
            Item {
                id: clipItem
                required property var modelData
                required property int index
                objectName: "lyricClip" + index
                readonly property int shownStart: modelData.start + (tl.dragFirst <= modelData.first && modelData.first <= tl.dragLast ? tl.dragStartDelta : 0)
                readonly property int shownEnd: modelData.end + (tl.dragFirst <= modelData.last && modelData.last <= tl.dragLast ? tl.dragEndDelta : 0)
                readonly property bool isSelected: tl.selection !== null && tl.selection.firstRow !== undefined &&
                    (tl.selection.kind === "line" || tl.selection.kind === "lines" || tl.selection.kind === "word") &&
                    tl.selection.firstRow <= index && index <= tl.selection.lastRow
                x: tl.px(shownStart / 1000); y: tl.lyricY + 4
                width: Math.max(3, (shownEnd - shownStart) / 1000 * tl.pps); height: tl.lyricH - 8
                visible: x + width > -2 && x < lanes.width + 2
                readonly property bool showWords: visible && tl.wordsFit(modelData.words)
                readonly property real widest: tl.widestWord(modelData.words)

                Rectangle {
                    anchors.fill: parent; radius: 4
                    color: clipItem.modelData.flag ? "#4a3b22" : "#28493f"
                    border.color: clipItem.isSelected ? "#ecedc9" : clipItem.modelData.flag ? "#e0a24a" : "#5fae96"
                    border.width: clipItem.isSelected ? 2 : 1
                }
                Text {
                    objectName: "lyricText" + clipItem.index
                    visible: !clipItem.showWords
                    anchors.fill: parent; anchors.leftMargin: 6; anchors.rightMargin: 4; anchors.topMargin: 2; anchors.bottomMargin: 2
                    verticalAlignment: Text.AlignVCenter
                    text: (clipItem.modelData.flag ? "? " : "") + clipItem.modelData.text
                    color: "#edf1ef"; font.pixelSize: 12; elide: Text.ElideRight
                    // Wrap between words while every word fits the clip's width; otherwise one elided row.
                    wrapMode: clipItem.width - 10 >= clipItem.widest ? Text.WordWrap : Text.NoWrap
                    maximumLineCount: Math.max(1, Math.floor(height / lineFont.height))
                }
                Text {
                    visible: clipItem.showWords
                    x: 6; y: 3; width: parent.width - 10
                    text: (clipItem.modelData.flag ? "? " : "") + clipItem.modelData.text
                    color: "#bcd3cc"; font.pixelSize: 10; elide: Text.ElideRight
                }
                // Word clips, shown once the words are wide enough to read.
                Repeater {
                    model: clipItem.showWords ? clipItem.modelData.words : []
                    Rectangle {
                        required property var modelData
                        objectName: "lyricWord" + modelData.index
                        readonly property bool dragged: tl.dragFirst <= modelData.index && modelData.index <= tl.dragLast
                        readonly property int ws: modelData.start + (dragged ? tl.dragStartDelta : 0)
                        readonly property int we: modelData.end + (dragged ? tl.dragEndDelta : 0)
                        readonly property bool chosen: tl.selection !== null && tl.selection.kind === "word" && tl.selection.word === modelData.index
                        x: (ws - clipItem.shownStart) / 1000 * tl.pps
                        y: 18; height: clipItem.height - 22
                        width: Math.max(2, (we - ws) / 1000 * tl.pps)
                        radius: 3
                        color: modelData.flag ? "#6b5228" : chosen ? "#3f7a68" : "#33604f"
                        border.color: chosen ? "#ecedc9" : "#76c4ab"
                        Text {
                            anchors.fill: parent; anchors.leftMargin: 3
                            verticalAlignment: Text.AlignVCenter
                            text: parent.modelData.word; color: "#edf1ef"; font.pixelSize: 11; elide: Text.ElideRight
                        }
                    }
                }

                MouseArea {
                    id: clipMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    property string mode: ""
                    property int first: -1
                    property int last: -1
                    property int s: 0
                    property int e: 0
                    property var limits
                    property real pressX: 0
                    property bool moved: false
                    property var word: null

                    function wordAt(x, y) {
                        // Zoomed in, the top strip grabs the whole line and the lower strip grabs words.
                        if (!clipItem.showWords || y < 16) return null
                        const t = clipItem.modelData.start + x / tl.pps * 1000
                        const slack = 6 / tl.pps * 1000
                        for (const w of clipItem.modelData.words)
                            if (t >= w.start - slack && t <= w.end + slack) return w
                        return null
                    }
                    function edge(x, startMs, endMs) {
                        const a = (startMs - clipItem.modelData.start) / 1000 * tl.pps
                        const b = (endMs - clipItem.modelData.start) / 1000 * tl.pps
                        if (Math.abs(x - a) <= 6) return "trimStart"
                        if (Math.abs(x - b) <= 6) return "trimEnd"
                        return "move"
                    }
                    cursorShape: {
                        const w = wordAt(mouseX, mouseY)
                        const kind = w ? edge(mouseX, w.start, w.end) : edge(mouseX, clipItem.modelData.start, clipItem.modelData.end)
                        return kind === "move" ? Qt.OpenHandCursor : Qt.SizeHorCursor
                    }
                    onPressed: m => {
                        pressX = laneX(m); moved = false
                        word = wordAt(m.x, m.y)
                        if (word) {
                            mode = edge(m.x, word.start, word.end)
                            first = last = word.index; s = word.start; e = word.end
                        } else {
                            mode = edge(m.x, clipItem.modelData.start, clipItem.modelData.end)
                            first = clipItem.modelData.first; last = clipItem.modelData.last
                            if (mode === "trimStart") { last = first; const w0 = clipItem.modelData.words[0]; s = w0.start; e = w0.end }
                            if (mode === "trimEnd") { first = last; const wn = clipItem.modelData.words[clipItem.modelData.words.length - 1]; s = wn.start; e = wn.end }
                        }
                        limits = mode === "move" ? editor.lyrics.moveLimits(first, last) : editor.lyrics.wordLimits(first)
                    }
                    // Drag distance is measured in lane coordinates because the clip itself moves.
                    function laneX(m) { return mapToItem(lanes, m.x, m.y).x }
                    function preview(x) {
                        const d = Math.round((x - pressX) / tl.pps * 1000)
                        tl.dragFirst = first; tl.dragLast = last
                        if (mode === "move") {
                            const v = Math.max(limits.min, Math.min(limits.max, d))
                            tl.dragStartDelta = v; tl.dragEndDelta = v
                        } else if (mode === "trimStart") {
                            const ns = Math.max(limits.lo, Math.min(e - 20, tl.snapMs(s + d)))
                            tl.dragStartDelta = ns - s; tl.dragEndDelta = 0
                        } else {
                            const ne = Math.min(limits.hi, Math.max(s + 20, tl.snapMs(e + d)))
                            tl.dragStartDelta = 0; tl.dragEndDelta = ne - e
                        }
                    }
                    onPositionChanged: m => {
                        if (!pressed) return
                        if (!moved && Math.abs(laneX(m) - pressX) < 3) return
                        moved = true
                        preview(laneX(m))
                    }
                    onReleased: m => {
                        // Commit after clearing the preview: an edit rebuilds these clips,
                        // destroying this handler's item while it would still be running.
                        const timeline = tl, lyrics = editor.lyrics
                        if (moved || Math.abs(laneX(m) - pressX) >= 3) { moved = true; preview(laneX(m)) }  // the release position is final
                        const wasMoved = moved, how = mode, f = first, l = last, ss = s, ee = e
                        const sd = timeline.dragStartDelta, ed = timeline.dragEndDelta
                        timeline.dragFirst = -1; timeline.dragLast = -2; timeline.dragStartDelta = 0; timeline.dragEndDelta = 0
                        if (wasMoved) {
                            if (how === "move") lyrics.moveWords(f, l, sd)
                            else lyrics.correct(f, ss + sd, ee + ed)
                            return
                        }
                        const row = clipItem.index
                        if (word) {
                            timeline.selected({kind: "word", word: word.index, firstRow: row, lastRow: row,
                                               start: word.start, end: word.end, label: word.word})
                            editor.seekLyric(word.start)
                        } else if ((m.modifiers & Qt.ShiftModifier) && timeline.selection && timeline.selection.firstRow !== undefined) {
                            const a = Math.min(timeline.selection.firstRow, row), b = Math.max(timeline.selection.lastRow, row)
                            timeline.selected({kind: "lines", firstRow: a, lastRow: b, start: timeline.lines[a].start,
                                               end: timeline.lines[b].end, label: (b - a + 1) + " lines"})
                        } else {
                            timeline.selected({kind: "line", firstRow: row, lastRow: row, start: clipItem.modelData.start,
                                               end: clipItem.modelData.end, label: clipItem.modelData.text})
                            editor.seekLyric(clipItem.modelData.start)
                        }
                    }
                    onCanceled: { tl.dragFirst = -1; tl.dragLast = -2; tl.dragStartDelta = 0; tl.dragEndDelta = 0 }
                    onDoubleClicked: editor.playRange(clipItem.modelData.start, clipItem.modelData.end)
                }
            }
        }

        // ---- audio lane
        Rectangle { y: tl.audioY; width: parent.width; height: tl.audioH; color: "#131e23" }
        WaveformView {
            y: tl.audioY + 2; width: parent.width; height: tl.audioH - 4
            source: editor
            pixelsPerSecond: tl.pps
            viewStart: tl.viewStart
            color: "#4f9c85"
        }
        Text {
            visible: !editor.peaksReady && editor.ready
            x: 10; y: tl.audioY + 6; text: "Reading waveform…"; color: "#6f8085"; font.pixelSize: 11
        }

        // ---- ruler, drawn above the tracks so they scroll beneath it
        Rectangle { width: parent.width; height: tl.rulerH; color: "#1a262d"; z: 9 }
        Repeater {
            id: ticks
            z: 10
            readonly property real step: {
                const steps = [0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60]
                for (const s of steps) if (s * tl.pps >= 64) return s
                return 120
            }
            readonly property real first: Math.floor(tl.viewStart / step) * step
            model: Math.ceil(tl.visibleSeconds / step) + 2
            Item {
                required property int index
                readonly property real t: ticks.first + index * ticks.step
                x: tl.px(t); z: 10
                visible: t >= 0 && t <= tl.duration
                Rectangle { y: 14; width: 1; height: 12; color: "#4b5b62" }
                Text { x: 4; y: 2; text: tl.clock(parent.t, ticks.step < 1); color: "#8fa0a4"; font.pixelSize: 10 }
            }
        }

        // ---- picture coverage along the bottom of the ruler: planned (teal) and uncovered (red)
        Repeater {
            model: tl.coverage
            Rectangle {
                required property var modelData
                objectName: modelData.covered ? "coverageCovered" : "coverageGap"
                z: 10
                x: tl.px(modelData.start / 1000); y: tl.rulerH - 3
                width: Math.max(1, (modelData.end - modelData.start) / 1000 * tl.pps); height: 3
                visible: x + width > 0 && x < lanes.width
                color: modelData.covered ? "#3f9c84" : "#d0645a"
            }
        }

        // ---- selection band and playhead
        Rectangle {
            visible: tl.selection !== null && tl.selection.end > tl.selection.start
            x: tl.selection ? tl.px(tl.selection.start / 1000) : 0
            width: tl.selection ? Math.max(1, (tl.selection.end - tl.selection.start) / 1000 * tl.pps) : 0
            y: 0; height: lanes.height
            color: "#ecedc9"; opacity: 0.07
        }
        Rectangle {
            visible: tl.selection !== null && tl.selection.end > tl.selection.start
            x: tl.selection ? tl.px(tl.selection.start / 1000) : 0
            width: tl.selection ? Math.max(1, (tl.selection.end - tl.selection.start) / 1000 * tl.pps) : 0
            y: tl.rulerH - 4; height: 4; color: "#d9c46a"; z: 11
        }
        Rectangle {
            x: tl.px(tl.playheadSeconds) - 1; y: 0; width: 2; height: lanes.height
            color: "#ecedc9"; z: 12
        }
    }

    ScrollBar {
        id: vbar
        objectName: "trackScrollBar"
        orientation: Qt.Vertical
        visible: tl.contentH > tl.trackViewH
        anchors.right: parent.right; y: tl.rulerH; height: tl.trackViewH
        policy: ScrollBar.AlwaysOn
        onPositionChanged: if (pressed) { tl.scrollY = position * tl.contentH; tl.clampScroll() }
    }
    ScrollBar {
        id: hbar
        orientation: Qt.Horizontal
        x: tl.header; width: tl.viewWidth; anchors.bottom: parent.bottom
        policy: ScrollBar.AlwaysOn
        onPositionChanged: if (pressed) { tl.viewStart = position * tl.duration; tl.clampView() }
    }
}
