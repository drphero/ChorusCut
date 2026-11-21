import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Everything generating, from anywhere in the project: takes for shots and pictures for the
// cast and locations, with how long each has been going, Show (the shot on the timeline, or the
// card in Plan), Cancel, and Try again or Dismiss for a failure. Below, how to be told when
// they finish.
Popup {
    id: jp
    objectName: "jobsPopup"
    focus: true
    padding: 14
    width: 440
    height: Math.min(implicitHeight, (Overlay.overlay ? Overlay.overlay.height : 700) - y - 20)
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
    background: Rectangle { color: "#1d2930"; radius: 6; border.color: "#34434a" }
    signal showRequested(string owner, int ownerId)

    readonly property var jobs: editor.generator.jobs
    readonly property var notifier: editor.notifier
    property double now: Date.now()
    Timer { interval: 1000; repeat: true; running: jp.visible; triggeredOnStart: true; onTriggered: jp.now = Date.now() }

    function clock(ms) {
        const s = Math.max(0, Math.floor(ms / 1000))
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }
    function titleOf(j) {
        editor.plan.revision
        if (j.owner === "shot") {
            const s = editor.plan.shot(j.ownerId)
            return s && s.label ? "Shot " + s.label : "A removed shot"
        }
        const it = editor.plan.item(j.owner, j.ownerId)
        const name = it && it.fields ? (it.fields.name || (j.owner === "location" ? "Unnamed location" : j.owner === "prop" ? "Unnamed prop" : j.owner === "look" ? "Unnamed look" : "Unnamed character")) : "Removed"
        const v = j.view ? editor.plan.item("view", j.view) : null
        return name + (v && v.fields && v.fields.name ? " · " + v.fields.name : "")
    }
    function kindOf(j) { return j.use === "take-upscale" ? "Upscale · " + (j.what || "take") : j.use === "take-face" ? "Refine · " + (j.what || "face") : j.use === "take-sync" ? "Lip-sync take" : j.owner === "shot" ? "Take" : "Picture" }

    component Small: Button {
        id: sb
        property bool active: false
        focusPolicy: Qt.NoFocus
        implicitHeight: 24; leftPadding: 8; rightPadding: 8
        contentItem: Text { text: sb.text; color: !sb.enabled ? "#5c7078" : sb.active ? "#122328" : "#edf1ef"; font.pixelSize: 11
                            horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: 4; color: sb.active ? (sb.hovered ? "#9fe3d0" : "#87dec7") : sb.hovered && sb.enabled ? "#3b4b52" : "#2b383f" }
    }
    // A switch row: a box and its words, both clickable.
    component Toggle: Item {
        id: tg
        property bool checked: false
        property string text: ""
        property bool dim: false
        signal toggled()
        Layout.fillWidth: true
        implicitHeight: 24
        opacity: enabled ? 1 : 0.45
        Rectangle {
            id: box
            width: 15; height: 15; radius: 3; anchors.verticalCenter: parent.verticalCenter
            color: tg.checked ? "#87dec7" : "transparent"; border.color: tg.checked ? "#87dec7" : "#5c7078"
            Text { anchors.centerIn: parent; text: "✓"; visible: tg.checked; color: "#122328"; font.pixelSize: 11; font.bold: true }
        }
        Text { x: 24; width: parent.width - 24; anchors.verticalCenter: parent.verticalCenter; elide: Text.ElideRight
               text: tg.text; color: tg.dim ? "#8fa0a4" : "#edf1ef"; font.pixelSize: 12 }
        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: tg.toggled() }
    }

    contentItem: ColumnLayout {
        spacing: 8
        RowLayout {
            Layout.fillWidth: true
            Text { text: "Generating"; color: "#edf1ef"; font.pixelSize: 15; font.bold: true; Layout.fillWidth: true }
            Text {
                text: { const a = jp.jobs.filter(j => j.state !== "failed").length; const f = jp.jobs.length - a
                        return (a ? a + " under way" : "") + (a && f ? " · " : "") + (f ? f + " failed" : "") }
                color: "#8fa0a4"; font.pixelSize: 12
            }
        }
        Text {
            visible: jp.jobs.length === 0
            text: "Nothing is generating. Takes and pictures you start show here, from any shot or card."
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#6f8085"; font.pixelSize: 12
        }
        ScrollView {
            visible: jp.jobs.length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(jobColumn.implicitHeight, 300)
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                id: jobColumn
                width: parent.width
                spacing: 6
                Repeater {
                    model: jp.jobs
                    Rectangle {
                        id: row
                        required property var modelData
                        readonly property bool failed: modelData.state === "failed"
                        objectName: "jobRow_" + modelData.owner + modelData.ownerId
                        Layout.fillWidth: true
                        implicitHeight: rowColumn.implicitHeight + 14
                        radius: 4; color: "#162027"; border.color: failed ? "#8a4a42" : "#34434a"
                        ColumnLayout {
                            id: rowColumn
                            x: 10; y: 7; width: parent.width - 20
                            spacing: 3
                            RowLayout {
                                Layout.fillWidth: true; spacing: 8
                                Rectangle { width: 8; height: 8; radius: 4; color: row.failed ? "#e07f6f" : "#e7b75f"
                                            SequentialAnimation on opacity { running: !row.failed && jp.visible; loops: Animation.Infinite
                                                NumberAnimation { to: 0.3; duration: 700 } NumberAnimation { to: 1; duration: 700 } } }
                                Text { text: jp.titleOf(row.modelData); color: "#edf1ef"; font.pixelSize: 12; font.weight: Font.DemiBold
                                       elide: Text.ElideRight; Layout.fillWidth: true }
                                Text { text: jp.kindOf(row.modelData); color: "#8fa0a4"; font.pixelSize: 11 }
                            }
                            Text {
                                objectName: "jobText"
                                Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideRight
                                text: row.modelData.text + (row.modelData.started && !row.failed ? "  ·  " + jp.clock(jp.now - row.modelData.started) : "")
                                color: row.failed ? "#f0a08f" : "#adb8ba"; font.pixelSize: 11
                            }
                            RowLayout {
                                spacing: 6
                                Text { text: row.modelData.workflow; color: "#6f8085"; font.pixelSize: 10; elide: Text.ElideRight; Layout.maximumWidth: 200 }
                                Item { Layout.fillWidth: true }
                                Small { objectName: "jobShow"; text: "Show"; onClicked: { jp.close(); jp.showRequested(row.modelData.owner, row.modelData.ownerId) } }
                                Small { objectName: "jobCancel"; visible: !row.failed; text: "Cancel"; onClicked: editor.generator.cancel(row.modelData.id) }
                                Small { objectName: "jobRetry"; visible: row.failed; text: "Try again"; onClicked: editor.generator.retry(row.modelData.id) }
                                Small { objectName: "jobDismiss"; visible: row.failed; text: "Dismiss"; onClicked: editor.generator.dismiss(row.modelData.id) }
                            }
                        }
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: "#34434a"; Layout.topMargin: 4 }
        Text { text: "TELL ME WHEN"; color: "#6f8085"; font.pixelSize: 11; font.letterSpacing: 0.6 }
        Toggle { objectName: "notifyTakes"; text: "A take is ready"; checked: jp.notifier.takes; onToggled: jp.notifier.takes = !checked }
        Toggle { objectName: "notifyPictures"; text: "Pictures are ready"; checked: jp.notifier.pictures; onToggled: jp.notifier.pictures = !checked }
        Toggle { objectName: "notifyFailures"; text: "Something fails"; checked: jp.notifier.failures; onToggled: jp.notifier.failures = !checked }
        Text { text: "HOW"; color: "#6f8085"; font.pixelSize: 11; font.letterSpacing: 0.6; Layout.topMargin: 4 }
        Toggle { objectName: "notifySystem"; text: "macOS notification"; checked: jp.notifier.system; onToggled: jp.notifier.system = !checked }
        Text {
            objectName: "notifyPermission"
            visible: jp.notifier.system && jp.notifier.permission !== "allowed"
            Layout.fillWidth: true; Layout.leftMargin: 24; wrapMode: Text.Wrap; font.pixelSize: 11
            color: "#e7b75f"
            text: jp.notifier.refusal ? jp.notifier.refusal
                : jp.notifier.permission === "denied" ? "macOS has notifications turned off for Chorus Cut."
                : jp.notifier.permission === "unavailable" ? "This build can't post macOS notifications; the banner and Dock still work."
                : "macOS will ask you once to allow them."
        }
        RowLayout {
            visible: jp.notifier.system && (jp.notifier.permission === "denied" || jp.notifier.permission === "notAsked")
            Layout.leftMargin: 24
            Small { objectName: "notifyAllow"; visible: jp.notifier.permission === "notAsked"; text: "Allow…"; onClicked: jp.notifier.requestPermission() }
            Small { objectName: "notifySettings"; visible: jp.notifier.permission === "denied"; text: "Open System Settings…"; onClicked: jp.notifier.openSystemSettings() }
        }
        Toggle { objectName: "notifySound"; Layout.leftMargin: 24; text: "With a sound"; enabled: jp.notifier.system; checked: jp.notifier.sound; onToggled: jp.notifier.sound = !checked }
        Toggle { objectName: "notifyDock"; text: "Bounce the Dock icon"; checked: jp.notifier.dock; onToggled: jp.notifier.dock = !checked }
        Toggle { objectName: "notifyBackground"; text: "Only while Chorus Cut is in the background"; checked: jp.notifier.onlyInBackground
                 onToggled: jp.notifier.onlyInBackground = !checked }
        Text {
            Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#6f8085"; font.pixelSize: 11
            text: "A banner in the app shows either way. Clicking a notification opens the shot or card."
        }
        RowLayout {
            Layout.fillWidth: true
            Item { Layout.fillWidth: true }
            Small { objectName: "notifyTest"; text: "Send a test"; onClicked: jp.notifier.test()
                    ToolTip.visible: hovered; ToolTip.delay: 400; ToolTip.text: "Shows the banner, and the notification if it would show now" }
        }
    }
}
