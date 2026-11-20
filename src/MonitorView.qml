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
