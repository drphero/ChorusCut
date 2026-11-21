import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Under the viewer while a take is auditioned (a new take opened from a notice, or one picked to
// watch) or while new takes are being reviewed: which take is playing and whether the cut uses
// it, with Use, Keep, the shot's other takes ([ and ]) and the next new take (N).
Rectangle {
    id: bar
    objectName: "takeReview"
    readonly property var win: Window.window
    readonly property var a: editor.audition
    readonly property bool auditioning: a.take !== undefined
    readonly property var takes: auditioning ? (editor.plan.revision, editor.plan.takesFor(a.shot)) : []
    readonly property var take: takes.find(t => t.id === a.take) || null
    readonly property var chosen: takes.find(t => t.chosen) || null
    readonly property var shot: auditioning ? (editor.plan.revision, editor.plan.shot(a.shot)) : null
    readonly property var others: editor.newTakes.filter(n => n.take !== a.take)
    visible: editor.ready && editor.generator.watching === 0 && ((auditioning && take !== null) || (win !== null && win.reviewing && others.length > 0))
    Layout.preferredHeight: visible ? row.implicitHeight + 12 : 0
    radius: 5
    color: "#2b2a1c"; border.color: "#8a7440"

    component Tab: Button {
        id: tab
        property bool active: false
        focusPolicy: Qt.NoFocus
        implicitHeight: 26; leftPadding: 10; rightPadding: 10
        contentItem: Text { text: tab.text; color: !tab.enabled ? "#748084" : tab.active ? "#122328" : "#edf1ef"; font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: tab.active ? "#e7b75f" : (tab.hovered ? "#4a4630" : "#3a3826"); radius: 4 }
        ToolTip.visible: hovered && ToolTip.text.length > 0
        ToolTip.delay: 500
    }

    RowLayout {
        id: row
        anchors.left: parent.left; anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter
        anchors.leftMargin: 10; anchors.rightMargin: 6
        spacing: 6
        Rectangle {
            width: 8; height: 8; radius: 4; color: "#e7b75f"
            SequentialAnimation on opacity { running: bar.auditioning && editor.playing; loops: Animation.Infinite
                NumberAnimation { to: 0.3; duration: 600 } NumberAnimation { to: 1; duration: 600 } }
        }
        Text {
            objectName: "reviewText"
            Layout.fillWidth: true
            elide: Text.ElideRight
            color: "#edf1ef"; font.pixelSize: 12
            textFormat: Text.StyledText
            text: {
                if (!bar.auditioning || !bar.take)
                    return bar.others.length === 1 ? "1 new take left to watch" : bar.others.length + " new takes left to watch"
                const head = "<b>Shot " + (bar.shot ? bar.shot.number : "") + " · take " + bar.take.number + "</b> of " + bar.takes.length
                if (bar.take.chosen) return head + "  ·  chosen, plays in the cut"
                return head + "  ·  <font color='#e7b75f'>not chosen</font>" + (bar.chosen ? ": the cut plays take " + bar.chosen.number : "")
            }
        }
        Tab {
            objectName: "reviewPrevTake"
            visible: bar.auditioning && bar.takes.length > 1
            enabled: bar.take !== null && bar.take.number > 1
            text: "‹"
            onClicked: bar.win.stepTake(-1)
            ToolTip.text: "The shot's previous take ([)"
        }
        Tab {
            objectName: "reviewNextTake"
            visible: bar.auditioning && bar.takes.length > 1
            enabled: bar.take !== null && bar.take.number < bar.takes.length
            text: "›"
            onClicked: bar.win.stepTake(1)
            ToolTip.text: "The shot's next take (])"
        }
        Tab {
            objectName: "reviewUse"
            visible: bar.auditioning && bar.take !== null && !bar.take.chosen
            active: true
            text: bar.take ? "Use take " + bar.take.number : ""
            onClicked: { const id = bar.take.id; editor.markTakeSeen(id); editor.plan.run({op: "set", kind: "take", id: id, fields: {approved: true}}) }
            ToolTip.text: "Play this take in the cut"
        }
        Tab {
            objectName: "reviewKeep"
            visible: bar.auditioning && bar.take !== null && !bar.take.chosen && bar.chosen !== null
            text: bar.chosen ? "Keep take " + bar.chosen.number : ""
            onClicked: editor.endAudition()
            ToolTip.text: "Go back to the take the cut plays"
        }
        Tab {
            objectName: "reviewRefine"
            visible: bar.auditioning && bar.take !== null
            text: "Refine faces…"
            onClicked: bar.win.openRefine(bar.take.id)
            ToolTip.text: "Redraw a face on this take, for people far from the camera"
        }
        Tab {
            objectName: "reviewUpscale"
            visible: bar.auditioning && bar.take !== null
            text: bar.take && bar.take.upscaledUrl ? "Upscaled ✓" : "Upscale…"
            onClicked: bar.win.openUpscale(bar.take.id)
            ToolTip.text: bar.take && bar.take.upscaledUrl ? "This take has an upscaled version (" + bar.take.upscaledWidth + " × " + bar.take.upscaledHeight + "). Click to make another."
                                                           : "Make a larger version of this take; Render uses it"
        }
        Tab {
            objectName: "reviewNext"
            visible: bar.others.length > 0
            text: "Next new take ▸" + (bar.others.length > 1 ? " " + bar.others.length : "")
            onClicked: bar.win.reviewNext()
            ToolTip.text: "Watch the next take you haven't seen, in song order (N)"
        }
        Tab {
            objectName: "reviewDone"
            visible: !bar.auditioning || (bar.take !== null && bar.take.chosen)
            text: "Done"
            // The review ends first: ending the audition hides the bar under this button.
            onClicked: { const w = bar.win; if (w) w.reviewing = false; editor.endAudition() }
        }
    }
}
