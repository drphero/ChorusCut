import QtQuick
import Chorus

// The viewer: the editor's current frame (imported video or chosen takes) with the lyric line
// under the playhead, its words lighting up as they are sung. Used on the Edit page and in the
// pop-out viewer window; both follow the same playhead.
Rectangle {
    id: mv
    color: "#0c1216"
    clip: true
    property var lines: []
    property string monitorName: ""
    // Larger viewers get larger lyrics.
    property real lyricScale: 1
    // Room kept under the lyric line (the pop-out's transport bar sits there while it shows).
    property real lyricBottom: 24 * lyricScale
    // A picture to show: imported video, or a take chosen for any shot.
    readonly property bool hasPicture: editor.sourceLength > 0 || editor.plan.shots.some(s => s.chosenTake !== undefined)
    readonly property bool lyricsShown: editor.hiddenTracks.indexOf("lyrics") < 0
    // The monitor's own "editor" property would shadow the context's inside it.
    readonly property var source: editor
    VideoMonitor { objectName: mv.monitorName; editor: mv.source; anchors.fill: parent; anchors.margins: 4; visible: mv.hasPicture }
    // A take's live preview, while the viewer watches one generating: over the cut, until the
    // take arrives (then the viewer plays it) or the user goes back to the cut.
    Rectangle {
        id: live
        objectName: mv.monitorName ? mv.monitorName + "Live" : ""
        readonly property int watching: editor.generator.watching
        readonly property var job: watching ? editor.generator.jobs.find(j => j.id === watching) || null : null
        visible: job !== null
        anchors.fill: parent
        color: mv.color
        z: 1
        LivePreview {
            objectName: mv.monitorName ? mv.monitorName + "LivePreview" : ""
            anchors.fill: parent; anchors.margins: 4
            source: live.job && live.job.state !== "failed" ? live.job.preview || "" : ""
            fps: live.job ? live.job.previewFps || 24 : 24
        }
        Text {
            visible: live.job !== null && (!live.job.preview || live.job.state === "failed")
            anchors.centerIn: parent; width: parent.width - 48
            horizontalAlignment: Text.AlignHCenter; wrapMode: Text.Wrap
            text: !live.job ? "" : live.job.state === "failed" ? live.job.text
                  : live.job.live ? live.job.text + "\nThe take will show here as it generates."
                  : live.job.text + "\nThis take was queued without a live preview."
            color: live.job && live.job.state === "failed" ? "#f0a08f" : "#adb8ba"
            font.pixelSize: 13 * Math.min(1.4, mv.lyricScale)
        }
        Rectangle {
            x: 12; y: 12
            visible: live.job !== null
            width: liveTag.implicitWidth + 16; height: liveTag.implicitHeight + 8; radius: 4
            color: "#d0102320"; border.color: "#87dec7"
            Text {
                id: liveTag
                anchors.centerIn: parent
                text: !live.job ? "" : "Generating" + (live.job.steps ? " · step " + live.job.step + " of " + live.job.steps : "")
                color: "#87dec7"
                font.pixelSize: 12 * Math.min(1.4, mv.lyricScale); font.weight: Font.DemiBold
            }
        }
    }
    // While a take is auditioned: on its shot, which take this is, so the pop-out says it too.
    Rectangle {
        id: auditionTag
        objectName: mv.monitorName ? mv.monitorName + "Audition" : ""
        readonly property var a: editor.audition
        readonly property var shot: a.shot !== undefined ? (editor.plan.revision, editor.plan.item("shot", a.shot)) : null
        readonly property var take: a.take !== undefined ? (editor.plan.revision, editor.plan.takesFor(a.shot).find(t => t.id === a.take)) || null : null
        visible: take !== null && shot !== null && editor.frame >= shot.start && editor.frame < shot.end
        x: 12; y: 12
        width: tagText.implicitWidth + 16; height: tagText.implicitHeight + 8; radius: 4
        color: "#d0201c12"; border.color: take && take.chosen ? "#87dec7" : "#e7b75f"
        Text {
            id: tagText
            anchors.centerIn: parent
            text: auditionTag.take ? "Take " + auditionTag.take.number + (auditionTag.take.chosen ? " · chosen" : " · not chosen") : ""
            color: auditionTag.take && auditionTag.take.chosen ? "#87dec7" : "#e7b75f"
            font.pixelSize: 12 * Math.min(1.4, mv.lyricScale); font.weight: Font.DemiBold
        }
    }
    Item {
        id: preview
        visible: mv.lyricsShown
        // Under the picture's lower edge when there is one; centred on black when there isn't.
        x: 24; width: parent.width - 48
        y: mv.hasPicture ? parent.height - height - mv.lyricBottom : (parent.height - height) / 2
        Behavior on y { NumberAnimation { duration: 180 } }
        height: previewText.implicitHeight
        readonly property real ms: 1000 * editor.frame / editor.fps
        readonly property var line: {
            let found = null
            for (const l of mv.lines) if (l.start <= ms + 300) found = l
            return found && ms <= found.end + 1500 ? found : null
        }
        Text {
            id: previewText
            objectName: mv.monitorName ? mv.monitorName + "Lyric" : ""
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            textFormat: Text.RichText
            font.pixelSize: (mv.hasPicture ? 22 : 30) * mv.lyricScale
            style: Text.Outline; styleColor: "#0c1216"
            text: {
                const l = preview.line
                if (!l) return mv.lines.length || editor.sourceLength > 0 ? "" : "<span style='color:#6f8085'>Add lyrics with “Lyrics…”</span>"
                return l.words.map(w => "<span style='color:" + (w.start <= preview.ms ? "#87dec7" : "#edf1ef") + "'>"
                                        + w.word + "</span>").join(" ")
            }
        }
    }
}
