// Workflow adapters and reference generation, against tests/comfy_stub.py, with real mouse and
// keyboard input: the library (import, Export (API) check, guessed adapters, remapping inputs in
// the Workflows sheet), generating a character's reference from its picture prompt and from
// approved pictures, the exact graph ComfyUI receives, failures with Try again and Dismiss,
// cancelling, resuming after reopening the project, undo, a location in the video's frame,
// and removing a workflow.
// usage: ChorusCutGenerateCheck PROJECT_COPY STUB_SCRIPT FIXTURES_DIR
#include "editor.h"
#include "library.h"
#include "waveform.h"
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QSqlQuery>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QDebug>
#include <functional>

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
static bool waitFor(const std::function<bool()>& ready, int timeout = 10000) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < timeout) { if (ready()) return true; QTest::qWait(20); }
    return ready();
}
static int fail(int code, const QString& why) { qCritical().noquote() << "FAIL" << code << why; return code; }
static QString json(const QVariant& v) { return QString::fromUtf8(QJsonDocument::fromVariant(v).toJson(QJsonDocument::Compact)); }

int main(int argc, char** argv) {
    if (argc != 4) { qCritical() << "usage: ChorusCutGenerateCheck PROJECT_COPY STUB_SCRIPT FIXTURES_DIR"; return 1; }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    qputenv("SDL_AUDIODRIVER", "disk"); qputenv("SDL_AUDIO_DRIVER", "disk");
    QTemporaryDir scratch;
    qputenv("SDL_AUDIO_DISK_OUTPUT_FILE", QFile::encodeName(scratch.filePath("capture.raw")));
    // The test's own workflow library, never the user's.
    qputenv("CHORUS_WORKFLOWS_DIR", QFile::encodeName(scratch.filePath("Workflows")));
    QGuiApplication app(argc, argv);
    app.setOrganizationName("Chorus Cut Tests");
    app.setApplicationName("Generate Check");
    { QSettings s; s.clear(); }
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    Mlt::Factory::init();
    const QDir fixtures(QFileInfo(argv[3]).absoluteFilePath());
    const QUrl project = QUrl::fromLocalFile(QFileInfo(argv[1]).absoluteFilePath());
    const QDir projectDir(project.toLocalFile());

    const int port = 19000 + int(QCoreApplication::applicationPid() % 1000);
    const QString stubUrl = QString("http://127.0.0.1:%1").arg(port);
    QProcess stub;
    stub.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    stub.start("python3", {argv[2], QString::number(port), fixtures.filePath("cherry_static_alignment.json")});
    QNetworkAccessManager net;
    auto call = [&](const QString& path, const QJsonObject& body = {}, bool post = false) {
        QNetworkRequest r(QUrl(stubUrl + path));
        r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        auto reply = post ? net.post(r, QJsonDocument(body).toJson()) : net.get(r);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        const auto out = QJsonDocument::fromJson(reply->readAll()).object();
        const bool ok = reply->error() == QNetworkReply::NoError;
        reply->deleteLater();
        return ok ? out : QJsonObject{{"unreachable", true}};
    };
    if (!waitFor([&] { return !call("/system_stats").contains("unreachable"); })) return fail(1, "stub did not start");
    auto stubLog = [&] { return call("/_stub/log"); };
    auto mode = [&](const QString& m) { call("/_stub/mode", {{"mode", m}}, true); };
    auto lastGraph = [&] { const auto p = stubLog()["prompts"].toArray(); return p.isEmpty() ? QJsonObject{} : p.last().toObject(); };
    auto input = [](const QJsonObject& graph, const char* node, const char* name) { return QJsonValue(graph.value(node).toObject().value("inputs").toObject().value(name)); };

    Editor editor;
    if (!editor.openProject(project)) return fail(2, editor.status());
    editor.comfy()->configure(stubUrl, {}, true);
    auto flows = editor.workflows();
    auto gen = editor.generator();
    auto plan = editor.plan();

    // ---- A. The library: import, the Export (API) check, guessed adapters
    auto imported = flows->importFile(QUrl::fromLocalFile(fixtures.filePath("workflow_saved.json")));
    if (imported["ok"].toBool() || !imported["error"].toString().contains("Export (API)")) return fail(10, "a saved workflow was not refused: " + json(imported));
    imported = flows->importFile(QUrl::fromLocalFile(fixtures.filePath("workflow_text_api.json")));
    if (!imported["ok"].toBool()) return fail(10, imported["error"].toString());
    const QString textId = imported["id"].toString();
    imported = flows->importFile(QUrl::fromLocalFile(fixtures.filePath("workflow_edit_api.json")));
    if (!imported["ok"].toBool()) return fail(10, imported["error"].toString());
    const QString editId = imported["id"].toString();
    auto slotsOf = [&](const QString& id) {
        QStringList out;
        for (const auto& v : flows->get(id)["inputs"].toList()) {
            const auto m = v.toMap();
            if (!m["slot"].toString().isEmpty())
                out << m["node"].toString() + "/" + m["input"].toString() + "=" + m["slot"].toString() + (m["slot"] == "picture" ? m["index"].toString() : "");
        }
        return out.join(" ");
    };
    if (slotsOf(textId) != "3/seed=seed 5/height=height 5/width=width 6/text=prompt 7/text=negative 9/filename_prefix=prefix")
        return fail(11, "text workflow adapter: " + slotsOf(textId));
    if (slotsOf(editId) != "10/image=picture0 11/image=picture1 20/prompt=prompt 30/height=height 30/width=width 40/seed=seed 60/filename_prefix=prefix")
        return fail(11, "edit workflow adapter: " + slotsOf(editId));
    if (flows->get(textId)["use"] != "ref-text" || flows->get(editId)["use"] != "ref-edit" || flows->forUse("ref-text") != textId ||
        flows->forUse("ref-edit") != editId || !flows->get(editId)["ready"].toBool() || flows->pictureCount(editId) != 2)
        return fail(12, "uses, defaults or readiness: " + json(flows->list()));
    if (flows->get(editId)["outputs"].toList() != QVariantList{"60 (SaveImage)"}) return fail(12, "the edit workflow should save from node 60 only");
    if (!QFileInfo::exists(scratch.filePath("Workflows/" + textId + "/workflow.json"))) return fail(12, "the workflow was not copied into the library");

    // An agent cannot pass off a picture as generated.
    const int mara = plan->run({{"op", "add"}, {"kind", "character"}, {"fields", QVariantMap{{"name", "Mara"}, {"role", "singer"}}}})["id"].toInt();
    QImage sample(64, 64, QImage::Format_RGB32);
    sample.fill(QColor("#a07050"));
    const QString samplePath = scratch.filePath("mara-face.png");
    sample.save(samplePath);
    auto r = plan->run({{"op", "batch"}, {"by", "agent"}, {"commands", QVariantList{QVariantMap{{"op", "ref.add"}, {"owner", "character"}, {"ownerId", mara},
                                                                                              {"path", samplePath}, {"source", "generated"}}}}});
    if (!r["ok"].toBool() || plan->referencesFor("character", mara).value(0).toMap()["source"] != "agent") return fail(13, "an agent's picture was marked generated");
    editor.undo();

    // ---- B. The Workflows sheet, with real input
    qmlRegisterType<Monitor>("Chorus", 1, 0, "VideoMonitor");
    qmlRegisterType<WaveformView>("Chorus", 1, 0, "WaveformView");
    QQmlApplicationEngine engine;
    ProjectLibrary library(&editor);
    engine.rootContext()->setContextProperty("editor", &editor);
    engine.rootContext()->setContextProperty("library", &library);
    engine.load(QUrl("qrc:/src/Main.qml"));
    if (engine.rootObjects().isEmpty()) return fail(20, "QML did not load");
    auto window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    window->resize(1280, 820);
    if (!QTest::qWaitForWindowExposed(window)) return fail(20, "window not exposed");
    QTest::qWait(300);
    const QString shots = qEnvironmentVariable("CHORUS_SHOTS");
    auto capture = [&](const QString& name) { if (!shots.isEmpty()) window->grabWindow().save(shots + "/" + name + ".png"); };
    auto center = [&](QQuickItem* item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); };
    auto scrollIntoView = [&](QQuickItem* item) {
        QQuickItem* view = item->parentItem();
        while (view && !view->inherits("QQuickFlickable")) view = view->parentItem();
        if (!view) return;
        for (int k = 0; k < 60; ++k) {
            const QRectF rect = item->mapRectToItem(view, QRectF(0, 0, item->width(), item->height()));
            if (rect.top() >= 0 && rect.bottom() <= view->height()) break;
            view->setProperty("contentY", qMax(0.0, view->property("contentY").toDouble() + rect.top() - 40));
            QTest::qWait(30);
        }
    };
    auto click = [&](QQuickItem* item) {
        if (!item) { qCritical() << "nothing to click"; return; }
        scrollIntoView(item);
        QTest::mouseClick(window, Qt::LeftButton, {}, center(item));
        QTest::qWait(150);
    };
    auto pause = [&] { QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 80); };
    auto typeText = [&](const QString& text) { for (const QChar ch : text) QTest::keyClick(window, ch.toLatin1()); QTest::qWait(50); };
    // A combo is opened with a click and chosen with the keyboard: Home, then steps down.
    auto choose = [&](QQuickItem* combo, int index) {
        if (!combo) { qCritical() << "combo not on screen"; return; }
        // Choosing can remove the row (an input that keeps its value is no longer listed).
        QPointer<QObject> popup = combo->property("popup").value<QObject*>();
        click(combo);
        waitFor([&] { return popup && popup->property("opened").toBool(); }, 1500);
        for (int k = 0; k < 20; ++k) QTest::keyClick(window, Qt::Key_Up);
        for (int k = 0; k < index; ++k) QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Return);
        waitFor([&] { return !popup || !popup->property("visible").toBool(); }, 1500);
        QTest::qWait(150);
    };
    // A menu item is clicked where it is drawn.
    auto clickMenuItem = [&](QQuickItem* owner, const QString& name) -> QQuickItem* {
        QQuickItem* item = nullptr;
        waitFor([&] { for (auto i : owner->findChildren<QQuickItem*>(name)) if (i->isVisible() && i->height() > 0) item = i; return item != nullptr; }, 1500);
        if (item && item->property("enabled").toBool()) { QTest::mouseClick(window, Qt::LeftButton, {}, center(item)); QTest::qWait(150); }
        return item;
    };

    click(find(window, "comfyButton"));
    click(find(window, "comfyWorkflows"));
    auto sheet = window->findChild<QObject*>("workflowsSheet");
    if (!sheet || !waitFor([&] { return sheet->property("opened").toBool(); }, 2000)) return fail(21, "the Workflows button did not open the sheet");
    if (!find(window, "workflowEntry0") || !find(window, "workflowEntry1")) return fail(21, "the sheet does not list both workflows");
    // Entries are in folder order: the edit workflow, then the text one.
    click(find(window, "workflowEntry1"));
    if (sheet->property("selected") != textId) return fail(21, "clicking the entry did not select the text workflow");
    if (!find(window, "workflowSlot_7_text")) return fail(22, "the negative prompt input is not listed");
    capture("workflows-sheet");
    choose(find(window, "workflowSlot_7_text"), 0);  // Keep its value
    if (slotsOf(textId).contains("7/text")) return fail(22, "choosing Keep its value did not unmap the negative prompt: " + slotsOf(textId));
    if (find(window, "workflowSlot_7_text")) return fail(22, "an unmapped input stayed listed without Show every input");
    click(find(window, "workflowAllInputs"));
    if (!find(window, "workflowSlot_4_ckpt_name")) return fail(22, "Show every input did not list the checkpoint name");
    choose(find(window, "workflowSlot_7_text"), 2);  // Negative prompt
    if (!slotsOf(textId).contains("7/text=negative")) return fail(22, "the negative prompt was not mapped back: " + slotsOf(textId));
    // A saved (not exported) workflow is refused with what to do instead.
    QMetaObject::invokeMethod(sheet, "importUrl", Q_ARG(QVariant, QUrl::fromLocalFile(fixtures.filePath("workflow_saved.json"))));
    QTest::qWait(100);
    auto note = find(window, "workflowsNote");
    if (!note || !note->property("text").toString().contains("Export (API)")) return fail(23, "the sheet did not explain the saved workflow");
    click(find(window, "workflowsDone"));
    if (!waitFor([&] { return !sheet->property("visible").toBool(); }, 1500)) return fail(23, "Done did not close the sheet");

    // ---- C. Generate a character's reference from its picture prompt
    click(find(window, "workspacePlan"));
    click(find(window, "planPage_cast"));
    auto promptField = find(window, "characterPrompt0");
    if (!promptField) return fail(30, "no picture prompt on the character's card");
    click(promptField);
    const QString portrait = "Studio portrait of a woman with cropped black hair";
    typeText(portrait);
    QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
    QTest::qWait(150);
    if (plan->item("character", mara)["fields"].toMap()["prompt"] != portrait) return fail(30, "the typed picture prompt was not saved");
    auto refs = find(window, "characterRefs0");
    auto genTile = refs ? findItem(refs, "genRef_character") : nullptr;
    if (!genTile) return fail(31, "no Generate tile on the character's pictures");
    mode("hold");
    pause();
    click(genTile);
    auto menu = genTile->findChild<QObject*>("genMenu");
    if (!menu || !waitFor([&] { return menu->property("opened").toBool(); }, 1500)) return fail(31, "Generate did not open its menu");
    auto fromPictures = clickMenuItem(genTile, "genFromPictures");
    if (!fromPictures || fromPictures->property("enabled").toBool()) return fail(31, "From approved pictures was offered without any approved pictures");
    if (!clickMenuItem(genTile, "genFromPrompt")) return fail(31, "no From the picture prompt item");
    if (!waitFor([&] { return gen->jobs().size() == 1 && gen->jobs()[0].toMap()["state"] == "running"; }))
        return fail(32, "the job did not start running: " + json(gen->jobs()));
    if (!find(window, "genJob_character_0")) return fail(32, "the running job has no tile on the strip");
    capture("generate-running");
    auto graph = lastGraph();
    const double seed = input(graph, "3", "seed").toDouble();
    if (input(graph, "6", "text") != portrait || input(graph, "7", "text") != "blurry, text" || seed == 42 || seed < 0 ||
        input(graph, "5", "width").toInt() != 768 || input(graph, "5", "height").toInt() != 1024 ||
        input(graph, "9", "filename_prefix") != "ChorusCut/reference" || input(graph, "4", "ckpt_name") != "model.safetensors")
        return fail(33, "the queued graph is wrong: " + QString::fromUtf8(QJsonDocument(graph).toJson(QJsonDocument::Compact)));
    call("/_stub/release", {}, true);
    if (!waitFor([&] { return plan->referencesFor("character", mara).size() == 1; })) return fail(34, "the picture did not arrive: " + json(gen->jobs()));
    auto ref = plan->referencesFor("character", mara)[0].toMap();
    if (ref["source"] != "generated" || ref["approved"].toBool() || ref["generation"].toMap()["seed"].toDouble() != seed ||
        ref["generation"].toMap()["prompt"] != portrait || ref["generation"].toMap()["workflow"] != textId || ref["missing"].toBool())
        return fail(34, "the generated reference: " + json(ref));
    if (editor.undoText() != "Add generated reference") return fail(34, "not one undo step: " + editor.undoText());
    if (!gen->jobs().isEmpty() || !QDir(projectDir.filePath("Jobs")).entryList({"*.png"}).isEmpty()) return fail(34, "the job or its download was left behind");
    const auto views = stubLog()["views"].toArray();
    if (views.isEmpty() || views.last().toObject().value("subfolder") != "ChorusCut" || views.last().toObject().value("type") != "output")
        return fail(34, "the picture was not fetched from the saved output");
    {
        QSqlQuery q(editor.current()->db);
        if (!q.exec("SELECT state, detail FROM jobs WHERE kind='reference'") || !q.next() || q.value(0).toString() != "done" ||
            QJsonDocument::fromJson(q.value(1).toString().toUtf8()).object().value("graph").toObject().value("6").toObject().value("inputs").toObject().value("text") != portrait)
            return fail(35, "the job record does not keep the graph it queued");
    }

    // ---- D. From approved pictures: approve it with its menu, add a second, generate an edit
    auto thumb = findItem(refs, "ref_character_0");
    click(thumb);
    QQuickItem* approve = nullptr;
    waitFor([&] { for (auto i : thumb->findChildren<QQuickItem*>("refApproved")) if (i->isVisible()) approve = i; return approve != nullptr; }, 1500);
    if (!approve) return fail(40, "the picture's menu did not open");
    click(approve);
    if (!plan->referencesFor("character", mara)[0].toMap()["approved"].toBool()) return fail(40, "the menu did not approve the picture");
    if (!plan->run({{"op", "ref.add"}, {"owner", "character"}, {"ownerId", mara}, {"path", samplePath}})["ok"].toBool()) return fail(40, plan->message());
    mode("finish");
    pause();
    click(genTile);
    if (!clickMenuItem(genTile, "genFromPictures") || !waitFor([&] { return plan->referencesFor("character", mara).size() == 3; }))
        return fail(41, "the edit did not arrive: " + json(gen->jobs()) + " " + json(gen->options("character", mara)));
    graph = lastGraph();
    const auto uploads = stubLog()["uploads"].toArray();
    if (uploads.size() < 2 || input(graph, "10", "image") != uploads[uploads.size() - 2].toObject().value("name") ||
        input(graph, "11", "image") != uploads.last().toObject().value("name") || input(graph, "20", "prompt") != portrait ||
        input(graph, "30", "width").toInt() != 768 || input(graph, "60", "filename_prefix") != "ChorusCut/reference")
        return fail(41, "the edit graph is wrong: " + QString::fromUtf8(QJsonDocument(graph).toJson(QJsonDocument::Compact)));
    if (plan->referencesFor("character", mara)[2].toMap()["generation"].toMap()["use"] != "ref-edit") return fail(41, "the edit was not recorded as one");

    // ---- E. A failure: Try again, and Dismiss
    mode("error");
    pause();
    click(genTile);
    clickMenuItem(genTile, "genFromPrompt");
    if (!waitFor([&] { return !gen->jobs().isEmpty() && gen->jobs()[0].toMap()["state"] == "failed"; })) return fail(50, "the failure was not shown");
    if (!gen->jobs()[0].toMap()["text"].toString().contains("Stub failure")) return fail(50, "the failure lost ComfyUI's reason: " + json(gen->jobs()));
    capture("generate-failed");
    auto failedTile = find(window, "genJob_character_0");
    mode("finish");
    pause();
    click(failedTile);
    if (!clickMenuItem(failedTile, "genRetry") || !waitFor([&] { return plan->referencesFor("character", mara).size() == 4 && gen->jobs().isEmpty(); }))
        return fail(51, "Try again did not generate the picture: " + json(gen->jobs()));
    mode("error");
    pause();
    click(genTile);
    clickMenuItem(genTile, "genFromPrompt");
    if (!waitFor([&] { return !gen->jobs().isEmpty() && gen->jobs()[0].toMap()["state"] == "failed"; })) return fail(52, "the second failure was not shown");
    failedTile = find(window, "genJob_character_0");
    pause();
    click(failedTile);
    if (!clickMenuItem(failedTile, "genDismiss") || !waitFor([&] { return gen->jobs().isEmpty() && !find(window, "genJob_character_0"); }))
        return fail(52, "Dismiss did not clear the failed job");

    // ---- F. Cancel a job waiting in the queue
    mode("wait");
    pause();
    click(genTile);
    clickMenuItem(genTile, "genFromPrompt");
    if (!waitFor([&] { return !gen->jobs().isEmpty() && gen->jobs()[0].toMap()["text"].toString().startsWith("Queued"); })) return fail(60, "the waiting job is not shown queued: " + json(gen->jobs()));
    const auto waitingId = stubLog()["jobs"].toObject().keys();
    auto waitingTile = find(window, "genJob_character_0");
    pause();
    click(waitingTile);
    if (!clickMenuItem(waitingTile, "genCancel") || !waitFor([&] { return gen->jobs().isEmpty(); })) return fail(61, "Cancel did not remove the job");
    if (!waitFor([&] { return !stubLog()["deleted"].toArray().isEmpty(); }, 3000)) return fail(61, "Cancel did not take the job off ComfyUI's queue");
    {
        QSqlQuery q(editor.current()->db);
        if (!q.exec("SELECT count(*) FROM jobs WHERE state='cancelled'") || !q.next() || q.value(0).toInt() != 1) return fail(61, "the cancel was not recorded");
    }

    // ---- G. A job survives closing and reopening the project
    mode("hold");
    if (!gen->generate("character", mara, "ref-text").isEmpty() || !waitFor([&] { return !gen->jobs().isEmpty() && gen->jobs()[0].toMap()["state"] == "running"; }))
        return fail(70, "could not start the job to resume");
    if (!editor.openProject(project)) return fail(70, editor.status());
    if (gen->jobs().size() != 1) return fail(70, "the job was not picked up again after reopening: " + json(gen->jobs()));
    call("/_stub/release", {}, true);
    if (!waitFor([&] { return plan->referencesFor("character", mara).size() == 5 && gen->jobs().isEmpty(); })) return fail(71, "the resumed job did not finish: " + json(gen->jobs()));

    // ---- H. Undo takes the generated picture off again
    click(find(window, "planPage_cast"));
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(150);
    if (plan->referencesFor("character", mara).size() != 4) return fail(80, "⌘Z did not remove the generated picture");

    // ---- I. A location is generated in the video's frame
    plan->run({{"op", "set"}, {"kind", "concept"}, {"fields", QVariantMap{{"aspect", "16:9"}}}});
    const int booth = plan->run({{"op", "add"}, {"kind", "location"}, {"fields", QVariantMap{{"name", "Booth"}, {"prompt", "An empty radio booth at night"}}}})["id"].toInt();
    mode("finish");
    if (!gen->generate("location", booth, "ref-text").isEmpty() || !waitFor([&] { return plan->referencesFor("location", booth).size() == 1; }))
        return fail(90, "the location's picture did not arrive");
    graph = lastGraph();
    if (input(graph, "5", "width").toInt() != 1024 || input(graph, "5", "height").toInt() != 576)
        return fail(90, QString("the location was not 16:9: %1×%2").arg(input(graph, "5", "width").toInt()).arg(input(graph, "5", "height").toInt()));
    if (plan->referencesFor("location", booth)[0].toMap()["role"] != "setting") return fail(90, "the location's picture is not a setting");

    // ---- J. Removing a workflow takes its kind of picture away
    click(find(window, "comfyButton"));
    click(find(window, "comfyWorkflows"));
    waitFor([&] { return sheet->property("opened").toBool(); }, 2000);
    click(find(window, "workflowEntry1"));
    click(find(window, "workflowRemove"));
    if (flows->list().size() != 2) return fail(95, "one click removed the workflow without asking");
    click(find(window, "workflowRemove"));
    if (flows->list().size() != 1 || !flows->forUse("ref-text").isEmpty()) return fail(95, "confirming did not remove the text workflow");
    if (QFileInfo::exists(scratch.filePath("Workflows/" + textId))) return fail(95, "the workflow's folder was left behind");
    click(find(window, "workflowsDone"));
    const auto options = gen->options("character", mara);
    if (options["text"].toMap()["ready"].toBool() || !options["text"].toMap()["why"].toString().startsWith("Set up a workflow"))
        return fail(96, "generating from a prompt is still offered: " + json(options));

    stub.kill();
    stub.waitForFinished();
    qInfo() << "PASS: library (import, saved-workflow refusal, guessed adapters, uses and defaults), Workflows sheet (open, select,"
            << "remap and unmap an input, show every input, refusal note, remove with confirmation), generation (picture prompt,"
            << "Generate menu, the queued graph, arrival unapproved with its workflow, prompt and seed, one undo step, job record),"
            << "from approved pictures (uploads in order), failure with Try again and Dismiss, cancel from the queue, resume after"
            << "reopening, ⌘Z, a location in 16:9, agents cannot mark pictures generated.";
    return 0;
}
