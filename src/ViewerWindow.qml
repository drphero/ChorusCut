import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "keys.js" as Kbd

// The viewer in a window of its own, for a second screen or full screen. It follows the same
// playhead as the Edit page. Space plays, ← → step a frame, F (or double-click) toggles full
// screen, Esc leaves full screen, and closing the window puts the viewer back on the Edit page.
Window {
    id: vw
    objectName: "viewerWindow"
    title: (editor.ready ? editor.projectName + " — " : "") + "Viewer"
    color: "#0c1216"
    width: 960; height: 600; minimumWidth: 320; minimumHeight: 200
    property var lines: []
    signal closedByUser()
    component Action: Button {
        id: btn
        property bool active: false
        focusPolicy: Qt.NoFocus
        implicitHeight: 28; leftPadding: 10; rightPadding: 10
        contentItem: Text { text: btn.text; color: btn.enabled ? (btn.active ? "#122328" : "#edf1ef") : "#748084"; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: btn.active ? "#87dec7" : (btn.hovered ? "#3b4b52" : "#2b383f"); radius: 5 }
        ToolTip.visible: hovered && ToolTip.text.length > 0
        ToolTip.delay: 500
    }

    Settings {
        id: place
        category: "viewerWindow"
        property int x: -1
        property int y: -1
        property int width: 960
        property int height: 600
    }
    function showAgain() {
        width = Math.max(minimumWidth, place.width); height = Math.max(minimumHeight, place.height)
        let onScreen = false
        for (const sc of Qt.application.screens)
            onScreen = onScreen || place.x + 80 > sc.virtualX && place.x < sc.virtualX + sc.width - 80 &&
                         place.y + 40 > sc.virtualY && place.y < sc.virtualY + sc.height - 40
        if (place.x !== -1 && onScreen) { x = place.x; y = place.y }
        show(); raise(); requestActivate()
    }
    function keepPlace() {
        if (visibility !== Window.Windowed) return
        place.x = x; place.y = y; place.width = width; place.height = height
    }
    Timer { id: keepSoon; interval: 400; onTriggered: vw.keepPlace() }
    onXChanged: keepSoon.restart()
    onYChanged: keepSoon.restart()
    onWidthChanged: keepSoon.restart()
    onHeightChanged: keepSoon.restart()
    onClosing: { keepPlace(); closedByUser() }
    function tc(frame) {
        const nominal = Math.round(editor.fps)
        const seconds = Math.floor(frame / nominal)
        return [Math.floor(seconds/3600), Math.floor(seconds/60)%60, seconds%60, frame%nominal]
            .map(n => String(n).padStart(2, "0")).join(":")
    }
    function toggleFullScreen() { if (visibility === Window.FullScreen) showNormal(); else showFullScreen() }

    MonitorView {
        anchors.fill: parent
        monitorName: "viewerWindowMonitor"
        lines: vw.lines
        radius: 0
        lyricScale: Math.max(1, Math.min(2.2, vw.height / 520))
        lyricBottom: bar.opacity > 0 ? bar.height + 26 : 24 * lyricScale
    }
    // The transport shows while the pointer moves, and stays while it is over the bar.
    property bool controlsShown: true
    Timer { id: hideSoon; interval: 2200; onTriggered: vw.controlsShown = bar.hovered }
    HoverHandler {
        onPointChanged: { vw.controlsShown = true; hideSoon.restart() }
        onHoveredChanged: if (!hovered) hideSoon.restart()
    }
    TapHandler { onDoubleTapped: vw.toggleFullScreen() }
    Rectangle {
        id: bar
        objectName: "viewerWindowBar"
        property bool hovered: barHover.hovered
        HoverHandler { id: barHover }
        anchors.horizontalCenter: parent.horizontalCenter; anchors.bottom: parent.bottom; anchors.bottomMargin: 14
        width: barRow.implicitWidth + 20; height: 38; radius: 6
        color: "#d0162027"; border.color: "#34434a"
        opacity: vw.controlsShown || !editor.playing ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        RowLayout {
            id: barRow
            anchors.centerIn: parent
            spacing: 6
            Action { objectName: "viewerWindowPlay"; enabled: editor.ready; text: editor.playing ? "Pause" : "Play"; active: true; onClicked: editor.togglePlay() }
            Action { text: "−1"; onClicked: editor.seek(editor.frame - 1); ToolTip.text: "Back one frame (←)" }
            Action { text: "+1"; onClicked: editor.seek(editor.frame + 1); ToolTip.text: "Forward one frame (→)" }
            Text { text: vw.tc(editor.frame); font.family: "Menlo"; font.pixelSize: 15; color: "#87dec7"; Layout.leftMargin: 6; Layout.rightMargin: 6 }
            Action { objectName: "viewerWindowFull"; text: vw.visibility === Window.FullScreen ? "Exit full screen" : "Full screen"; onClicked: vw.toggleFullScreen()
                     ToolTip.text: "F, or double-click the picture" }
            Action { objectName: "viewerWindowBack"; text: "Back to Edit"; onClicked: vw.close(); ToolTip.text: "Put the viewer back on the Edit page (" + Kbd.mod() + "W)" }
        }
    }
    Shortcut { sequence: "Space"; onActivated: editor.togglePlay() }
    Shortcut { sequence: "Left"; onActivated: editor.seek(editor.frame - 1) }
    Shortcut { sequence: "Right"; onActivated: editor.seek(editor.frame + 1) }
    Shortcut { sequence: "F"; onActivated: vw.toggleFullScreen() }
    Shortcut { sequence: "Escape"; enabled: vw.visibility === Window.FullScreen; onActivated: vw.showNormal() }
    Shortcut { sequences: [StandardKey.Close]; onActivated: vw.close() }
}
