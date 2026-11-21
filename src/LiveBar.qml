import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Under the viewer while it plays a take's live preview: which shot, how far sampling has got,
// Stop (cancels the job, as soon as a take is clearly wrong) and Back to the cut. When the take
// arrives the viewer plays it and the usual take bar (Use, Keep…) takes over.
Rectangle {
    id: bar
    objectName: "liveBar"
    readonly property var win: Window.window
    readonly property int watching: editor.generator.watching
    readonly property var job: watching ? editor.generator.jobs.find(j => j.id === watching) || null : null
    readonly property var shot: job && job.owner === "shot" ? (editor.plan.revision, editor.plan.shot(job.ownerId)) : null
    property double now: Date.now()
    visible: editor.ready && job !== null
    Layout.preferredHeight: visible ? row.implicitHeight + 12 : 0
    radius: 5
    color: "#1c2b2a"; border.color: "#3f9c84"

    Timer { interval: 1000; repeat: true; running: bar.visible; onTriggered: bar.now = Date.now() }

    function clock(ms) {
        const s = Math.max(0, Math.floor(ms / 1000))
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }

    component Tab: Button {
        id: tab
        focusPolicy: Qt.NoFocus
        implicitHeight: 26; leftPadding: 10; rightPadding: 10
        property color tint: "#2c4440"
        contentItem: Text { text: tab.text; color: "#edf1ef"; font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: tab.hovered ? Qt.lighter(tab.tint, 1.3) : tab.tint; radius: 4 }
        ToolTip.visible: hovered && ToolTip.text.length > 0
        ToolTip.delay: 500
    }

    RowLayout {
        id: row
        anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 10; anchors.rightMargin: 6
        spacing: 6
        Rectangle {
            width: 8; height: 8; radius: 4; color: "#87dec7"
            SequentialAnimation on opacity { running: bar.visible; loops: Animation.Infinite
                NumberAnimation { to: 0.3; duration: 600 } NumberAnimation { to: 1; duration: 600 } }
        }
        Text {
            objectName: "liveText"
            Layout.fillWidth: true
            elide: Text.ElideRight
            color: "#edf1ef"; font.pixelSize: 12
            textFormat: Text.StyledText
            text: {
                const j = bar.job
                if (!j) return ""
                const what = j.use === "take-upscale" ? j.what + " upscaling" : j.use === "take-face" ? j.what : "new take"
                const head = "<b>Shot " + (bar.shot ? bar.shot.number : "") + " · " + what + "</b>  ·  "
                const where = j.steps ? "generating, step " + j.step + " of " + j.steps : j.text
                return head + where + (j.started ? "  ·  " + bar.clock(bar.now - j.started) : "")
            }
        }
        Tab {
            objectName: "liveStop"
            readonly property bool failed: bar.job !== null && bar.job.state === "failed"
            text: failed ? "Dismiss" : "Stop"
            tint: failed ? "#2c4440" : "#6a3029"
            onClicked: failed ? editor.generator.dismiss(bar.watching) : editor.generator.cancel(bar.watching)
            ToolTip.text: failed ? "Forget this failed take" : "Cancel this take on ComfyUI now"
        }
        Tab {
            objectName: "liveBack"
            text: "Back to the cut"
            onClicked: editor.generator.watch(0)
            ToolTip.text: "Stop watching; it keeps generating, and its tile in the shot still shows it"
        }
    }
}
