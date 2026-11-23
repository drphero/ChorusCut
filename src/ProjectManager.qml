import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

// Resolve-style Project Manager: libraries on the left, project cards on the right.
// It covers the editor at launch and on ⌘O; Esc returns to the open project.
FocusScope {
    id: pm
    readonly property color ink: "#edf1ef"
    readonly property color quiet: "#adb8ba"
    readonly property color faint: "#748084"
    readonly property color mint: "#87dec7"
    readonly property color amber: "#e7b75f"
    readonly property color panel: "#1d2930"
    readonly property color line: "#34434a"

    signal finished()
    property string selectedPath: ""
    property string sortBy: "modified"
    readonly property var selectedCard: {
        for (const p of library.projects) if (p.path === selectedPath) return p
        return null
    }
    readonly property var shown: {
        const needle = search.text.trim().toLowerCase()
        const list = library.projects.filter(p => !needle || p.name.toLowerCase().includes(needle))
        if (sortBy === "name") list.sort((a, b) => a.name.localeCompare(b.name, undefined, {sensitivity: "base"}))
        else list.sort((a, b) => b.modified - a.modified)
        return list
    }

    function show() {
        library.refresh()
        const open = library.projects.find(p => p.open)
        if (open) selectedPath = open.path
        grid.forceActiveFocus()
    }
    function openProject(path) { if (path && library.open(path)) pm.finished() }
    function duration(ms) {
        const s = Math.round(ms / 1000)
        return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0")
    }
    function leaf(url) { return decodeURIComponent(String(url).split("/").pop()) }
    function menuFor(card, item, x, y) {
        selectedPath = card.path
        cardMenu.card = card
        cardMenu.popup(item, x, y)
    }
    function askTrash(card) { trash.card = card; trash.open() }
    function askRename(card) { rename.card = card; rename.open() }

    Keys.onPressed: event => {
        if (event.key === Qt.Key_Escape && editor.ready) { pm.finished(); event.accepted = true }
    }

    Rectangle { anchors.fill: parent; color: "#11191e" }
    // Swallow clicks and wheel so nothing reaches the editor underneath.
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: wheel => wheel.accepted = true }

    component Action: Button {
        id: btn
        property bool active: false
        focusPolicy: Qt.NoFocus
        implicitHeight: 32; leftPadding: 14; rightPadding: 14
        contentItem: Text { text: btn.text; color: btn.enabled ? (btn.active ? "#122328" : pm.ink) : pm.faint; font.pixelSize: 13; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: btn.active ? (btn.enabled ? pm.mint : "#3d5752") : (btn.hovered ? "#3b4b52" : "#2b383f"); radius: 5 }
    }
    component MItem: MenuItem {
        id: mi
        implicitHeight: 28
        contentItem: Text { text: mi.text; color: mi.enabled ? (mi.danger ? "#f0a08f" : pm.ink) : pm.faint; font.pixelSize: 13; leftPadding: 6; verticalAlignment: Text.AlignVCenter }
        background: Rectangle { color: mi.highlighted ? "#34505a" : "transparent"; radius: 4 }
        property bool danger: false
    }
    component DarkMenu: Menu {
        padding: 4
        background: Rectangle { implicitWidth: 200; color: "#223038"; radius: 6; border.color: pm.line }
    }
    component Field: TextField {
        color: pm.ink; font.pixelSize: 14; selectByMouse: true
        placeholderTextColor: pm.faint
        background: Rectangle { color: "#162027"; radius: 4; border.color: parent.activeFocus ? pm.mint : pm.line }
    }
    component Sheet: Popup {
        anchors.centerIn: Overlay.overlay
        modal: true
        padding: 20
        background: Rectangle { color: pm.panel; radius: 8; border.color: pm.line }
        Overlay.modal: Rectangle { color: "#99000000" }
    }

    ColumnLayout {
        anchors.fill: parent; anchors.margins: 18; spacing: 14

        RowLayout {
            Layout.fillWidth: true
            Text { text: "Project Manager"; color: pm.ink; font.pixelSize: 22; font.weight: Font.DemiBold }
            Item { Layout.fillWidth: true }
            Action { objectName: "closeManager"; visible: editor.ready; text: "Back to " + editor.projectName; onClicked: pm.finished() }
        }

        RowLayout {
            Layout.fillWidth: true; Layout.fillHeight: true; spacing: 14

            // ------------------------------------------------ libraries
            Rectangle {
                Layout.preferredWidth: 230; Layout.fillHeight: true
                color: pm.panel; radius: 6; border.color: pm.line
                ColumnLayout {
                    anchors.fill: parent; anchors.margins: 10; spacing: 6
                    Text { text: "PROJECT LIBRARIES"; color: pm.faint; font.pixelSize: 11; font.letterSpacing: 1; Layout.leftMargin: 4 }
                    ListView {
                        id: libs
                        Layout.fillWidth: true; Layout.fillHeight: true
                        clip: true; spacing: 2
                        model: library.libraries
                        delegate: Rectangle {
                            id: libRow
                            required property var modelData
                            required property int index
                            objectName: "library_" + modelData.name
                            width: libs.width; height: 46; radius: 5
                            color: index === library.current ? "#2d4a50" : (libHover.hovered ? "#26353c" : "transparent")
                            HoverHandler { id: libHover }
                            Column {
                                anchors.fill: parent; anchors.leftMargin: 10; anchors.rightMargin: 8; anchors.topMargin: 6
                                Text { width: parent.width; text: libRow.modelData.name; color: pm.ink; font.pixelSize: 14; elide: Text.ElideRight }
                                Text { width: parent.width; text: libRow.modelData.exists ? libRow.modelData.shown : "Not created yet · " + libRow.modelData.shown
                                       color: pm.faint; font.pixelSize: 11; elide: Text.ElideMiddle }
                            }
                            MouseArea {
                                anchors.fill: parent
                                acceptedButtons: Qt.LeftButton | Qt.RightButton
                                onClicked: mouse => {
                                    library.current = libRow.index
                                    if (mouse.button === Qt.RightButton) { libMenu.which = libRow.index; libMenu.popup() }
                                }
                            }
                        }
                    }
                    Action { objectName: "addLibrary"; Layout.fillWidth: true; text: "Add library…"; onClicked: addLibraryDialog.open() }
                }
            }

            // ------------------------------------------------ projects
            ColumnLayout {
                Layout.fillWidth: true; Layout.fillHeight: true; spacing: 10

                RowLayout {
                    Layout.fillWidth: true; spacing: 10
                    Column {
                        Layout.fillWidth: true
                        Text { text: library.libraries.length ? library.libraries[library.current].name : ""; color: pm.ink; font.pixelSize: 17; font.weight: Font.DemiBold }
                        Text { text: library.projects.length === 1 ? "1 project" : library.projects.length + " projects"; color: pm.quiet; font.pixelSize: 12 }
                    }
                    Field { id: search; objectName: "projectSearch"; Layout.preferredWidth: 220; placeholderText: "Search projects"
                            Keys.onEscapePressed: { text = ""; grid.forceActiveFocus() } }
                    Action { text: pm.sortBy === "name" ? "Sort: Name" : "Sort: Last modified"
                             onClicked: pm.sortBy = pm.sortBy === "name" ? "modified" : "name" }
                }

                Rectangle {
                    Layout.fillWidth: true; Layout.fillHeight: true
                    color: "#162027"; radius: 6; border.color: pm.line; clip: true

                    GridView {
                        id: grid
                        objectName: "projectGrid"
                        anchors.fill: parent; anchors.margins: 12
                        focus: true
                        readonly property int columns: Math.max(1, Math.floor(width / 250))
                        cellWidth: Math.floor(width / columns); cellHeight: 214
                        model: pm.shown
                        ScrollBar.vertical: ScrollBar {}
                        Keys.onReturnPressed: pm.openProject(pm.selectedPath)
                        Keys.onEnterPressed: pm.openProject(pm.selectedPath)
                        Keys.onPressed: event => {
                            if ((event.key === Qt.Key_Backspace || event.key === Qt.Key_Delete) && pm.selectedCard &&
                                (event.modifiers & Qt.ControlModifier)) { pm.askTrash(pm.selectedCard); event.accepted = true }
                        }
                        delegate: Item {
                            id: cell
                            required property var modelData
                            width: grid.cellWidth; height: grid.cellHeight
                            Rectangle {
                                id: card
                                objectName: "projectCard_" + cell.modelData.name
                                anchors.fill: parent; anchors.margins: 6
                                radius: 7
                                readonly property bool selected: pm.selectedPath === cell.modelData.path
                                color: selected ? "#22383d" : (hover.hovered ? "#1e2c33" : "transparent")
                                border.color: selected ? pm.mint : "transparent"; border.width: 2
                                HoverHandler { id: hover }

                                Rectangle {
                                    id: thumb
                                    x: 8; y: 8; width: parent.width - 16; height: Math.round(width * 9 / 16)
                                    radius: 5; color: "#0c1216"; clip: true
                                    Image {
                                        anchors.fill: parent
                                        visible: status === Image.Ready
                                        source: cell.modelData.thumbnail
                                        cache: false; asynchronous: true
                                        fillMode: Image.PreserveAspectCrop
                                        sourceSize.width: 480
                                    }
                                    Text {
                                        anchors.centerIn: parent
                                        visible: !cell.modelData.thumbnail.toString()
                                        text: cell.modelData.name.charAt(0).toUpperCase()
                                        color: "#2f4a4c"; font.pixelSize: 56; font.weight: Font.Bold
                                    }
                                    Rectangle {
                                        visible: cell.modelData.open
                                        x: 6; y: 6; radius: 3; color: pm.mint
                                        width: openLabel.implicitWidth + 10; height: 18
                                        Text { id: openLabel; anchors.centerIn: parent; text: "OPEN"; color: "#122328"; font.pixelSize: 10; font.bold: true }
                                    }
                                    Text {
                                        anchors.right: parent.right; anchors.bottom: parent.bottom; anchors.margins: 6
                                        visible: cell.modelData.durationMs > 0
                                        text: pm.duration(cell.modelData.durationMs)
                                        color: pm.ink; font.pixelSize: 11; font.family: "Menlo"
                                        style: Text.Outline; styleColor: "#0c1216"
                                    }
                                }
                                Column {
                                    anchors.left: thumb.left; anchors.right: thumb.right; anchors.top: thumb.bottom; anchors.topMargin: 7
                                    spacing: 2
                                    Text { width: parent.width - 26; text: cell.modelData.name; color: pm.ink; font.pixelSize: 14; font.weight: Font.DemiBold; elide: Text.ElideRight }
                                    Text { width: parent.width; text: cell.modelData.modifiedText + (cell.modelData.picture ? " · Picture" : " · Song only")
                                           color: pm.quiet; font.pixelSize: 11; elide: Text.ElideRight }
                                    Text { width: parent.width; text: cell.modelData.lyrics; font.pixelSize: 11; elide: Text.ElideRight
                                           color: cell.modelData.flagged > 0 ? pm.amber : (cell.modelData.lines > 0 ? pm.mint : pm.faint) }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                                    onPressed: mouse => { pm.selectedPath = cell.modelData.path; grid.forceActiveFocus() }
                                    onClicked: mouse => { if (mouse.button === Qt.RightButton) pm.menuFor(cell.modelData, card, mouse.x, mouse.y) }
                                    onDoubleClicked: mouse => { if (mouse.button === Qt.LeftButton) pm.openProject(cell.modelData.path) }
                                }
                                // Visible handle for the same menu as a right-click.
                                Rectangle {
                                    objectName: "cardMenu_" + cell.modelData.name
                                    anchors.right: thumb.right; anchors.top: thumb.bottom; anchors.topMargin: 6
                                    width: 24; height: 20; radius: 4
                                    visible: hover.hovered || card.selected
                                    color: more.containsMouse ? "#3b4b52" : "transparent"
                                    Text { anchors.centerIn: parent; text: "•••"; color: pm.quiet; font.pixelSize: 10 }
                                    MouseArea { id: more; anchors.fill: parent; hoverEnabled: true
                                                onClicked: pm.menuFor(cell.modelData, parent, 0, parent.height) }
                                }
                            }
                        }
                    }

                    // Empty library.
                    Column {
                        anchors.centerIn: parent; spacing: 12
                        visible: pm.shown.length === 0
                        Text { anchors.horizontalCenter: parent.horizontalCenter; color: pm.quiet; font.pixelSize: 15
                               text: library.projects.length ? "No projects match “" + search.text.trim() + "”." : "No projects in this library yet." }
                        Action { anchors.horizontalCenter: parent.horizontalCenter; visible: !library.projects.length; active: true
                                 text: "New Project…"; onClicked: newProject.open() }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    Text { Layout.fillWidth: true; elide: Text.ElideRight; font.pixelSize: 12
                           color: pm.quiet; text: library.message }
                    Action { text: "Open other…"; onClicked: otherProject.open() }
                    Action { objectName: "newProject"; text: "New Project…"; enabled: !editor.importing; onClicked: newProject.open() }
                    Action { objectName: "openSelected"; text: "Open"; active: true; enabled: pm.selectedCard !== null
                             onClicked: pm.openProject(pm.selectedPath) }
                }
            }
        }
    }

    // ---------------------------------------------------------------- menus and dialogs
    DarkMenu {
        id: cardMenu
        property var card: null
        MItem { objectName: "menuOpen"; text: "Open"; onTriggered: pm.openProject(cardMenu.card.path) }
        MItem { objectName: "menuRename"; text: "Rename…"; onTriggered: pm.askRename(cardMenu.card) }
        MItem { objectName: "menuDuplicate"; text: "Duplicate"; enabled: !library.busy; onTriggered: library.duplicate(cardMenu.card.path) }
        MItem { text: library.revealLabel; onTriggered: library.reveal(cardMenu.card.path) }
        MenuSeparator { contentItem: Rectangle { implicitHeight: 1; color: pm.line } }
        MItem { objectName: "menuTrash"; text: "Move to Trash…"; danger: true; onTriggered: pm.askTrash(cardMenu.card) }
    }
    DarkMenu {
        id: libMenu
        property int which: -1
        MItem { text: library.revealLabel; onTriggered: library.reveal(library.libraries[libMenu.which].path) }
        MItem { text: "Remove from list"; enabled: library.libraries.length > 1; onTriggered: library.removeLibrary(libMenu.which) }
    }
    FolderDialog { id: addLibraryDialog; title: "Choose a folder to use as a project library"; onAccepted: library.addLibrary(selectedFolder) }
    FolderDialog { id: otherProject; title: "Open a ChorusCut project folder"; onAccepted: if (editor.openProject(selectedFolder)) pm.finished() }

    Sheet {
        id: rename
        objectName: "renameSheet"
        property var card: null
        width: 420
        onOpened: { renameField.text = card.name; renameField.selectAll(); renameField.forceActiveFocus() }
        onClosed: grid.forceActiveFocus()
        readonly property string problem: card ? library.checkName(renameField.text, card.name) : ""
        function accept() { if (!problem && library.rename(card.path, renameField.text)) { pm.selectedPath = ""; close() } }
        contentItem: ColumnLayout {
            spacing: 10
            Text { text: "Rename project"; color: pm.ink; font.pixelSize: 16; font.weight: Font.DemiBold }
            Field { id: renameField; objectName: "renameField"; Layout.fillWidth: true
                    Keys.onReturnPressed: rename.accept(); Keys.onEnterPressed: rename.accept() }
            Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: pm.amber; font.pixelSize: 12
                   visible: renameField.text.trim().length > 0 && rename.problem.length > 0; text: rename.problem }
            Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: pm.quiet; font.pixelSize: 12
                   visible: rename.card && rename.card.open; text: "The project closes and reopens under its new name." }
            RowLayout {
                Item { Layout.fillWidth: true }
                Action { text: "Cancel"; onClicked: rename.close() }
                Action { objectName: "renameAccept"; text: "Rename"; active: true; enabled: !rename.problem; onClicked: rename.accept() }
            }
        }
    }

    Sheet {
        id: trash
        objectName: "trashSheet"
        property var card: null
        width: 440
        onClosed: grid.forceActiveFocus()
        contentItem: ColumnLayout {
            spacing: 10
            Text { text: "Move “" + (trash.card ? trash.card.name : "") + "” to the Trash?"; color: pm.ink; font.pixelSize: 16; font.weight: Font.DemiBold
                   Layout.fillWidth: true; wrapMode: Text.Wrap }
            Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: pm.quiet; font.pixelSize: 13
                   text: "The whole project folder goes, including its copies of the media. You can restore it from the Trash." }
            RowLayout {
                Item { Layout.fillWidth: true }
                Action { objectName: "trashCancel"; text: "Cancel"; onClicked: trash.close() }
                Action { objectName: "trashAccept"; text: "Move to Trash"
                         background: Rectangle { color: parent.hovered ? "#b35a48" : "#9a4a3b"; radius: 5 }
                         onClicked: { library.moveToTrash(trash.card.path); pm.selectedPath = ""; trash.close() } }
            }
        }
    }

    Sheet {
        id: newProject
        objectName: "newProjectSheet"
        width: Math.min(620, pm.width - 60)
        closePolicy: editor.importing ? Popup.NoAutoClose : Popup.CloseOnEscape
        property url master
        property string failure: ""
        property string suggested: ""
        readonly property string problem: library.checkName(nameField.text)
        onOpened: {
            failure = ""; master = ""; newLyrics.text = ""
            newLanguage.currentIndex = Math.max(0, newLanguage.indexOfValue(editor.comfy.language))
            let name = "Untitled Project", n = 1
            while (library.checkName(name)) name = "Untitled Project " + (++n)
            suggested = name
            nameField.text = name; nameField.selectAll(); nameField.forceActiveFocus()
        }
        onClosed: grid.forceActiveFocus()
        readonly property bool complete: !problem && master.toString().length > 0
        function accept() { if (complete && !editor.importing) { failure = ""; library.create(nameField.text, master, "", "") } }
        // Name the project after the song unless a name was already typed.
        onMasterChanged: {
            if (!master.toString() || nameField.text !== suggested) return
            const title = pm.leaf(master).replace(/\.[^.]+$/, "").replace(/[_]+/g, " ").trim()
            if (title && !library.checkName(title)) { nameField.text = title; suggested = title }
        }
        Connections {
            target: library
            function onCreated(success) {
                if (!success) { newProject.failure = library.message; return }
                // With lyrics pasted, save them and start aligning when ComfyUI is connected.
                const text = newLyrics.text.trim()
                if (text.length && editor.lyrics.saveText(text) && editor.comfy.connection === "ready")
                    editor.comfy.alignLyrics(newLanguage.currentValue)
                newProject.close(); pm.finished()
            }
        }
        FileDialog { id: pick; title: "Choose the song"
                     nameFilters: ["Audio (*.wav *.aif *.aiff *.flac *.mp3 *.m4a)", "All files (*)"]
                     onAccepted: newProject.master = selectedFile }
        contentItem: ColumnLayout {
            spacing: 10
            enabled: !editor.importing
            Text { text: "New project"; color: pm.ink; font.pixelSize: 16; font.weight: Font.DemiBold }
            RowLayout {
                Layout.fillWidth: true
                Action { objectName: "chooseSong"; Layout.preferredWidth: 110; text: "Song…"; onClicked: pick.open() }
                Text { Layout.fillWidth: true; elide: Text.ElideMiddle; font.pixelSize: 13
                       readonly property string file: newProject.master.toString() ? pm.leaf(newProject.master) : ""
                       text: file || "Choose the master recording"; color: file ? pm.ink : pm.faint }
            }
            Field { id: nameField; objectName: "newProjectName"; Layout.fillWidth: true; placeholderText: "Project name"
                    Keys.onReturnPressed: newProject.accept(); Keys.onEnterPressed: newProject.accept() }
            Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: pm.amber; font.pixelSize: 12
                   visible: nameField.text.trim().length > 0 && newProject.problem.length > 0; text: newProject.problem }
            Text { Layout.fillWidth: true; elide: Text.ElideMiddle; color: pm.faint; font.pixelSize: 12
                   text: "Saved in " + (library.libraries.length ? library.libraries[library.current].shown : "") }
            Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: pm.faint; font.pixelSize: 12
                   text: "The song is copied into the project folder, so the project moves as one piece." }
            RowLayout {
                Layout.fillWidth: true
                Text { Layout.fillWidth: true; text: "Lyrics (optional)"; color: pm.quiet; font.pixelSize: 13 }
                ComboBox {
                    id: newLanguage
                    objectName: "newProjectLanguage"
                    implicitHeight: 28; implicitWidth: 110
                    textRole: "label"; valueRole: "code"
                    model: [{label: "English", code: "en"}, {label: "French", code: "fr"}, {label: "German", code: "de"}, {label: "Spanish", code: "es"}]
                    Accessible.name: "Language of the lyrics"
                    contentItem: Text { leftPadding: 10; text: newLanguage.displayText; color: pm.ink; font.pixelSize: 13; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { color: newLanguage.hovered ? "#3b4b52" : "#2b383f"; radius: 5 }
                    indicator: Text { x: newLanguage.width - width - 10; anchors.verticalCenter: parent.verticalCenter; text: "▾"; color: pm.quiet }
                }
            }
            ScrollView {
                Layout.fillWidth: true; Layout.preferredHeight: 170
                TextArea {
                    id: newLyrics
                    objectName: "newProjectLyrics"
                    wrapMode: TextEdit.Wrap; selectByMouse: true
                    color: pm.ink; font.pixelSize: 13; placeholderTextColor: pm.faint
                    placeholderText: "Paste the lyrics as sung, with section names like [Chorus] on their own lines."
                    background: Rectangle { color: "#162027"; radius: 4; border.color: newLyrics.activeFocus ? pm.mint : pm.line }
                }
            }
            Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: pm.faint; font.pixelSize: 12
                   text: !newLyrics.text.trim().length ? "You can also add lyrics after the project opens."
                       : editor.comfy.connection === "ready" ? "The lyrics are aligned on ComfyUI as soon as the project opens."
                       : "ComfyUI is not connected, so the lyrics are saved; choose Align in Lyrics… once it is." }
            Text { Layout.fillWidth: true; wrapMode: Text.Wrap; color: "#f0a08f"; font.pixelSize: 12
                   visible: text.length > 0; text: newProject.failure }
            RowLayout {
                Item { Layout.fillWidth: true }
                Action { text: "Cancel"; onClicked: newProject.close() }
                Action { objectName: "createProject"; active: true; enabled: newProject.complete && !editor.importing
                         text: editor.importing ? "Copying media…" : "Create"; onClicked: newProject.accept() }
            }
        }
    }
}
