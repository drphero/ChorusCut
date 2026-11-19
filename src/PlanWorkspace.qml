import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The Plan workspace, beside Edit: concept, look, cast, locations and the arc for each song
// section, plus the plan checks. Every field writes through editor.plan, so each edit is one
// undo step, and agent passes (later, through MCP) use the same commands.
Rectangle {
    id: ws
    color: "#121b20"
    radius: 5
    property string page: "concept"
    signal showCheck(var check)
    readonly property var plan: editor.plan
    readonly property var opts: editor.plan.options

    component NavButton: Button {
        id: nb
        property string key
        property string detail: ""
        objectName: "planPage_" + key
        focusPolicy: Qt.NoFocus
        Layout.fillWidth: true
        implicitHeight: 34
        contentItem: RowLayout {
            Text { text: nb.text; color: ws.page === nb.key ? "#122328" : "#edf1ef"; font.pixelSize: 13; Layout.fillWidth: true; leftPadding: 4 }
            Text { text: nb.detail; color: ws.page === nb.key ? "#2b4a44" : "#8fa0a4"; font.pixelSize: 11; rightPadding: 4 }
        }
        background: Rectangle { radius: 5; color: ws.page === nb.key ? "#87dec7" : nb.hovered ? "#2b383f" : "transparent" }
        onClicked: ws.page = key
    }
    component Heading: Text { color: "#edf1ef"; font.pixelSize: 18; font.weight: Font.DemiBold }
    component Note: Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#8fa0a4"; font.pixelSize: 12 }
    component Card: Rectangle {
        default property alias body: cardBody.data
        Layout.fillWidth: true
        implicitHeight: cardBody.implicitHeight + 24
        radius: 6; color: "#18242b"; border.color: "#26343b"
        ColumnLayout { id: cardBody; x: 12; y: 12; width: parent.width - 24; spacing: 10 }
    }
    component Small: Button {
        id: sb
        focusPolicy: Qt.NoFocus
        implicitHeight: 28; leftPadding: 10; rightPadding: 10
        property bool danger: false
        contentItem: Text { text: sb.text; color: sb.danger ? "#f0a08f" : "#edf1ef"; font.pixelSize: 12; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { radius: 4; color: sb.hovered ? "#3b4b52" : "#2b383f" }
    }
    function clock(ms) {
        const s = ms / 1000, m = Math.floor(s / 60)
        return m + ":" + String(Math.floor(s - 60 * m)).padStart(2, "0")
    }

    RowLayout {
        anchors.fill: parent; anchors.margins: 12
        spacing: 14

        // ---------------------------------------------------------- pages
        ColumnLayout {
            Layout.preferredWidth: 190; Layout.fillHeight: true
            spacing: 2
            Text { text: "PLAN"; color: "#6f8085"; font.pixelSize: 11; font.letterSpacing: 1.2; Layout.bottomMargin: 6; leftPadding: 4 }
            NavButton { key: "concept"; text: "Concept" }
            NavButton { key: "look"; text: "Look" }
            NavButton { key: "cast"; text: "Cast"; detail: ws.plan.characters.length || "" }
            NavButton { key: "locations"; text: "Locations"; detail: ws.plan.locations.length || "" }
            NavButton { key: "arc"; text: "Arc"; detail: ws.plan.sections.length + " sections" }
            NavButton { key: "checks"; text: "Checks"; detail: ws.plan.errorCount ? ws.plan.errorCount + " open" : "" }
            Item { Layout.fillHeight: true }
            // Where the plan stands against its gates.
            Repeater {
                model: ws.plan.gates
                RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 8
                    Rectangle { width: 8; height: 8; radius: 4; color: parent.modelData.pass ? "#87dec7" : "#e0a24a" }
                    Text { text: parent.modelData.gate + "  " + parent.modelData.label; color: "#adb8ba"; font.pixelSize: 12; Layout.fillWidth: true }
                    Text { text: parent.modelData.pass ? "ready" : parent.modelData.open + " to do"; color: "#6f8085"; font.pixelSize: 11 }
                }
            }
            Note { Layout.topMargin: 6; text: "Shots are planned on V1 and V2 in Edit. Errors and to-dos hold back batch rendering; any single shot can still be tried." }
        }
        Rectangle { Layout.fillHeight: true; width: 1; color: "#26343b" }

        StackLayout {
            Layout.fillWidth: true; Layout.fillHeight: true
            currentIndex: ["concept", "look", "cast", "locations", "arc", "checks"].indexOf(ws.page)

            // ------------------------------------------------------ concept
            ScrollView {
                id: conceptPage
                contentWidth: availableWidth
                clip: true
                readonly property var item: ws.plan.concept
                ColumnLayout {
                    width: Math.min(conceptPage.availableWidth - 16, 760)
                    spacing: 14
                    Heading { text: "Concept" }
                    Note { text: "The treatment everything else follows. Changing it marks the shots written before as stale; nothing is rewritten for you." }
                    PlanField {
                        kind: "concept"; field: "logline"; label: "Logline"; item: conceptPage.item
                        PlanText { objectName: "conceptLogline"; value: parent.parent.value; placeholder: "One sentence: who, what, and the turn"; onCommit: t => parent.parent.set(t) }
                    }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 12
                        PlanField {
                            kind: "concept"; field: "videoKind"; label: "Kind of video"; item: conceptPage.item
                            PlanCombo { objectName: "conceptKind"; options: ws.opts.videoKind; value: parent.parent.value; onPicked: v => parent.parent.set(v) }
                        }
                        PlanField {
                            kind: "concept"; field: "mix"; label: "Mix"; item: conceptPage.item
                            PlanText { value: parent.parent.value; placeholder: "e.g. 60% performance, 40% story"; onCommit: t => parent.parent.set(t) }
                        }
                    }
                    PlanField {
                        kind: "concept"; field: "treatment"; label: "Treatment"; item: conceptPage.item
                        PlanText { multiline: true; lines: 6; value: parent.parent.value; placeholder: "What the video is, how it moves through the song, and how it ends"; onCommit: t => parent.parent.set(t) }
                    }
                    Text { text: "Format"; color: "#edf1ef"; font.pixelSize: 14; font.weight: Font.DemiBold; Layout.topMargin: 4 }
                    RowLayout {
                        Layout.fillWidth: true; spacing: 12
                        PlanField {
                            kind: "concept"; field: "aspect"; label: "Aspect ratio"; item: conceptPage.item
                            PlanCombo { objectName: "conceptAspect"; options: ws.opts.aspect; value: parent.parent.value; onPicked: v => parent.parent.set(v) }
                        }
                        PlanField {
                            kind: "concept"; field: "resolution"; label: "Delivery resolution"; item: conceptPage.item
                            PlanText { value: parent.parent.value; placeholder: "e.g. 1920×1080"; onCommit: t => parent.parent.set(t) }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true; spacing: 3
                            Text { text: "Frame rate"; color: "#adb8ba"; font.pixelSize: 11 }
                            Text { text: Number(editor.fps.toFixed(3)) + " fps, the project's rate"; color: "#edf1ef"; font.pixelSize: 13; Layout.preferredHeight: 28; verticalAlignment: Text.AlignVCenter }
                        }
                    }
                    Note { text: "H3 renders at 24 fps, at most " + (362 / 24).toFixed(1) + " s per shot. Workflows for each route are chosen with adapters, after this build." }
                    PlanField {
                        kind: "concept"; field: "rules"; label: "Rules"; item: conceptPage.item
                        hint: "One per line: what must or must never appear. Every prompt the agent writes follows them."
                        PlanText {
                            multiline: true; lines: 4
                            value: parent.parent.list.join("\n")
                            placeholder: "Never show the singer's face in the verses"
                            onCommit: t => parent.parent.set(t.split("\n").map(r => r.trim()).filter(r => r.length > 0))
                        }
                    }
                    Item { height: 12 }
                }
            }

            // ------------------------------------------------------ look
            ScrollView {
                id: lookPage
                contentWidth: availableWidth
                clip: true
                readonly property var item: ws.plan.concept
                ColumnLayout {
                    width: Math.min(lookPage.availableWidth - 16, 760)
                    spacing: 14
                    Heading { text: "Look" }
                    Note { text: "How the whole video looks. A mood board of reference images comes with reference generation." }
                    Repeater {
                        model: [
                            {field: "style", label: "Visual style", hint: "e.g. grainy 16 mm, handheld, intimate", lines: 2},
                            {field: "palette", label: "Palette", hint: "e.g. sodium orange against deep teal", lines: 1},
                            {field: "lighting", label: "Lighting", hint: "e.g. practicals only, hard key from the radio dial", lines: 2},
                            {field: "era", label: "Era", hint: "e.g. late 1980s", lines: 1},
                            {field: "lens", label: "Lens and format feel", hint: "e.g. anamorphic flares, shallow focus", lines: 1},
                            {field: "grade", label: "Grade intent", hint: "e.g. crushed blacks, warm skin, green shadows", lines: 2}
                        ]
                        PlanField {
                            required property var modelData
                            kind: "concept"; field: modelData.field; label: modelData.label; item: lookPage.item
                            PlanText {
                                objectName: "look_" + parent.parent.field
                                multiline: parent.parent.modelData.lines > 1; lines: parent.parent.modelData.lines
                                value: parent.parent.value; placeholder: parent.parent.modelData.hint
                                onCommit: t => parent.parent.set(t)
                            }
                        }
                    }
                    Item { height: 12 }
                }
            }

            // ------------------------------------------------------ cast
            ScrollView {
                id: castPage
                contentWidth: availableWidth
                clip: true
                ColumnLayout {
                    width: Math.min(castPage.availableWidth - 16, 760)
                    spacing: 14
                    RowLayout {
                        Layout.fillWidth: true
                        Heading { text: "Cast"; Layout.fillWidth: true }
                        Small { objectName: "addCharacter"; text: "Add character"; onClicked: ws.plan.run({op: "add", kind: "character", fields: {role: ws.plan.characters.length ? "actor" : "singer"}}) }
                    }
                    Note { text: "Everyone who appears, with reference pictures and named looks (wardrobe and makeup). Add pictures with + or drop image files on a card's pictures; they are copied into the project. Set who sings each line from the lyric lines in Edit." }
                    Note { visible: ws.plan.characters.length === 0; text: "No characters yet."; color: "#6f8085" }
                    Repeater {
                        // A count, so editing a card updates it in place instead of rebuilding the list.
                        model: ws.plan.characters.length
                        Card {
                            id: characterCard
                            required property int index
                            objectName: "characterCard" + index
                            readonly property var item: ws.plan.characters[index] || null
                            readonly property int itemId: item ? item.id : 0
                            readonly property var myLooks: ws.plan.looks.filter(l => l.character === itemId)
                            readonly property int sung: ws.plan.singers.filter(s => s.singers.indexOf(itemId) >= 0).length
                            RowLayout {
                                Layout.fillWidth: true; spacing: 12
                                PlanField {
                                    kind: "character"; itemId: characterCard.itemId; field: "name"; label: "Name"; item: characterCard.item
                                    PlanText { objectName: "characterName" + characterCard.index; value: parent.parent.value; onCommit: t => parent.parent.set(t) }
                                }
                                PlanField {
                                    kind: "character"; itemId: characterCard.itemId; field: "role"; label: "Role"; item: characterCard.item
                                    PlanCombo { options: ws.opts.role; value: parent.parent.value; onPicked: v => parent.parent.set(v) }
                                }
                            }
                            PlanField {
                                kind: "character"; itemId: characterCard.itemId; field: "identity"; label: "Identity"; item: characterCard.item
                                PlanText { multiline: true; lines: 2; value: parent.parent.value; placeholder: "Face, hair, build, age: what stays the same in every shot"; onCommit: t => parent.parent.set(t) }
                            }
                            PlanField {
                                kind: "character"; itemId: characterCard.itemId; field: "voice"; label: "Voice"; item: characterCard.item
                                PlanText { value: parent.parent.value; placeholder: "For engines that generate speech or singing"; onCommit: t => parent.parent.set(t) }
                            }
                            Text { text: "Reference pictures"; color: "#adb8ba"; font.pixelSize: 11 }
                            ReferenceStrip { owner: "character"; ownerId: characterCard.itemId }
                            Text { text: "Looks"; color: "#adb8ba"; font.pixelSize: 11; Layout.topMargin: 4 }
                            Repeater {
                                model: characterCard.myLooks.length
                                ColumnLayout {
                                    id: lookBlock
                                    required property int index
                                    readonly property var item: characterCard.myLooks[index] || null
                                    readonly property int itemId: item ? item.id : 0
                                    Layout.fillWidth: true; spacing: 6
                                RowLayout {
                                    id: lookRow
                                    readonly property var item: lookBlock.item
                                    readonly property int itemId: lookBlock.itemId
                                    Layout.fillWidth: true; spacing: 8
                                    PlanField {
                                        Layout.preferredWidth: 160; Layout.fillWidth: false
                                        kind: "look"; itemId: lookRow.itemId; field: "name"; label: "Look"; item: lookRow.item
                                        PlanText { value: parent.parent.value; onCommit: t => parent.parent.set(t) }
                                    }
                                    PlanField {
                                        kind: "look"; itemId: lookRow.itemId; field: "description"; label: "Wardrobe and makeup"; item: lookRow.item
                                        PlanText { value: parent.parent.value; placeholder: "e.g. red wool coat, wet hair"; onCommit: t => parent.parent.set(t) }
                                    }
                                    Small { Layout.alignment: Qt.AlignBottom; text: "Remove"; danger: true; onClicked: ws.plan.run({op: "remove", kind: "look", id: lookRow.itemId}) }
                                }
                                ReferenceStrip { owner: "look"; ownerId: lookBlock.itemId; size: 56 }
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Small { text: "Add look"; onClicked: ws.plan.run({op: "add", kind: "look", character: characterCard.itemId}) }
                                Text {
                                    Layout.fillWidth: true; leftPadding: 8
                                    text: characterCard.sung ? "Sings " + characterCard.sung + (characterCard.sung === 1 ? " line" : " lines") : ""
                                    color: "#8fa0a4"; font.pixelSize: 12
                                }
                                Small { text: "Remove character"; danger: true; onClicked: ws.plan.run({op: "remove", kind: "character", id: characterCard.itemId}) }
                            }
                        }
                    }
                    Item { height: 12 }
                }
            }

            // ------------------------------------------------------ locations
            ScrollView {
                id: locationsPage
                contentWidth: availableWidth
                clip: true
                ColumnLayout {
                    width: Math.min(locationsPage.availableWidth - 16, 760)
                    spacing: 14
                    RowLayout {
                        Layout.fillWidth: true
                        Heading { text: "Locations"; Layout.fillWidth: true }
                        Small { objectName: "addLocation"; text: "Add location"; onClicked: ws.plan.run({op: "add", kind: "location"}) }
                    }
                    Note { text: "Places and sets: what the environment is, not the camera angle. Add reference pictures with + or by dropping image files." }
                    Note { visible: ws.plan.locations.length === 0; text: "No locations yet."; color: "#6f8085" }
                    Repeater {
                        model: ws.plan.locations.length
                        Card {
                            id: locationCard
                            required property int index
                            readonly property var item: ws.plan.locations[index] || null
                            readonly property int itemId: item ? item.id : 0
                            RowLayout {
                                Layout.fillWidth: true; spacing: 12
                                PlanField {
                                    kind: "location"; itemId: locationCard.itemId; field: "name"; label: "Name"; item: locationCard.item
                                    PlanText { objectName: "locationName" + locationCard.index; value: parent.parent.value; onCommit: t => parent.parent.set(t) }
                                }
                                PlanField {
                                    kind: "location"; itemId: locationCard.itemId; field: "timeOfDay"; label: "Time of day"; item: locationCard.item
                                    PlanCombo { options: ws.opts.timeOfDay; value: parent.parent.value; onPicked: v => parent.parent.set(v) }
                                }
                            }
                            PlanField {
                                kind: "location"; itemId: locationCard.itemId; field: "description"; label: "Description"; item: locationCard.item
                                PlanText { multiline: true; lines: 2; value: parent.parent.value; placeholder: "What is there, and what it feels like"; onCommit: t => parent.parent.set(t) }
                            }
                            PlanField {
                                kind: "location"; itemId: locationCard.itemId; field: "lighting"; label: "Lighting"; item: locationCard.item
                                PlanText { value: parent.parent.value; onCommit: t => parent.parent.set(t) }
                            }
                            Text { text: "Reference pictures"; color: "#adb8ba"; font.pixelSize: 11 }
                            ReferenceStrip { owner: "location"; ownerId: locationCard.itemId }
                            RowLayout {
                                Item { Layout.fillWidth: true }
                                Small { text: "Remove location"; danger: true; onClicked: ws.plan.run({op: "remove", kind: "location", id: locationCard.itemId}) }
                            }
                        }
                    }
                    Item { height: 12 }
                }
            }

            // ------------------------------------------------------ arc
            ScrollView {
                id: arcPage
                contentWidth: availableWidth
                clip: true
                ColumnLayout {
                    width: Math.min(arcPage.availableWidth - 16, 760)
                    spacing: 14
                    Heading { text: "Arc" }
                    Note { text: "What happens in each part of the song, in the song's order. Sections come from the [headings] in the lyrics and the instrumental parts found when aligning." }
                    Repeater {
                        model: ws.plan.arc.length
                        Card {
                            id: arcCard
                            required property int index
                            objectName: "arcCard" + index
                            readonly property var item: ws.plan.arc[index] || null
                            RowLayout {
                                Layout.fillWidth: true
                                Text { text: arcCard.item ? arcCard.item.label : ""; color: "#9fe3d0"; font.pixelSize: 14; font.weight: Font.DemiBold }
                                Text { text: arcCard.item ? ws.clock(arcCard.item.start) + "–" + ws.clock(arcCard.item.end) : ""; color: "#8fa0a4"; font.pixelSize: 12; Layout.fillWidth: true; leftPadding: 6 }
                            }
                            Text {
                                visible: arcCard.item !== null && arcCard.item.mismatch
                                Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#f0a08f"; font.pixelSize: 12
                                text: arcCard.item ? "Written for “" + arcCard.item.written + "”; the song's sections changed. Rewrite it for this section." : ""
                            }
                            PlanField {
                                kind: "arc"; itemId: arcCard.index; field: "happens"; label: "What happens"; item: arcCard.item
                                PlanText { objectName: "arcHappens" + arcCard.index; multiline: true; lines: 2; value: parent.parent.value; onCommit: t => parent.parent.set(t) }
                            }
                            RowLayout {
                                Layout.fillWidth: true; spacing: 12
                                PlanField {
                                    kind: "arc"; itemId: arcCard.index; field: "beat"; label: "Emotional beat"; item: arcCard.item
                                    PlanText { value: parent.parent.value; placeholder: "e.g. longing turns to hope"; onCommit: t => parent.parent.set(t) }
                                }
                                PlanField {
                                    Layout.preferredWidth: 200; Layout.fillWidth: false
                                    kind: "arc"; itemId: arcCard.index; field: "density"; label: "Cutting"; item: arcCard.item
                                    PlanCombo { options: ws.opts.density; value: parent.parent.value; onPicked: v => parent.parent.set(v) }
                                }
                            }
                        }
                    }
                    Item { height: 12 }
                }
            }

            // ------------------------------------------------------ checks
            Checks {
                onShowCheck: c => ws.showCheck(c)
            }
        }
    }
}
