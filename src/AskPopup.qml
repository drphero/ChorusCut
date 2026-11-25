import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "keys.js" as Keys

// "Ask the agent…": a request about one field or item (or the whole plan) for the user's agent.
// It is kept in the project's requests and, when Claude Code is connected with ChorusCut as a
// channel, reaches it at once. Suggestions for the field clicked fill the box (they never send
// by themselves). Most are about what the reference pictures need to be for the video model
// (H3), which keeps each referenced person, outfit or place the same from shot to shot: a
// subject named by what makes it recognisable (nothing is locked: looks can change hair and
// more), and views that say how it is seen (plain backdrop, even light, places empty).
Popup {
    id: ask
    objectName: "askPopup"
    modal: true
    padding: 20
    anchors.centerIn: Overlay.overlay
    width: Math.min(560, (Overlay.overlay ? Overlay.overlay.width : 560) - 60)
    background: Rectangle { color: "#1d2930"; radius: 6; border.color: "#34434a" }

    property string kind
    property int itemId
    property string field
    property string label
    property string currentValue: ""
    property bool emptyValue: true
    signal sent(int thread)

    function start(kind, id, field, label, value) {
        ask.kind = kind; ask.itemId = id; ask.field = field; ask.label = label
        currentValue = value === undefined || value === null ? "" : (typeof value === "object" ? JSON.stringify(value) : String(value))
        emptyValue = value === undefined || value === null || (Array.isArray(value) ? value.length === 0 : String(value).trim() === "")
        open()
    }
    // A view made from picked pictures: how many, and what its workflow calls them.
    function editedView(id) {
        editor.plan.revision
        const v = editor.plan.item("view", id)
        if (!v.fields || v.fields.from !== "pictures") return null
        const o = editor.generator.viewOptions(id, "ref-edit", "")
        const n = (o.inputs || []).length
        if (!n) return null
        const label = i => o.workflow ? editor.generator.pictureLabel(o.workflow, i) : "picture " + i
        return {count: n, names: n === 2 ? label(1) + " and " + label(2) : label(1) + ", " + label(2) + "…"}
    }
    function use(suggestion) {
        text.text = suggestion
        text.forceActiveFocus()
        text.cursorPosition = text.length
    }

    // What to ask, by what was clicked: [text], at most four. Empty fields are offered a first
    // draft; filled ones, ways to make them work better as references for the video.
    readonly property var suggestions: suggestionsFor(kind, field, emptyValue)
    function suggestionsFor(kind, field, empty) {
        const look = {style: 1, palette: 1, lighting: 1, era: 1, lens: 1, grade: 1}
        if (!kind) return [
            "Which characters, looks, locations and props still lack a usable picture prompt? Write them.",
            "Check every picture prompt: one clear subject, concrete features, the concept's style, no names or text.",
            "What would make the references inconsistent from shot to shot? Fix what you can.",
            "Check the plan against the brief and tell me what's weakest."]
        if (kind === "character" && field === "prompt") return empty ? [
            "Write their appearance: only what makes them recognisable, concretely, with no framing, pose or backdrop."] : [
            "Make it concrete enough to recognise them in every view: face shape, eyes, skin, marks, and hair while it stays the same.",
            "Take out framing, pose, backdrop and light: those belong to the views.",
            "Fit it to the concept's style without hiding the face.",
            "Suggest the views this character needs, and add them."]
        if (kind === "look" && field === "prompt") return empty ? [
            "Write this look's appearance: every garment with its colour and material, shoes, accessories, and what it changes (hair, makeup, more)."] : [
            "Spell out every garment, colour, material and accessory so the outfit can't drift between shots.",
            "Say exactly what this look changes about the character, and set its changes.",
            "Take out framing, pose and backdrop: those belong to the views.",
            "Suggest the views this look needs, such as the back, a detail or an outfit sheet."]
        if (kind === "location" && field === "prompt") return empty ? [
            "Write this place's appearance: layout, materials, colours and the fixed details that identify it, with no people."] : [
            "Name the fixed features that must stay put between shots: layout, signs, furniture, materials, colours.",
            "Make the light match this location's time of day and lighting, and the concept's grade.",
            "Remove people, vehicles and anything moving, so they don't turn up in every shot here.",
            "Suggest the views this place needs, such as a reverse angle or a detail."]
        if (kind === "prop" && field === "prompt") return empty ? [
            "Write this prop's appearance: shape, size, materials, colours, wear and markings, with no hands or setting."] : [
            "Name the details that must stay the same in every shot: markings, damage, colours, materials.",
            "Take out hands, setting and framing: those belong to the shots and views.",
            "Suggest the views this prop needs, such as a detail or another side."]
        if (kind === "prop" && !field) return [
            "Is this prop ready for a reference picture? Fix what isn't.",
            "Write or improve this prop's picture prompt: the object alone, on a plain backdrop.",
            "Which shots should show it?"]
        if (kind === "shot" && field === "prompts") {
            const shot = (editor.plan.revision, editor.plan.shot(itemId))
            const route = shot.fields ? shot.fields.route : ""
            const has = shot.fields && shot.fields.prompts && (shot.fields.prompts[route] || "").trim() !== ""
            const sync = route === "h3-lipsync"
            return has ? [
                "Check this prompt against the shot's pictures and lyrics, and make the timing match the take.",
                sync ? "Quote the lines she sings exactly; no <Audio> tags, the song is locked in by the sync node." : "Make the action continuous through the handles, so the cut can be trimmed.",
                "Name every picture the way the workflow does, and say what each one keeps steady.",
                "Make the camera move and framing match the shot's fields."] : [
                sync ? "Write this shot's lip-sync video prompt in H3's reference-to-video format, pictures only, quoting the lines sung."
                     : "Write this shot's video prompt in H3's reference-to-video format, from its fields, pictures and lyrics."]
        }
        if (kind === "view" && field === "framing" && editedView(itemId)) {
            const ev = editedView(itemId)
            const look = "Look at the " + (ev.count === 1 ? "picked picture" : ev.count + " picked pictures")
            return empty ? [
                look + " and write the instruction: what to make from " + (ev.count === 1 ? "it" : "them, naming each as " + ev.names) + ", what to keep exactly and what to change."] : [
                look + " and improve this instruction" + (ev.count === 1 ? "" : ", naming each as " + ev.names + ",") + " so it says exactly what to keep and what to change.",
                "Make it a clean reference: the whole subject in frame, plain backdrop, soft even light.",
                "Make it a turnaround sheet: the same subject four times side by side, front, three-quarter, side and back, plain white background.",
                "Write it the way this view's workflow expects (how it names its pictures)."]
        }
        if (kind === "view" && field === "framing") return empty ? [
            "Write this view's framing in one line: angle, distance, backdrop and light."] : [
            "Make it a clean reference: the whole subject in frame, plain backdrop, soft even light.",
            "Write it as an editing instruction for the picked pictures: what to keep exactly and what to change.",
            "Make it a turnaround sheet: the same subject four times side by side, front, three-quarter, side and back, plain white background.",
            "Write it the way this view's workflow expects (how it names its pictures)."]
        if (kind === "character" && field === "identity") return empty ? [
            "Describe how this person looks, concretely: age, face, eyes, hair, skin, build, distinguishing marks."] : [
            "Make this concrete enough to recognise them every time: shapes, colours and marks, not adjectives.",
            "Update the picture prompt to match this identity.",
            "Suggest two other takes on this character that suit the song."]
        if (kind === "look" && field === "description") return empty ? [
            "Describe this look: each garment with its colour and material, shoes, accessories, hair and makeup, and anything else it changes."] : [
            "Make it specific enough that the outfit stays identical across shots: garments, colours, materials, accessories.",
            "Fit it to the concept's palette and era.",
            "Update this look's picture prompt to match."]
        if (kind === "location" && field === "description") return empty ? [
            "Describe this place: layout, materials, colours, and the fixed details that make it recognisable."] : [
            "Add the fixed details that identify it from any angle: signs, furniture, materials, colours.",
            "Fit it to the concept's palette and era.",
            "Update the picture prompt to match."]
        if (kind === "location" && (field === "lighting" || field === "timeOfDay")) return [
            empty ? "Suggest a time of day and lighting for this place that suit the sections set here." : "Is this right for the sections set here? Suggest better if not.",
            "Describe the light concretely: its sources, direction, colour and contrast.",
            "Update the picture prompt so its light matches."]
        if (kind === "concept" && look[field]) return empty ? [
            "Suggest this from the song and the brief, in concrete terms an image model can follow."] : [
            "Make this concrete enough for an image model: colours, light, textures and film stock, not moods.",
            "Will this keep faces and outfits readable in the reference pictures? Adjust it if not.",
            "Give me three alternatives that suit the song."]
        if (kind === "concept" && field === "rules") return [
            "Suggest rules that keep characters, outfits and places consistent from shot to shot.",
            "Which of these rules will the image and video models struggle to follow? Reword them.",
            "Check every picture prompt against these rules."]
        if (kind === "concept" && field === "brief") return [
            "Ask me questions to sharpen this brief.",
            "What's missing from it to plan the video?"]
        if (kind === "concept" && (field === "logline" || field === "treatment")) return empty ? [
            "Write this from the brief and the song."] : [
            "Make it more visual: what we would actually see.",
            "Give me three alternatives.",
            "Explain how this fits the song."]
        if (kind === "arc" && field === "happens") return empty ? [
            "Suggest what happens in this section, as what we would see.",
            "Which characters, looks and locations does this section need?"] : [
            "Make this more visual and specific: who, where, doing what.",
            "Which characters, looks and locations does this need? Add any that are missing, with picture prompts.",
            "How does this follow the section before and lead into the next?"]
        if (kind === "arc" && field === "beat") return ["Suggest the emotional beat from this section's lyrics and energy."]
        if (kind === "arc" && field === "density") return ["How fast should this section cut, given its energy? Set it."]
        if (kind === "character" && !field) return [
            "Is this character ready for reference pictures? Fix what isn't: identity, picture prompt, looks.",
            "Write or improve the picture prompts for this character and each look.",
            "Suggest the looks this character needs for the sections they appear in."]
        if (kind === "look" && !field) return [
            "Is this look ready for a reference picture? Fix what isn't.",
            "Write or improve this look's picture prompt: full body, the same face, every garment named."]
        if (kind === "location" && !field) return [
            "Is this location ready for a reference picture? Fix what isn't.",
            "Write or improve this location's picture prompt: empty, wide, in the video's frame.",
            "Which sections should happen here?"]
        if (kind === "arc" && !field) return [
            "Plan this section: what happens, who and where.",
            "What's missing in this section?"]
        if (field) return empty ? ["Suggest this, and tell me why."] : ["What would you suggest instead, and why?"]
        return []
    }
    function send() {
        const thread = editor.requests.ask(kind, itemId, field, label, text.text)
        if (!thread) return
        close()
        sent(thread)
    }
    onOpened: { text.text = ""; text.forceActiveFocus() }

    contentItem: ColumnLayout {
        spacing: 10
        Text { text: "Ask your agent"; color: "#8fa0a4"; font.pixelSize: 12 }
        Text { objectName: "askLabel"; text: ask.label; color: "#edf1ef"; font.pixelSize: 16; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
        Text {
            visible: ask.currentValue.length > 0
            Layout.fillWidth: true; wrapMode: Text.Wrap; maximumLineCount: 3; elide: Text.ElideRight
            text: "Now: " + ask.currentValue; color: "#8fa0a4"; font.pixelSize: 12
        }
        Flow {
            Layout.fillWidth: true
            spacing: 6
            visible: ask.suggestions.length > 0
            Repeater {
                model: ask.suggestions
                Rectangle {
                    id: chip
                    required property string modelData
                    required property int index
                    objectName: "askSuggestion" + index
                    width: Math.min(chipText.implicitWidth + 16, ask.availableWidth)
                    height: chipText.implicitHeight + 10
                    radius: 4
                    color: chipMouse.containsMouse ? "#2f4a52" : "#22313a"
                    border.color: "#34434a"
                    Text {
                        id: chipText
                        x: 8; y: 5; width: chip.width - 16
                        text: chip.modelData; wrapMode: Text.Wrap
                        color: "#d5dcdb"; font.pixelSize: 12
                    }
                    MouseArea { id: chipMouse; anchors.fill: parent; hoverEnabled: true; cursorShape: Qt.PointingHandCursor; onClicked: ask.use(chip.modelData) }
                }
            }
        }
        ScrollView {
            id: askScroll
            Layout.fillWidth: true; Layout.preferredHeight: 120
            contentWidth: availableWidth
            background: Rectangle { radius: 4; color: "#162027"; border.color: text.activeFocus ? "#87dec7" : "#34434a" }
            TextArea {
                id: text
                objectName: "askText"
                width: askScroll.availableWidth
                wrapMode: TextEdit.Wrap; selectByMouse: true
                color: "#edf1ef"; font.pixelSize: 13
                leftPadding: 10; rightPadding: 10; topPadding: 8; bottomPadding: 8
                placeholderText: "e.g. Make this more 80s, or: why a crane move here?"
                placeholderTextColor: "#6f8085"
                background: null
                Keys.onPressed: e => {
                    if ((e.key === Qt.Key_Return || e.key === Qt.Key_Enter) && (e.modifiers & Qt.ControlModifier)) { ask.send(); e.accepted = true }
                    if (e.key === Qt.Key_Escape) { ask.close(); e.accepted = true }
                }
            }
        }
        Text {
            objectName: "askDelivery"
            Layout.fillWidth: true; wrapMode: Text.Wrap; font.pixelSize: 12
            color: editor.agent.liveClient ? "#87dec7" : "#8fa0a4"
            text: editor.agent.liveClient
                  ? "Claude Code is connected. If it was started with ChorusCut as a channel, it gets this right away; otherwise it sees it next time it checks."
                  : "No Claude Code is connected right now. The request waits in Plan ▸ Requests; ask your agent to check its ChorusCut requests."
        }
        RowLayout {
            Layout.fillWidth: true
            Text { text: Keys.mod() + "Return sends, Esc cancels"; color: "#6f8085"; font.pixelSize: 11; Layout.fillWidth: true }
            Button {
                id: cancelButton
                focusPolicy: Qt.NoFocus
                implicitHeight: 30; leftPadding: 12; rightPadding: 12
                text: "Cancel"
                contentItem: Text { text: cancelButton.text; color: "#edf1ef"; font.pixelSize: 13; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { radius: 5; color: cancelButton.hovered ? "#3b4b52" : "#2b383f" }
                onClicked: ask.close()
            }
            Button {
                id: sendButton
                objectName: "askSend"
                focusPolicy: Qt.NoFocus
                enabled: text.text.trim().length > 0
                implicitHeight: 30; leftPadding: 12; rightPadding: 12
                text: "Send"
                contentItem: Text { text: sendButton.text; color: sendButton.enabled ? "#122328" : "#4b6b66"; font.pixelSize: 13; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { radius: 5; color: sendButton.enabled ? "#87dec7" : "#2b4a44" }
                onClicked: ask.send()
            }
        }
    }
}
