// Agent control end to end: launches the real choruscut-mcp over stdio, speaks MCP to it the way
// Claude Code or Codex would, and checks that its tool calls reach the app's plan commands, that
// each pass is one undo step (undone with the banner's Undo button and ⌘Z, by real input), and
// that user-owned and locked fields stay untouched.
// usage: ChorusCutMcpCheck PROJECT_COPY ALIGNMENT_JSON CHORUSCUT_MCP
#include "editor.h"
#include "waveform.h"
#include "library.h"
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QDebug>
#include <functional>

static int fail(int code, const QString& why) { qCritical().noquote() << "FAIL" << code << why; return code; }
static bool waitFor(const std::function<bool()>& ready, int timeout = 10000) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < timeout) { if (ready()) return true; QTest::qWait(20); }
    return ready();
}
static QQuickItem* findItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name && root->isVisible()) return root;
    for (auto child : root->childItems())
        if (auto found = findItem(child, name)) return found;
    return nullptr;
}
static QQuickItem* find(QQuickWindow* window, const QString& name) {
    if (auto item = findItem(window->contentItem(), name)) return item;
    for (auto item : window->findChildren<QQuickItem*>(name))
        if (item->isVisible()) return item;
    return nullptr;
}

// A minimal MCP client over a child process's stdio. Waiting keeps the event loop running,
// because the app answering the calls lives on this thread.
struct Client {
    QProcess process;
    int serial = 0;
    QByteArray buffer;
    QList<QJsonObject> notes;  // notifications the server sent
    void pump() {
        buffer += process.readAllStandardOutput();
        int nl;
        while ((nl = buffer.indexOf('\n')) >= 0) {
            const auto message = QJsonDocument::fromJson(buffer.left(nl)).object();
            buffer.remove(0, nl + 1);
            if (message.contains("method") && !message.contains("id")) notes.append(message);
            else pending.append(message);
        }
    }
    QList<QJsonObject> pending;
    // Waits for the next notification with this method.
    QJsonObject note(const QString& method, int timeout = 5000) {
        QElapsedTimer t; t.start();
        while (t.elapsed() < timeout) {
            pump();
            for (int i = 0; i < notes.size(); ++i)
                if (notes[i].value("method") == method) return notes.takeAt(i);
            QTest::qWait(10);
        }
        return {};
    }
    QJsonObject request(const QString& method, const QJsonObject& params = {}, int timeout = 20000) {
        const int id = ++serial;
        process.write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}).toJson(QJsonDocument::Compact) + '\n');
        QElapsedTimer t; t.start();
        while (t.elapsed() < timeout) {
            pump();
            for (int i = 0; i < pending.size(); ++i)
                if (pending[i].value("id").toInt() == id) return pending.takeAt(i);
            QTest::qWait(10);
        }
        return {{"timeout", true}};
    }
    void notify(const QString& method) {
        process.write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"method", method}}).toJson(QJsonDocument::Compact) + '\n');
    }
    // tools/call: the result object ({content, isError}).
    QJsonObject call(const QString& tool, const QJsonObject& arguments = {}) {
        return request("tools/call", {{"name", tool}, {"arguments", arguments}}).value("result").toObject();
    }
    // The JSON a text result carries.
    QJsonObject data(const QString& tool, const QJsonObject& arguments = {}) {
        const auto r = call(tool, arguments);
        return QJsonDocument::fromJson(r["content"].toArray()[0].toObject()["text"].toString().toUtf8()).object();
    }
};
static QString textOf(const QJsonObject& result) { return result["content"].toArray()[0].toObject()["text"].toString(); }

