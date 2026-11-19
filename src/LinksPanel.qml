import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// How the thing picked on a Plan page connects to the rest of the plan: a section's shots, cast,
// locations and lines; where a character or location appears across the song; or, with nothing
// picked, who and where in every section. Everything here is read-only and computed by
// editor.plan.links() and overview(); chips and rows open what they name, and a shot opens in Edit.
Rectangle {
    id: lp
    required property var ws                  // the Plan workspace: linked, link(), linkBack(), …
    property var shotColors: ({})             // the timeline's colours by shot type
    color: "#18242b"; radius: 6
    border.color: "#26343b"; border.width: 1

    readonly property var plan: ws.plan
    readonly property var target: ws.linked
    readonly property var info: {
        plan.revision
        const l = target ? plan.links(target.kind, target.id) : null
        return l && l.kind ? l : null
    }
    readonly property var overview: (plan.revision, info ? null : plan.overview())
    readonly property real songMs: Math.max(1, (info ? info.songFrames : overview ? overview.songFrames : 0) * 1000 / editor.fps)
    readonly property var sections: (plan.revision, plan.sections)

    component Label: Text { color: "#6f8085"; font.pixelSize: 11; font.letterSpacing: 0.6; Layout.topMargin: 10 }
    component Count: Text { color: "#8fa0a4"; font.pixelSize: 12 }
    // A row that opens something: a section, a shot.
    component LinkRow: Rectangle {
        id: row
        default property alias body: rowBody.data
        signal activated()
        Layout.fillWidth: true
        implicitHeight: rowBody.implicitHeight + 8
        radius: 4
        color: rowMouse.containsMouse ? "#22313a" : "transparent"
        RowLayout { id: rowBody; x: 6; y: 4; width: parent.width - 12; spacing: 8 }
        MouseArea { id: rowMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: row.activated() }
    }

    // The overlay version can be dismissed; the docked one sits beside the page.
    signal closeRequested()
    property bool closable: false

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 12
        spacing: 6

        // ------------------------------------------------------------ header
        RowLayout {
            Layout.fillWidth: true; spacing: 8
            Button {
                objectName: "linksBack"
                visible: lp.ws.linkHistory.length > 0
                focusPolicy: Qt.NoFocus
                implicitWidth: 28; implicitHeight: 28
                contentItem: Text { text: "‹"; color: "#edf1ef"; font.pixelSize: 18; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { radius: 4; color: parent.hovered ? "#3b4b52" : "#2b383f" }
                onClicked: lp.ws.linkBack()
                ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: "Back (⌘[)"
            }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 1
                Text {
                    objectName: "linksTitle"
                    Layout.fillWidth: true; elide: Text.ElideRight
                    text: lp.info ? lp.info.name : "Who and where"
                    color: "#edf1ef"; font.pixelSize: 15; font.weight: Font.DemiBold
                }
                Count {
                    Layout.fillWidth: true; elide: Text.ElideRight
                    text: {
                        if (!lp.info) return "Shots per section for each character and location"
                        const n = lp.info.shots.length, shots = n + (n === 1 ? " shot" : " shots")
                        if (lp.info.kind === "section") return lp.ws.clock(lp.info.start) + "–" + lp.ws.clock(lp.info.end) + " · " + shots
                        const s = lp.info.sections.length
                        return shots + " in " + s + (s === 1 ? " section" : " sections")
                    }
                }
            }
            Button {
                objectName: "linksShowCard"
                visible: lp.info !== null
                focusPolicy: Qt.NoFocus
                implicitHeight: 28; leftPadding: 10; rightPadding: 10
                contentItem: Text { text: "Show card"; color: "#edf1ef"; font.pixelSize: 12; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { radius: 4; color: parent.hovered ? "#3b4b52" : "#2b383f" }
                onClicked: lp.ws.reveal(lp.target.kind, lp.target.id)
            }
            Button {
                objectName: "linksClose"
                visible: lp.info !== null || lp.closable
                focusPolicy: Qt.NoFocus
                implicitWidth: 28; implicitHeight: 28
                contentItem: Text { text: "×"; color: "#adb8ba"; font.pixelSize: 16; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { radius: 4; color: parent.hovered ? "#3b4b52" : "transparent" }
                onClicked: lp.closable ? lp.closeRequested() : lp.ws.link(null)
                ToolTip.visible: hovered; ToolTip.delay: 500; ToolTip.text: lp.closable ? "Hide links" : "Back to who and where"
            }
        }

        // ------------------------------------------------------------ the song
        Label { text: "WHERE IN THE SONG"; Layout.topMargin: 6 }
        Item {
            id: songStrip
            objectName: "linksSong"
            Layout.fillWidth: true
            implicitHeight: lp.info && lp.info.kind !== "section" ? 30 : 16
            Repeater {
                model: lp.sections
                Rectangle {
                    required property var modelData
                    required property int index
                    readonly property bool current: lp.info !== null && lp.info.kind === "section" && lp.info.id === index
                    x: songStrip.width * modelData.start / lp.songMs
                    width: Math.max(2, songStrip.width * (modelData.end - modelData.start) / lp.songMs - 1)
                    height: 10; radius: 2
                    color: current ? "#87dec7" : sectionMouse.containsMouse ? "#5c7078" : "#3a4a51"
                    MouseArea {
                        id: sectionMouse
                        anchors.fill: parent; anchors.margins: -3
                        hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                        onClicked: lp.ws.link({kind: "section", id: parent.index})
                    }
                    ToolTip.visible: sectionMouse.containsMouse; ToolTip.delay: 300
                    ToolTip.text: modelData.label + "  " + lp.ws.clock(modelData.start)
                }
            }
            // Where a character or location appears.
            Repeater {
                model: lp.info && lp.info.kind !== "section" ? lp.info.marks : []
                Rectangle {
                    required property var modelData
                    y: 15; height: 10; radius: 1
                    x: songStrip.width * (modelData.start * 1000 / editor.fps) / lp.songMs
                    width: Math.max(2, songStrip.width * ((modelData.end - modelData.start) * 1000 / editor.fps) / lp.songMs - 1)
                    color: lp.info.kind === "character" ? "#d98bb0" : "#79cdb5"
                }
            }
        }

        ScrollView {
            id: scroller
            Layout.fillWidth: true; Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                width: scroller.availableWidth
                spacing: 2

                // -------------------------------------------------------- overview
                Repeater {
                    model: lp.overview ? lp.overview.rows : []
                    RowLayout {
                        id: overviewRow
                        required property var modelData
                        required property int index
                        objectName: "overviewRow" + index
                        Layout.fillWidth: true; Layout.topMargin: index === 0 ? 8 : 0
                        spacing: 8
                        LinkChip {
                            Layout.preferredWidth: 112; Layout.maximumWidth: 112
                            kind: overviewRow.modelData.kind; itemId: overviewRow.modelData.id; text: overviewRow.modelData.name
                            onClicked: lp.ws.link({kind: kind, id: itemId})
                        }
                        Item {
                            id: cells
                            Layout.fillWidth: true; implicitHeight: 22
                            Repeater {
                                model: overviewRow.modelData.cells
                                Rectangle {
                                    required property int modelData
                                    required property int index
                                    readonly property var section: lp.sections[index] || {start: 0, end: 0, label: ""}
                                    x: cells.width * section.start / lp.songMs
                                    width: Math.max(2, cells.width * (section.end - section.start) / lp.songMs - 1)
                                    height: 22; radius: 2
                                    color: modelData === 0 ? "#1d2a31"
                                         : overviewRow.modelData.kind === "character" ? Qt.rgba(0.85, 0.55, 0.69, Math.min(1, 0.3 + 0.18 * modelData))
                                                                                        : Qt.rgba(0.47, 0.8, 0.71, Math.min(1, 0.3 + 0.18 * modelData))
                                    Text {
                                        anchors.centerIn: parent
                                        visible: parent.modelData > 0 && parent.width > 14
                                        text: parent.modelData; color: "#122328"; font.pixelSize: 10
                                    }
                                    MouseArea {
                                        id: cellMouse
                                        anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor
                                        onClicked: lp.ws.link({kind: "section", id: parent.index})
                                    }
                                    ToolTip.visible: cellMouse.containsMouse; ToolTip.delay: 300
                                    ToolTip.text: section.label + ": " + modelData + (modelData === 1 ? " shot" : " shots")
                                }
                            }
                        }
                    }
                }
                Text {
                    visible: lp.overview !== null && lp.overview.rows.length === 0
                    Layout.fillWidth: true; Layout.topMargin: 8; wrapMode: Text.Wrap
                    text: "Add characters and locations, then plan shots on V1 and V2; this shows who and where in each section."
                    color: "#6f8085"; font.pixelSize: 12
                }
                Text {
                    visible: lp.overview !== null && lp.overview.rows.length > 0
                    Layout.fillWidth: true; Layout.topMargin: 10; wrapMode: Text.Wrap
                    text: "Click a name, a section or a cell. Clicking a card on any page shows its links here."
                    color: "#6f8085"; font.pixelSize: 12
                }

                // -------------------------------------------------------- a section's shots
                Label { visible: lp.info !== null && lp.info.kind === "section"; text: "SHOTS" }
                Item {
                    id: lanes
                    objectName: "linksLanes"
                    visible: lp.info !== null && lp.info.kind === "section" && lp.info.shots.length > 0
                    Layout.fillWidth: true; implicitHeight: 52
                    readonly property real a: lp.info && lp.info.kind === "section" ? lp.info.start : 0
                    readonly property real span: lp.info && lp.info.kind === "section" ? Math.max(1, lp.info.end - lp.info.start) : 1
                    Text { y: 4; text: "V2"; color: "#6f8085"; font.pixelSize: 10 }
                    Text { y: 30; text: "V1"; color: "#6f8085"; font.pixelSize: 10 }
                    Repeater {
                        model: lanes.visible ? lp.info.shots : []
                        Rectangle {
                            required property var modelData
                            readonly property real from: Math.max(0, (modelData.startMs - lanes.a) / lanes.span)
                            readonly property real to: Math.min(1, (modelData.endMs - lanes.a) / lanes.span)
                            readonly property var tone: lp.shotColors[modelData.type] || ["#2e3a40", "#7a8a90"]
                            objectName: "laneShot" + modelData.number
                            x: 20 + (lanes.width - 20) * from
                            width: Math.max(4, (lanes.width - 20) * (to - from) - 2)
                            y: modelData.track === 2 ? 0 : 26
                            height: 22; radius: 3
                            color: laneMouse.containsMouse ? Qt.lighter(tone[0], 1.3) : tone[0]
                            border.color: tone[1]; border.width: 1
                            Text {
                                anchors.centerIn: parent; visible: parent.width > 18
                                text: parent.modelData.number; color: "#edf1ef"; font.pixelSize: 11; font.family: "Menlo"
                            }
                            MouseArea { id: laneMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: lp.ws.showShot(parent.modelData.id) }
                            ToolTip.visible: laneMouse.containsMouse; ToolTip.delay: 300
                            ToolTip.text: "Shot " + modelData.number + (modelData.purpose ? ": " + modelData.purpose : "") + "\nOpen in Edit"
                        }
                    }
                }
                Text {
                    visible: lp.info !== null && lp.info.kind === "section" && lp.info.shots.length === 0
                    text: "No shots here yet. Plan them on V1 and V2 in Edit."; color: "#6f8085"; font.pixelSize: 12
                }

                // -------------------------------------------------------- cast
                Label {
                    visible: lp.info !== null && lp.info.kind !== "character"
                    text: lp.info && lp.info.kind === "location" ? "WHO IS HERE" : "CAST"
                }
                Repeater {
                    model: lp.info && lp.info.kind !== "character" ? lp.info.characters : []
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true; spacing: 8
                        LinkChip { kind: "character"; itemId: parent.modelData.id; text: parent.modelData.name; onClicked: lp.ws.link({kind: kind, id: itemId}) }
                        Count {
                            Layout.fillWidth: true; elide: Text.ElideRight; horizontalAlignment: Text.AlignRight
                            text: (parent.modelData.looks && parent.modelData.looks.length ? parent.modelData.looks.join(", ") + " · " : "")
                                  + parent.modelData.shots + (parent.modelData.shots === 1 ? " shot" : " shots")
                        }
                    }
                }
                Count {
                    visible: lp.info !== null && lp.info.kind !== "character" && (lp.info.characters.length === 0 || lp.info.empty > 0)
                    text: lp.info && lp.info.kind === "section" && lp.info.empty > 0
                          ? "No one in frame in " + lp.info.empty + (lp.info.empty === 1 ? " shot" : " shots")
                          : "No one in frame"
                }

                // -------------------------------------------------------- looks
                Label { visible: lp.info !== null && lp.info.kind === "character"; text: "LOOKS" }
                Repeater {
                    model: lp.info && lp.info.kind === "character" ? lp.info.looks : []
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true; spacing: 8
                        Text { text: parent.modelData.name; color: parent.modelData.id ? "#edf1ef" : "#8fa0a4"; font.pixelSize: 13; Layout.fillWidth: true; elide: Text.ElideRight }
                        Count { text: parent.modelData.shots + (parent.modelData.shots === 1 ? " shot" : " shots") }
                    }
                }
                Count {
                    visible: lp.info !== null && lp.info.kind === "character" && lp.info.sung > 0
                    Layout.topMargin: 4
                    text: lp.info && lp.info.sung ? "Sings " + lp.info.sung + (lp.info.sung === 1 ? " line" : " lines") : ""
                }

                // -------------------------------------------------------- locations
                Label {
                    visible: lp.info !== null && lp.info.kind !== "location"
                    text: lp.info && lp.info.kind === "character" ? "SEEN AT" : "LOCATIONS"
                }
                Repeater {
                    model: lp.info && lp.info.kind !== "location" ? lp.info.locations : []
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true; spacing: 8
                        LinkChip { kind: "location"; itemId: parent.modelData.id; text: parent.modelData.name; onClicked: lp.ws.link({kind: kind, id: itemId}) }
                        Count { Layout.fillWidth: true; horizontalAlignment: Text.AlignRight; text: parent.modelData.shots + (parent.modelData.shots === 1 ? " shot" : " shots") }
                    }
                }
                Count {
                    visible: lp.info !== null && lp.info.kind !== "location" && lp.info.locations.length === 0
                    text: "No location set"
                }

                // -------------------------------------------------------- sections
                Label { visible: lp.info !== null && lp.info.kind !== "section"; text: "SECTIONS" }
                Repeater {
                    model: lp.info && lp.info.kind !== "section" ? lp.info.sections : []
                    LinkRow {
                        required property var modelData
                        required property int index
                        objectName: "linksSection" + index
                        onActivated: lp.ws.link({kind: "section", id: modelData.ordinal})
                        Text { text: parent.parent.modelData.name; color: "#9fe3d0"; font.pixelSize: 13 }
                        Count { text: lp.ws.clock(parent.parent.modelData.start); Layout.fillWidth: true }
                        Count { text: parent.parent.modelData.shots + (parent.parent.modelData.shots === 1 ? " shot" : " shots") }
                    }
                }

                // -------------------------------------------------------- lines
                Label { visible: lp.info !== null && lp.info.kind === "section" && lp.info.lines.length > 0; text: "LINES" }
                Repeater {
                    model: lp.info && lp.info.kind === "section" ? lp.info.lines : []
                    RowLayout {
                        required property var modelData
                        Layout.fillWidth: true; spacing: 8
                        Text { text: parent.modelData.text; color: "#c9d2d3"; font.pixelSize: 12; Layout.fillWidth: true; elide: Text.ElideRight }
                        Count { text: parent.modelData.singers.length ? parent.modelData.singers.join(", ") : "no singer"; color: parent.modelData.singers.length ? "#8fa0a4" : "#e0a24a" }
                    }
                }

                // -------------------------------------------------------- shot list
                Label { visible: lp.info !== null && lp.info.shots.length > 0; text: lp.info && lp.info.kind === "section" ? "IN ORDER" : "SHOTS" }
                Repeater {
                    model: lp.info ? lp.info.shots : []
                    LinkRow {
                        required property var modelData
                        objectName: "linksShot" + modelData.number
                        onActivated: lp.ws.showShot(modelData.id)
                        Text { text: parent.parent.modelData.number; color: "#edf1ef"; font.pixelSize: 12; font.family: "Menlo"; Layout.preferredWidth: 22 }
                        Text { text: parent.parent.modelData.framing || "—"; color: "#adb8ba"; font.pixelSize: 11; Layout.preferredWidth: 30 }
                        Text {
                            Layout.fillWidth: true; elide: Text.ElideRight; color: "#c9d2d3"; font.pixelSize: 12
                            text: {
                                const s = parent.parent.modelData
                                const who = lp.info.kind === "character" ? s.where : s.who
                                return [who, s.purpose].filter(x => x).join(" — ") || s.type || "Not specified"
                            }
                        }
                    }
                }
                Item { implicitHeight: 8 }
            }
        }
    }
}