int main(int argc, char** argv) {
    if (argc != 4) { qCritical() << "usage: ChorusCutMcpCheck PROJECT_COPY ALIGNMENT_JSON CHORUSCUT_MCP"; return 1; }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    qputenv("SDL_AUDIODRIVER", "disk"); qputenv("SDL_AUDIO_DRIVER", "disk");
    QTemporaryDir scratch;
    qputenv("SDL_AUDIO_DISK_OUTPUT_FILE", QFile::encodeName(scratch.filePath("capture.raw")));
    // Our own socket, so a running Chorus Cut is never involved.
    const QByteArray socketName = "chorus-mcp-check-" + QByteArray::number(QCoreApplication::applicationPid());
    qputenv("CHORUS_AGENT_SOCKET", socketName);
    QGuiApplication app(argc, argv);
    app.setOrganizationName("Chorus Cut Tests");
    app.setApplicationName("MCP Check");
    Mlt::Factory::init();
    const QString shim = QFileInfo(argv[3]).absoluteFilePath();

    // 0. With no app on the socket, the server still starts and says the app is not running.
    {
        Client lone;
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert("CHORUS_AGENT_SOCKET", QString::fromLatin1(socketName) + "-nobody");
        lone.process.setProcessEnvironment(env);
        lone.process.start(shim, {});
        if (!lone.process.waitForStarted()) return fail(2, "choruscut-mcp did not start");
        if (lone.request("initialize", {{"protocolVersion", "2025-06-18"}, {"capabilities", QJsonObject{}}, {"clientInfo", QJsonObject{{"name", "lone"}}}})
                .value("result").toObject().value("protocolVersion") != "2025-06-18") return fail(2, "initialize failed without the app");
        const auto r = lone.call("get_project");
        if (!r["isError"].toBool() || !textOf(r).contains("not running")) return fail(2, "a call without the app should say Chorus Cut is not running");
        lone.process.closeWriteChannel();
        lone.process.waitForFinished(3000);
    }

    Editor editor;
    if (!editor.openProject(QUrl::fromLocalFile(QFileInfo(argv[1]).absoluteFilePath()))) return fail(3, editor.status());
    if (!editor.lyrics()->importAlignment(QUrl::fromLocalFile(QFileInfo(argv[2]).absoluteFilePath()))) return fail(3, editor.lyrics()->message());
    Plan* plan = editor.plan();
    // The user already wrote and locked the logline.
    plan->run({{"op", "set"}, {"kind", "concept"}, {"fields", QVariantMap{{"logline", "The user's logline"}}}});
    plan->run({{"op", "lock"}, {"kind", "concept"}, {"field", "logline"}, {"locked", true}});

    qmlRegisterType<Monitor>("Chorus", 1, 0, "VideoMonitor");
    qmlRegisterType<WaveformView>("Chorus", 1, 0, "WaveformView");
    QQmlApplicationEngine engine;
    ProjectLibrary library(&editor);
    engine.rootContext()->setContextProperty("editor", &editor);
    engine.rootContext()->setContextProperty("library", &library);
    engine.load(QUrl("qrc:/src/Main.qml"));
    if (engine.rootObjects().isEmpty()) return fail(4, "QML did not load");
    auto window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    window->resize(1280, 820);
    if (!QTest::qWaitForWindowExposed(window)) return fail(4, "window not exposed");
    if (!editor.agent()->start()) return fail(4, "the app could not listen for agents: " + editor.agent()->note());
    auto center = [&](QQuickItem* item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); };
    const QString shots = qEnvironmentVariable("CHORUS_SHOTS");
    auto capture = [&](const QString& name) { if (!shots.isEmpty()) window->grabWindow().save(shots + "/" + name + ".png"); };
    auto click = [&](QQuickItem* item) {
        if (!item) { qCritical() << "nothing to click"; return; }
        QTest::mouseClick(window, Qt::LeftButton, {}, center(item)); QTest::qWait(150);
    };

    // 1. Handshake, tools and the planning prompt.
    Client agent;
    agent.process.start(shim, {});
    if (!agent.process.waitForStarted()) return fail(5, "choruscut-mcp did not start");
    const auto init = agent.request("initialize", {{"protocolVersion", "2025-03-26"}, {"capabilities", QJsonObject{}},
                                                   {"clientInfo", QJsonObject{{"name", "Check Agent"}, {"version", "1"}}}}).value("result").toObject();
    if (init["protocolVersion"] != "2025-03-26" || !init["instructions"].toString().contains("apply_plan") || !init["capabilities"].toObject().contains("tools"))
        return fail(5, "initialize answer is incomplete");
    agent.notify("notifications/initialized");
    if (!waitFor([&] { return editor.agent()->clients() == QStringList{"Check Agent"}; }, 3000))
        return fail(5, "the app does not show the connected agent: " + editor.agent()->clients().join(","));
    QStringList names;
    for (const auto& t : agent.request("tools/list").value("result").toObject()["tools"].toArray()) names << t.toObject()["name"].toString();
    if (!init["capabilities"].toObject()["experimental"].toObject().contains("claude/channel"))
        return fail(5, "the server does not offer itself as a Claude Code channel");
    if (names != QStringList{"get_guide", "get_project", "get_song", "get_plan", "get_checks", "get_schema", "view_reference", "apply_plan",
                             "get_requests", "reply_request", "ask_user"})
        return fail(6, "unexpected tools: " + names.join(", "));
    const QString guide = textOf(agent.call("get_guide"));
    if (!guide.contains("Cast and locations pass") || !guide.contains("Intake") || !guide.contains("Just draft it all") || !guide.contains("pause"))
        return fail(6, "get_guide does not offer the intake and the pause after the concept");
    const auto prompt = agent.request("prompts/get", {{"name", "plan"}, {"arguments", QJsonObject{{"direction", "noir, one singer"}}}});
    if (!prompt.value("result").toObject()["messages"].toArray()[0].toObject()["content"].toObject()["text"].toString().contains("noir, one singer"))
        return fail(6, "the planning prompt does not carry the user's direction");

    // 2. Reading the project, song, schema, plan and checks.
    const auto project = agent.data("get_project");
    if (!project["open"].toBool() || project["fps"].toDouble() != editor.fps() || project["frames"].toInt() != editor.length())
        return fail(7, "get_project does not describe the open project");
    const auto song = agent.data("get_song", {{"include_words", true}});
    const auto lines = song["lines"].toArray();
    if (lines.size() != editor.lyrics()->lines().size() || lines[0].toObject()["words"].toArray().isEmpty() || song["sections"].toArray().size() < 4)
        return fail(7, "get_song is missing lines, words or sections");
    const auto schema = agent.data("get_schema");
    if (!schema["kinds"].toObject().contains("shot") || schema["routes"].toArray().isEmpty() || schema["ops"].toArray().size() < 8)
        return fail(7, "get_schema is incomplete");
    if (agent.data("get_checks")["gates"].toArray().size() != 4) return fail(7, "get_checks should list four gates");

    // 3. A pass that tries to overwrite the user's locked logline: everything else lands, the logline is skipped.
    auto pass = agent.data("apply_plan", {{"label", "Draft concept"}, {"commands", QJsonArray{
        QJsonObject{{"op", "set"}, {"kind", "concept"}, {"fields", QJsonObject{{"logline", "Agent logline"}, {"videoKind", "mixed"}, {"aspect", "16:9"},
                                                                              {"style", "Night radio noir"}}}}}}});
    if (!pass["applied"].toBool() || pass["skipped"].toArray() != QJsonArray{"logline"} || plan->concept()["fields"].toMap()["logline"] != "The user's logline")
        return fail(8, "the user's locked logline was not protected: " + QString::fromUtf8(QJsonDocument(pass).toJson(QJsonDocument::Compact)));
    if (plan->concept()["prov"].toMap()["style"].toMap()["by"] != "agent") return fail(8, "the agent's fields are not marked as the agent's");
    // The intake's answers go at the end of the brief; rewriting it fails the whole pass.
    plan->run({{"op", "set"}, {"kind", "concept"}, {"fields", QVariantMap{{"brief", "One singer."}}}});
    pass = agent.data("apply_plan", {{"label", "Record intake"}, {"commands", QJsonArray{
        QJsonObject{{"op", "set"}, {"kind", "concept"}, {"fields", QJsonObject{{"brief", "One singer.\n- Mostly performance\n- 16:9"}}}}}}});
    if (!pass["applied"].toBool() || !plan->concept()["fields"].toMap()["brief"].toString().endsWith("16:9")) return fail(8, "the agent could not add to the brief");
    const auto rewrite = agent.call("apply_plan", {{"label", "Rewrite"}, {"commands", QJsonArray{
        QJsonObject{{"op", "set"}, {"kind", "concept"}, {"mode", "replace"}, {"fields", QJsonObject{{"brief", "Something else"}}}}}}});
    if (!rewrite["isError"].toBool() || !textOf(rewrite).contains("only add to the brief")) return fail(8, "rewriting the brief was not refused: " + textOf(rewrite));
    // A command claiming to be the user's is still the agent's.
    agent.data("apply_plan", {{"label", "Sneaky"}, {"commands", QJsonArray{
        QJsonObject{{"op", "set"}, {"kind", "concept"}, {"by", "user"}, {"fields", QJsonObject{{"era", "1980s"}}}}}}});
    if (plan->concept()["prov"].toMap()["era"].toMap()["by"] != "agent") return fail(8, "a command inside an agent pass passed itself off as the user's");
    if (!agent.call("apply_plan", {{"label", "Lock"}, {"commands", QJsonArray{QJsonObject{{"op", "lock"}, {"kind", "concept"}, {"field", "style"}}}}})["isError"].toBool())
        return fail(8, "an agent was allowed to lock a field");

    // 4. One pass that builds on its own new ids, and a failing pass that leaves nothing behind.
    const auto l0 = lines[0].toObject(), l1 = lines[1].toObject();
    const QString before = editor.undoText();
    auto broken = agent.call("apply_plan", {{"label", "Broken"}, {"commands", QJsonArray{
        QJsonObject{{"op", "add"}, {"kind", "character"}, {"fields", QJsonObject{{"name", "Ghost"}}}},
        QJsonObject{{"op", "shot.add"}, {"track", 1}, {"start", 10}, {"end", 5}}}}});
    if (!broken["isError"].toBool() || !textOf(broken).contains("Command 2") || plan->characters().size() != 0 || editor.undoText() != before)
        return fail(9, "a failing pass left changes behind or did not say which command failed: " + textOf(broken));
    pass = agent.data("apply_plan", {{"label", "Cast, places and first shot"}, {"commands", QJsonArray{
        QJsonObject{{"op", "add"}, {"kind", "character"}, {"ref", "mara"}, {"fields", QJsonObject{{"name", "Mara"}, {"role", "singer"},
                                                                                                    {"identity", "Late twenties, cropped black hair"}}}},
        QJsonObject{{"op", "add"}, {"kind", "look"}, {"ref", "coat"}, {"character", "$mara"}, {"fields", QJsonObject{{"name", "Red coat"}}}},
        QJsonObject{{"op", "add"}, {"kind", "location"}, {"ref", "booth"}, {"fields", QJsonObject{{"name", "Radio booth"}, {"timeOfDay", "night"}}}},
        QJsonObject{{"op", "singers"}, {"rows", QJsonArray{0, 1}}, {"singers", QJsonArray{"$mara"}}},
        QJsonObject{{"op", "set"}, {"kind", "arc"}, {"id", 1}, {"section", song["sections"].toArray()[1].toObject()["label"]},
                    {"fields", QJsonObject{{"happens", "She finds the frequency."}}}},
        QJsonObject{{"op", "shot.add"}, {"track", 1}, {"start", l0["startFrame"]}, {"end", l1["endFrame"]}, {"ref", "first"},
                    {"fields", QJsonObject{{"type", "lipsync"}, {"purpose", "Mara finds the frequency"}, {"who", QJsonArray{QJsonObject{{"character", "$mara"}, {"look", "$coat"}}}},
                                           {"where", "$booth"}, {"framing", "mcu"}, {"move", "push"}, {"route", "h3-lipsync"}}}}}}});
    if (!pass["applied"].toBool() || pass["ids"].toArray().size() != 6 || pass["refs"].toObject().size() != 4)
        return fail(10, "the pass did not report its ids: " + QString::fromUtf8(QJsonDocument(pass).toJson(QJsonDocument::Compact)));
    const int shotId = pass["refs"].toObject()["first"].toInt();
    const auto shot = plan->shot(shotId);
    if (shot["fields"].toMap()["who"].toList().value(0).toMap()["look"].toInt() != pass["refs"].toObject()["coat"].toInt() ||
        plan->singers()[1].toMap()["singers"].toList() != QVariantList{pass["refs"].toObject()["mara"].toInt()})
        return fail(10, "$name references were not resolved to the new ids");
    if (editor.undoText() != "Agent: Cast, places and first shot") return fail(10, "the pass is not one undo step: " + editor.undoText());

    // 5. The banner names the pass; its Undo button removes the whole pass; ⇧⌘Z brings it back; ⌘Z removes it again.
    auto banner = find(window, "agentPassBanner");
    auto undo = find(window, "agentPassUndo");
    if (!banner || !undo || !undo->isEnabled()) return fail(11, "no banner with Undo after the pass");
    capture("agent-pass");
    click(undo);
    if (plan->characters().size() != 0 || plan->locations().size() != 0 || plan->shots().size() != 0 || !plan->singers()[0].toMap()["singers"].toList().isEmpty() ||
        plan->concept()["fields"].toMap()["style"] != "Night radio noir")
        return fail(11, "Undo did not remove exactly the last pass");
    if (find(window, "agentPassBanner")) return fail(11, "the banner stayed after Undo");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    if (plan->characters().size() != 1 || plan->shots().size() != 1) return fail(12, "⇧⌘Z did not redo the pass");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    if (plan->characters().size() != 0 || plan->shots().size() != 0) return fail(12, "⌘Z did not undo the pass in one step");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);

    // 6. get_plan shows what the agent wrote, and view_reference shows a picture the user added.
    const auto view = agent.data("get_plan");
    const auto mara = view["characters"].toArray()[0].toObject();
    if (mara["fields"].toObject()["name"] != "Mara" || mara["provenance"].toObject()["name"].toObject()["by"] != "agent" ||
        mara["looks"].toArray().size() != 1 || view["shots"].toArray()[0].toObject()["lyricRows"].toArray().size() < 2)
        return fail(13, "get_plan does not reflect the pass");
    QImage face(600, 800, QImage::Format_RGB32);
    face.fill(QColor("#c08060"));
    face.save(scratch.filePath("face.png"));
    const int refId = plan->run({{"op", "ref.add"}, {"owner", "character"}, {"ownerId", mara["id"].toInt()}, {"path", scratch.filePath("face.png")}})["id"].toInt();
    const auto picture = agent.call("view_reference", {{"id", refId}});
    const auto content = picture["content"].toArray();
    const QImage seen = QImage::fromData(QByteArray::fromBase64(content[0].toObject()["data"].toString().toLatin1()), "JPG");
    if (picture["isError"].toBool() || content[0].toObject()["type"] != "image" || seen.size() != QSize(600, 800) || !content[1].toObject()["text"].toString().contains("Mara"))
        return fail(14, "view_reference did not return the picture");
    if (!agent.call("view_reference", {{"id", 999}})["isError"].toBool()) return fail(14, "an unknown reference id was not an error");
    // Views: get_plan lists them with the chosen picture; the agent adds one and a look's
    // changes, but cannot choose a picture.
    {
        const auto again = agent.data("get_plan");
        const auto maraViews = again["characters"].toArray()[0].toObject()["views"].toArray();
        if (maraViews.size() != 1 || maraViews[0].toObject()["fields"].toObject()["name"] != "Face" || maraViews[0].toObject()["chosen"].toInt() != refId ||
            again["characters"].toArray()[0].toObject()["looks"].toArray()[0].toObject()["views"].toArray().size() != 1 || !again.contains("imageWorkflows"))
            return fail(15, "get_plan does not list views: " + QString::fromUtf8(QJsonDocument(maraViews).toJson(QJsonDocument::Compact)));
        const int maraId = mara["id"].toInt(), coatId = mara["looks"].toArray()[0].toObject()["id"].toInt();
        const auto added = agent.data("apply_plan", {{"label", "Views"}, {"commands", QJsonArray{
            QJsonObject{{"op", "add"}, {"kind", "view"}, {"owner", "character"}, {"ownerId", maraId},
                        {"fields", QJsonObject{{"name", "Profile"}, {"framing", "Side profile facing left, plain grey backdrop"}, {"role", "face"}, {"from", "pictures"}}}},
            QJsonObject{{"op", "set"}, {"kind", "look"}, {"id", coatId}, {"fields", QJsonObject{{"changes", QJsonArray{"hair"}}}}}}}});
        if (!added["applied"].toBool() || plan->viewsFor("character", maraId).size() != 2 ||
            plan->item("look", coatId)["fields"].toMap()["changes"].toList() != QVariantList{"hair"})
            return fail(15, "the agent could not add a view or a look's changes: " + QString::fromUtf8(QJsonDocument(added).toJson(QJsonDocument::Compact)));
        QImage other(300, 400, QImage::Format_RGB32);
        other.fill(QColor("#405060"));
        other.save(scratch.filePath("face3.png"));
        const int altId = plan->run({{"op", "ref.add"}, {"owner", "character"}, {"ownerId", maraId}, {"path", scratch.filePath("face3.png")}})["id"].toInt();
        if (!agent.call("apply_plan", {{"label", "Pick"}, {"commands", QJsonArray{
                QJsonObject{{"op", "set"}, {"kind", "reference"}, {"id", altId}, {"fields", QJsonObject{{"approved", true}}}}}}})["isError"].toBool())
            return fail(15, "an agent chose a picture");
        if (!textOf(agent.call("get_guide")).contains("Nothing about a person is locked"))
            return fail(15, "the guide does not explain subjects, views and looks' changes");
    }

    // 7. The Agent sheet shows the setup commands and who is connected.
    click(find(window, "agentButton"));
    auto status = find(window, "agentStatus");
    if (!status || !status->property("text").toString().contains("Check Agent")) return fail(15, "the Agent sheet does not show the connection");
    if (!find(window, "channelCommand")) return fail(15, "the Agent sheet does not show how to start Claude Code with live requests");
    capture("agent-sheet");
    click(find(window, "agentDone"));
    if (!waitFor([&] { return !window->findChild<QObject*>("agentSheet")->property("visible").toBool(); }, 1500)) return fail(15, "Done did not close the Agent sheet");

    // 7b. Requests from the app: a Claude Code session gets them live as channel events; another
    // client does not. The agent reads, answers and asks back; everything stays in the project.
    {
        Client claude;
        claude.process.start(shim, {});
        if (!claude.process.waitForStarted()) return fail(20, "a second choruscut-mcp did not start");
        claude.request("initialize", {{"protocolVersion", "2025-06-18"}, {"capabilities", QJsonObject{}},
                                      {"clientInfo", QJsonObject{{"name", "claude-code"}, {"version", "2"}}}});
        claude.notify("notifications/initialized");
        if (!waitFor([&] { return editor.agent()->liveClient(); }, 3000)) return fail(20, "the app does not see Claude Code connected");
        auto typeText = [&](const QString& t) { for (const QChar ch : t) QTest::keyClick(window, ch.toLatin1()); QTest::qWait(50); };
        click(find(window, "workspacePlan"));
        click(find(window, "planPage_concept"));
        auto askLink = find(window, "ask_concept_logline");
        if (!askLink) return fail(21, "no ask link on the logline");
        click(askLink);
        auto popup = window->findChild<QObject*>("askPopup");
        if (!popup || !waitFor([&] { return popup->property("opened").toBool(); }, 1500)) return fail(21, "ask did not open the request");
        if (find(window, "askLabel")->property("text") != "Concept · Logline") return fail(21, "the request is not labelled with what it is about");
        // Suggestions for the field: clicking one fills the box and sends nothing.
        auto suggestion = find(window, "askSuggestion0");
        if (!suggestion || suggestion->property("modelData") != "Make it more visual: what we would actually see.")
            return fail(21, "the filled logline is not offered its suggestions");
        click(suggestion);
        if (find(window, "askText")->property("text") != suggestion->property("modelData") || !editor.requests()->list().isEmpty())
            return fail(21, "clicking a suggestion did not just fill the box");
        capture("ask-suggestions");
        // Picture prompts are steered towards references the video model can hold steady.
        auto suggest = [&](const char* kind, const char* field, bool empty) {
            QVariant out;
            QMetaObject::invokeMethod(popup, "suggestionsFor", Q_RETURN_ARG(QVariant, out), Q_ARG(QVariant, kind), Q_ARG(QVariant, field), Q_ARG(QVariant, empty));
            return out.toStringList();
        };
        const auto faceEmpty = suggest("character", "prompt", true), face = suggest("character", "prompt", false);
        const auto place = suggest("location", "prompt", false), outfit = suggest("look", "prompt", false), wholePlan = suggest("", "", true);
        const auto framing = suggest("view", "framing", false);
        if (faceEmpty.size() != 1 || !faceEmpty[0].contains("no framing, pose or backdrop") || face.size() != 4 || !face.join(" ").contains("hair while it stays the same") ||
            !face.join(" ").contains("belong to the views") || !place.join(" ").contains("Remove people") || !outfit.join(" ").contains("what this look changes") ||
            !framing.join(" ").contains("turnaround sheet") || wholePlan.size() != 4)
            return fail(21, "picture prompt suggestions: " + face.join(" | ") + " / " + place.join(" | "));
        for (const auto* k : {"character", "look", "location", "view", "arc", "concept", ""})
            for (const auto* f : {"", "prompt", "identity", "description", "lighting", "style", "rules", "brief", "logline", "happens", "beat", "name", "framing"})
                for (bool e : {true, false})
                    if (suggest(k, f, e).size() > 4) return fail(21, QString("more than four suggestions for %1.%2").arg(k, f));
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        typeText("Make it punchier");
        capture("ask-popup");
        QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
        QTest::qWait(150);
        const auto threads = editor.requests()->list();
        if (threads.size() != 1 || threads[0].toMap()["turn"] != "agent") return fail(21, "the request was not saved");
        const int thread = threads[0].toMap()["id"].toInt();
        auto event = claude.note("notifications/claude/channel");
        const auto params = event["params"].toObject();
        const auto meta = params["meta"].toObject();
        if (!params["content"].toString().contains("Make it punchier") || meta["request_id"] != QString::number(thread) ||
            meta["about_kind"] != "concept" || meta["field"] != "logline")
            return fail(22, "Claude Code did not get the request as a channel event: " + QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
        if (!agent.note("notifications/claude/channel", 500).isEmpty()) return fail(22, "a client that is not Claude Code got a channel event");
        if (!editor.requests()->list()[0].toMap()["sent"].toBool() || !find(window, "thread" + QString::number(thread)))
            return fail(22, "the Requests page does not show the request as sent live");
        // The agent reads it, with the field's current value, and answers.
        const auto got = claude.data("get_requests")["requests"].toArray();
        if (got.size() != 1 || got[0].toObject()["currentValue"] != "The user's logline" || got[0].toObject()["waitingFor"] != "agent")
            return fail(23, "get_requests: " + QString::fromUtf8(QJsonDocument(got).toJson(QJsonDocument::Compact)));
        if (!editor.requests()->list()[0].toMap()["seen"].toBool()) return fail(23, "reading the request did not mark it seen");
        if (claude.call("reply_request", {{"request_id", thread}, {"text", "It is locked; here is a punchier one: Two strangers, one frequency."}})["isError"].toBool())
            return fail(23, "reply_request failed");
        if (editor.requests()->waitingOnUser() != 1 || !find(window, "agentButton")->property("text").toString().endsWith("· 1"))
            return fail(23, "the agent's answer is not waiting for the user");
        auto chip = find(window, "asked_concept_logline");
        click(find(window, "planPage_concept"));
        chip = find(window, "asked_concept_logline");
        if (!chip) return fail(24, "the logline does not show its open request");
        click(chip);
        if (find(window, "planWorkspace")->property("page") != "requests") return fail(24, "the request chip did not open Requests");
        capture("requests-page");
        // The user answers back in the thread; that reaches Claude Code too.
        auto card = find(window, "thread" + QString::number(thread));
        if (!card || !findItem(card, "replyText")) return fail(25, "no reply box on the request");
        click(findItem(card, "replyText"));
        typeText("Thanks, use it");
        QTest::keyClick(window, Qt::Key_Return);
        QTest::qWait(150);
        event = claude.note("notifications/claude/channel");
        if (!event["params"].toObject()["content"].toString().contains("Thanks, use it") || editor.requests()->waitingOnAgent() != 1)
            return fail(25, "the user's reply did not go back to the agent");
        claude.call("reply_request", {{"request_id", thread}, {"text", "Done."}, {"done", true}});
        if (editor.requests()->list()[0].toMap()["turn"] != "done") return fail(25, "done did not close the request");
        // The agent asks a question with answers to click.
        const int booth = plan->locations()[0].toMap()["id"].toInt();
        const auto asked = claude.data("ask_user", {{"question", "Booth or rooftop for the bridge?"}, {"about", QJsonObject{{"kind", "location"}, {"id", booth}}},
                                                   {"options", QJsonArray{"Booth", "Rooftop"}}});
        const int question = asked["request"].toInt();
        if (!question || editor.requests()->waitingOnUser() != 1) return fail(26, "ask_user did not reach the app");
        QTest::qWait(150);
        auto qcard = find(window, "thread" + QString::number(question));
        auto option = qcard ? findItem(qcard, "option1") : nullptr;
        if (!option) return fail(26, "the question's answers are not shown");
        click(option);
        event = claude.note("notifications/claude/channel");
        if (!event["params"].toObject()["content"].toString().contains("answered your question") ||
            !event["params"].toObject()["content"].toString().contains("Rooftop"))
            return fail(26, "the answer did not reach the agent: " + QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact)));
        if (claude.data("get_project")["requests"].toObject()["waitingForYou"].toInt() != 1) return fail(26, "get_project does not count the request waiting for the agent");
        // The conversation is kept in the project.
        if (!editor.openProject(QUrl::fromLocalFile(QFileInfo(argv[1]).absoluteFilePath())) || editor.requests()->list().size() != 2)
            return fail(27, "the requests did not survive reopening the project");
        claude.process.closeWriteChannel();
        claude.process.waitForFinished(3000);
        click(find(window, "workspaceEdit"));
    }

    // 8. The app restarts (here: stops and starts listening again); the next call still works.
    editor.agent()->stop();
    QTest::qWait(200);
    if (!editor.agent()->start()) return fail(16, "the bridge did not start again");
    if (!agent.data("get_project")["open"].toBool()) return fail(16, "the first call after the app came back failed");

    // 9. When the agent quits, the app notices.
    agent.process.closeWriteChannel();
    agent.process.waitForFinished(3000);
    if (!waitFor([&] { return editor.agent()->clients().isEmpty(); }, 3000)) return fail(16, "the app still lists a disconnected agent");
    qInfo() << "PASS: MCP without the app, handshake, tools, prompt, reading the project, protected and locked fields,"
            << "agent-only provenance, no locking, failing pass leaves nothing, $refs, banner Undo, redo and ⌘Z,"
            << "get_plan, view_reference, Agent sheet, requests (ask from a field, live channel event to Claude Code only,"
            << "get_requests with the current value, seen, reply, the chip and Requests page, the user's reply, done, ask_user with"
            << "answers to click, reopen), disconnect.";
    return 0;
}
