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

// --live URL PROJECT_COPY TEXT_WORKFLOW EDIT_WORKFLOW OUT_DIR: the real thing on a ComfyUI with
// models: a character's Face from its subject, chosen; a Three-quarter view edited from it; a
// composed sheet of both. Pictures are copied to OUT_DIR. Uses its own workflow library.
static int live(int argc, char** argv) {
    if (argc != 7) { qCritical() << "usage: ChorusCutGenerateCheck --live URL PROJECT_COPY TEXT_WORKFLOW EDIT_WORKFLOW OUT_DIR"; return 1; }
    QTemporaryDir scratch;
    qputenv("CHORUS_WORKFLOWS_DIR", QFile::encodeName(scratch.filePath("Workflows")));
    qputenv("SDL_AUDIODRIVER", "disk"); qputenv("SDL_AUDIO_DRIVER", "disk");
    qputenv("SDL_AUDIO_DISK_OUTPUT_FILE", QFile::encodeName(scratch.filePath("capture.raw")));
    QGuiApplication app(argc, argv);
    app.setOrganizationName("Chorus Cut Tests");
    app.setApplicationName("Generate Live");
    Mlt::Factory::init();
    Editor editor;
    if (!editor.openProject(QUrl::fromLocalFile(QFileInfo(argv[3]).absoluteFilePath()))) return fail(2, editor.status());
    editor.comfy()->configure(argv[2], {}, true);
    auto flows = editor.workflows();
    auto gen = editor.generator();
    auto plan = editor.plan();
    const QDir out(QFileInfo(argv[6]).absoluteFilePath());
    QDir().mkpath(out.path());
    QElapsedTimer clock;
    auto importAs = [&](const char* file, const QString& use) {
        auto r = flows->importFile(QUrl::fromLocalFile(QFileInfo(file).absoluteFilePath()), use);
        QStringList mapped;
        for (const auto& v : flows->get(r["id"].toString())["inputs"].toList())
            if (!v.toMap()["slot"].toString().isEmpty()) mapped << v.toMap()["node"].toString() + "/" + v.toMap()["input"].toString() + "=" + v.toMap()["slot"].toString();
        qInfo().noquote() << "imported" << file << r["id"].toString() << "ready" << flows->get(r["id"].toString())["ready"].toBool() << mapped.join(" ")
                          << flows->get(r["id"].toString())["problems"].toStringList().join(" ");
        return r["id"].toString();
    };
    const QString textId = importAs(argv[4], "ref-text"), editId = importAs(argv[5], "ref-edit");
    const QString megapixels = [&] { for (const auto& v : flows->settings(textId)) if (v.toMap()["input"] == "megapixels") return v.toMap()["key"].toString(); return QString(); }();
    const QVariantMap small = megapixels.isEmpty() ? QVariantMap{} : QVariantMap{{megapixels, 4}};
    const int mara = plan->run({{"op", "add"}, {"kind", "character"}, {"fields", QVariantMap{{"name", "Mara"}, {"role", "singer"},
        {"prompt", "A woman in her late twenties, warm brown skin, oval face, dark brown eyes, tight black curls cropped to the jaw, a small gap in her front teeth, a thin silver hoop in her left ear, a charcoal crew-neck sweater."}}}})["id"].toInt();
    auto wait = [&](int count, const char* what) {
        clock.restart();
        while (clock.elapsed() < 600000) {
            QTest::qWait(500);
            if (plan->referencesFor("character", mara).size() >= count && gen->jobs().isEmpty()) {
                qInfo().noquote() << what << "arrived after" << clock.elapsed() / 1000.0 << "s";
                return true;
            }
            for (const auto& j : gen->jobs()) if (j.toMap()["state"] == "failed") { qCritical().noquote() << what << "failed:" << j.toMap()["text"].toString(); return false; }
        }
        qCritical() << what << "timed out" << QJsonDocument::fromVariant(gen->jobs()).toJson(QJsonDocument::Compact);
        return false;
    };
    // 1. Face, from the subject.
    QString why = gen->generateWith("character", mara, "", {{"settings", small}});
    if (!why.isEmpty()) return fail(201, why);
    if (!wait(1, "Face")) return 201;
    const auto face = plan->referencesFor("character", mara)[0].toMap();
    qInfo().noquote() << "Face" << face["width"].toInt() << "x" << face["height"].toInt() << "settings" << QJsonDocument::fromVariant(face["generation"].toMap()["settings"]).toJson(QJsonDocument::Compact);
    plan->run({{"op", "set"}, {"kind", "reference"}, {"id", face["id"]}, {"fields", QVariantMap{{"approved", true}}}});
    // 2. Three-quarter, edited from the chosen Face.
    const int quarter = plan->run({{"op", "add"}, {"kind", "view"}, {"owner", "character"}, {"ownerId", mara},
        {"fields", QVariantMap{{"name", "Three-quarter"}, {"role", "face"}, {"from", "pictures"},
                               {"framing", "The woman from " + gen->pictureLabel(editId, 1) + ", turned 45 degrees to her left, face and shoulders, plain neutral grey backdrop, soft even light. Keep her face, hair, skin and earring exactly the same."}}}})["id"].toInt();
    qInfo().noquote() << "edit workflow names pictures" << gen->pictureLabel(editId, 1) << "guessed inputs" << QJsonDocument::fromVariant(gen->viewOptions(quarter)["inputs"]).toJson(QJsonDocument::Compact);
    const QString editMp = [&] { for (const auto& v : flows->settings(editId)) if (v.toMap()["input"] == "megapixels") return v.toMap()["key"].toString(); return QString(); }();
    why = gen->generateWith("character", mara, "", {{"view", quarter}, {"settings", editMp.isEmpty() ? QVariantMap{} : QVariantMap{{editMp, 4}}}});
    if (!why.isEmpty()) return fail(202, why);
    if (!wait(2, "Three-quarter")) return 202;
    const auto side = plan->viewsFor("character", mara)[1].toMap()["pictures"].toList()[0].toMap();
    plan->run({{"op", "set"}, {"kind", "reference"}, {"id", side["id"]}, {"fields", QVariantMap{{"approved", true}}}});
    // 3. A sheet of both, composed here.
    const int sheet = plan->run({{"op", "add"}, {"kind", "view"}, {"owner", "character"}, {"ownerId", mara},
                                 {"fields", QVariantMap{{"name", "Turnaround sheet"}, {"role", "sheet"}, {"from", "pictures"}}}})["id"].toInt();
    why = gen->composeSheet(sheet, {plan->viewsFor("character", mara)[0].toMap()["id"], quarter});
    if (!why.isEmpty()) return fail(203, why);
    const QDir project(QFileInfo(argv[3]).absoluteFilePath());
    for (const auto& v : plan->viewsFor("character", mara)) {
        const auto chosen = v.toMap()["chosen"].toMap();
        const QString target = out.filePath(v.toMap()["name"].toString().replace(' ', '-').toLower() + "." + QFileInfo(chosen["file"].toString()).suffix());
        QFile::remove(target);
        QFile::copy(project.filePath(chosen["file"].toString()), target);
        qInfo().noquote() << v.toMap()["name"].toString() << chosen["width"].toInt() << "x" << chosen["height"].toInt() << "->" << target;
    }
    qInfo() << "PASS live";
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && QString(argv[1]) == "--live") return live(argc, argv);
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
    const QString samplerSettings = "/cfg=setting %1/denoise=setting %1/sampler_name=setting %1/scheduler=setting %1/seed=seed %1/steps=setting";
    if (slotsOf(textId) != "3" + samplerSettings.arg(3) + " 5/height=height 5/width=width 6/text=prompt 7/text=negative 9/filename_prefix=prefix")
        return fail(11, "text workflow adapter: " + slotsOf(textId));
    if (slotsOf(editId) != "10/image=picture0 11/image=picture1 20/prompt=prompt 30/height=height 30/width=width 40" + samplerSettings.arg(40) + " 60/filename_prefix=prefix")
        return fail(11, "edit workflow adapter: " + slotsOf(editId));
    {
        // The usual sampler inputs are offered as settings, with the workflow's own values.
        QStringList labels;
        for (const auto& v : flows->settings(textId)) labels << v.toMap()["label"].toString() + "=" + v.toMap()["value"].toString() + ":" + v.toMap()["type"].toString();
        if (labels.join(" ") != "CFG=7:number Denoise=1:number Sampler name=euler:text Scheduler=normal:text Steps=20:number")
            return fail(14, "the text workflow's settings: " + labels.join(" "));
    }
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
    // A menu item the menu makes itself (a Repeater's), clicked where it is drawn.
    auto clickPopupItem = [&](const QString& name) -> bool {
        QQuickItem* item = nullptr;
        waitFor([&] { item = find(window, name); return item != nullptr && item->height() > 0; }, 1500);
        if (!item) return false;
        QTest::mouseClick(window, Qt::LeftButton, {}, center(item));
        QTest::qWait(150);
        return true;
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
    // Open the Face view by its tile.
    click(refs ? findItem(refs, "viewTile_character_0") : nullptr);
    auto genTile = refs ? findItem(refs, "genRef_character") : nullptr;
    if (!genTile) return fail(31, "no Generate tile on the character's pictures");
    mode("hold");
    pause();
    click(genTile);
    auto menu = genTile->findChild<QObject*>("genMenu");
    if (!menu || !waitFor([&] { return menu->property("opened").toBool(); }, 1500)) return fail(31, "Generate did not open its menu");
    // The Face view is drawn from the prompt: its framing, then the subject.
    const QString faceFraming = plan->viewsFor("character", mara)[0].toMap()["framing"].toString();
    const QString facePrompt = faceFraming + "\n\n" + portrait;
    auto quick = clickMenuItem(genTile, "genQuick");
    if (!quick || !quick->property("text").toString().startsWith("From the prompt")) return fail(31, "no From the prompt item on the Face view");
    if (!waitFor([&] { return gen->jobs().size() == 1 && gen->jobs()[0].toMap()["state"] == "running"; }))
        return fail(32, "the job did not start running: " + json(gen->jobs()));
    if (!find(window, "genJob_character_0")) return fail(32, "the running job has no tile on the strip");
    capture("generate-running");
    auto graph = lastGraph();
    const double seed = input(graph, "3", "seed").toDouble();
    if (input(graph, "6", "text") != facePrompt || input(graph, "7", "text") != "blurry, text" || seed == 42 || seed < 0 ||
        input(graph, "5", "width").toInt() != 768 || input(graph, "5", "height").toInt() != 1024 ||
        input(graph, "9", "filename_prefix") != "ChorusCut/reference" || input(graph, "4", "ckpt_name") != "model.safetensors")
        return fail(33, "the queued graph is wrong: " + QString::fromUtf8(QJsonDocument(graph).toJson(QJsonDocument::Compact)));
    call("/_stub/release", {}, true);
    if (!waitFor([&] { return plan->referencesFor("character", mara).size() == 1; })) return fail(34, "the picture did not arrive: " + json(gen->jobs()));
    auto ref = plan->referencesFor("character", mara)[0].toMap();
    if (ref["source"] != "generated" || ref["approved"].toBool() || ref["generation"].toMap()["seed"].toDouble() != seed ||
        ref["generation"].toMap()["prompt"] != facePrompt || ref["generation"].toMap()["workflow"] != textId || ref["missing"].toBool() ||
        ref["view"].toInt() != plan->viewsFor("character", mara)[0].toMap()["id"].toInt())
        return fail(34, "the generated reference: " + json(ref));
    if (editor.undoText() != "Add generated reference") return fail(34, "not one undo step: " + editor.undoText());
    if (!gen->jobs().isEmpty() || !QDir(projectDir.filePath("Jobs")).entryList({"*.png"}).isEmpty()) return fail(34, "the job or its download was left behind");
    const auto views = stubLog()["views"].toArray();
    if (views.isEmpty() || views.last().toObject().value("subfolder") != "ChorusCut" || views.last().toObject().value("type") != "output")
        return fail(34, "the picture was not fetched from the saved output");
    {
        QSqlQuery q(editor.current()->db);
        if (!q.exec("SELECT state, detail FROM jobs WHERE kind='reference'") || !q.next() || q.value(0).toString() != "done" ||
            QJsonDocument::fromJson(q.value(1).toString().toUtf8()).object().value("graph").toObject().value("6").toObject().value("inputs").toObject().value("text") != facePrompt)
            return fail(35, "the job record does not keep the graph it queued");
    }

    // ---- D. The picture, large: approve it there, see its seed, step through; then an edit
    auto thumb = findItem(refs, "ref_character_0");
    click(thumb);
    auto viewer = window->findChild<QObject*>("refViewer");
    if (!viewer || !waitFor([&] { return viewer->property("opened").toBool(); }, 1500)) return fail(40, "clicking a picture did not open it large");
    {
        auto picture = find(window, "refViewerPicture");
        if (!picture || picture->width() < 600 || picture->height() < 500)
            return fail(40, QString("the large picture is small: %1×%2").arg(picture ? picture->width() : 0).arg(picture ? picture->height() : 0));
    }
    if (find(window, "refViewerSeed")->property("text").toString() != QString::number(qint64(seed)) ||
        find(window, "refViewerPrompt")->property("text") != facePrompt || find(window, "refViewerState")->property("text") != "To choose")
        return fail(40, "the viewer does not show how the picture was made");
    capture("reference-viewer");
    click(find(window, "refViewerApprove"));
    if (!plan->referencesFor("character", mara)[0].toMap()["approved"].toBool()) return fail(40, "Choose in the viewer did not choose the picture");
    if (find(window, "refViewerState")->property("text") != "Chosen") return fail(40, "the viewer did not show the choice");
    // Click the picture for full size, click again to fit; Esc closes.
    click(find(window, "refViewerPicture"));
    if (!viewer->property("zoomed").toBool()) return fail(40, "clicking the large picture did not show it at full size");
    click(find(window, "refViewerPicture"));
    if (viewer->property("zoomed").toBool()) return fail(40, "clicking again did not fit it");
    QTest::keyClick(window, Qt::Key_Escape);
    if (!waitFor([&] { return !viewer->property("visible").toBool(); }, 1500)) return fail(40, "Esc did not close the viewer");
    // Right-click still has the menu. (Approving rebuilt the thumbnails: find it again.)
    thumb = findItem(refs, "ref_character_0");
    QTest::mouseClick(window, Qt::RightButton, {}, center(thumb));
    QQuickItem* approve = nullptr;
    waitFor([&] { for (auto i : thumb->findChildren<QQuickItem*>("refApproved")) if (i->isVisible()) approve = i; return approve != nullptr; }, 1500);
    if (!approve) return fail(40, "right-clicking a picture did not open its menu");
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::qWait(150);
    // A second picture in the Face view waits as an alternative.
    const int faceChosen = plan->referencesFor("character", mara)[0].toMap()["id"].toInt();
    const int sampleRef = plan->run({{"op", "ref.add"}, {"owner", "character"}, {"ownerId", mara}, {"path", samplePath}})["id"].toInt();
    if (!sampleRef || plan->item("reference", sampleRef)["approved"].toBool()) return fail(40, "the second Face picture should be an alternative: " + plan->message());
    // Add the suggested Three-quarter view: edited from pictures, starting from the chosen Face.
    click(findItem(refs, "addViewTile_character"));
    if (!clickPopupItem("suggestView_character_Three-quarter")) {
        return fail(41, "no Three-quarter in the + view menu");
    }
    if (plan->viewsFor("character", mara).size() != 2) return fail(41, "the Three-quarter suggestion did not add a view");
    const auto quarter = plan->viewsFor("character", mara)[1].toMap();
    const int quarterView = quarter["id"].toInt();
    if (quarter["from"] != "pictures" || gen->viewOptions(quarterView)["inputs"].toList() != QVariantList{faceChosen})
        return fail(41, "the new view does not start from the chosen Face: " + json(gen->viewOptions(quarterView)));
    auto quarterStrip = find(window, "viewRefs_character_1");
    auto quarterTile = quarterStrip ? findItem(quarterStrip, "genRef_character") : nullptr;
    if (!quarterTile) return fail(41, "no Generate tile on the Three-quarter view");
    mode("finish");
    pause();
    click(quarterTile);
    quick = clickMenuItem(quarterTile, "genQuick");
    if (!quick || !waitFor([&] { return plan->viewsFor("character", mara)[1].toMap()["pictures"].toList().size() == 1 && gen->jobs().isEmpty(); }))
        return fail(41, "the edit did not arrive: " + json(gen->jobs()) + " " + json(gen->viewOptions(quarterView)));
    graph = lastGraph();
    const QString quarterPrompt = quarter["framing"].toString() + "\n\n" + portrait;
    // One picture into a workflow with two Load Image inputs: the unused one is taken out, since
    // ComfyUI says the encoder's second image is optional.
    auto uploads = stubLog()["uploads"].toArray();
    const QJsonObject encoder = graph.value("20").toObject().value("inputs").toObject();
    if (input(graph, "10", "image") != uploads.last().toObject().value("name") || graph.contains("11") || encoder.contains("image2") ||
        !encoder.contains("image1") || encoder.value("prompt") != quarterPrompt || input(graph, "60", "filename_prefix") != "ChorusCut/reference")
        return fail(41, "the one-picture edit graph is wrong: " + QString::fromUtf8(QJsonDocument(graph).toJson(QJsonDocument::Compact)));
    auto made = plan->viewsFor("character", mara)[1].toMap()["pictures"].toList()[0].toMap();
    if (made["generation"].toMap()["use"] != "ref-edit" || made["generation"].toMap()["inputs"].toList() != QVariantList{faceChosen} || made["approved"].toBool())
        return fail(41, "the edit was not recorded with its picture: " + json(made));
    // With options: pick the second Face picture too; a third is refused (the workflow takes two).
    pause();
    click(quarterTile);
    if (!clickMenuItem(quarterTile, "genOptions")) return fail(42, "no With options… item on the view");
    auto dialog = window->findChild<QObject*>("generatePopup");
    if (!dialog || !waitFor([&] { return dialog->property("opened").toBool(); }, 1500)) return fail(42, "With options… did not open the Generate dialog");
    if (dialog->property("use") != "ref-edit" || dialog->property("view").toInt() != quarterView || dialog->property("inputs").toList() != QVariantList{faceChosen})
        return fail(42, "the dialog did not open on the view's pictures: " + json(dialog->property("inputs")));
    click(find(window, QString("genPick_%1").arg(sampleRef)));
    if (dialog->property("inputs").toList() != QVariantList{faceChosen, sampleRef} || !find(window, "genPickCount")->property("text").toString().startsWith("2 of 2"))
        return fail(42, "picking the second picture: " + json(dialog->property("inputs")));
    click(find(window, QString("genPick_%1").arg(made["id"].toInt())));
    if (dialog->property("inputs").toList().size() != 2 || !dialog->property("note").toString().contains("takes 2")) return fail(42, "a third picture was not refused");
    capture("generate-made-from");
    click(find(window, "genGo"));
    if (!waitFor([&] { return plan->viewsFor("character", mara)[1].toMap()["pictures"].toList().size() == 2 && gen->jobs().isEmpty(); }))
        return fail(43, "the two-picture edit did not arrive: " + json(gen->jobs()));
    graph = lastGraph();
    uploads = stubLog()["uploads"].toArray();
    if (input(graph, "10", "image") != uploads[uploads.size() - 2].toObject().value("name") || input(graph, "11", "image") != uploads.last().toObject().value("name"))
        return fail(43, "the pictures did not go in the order picked: " + QString::fromUtf8(QJsonDocument(graph).toJson(QJsonDocument::Compact)));
    if (plan->item("view", quarterView)["fields"].toMap()["inputs"].toList() != QVariantList{faceChosen, sampleRef})
        return fail(43, "the picked pictures did not stay with the view");
    if (plan->referencesFor("character", mara).size() != 4) return fail(43, "expected four pictures of Mara by now");
    // Back to the Face view: its strip, and Generate tile, are new ones.
    click(findItem(refs, "viewTile_character_0"));
    genTile = findItem(find(window, "viewRefs_character_0"), "genRef_character");

    // ---- D2. Generate with options: a fixed seed, two pictures, changed settings, a sampler
    //          from ComfyUI's list; then Generate again… from the viewer keeps the seed
    {
        const int before = plan->referencesFor("character", mara).size();
        mode("finish");
        pause();
        click(genTile);
        if (!clickMenuItem(genTile, "genOptions")) return fail(42, "no With options… item");
        if (!dialog || !waitFor([&] { return dialog->property("opened").toBool(); }, 1500)) return fail(42, "With options… did not open the Generate dialog");
        if (dialog->property("use") != "ref-text" || dialog->property("workflow") != textId) return fail(42, "the dialog did not start on the prompt and its workflow");
        click(find(window, "genSeedFixed"));
        auto seedField = find(window, "genSeed");
        click(seedField);
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        typeText("1234");
        click(find(window, "genMore"));
        if (find(window, "genCount")->property("text") != "2") return fail(42, "+ did not make it two pictures");
        auto steps = find(window, "genSetting_3_steps");
        auto stepsField = steps ? findItem(steps, "genSettingField") : nullptr;
        if (!stepsField) return fail(42, "no Steps setting in the dialog");
        click(stepsField);
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        typeText("30");
        if (!findItem(steps, "genSettingReset")) return fail(42, "a changed setting offers no Reset");
        // The sampler is chosen from what ComfyUI lists.
        auto samplerChoice = [&]() -> QQuickItem* { auto row = find(window, "genSetting_3_sampler_name"); return row ? findItem(row, "genSettingChoice") : nullptr; };
        if (!waitFor([&] { return samplerChoice() != nullptr; }, 3000))
            return fail(43, "the sampler is not a list from ComfyUI: " + json(gen->choices()));
        choose(samplerChoice(), 1);  // dpmpp_2m
        capture("generate-options");
        click(find(window, "genGo"));
        if (!waitFor([&] { return plan->referencesFor("character", mara).size() == before + 2 && gen->jobs().isEmpty(); }))
            return fail(44, "the two pictures did not arrive: " + json(gen->jobs()));
        const auto prompts = stubLog()["prompts"].toArray();
        const auto a = prompts[prompts.size() - 2].toObject(), b = prompts.last().toObject();
        QList<double> seeds{input(a, "3", "seed").toDouble(), input(b, "3", "seed").toDouble()};
        std::sort(seeds.begin(), seeds.end());
        if (seeds != QList<double>{1234, 1235} || input(a, "3", "steps").toInt() != 30 || input(b, "3", "sampler_name") != "dpmpp_2m" ||
            input(a, "3", "cfg").toInt() != 7)
            return fail(44, "the graphs did not take the seed, count and settings: " + QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)));
        const auto made = plan->referencesFor("character", mara)[before].toMap()["generation"].toMap();
        QStringList shownSettings;
        for (const auto& v : made["settings"].toList()) shownSettings << v.toMap()["label"].toString() + "=" + v.toMap()["value"].toString();
        if (!shownSettings.contains("Steps=30") || !shownSettings.contains("Sampler name=dpmpp_2m")) return fail(44, "the picture does not keep its settings: " + json(made));
        // The settings stay with the workflow: the quick item uses them too.
        for (const auto& v : flows->settings(textId))
            if (v.toMap()["input"] == "steps" && v.toMap()["value"].toInt() != 30) return fail(45, "the chosen steps were not kept with the workflow");
        mode("finish");
        if (!gen->generate("character", mara, "ref-text").isEmpty() || !waitFor([&] { return gen->jobs().isEmpty() && plan->referencesFor("character", mara).size() == before + 3; }))
            return fail(45, "the quick generate failed");
        if (input(lastGraph(), "3", "steps").toInt() != 30) return fail(45, "the quick generate did not use the kept steps");
        if (!gen->generateWith("character", mara, "ref-text", {{"seed", "-4"}}).contains("whole number")) return fail(45, "a negative seed was not refused");
        if (!gen->generateWith("character", mara, "ref-text", {{"settings", QVariantMap{{"3/steps", "many"}}}}).contains("Steps must be a number"))
            return fail(45, "a setting that is not a number was not refused");
        // Generate again… from the viewer: the same workflow, seed and settings, ready to change.
        // The first picture with seed 1234 or 1235, where it sits in the Face view's strip.
        const auto seededRef = plan->referencesFor("character", mara)[before].toMap();
        const double seedOf = seededRef["generation"].toMap()["seed"].toDouble();
        int seeded = 0;
        const auto facePictures = plan->viewsFor("character", mara)[0].toMap()["pictures"].toList();
        for (int k = 0; k < facePictures.size(); ++k) if (facePictures[k].toMap()["id"] == seededRef["id"]) seeded = k;
        click(findItem(find(window, "viewRefs_character_0"), QString("ref_character_%1").arg(seeded)));
        if (!waitFor([&] { return viewer->property("opened").toBool(); }, 1500)) return fail(46, "the viewer did not open");
        click(find(window, "refViewerNext"));
        click(find(window, "refViewerPrev"));
        if (viewer->property("index").toInt() != seeded) return fail(46, "‹ and › did not step through the pictures");
        QTest::keyClick(window, Qt::Key_Right);
        if (viewer->property("index").toInt() != seeded + 1) return fail(46, "→ did not step to the next picture");
        QTest::keyClick(window, Qt::Key_Left);
        click(find(window, "refViewerAgain"));
        if (!waitFor([&] { return dialog->property("opened").toBool() && !viewer->property("visible").toBool(); }, 1500))
            return fail(46, "Generate again… did not open the dialog");
        if (!dialog->property("fixedSeed").toBool() || dialog->property("seedText").toString() != QString::number(qint64(seedOf)))
            return fail(46, "Generate again… did not keep the seed: " + dialog->property("seedText").toString());
        // Reset puts a setting back to the workflow's own value.
        steps = find(window, "genSetting_3_steps");
        click(findItem(steps, "genSettingReset"));
        if (findItem(steps, "genSettingField")->property("text") != "20") return fail(47, "Reset did not put Steps back to 20");
        QTest::keyClick(window, Qt::Key_Escape);
        if (!waitFor([&] { return !dialog->property("visible").toBool(); }, 1500)) return fail(47, "Esc did not close the dialog");
        // Leave the kept settings as the workflow had them, for the later checks.
        flows->setValues(textId, QJsonObject{{"3/steps", 20}, {"3/sampler_name", "euler"}});
        QFile adapterFile(scratch.filePath("Workflows/" + textId + "/adapter.json"));
        if (!adapterFile.open(QIODevice::ReadOnly) || QJsonDocument::fromJson(adapterFile.readAll()).object().contains("values"))
            return fail(47, "values equal to the workflow's own were kept as changes");
    }

    // ---- E. A failure: Try again, and Dismiss
    mode("error");
    pause();
    click(genTile);
    clickMenuItem(genTile, "genQuick");
    if (!waitFor([&] { return !gen->jobs().isEmpty() && gen->jobs()[0].toMap()["state"] == "failed"; })) return fail(50, "the failure was not shown");
    if (!gen->jobs()[0].toMap()["text"].toString().contains("Stub failure")) return fail(50, "the failure lost ComfyUI's reason: " + json(gen->jobs()));
    capture("generate-failed");
    auto failedTile = find(window, "genJob_character_0");
    const QJsonValue failedSeed = input(lastGraph(), "3", "seed");
    mode("finish");
    pause();
    click(failedTile);
    if (!clickMenuItem(failedTile, "genRetry") || !waitFor([&] { return plan->referencesFor("character", mara).size() == 8 && gen->jobs().isEmpty(); }))
        return fail(51, "Try again did not generate the picture: " + json(gen->jobs()));
    if (input(lastGraph(), "3", "seed") != failedSeed) return fail(51, "Try again did not keep the seed");
    mode("error");
    pause();
    click(genTile);
    clickMenuItem(genTile, "genQuick");
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
    clickMenuItem(genTile, "genQuick");
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
    if (!waitFor([&] { return plan->referencesFor("character", mara).size() == 9 && gen->jobs().isEmpty(); })) return fail(71, "the resumed job did not finish: " + json(gen->jobs()));

    // ---- H. Undo takes the generated picture off again
    click(find(window, "planPage_cast"));
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    QTest::qWait(150);
    if (plan->referencesFor("character", mara).size() != 8) return fail(80, "⌘Z did not remove the generated picture");

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

    // ---- K. Any model, through its workflow: one that draws from text or edits from up to three
    //          pictures (shaped like ComfyUI's own H3 image template)
    {
        auto added = flows->importFile(QUrl::fromLocalFile(fixtures.filePath("workflow_any_api.json")));
        if (!added["ok"].toBool()) return fail(100, added["error"].toString());
        const QString anyId = added["id"].toString();
        // The prompt is the user's text node, not the enhancer's instructions.
        if (slotsOf(anyId) != "10/value=prompt 20/image=picture0 21/image=picture1 22/image=picture2 40/cfg=setting 40/denoise=setting "
                              "40/sampler_name=setting 40/scheduler=setting 40/seed=seed 40/steps=setting 60/filename_prefix=prefix")
            return fail(100, "the H3-shaped workflow's adapter: " + slotsOf(anyId));
        if (!flows->setUse(anyId, "ref-any") || !flows->serves(anyId, "ref-text") || !flows->serves(anyId, "ref-edit"))
            return fail(100, "a workflow could not be used for both");
        // From the prompt: no pictures, so all three Load Image nodes come out and the conditioning
        // loses its optional images.
        mode("finish");
        const int before = plan->referencesFor("character", mara).size();
        QString why = gen->generateWith("character", mara, "ref-text", {{"workflow", anyId}});
        if (!why.isEmpty() || !waitFor([&] { return plan->referencesFor("character", mara).size() == before + 1 && gen->jobs().isEmpty(); }))
            return fail(101, "text to image through it: " + why + " " + json(gen->jobs()));
        graph = lastGraph();
        const auto cond = graph.value("30").toObject().value("inputs").toObject();
        if (graph.contains("20") || graph.contains("21") || graph.contains("22") || cond.contains("images.image_1") || !cond.contains("prompt") ||
            input(graph, "10", "value") != facePrompt || input(graph, "11", "value").toString().indexOf("{input text}") < 0)
            return fail(101, "the picture-free graph: " + QString::fromUtf8(QJsonDocument(graph).toJson(QJsonDocument::Compact)));
        // Its nodes say how it names pictures.
        if (gen->pictureLabel(anyId, 2) != "<Picture 2>") return fail(102, "the picture name was not learned from the node: " + gen->pictureLabel(anyId, 2));
        flows->setConventions(anyId, "@Image{n}", "Write edit instructions.");
        if (gen->pictureLabel(anyId, 3) != "@Image3" || flows->notes(anyId) != "Write edit instructions.") return fail(102, "the user's picture name and notes");
        flows->setConventions(anyId, "", "");
        // From two pictures: the third Load Image comes out.
        why = gen->generateWith("character", mara, "ref-edit", {{"workflow", anyId}, {"view", quarterView}, {"inputs", QVariantList{faceChosen, sampleRef}},
                                                              {"instruction", "Turn <Picture 1> to the left."}});
        if (!why.isEmpty() || !waitFor([&] { return plan->referencesFor("character", mara).size() == before + 2 && gen->jobs().isEmpty(); }))
            return fail(103, "editing through it: " + why + " " + json(gen->jobs()));
        graph = lastGraph();
        const auto cond2 = graph.value("30").toObject().value("inputs").toObject();
        if (!graph.contains("20") || !graph.contains("21") || graph.contains("22") || !cond2.contains("images.image_2") || cond2.contains("images.image_3") ||
            !input(graph, "10", "value").toString().startsWith("Turn <Picture 1> to the left.\n\n"))
            return fail(103, "the two-picture graph: " + QString::fromUtf8(QJsonDocument(graph).toJson(QJsonDocument::Compact)));
        // A picture that sets the size cannot be left out: pruning stops before an output goes.
        QJsonObject sized{{"1", QJsonObject{{"class_type", "LoadImage"}, {"inputs", QJsonObject{{"image", "a.png"}}}}},
                          {"2", QJsonObject{{"class_type", "GetImageSize"}, {"inputs", QJsonObject{{"image", QJsonArray{"1", 0}}}}}},
                          {"3", QJsonObject{{"class_type", "ImageConditioning"}, {"inputs", QJsonObject{{"width", QJsonArray{"2", 0}}, {"images.image_1", QJsonArray{"1", 0}}}}}},
                          {"4", QJsonObject{{"class_type", "SaveImage"}, {"inputs", QJsonObject{{"images", QJsonArray{"3", 0}}}}}}};
        Workflows::OptionalInputs optional{{"ImageConditioning", {"images", "vae"}}};
        QJsonObject copy = sized;
        if (Workflows::prune(copy, {"1"}, QJsonArray{"4"}, optional).isEmpty() || copy != sized) return fail(104, "a required picture was pruned");
        flows->remove(anyId);
    }

    // ---- L. A turnaround sheet composed from views' chosen pictures, on this computer
    {
        // Choose a Three-quarter picture so two views have one.
        const int quarterPick = plan->viewsFor("character", mara)[1].toMap()["pictures"].toList()[0].toMap()["id"].toInt();
        plan->run({{"op", "set"}, {"kind", "reference"}, {"id", quarterPick}, {"fields", QVariantMap{{"approved", true}}}});
        click(find(window, "planPage_cast"));
        click(findItem(find(window, "characterRefs0"), "addViewTile_character"));
        clickPopupItem("suggestView_character_Turnaround sheet");
        const auto views = plan->viewsFor("character", mara);
        if (views.size() != 3 || views[2].toMap()["role"] != "sheet") return fail(110, "the Turnaround sheet suggestion did not add a sheet view");
        const int sheetView = views[2].toMap()["id"].toInt();
        click(find(window, "viewCompose_character_2"));
        if (!find(window, "composeTick_Face") || !find(window, "composeTick_Three-quarter")) return fail(110, "Compose did not offer the views to tick");
        capture("compose-sheet");
        click(find(window, "composeGo"));
        const auto sheet = plan->viewsFor("character", mara)[2].toMap();
        const auto chosen = sheet["chosen"].toMap();
        const auto face = plan->item("reference", plan->viewsFor("character", mara)[0].toMap()["chosen"].toMap()["id"].toInt());
        const auto side = plan->item("reference", quarterPick);
        const int expectWidth = int(std::lround(double(face["width"].toInt()) * chosen["height"].toInt() / face["height"].toInt()))
                              + int(std::lround(double(side["width"].toInt()) * chosen["height"].toInt() / side["height"].toInt()));
        if (chosen.isEmpty() || chosen["source"] != "composed" || chosen["width"].toInt() != expectWidth ||
            plan->item("view", sheetView)["fields"].toMap()["panels"].toList().size() != 2 || editor.undoText() != "Compose Turnaround sheet")
            return fail(111, "the composed sheet: " + json(chosen) + " undo: " + editor.undoText());
        editor.undo();
        if (!plan->viewsFor("character", mara)[2].toMap()["pictures"].toList().isEmpty()) return fail(111, "undo did not take the sheet off");
    }

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
            << "the large viewer (approve, full size, ‹ › and arrow keys, Esc, right-click menu), Generate with options (fixed seed,"
            << "two pictures, settings kept with the workflow, sampler from ComfyUI's list, Reset, refusals), Generate again keeps the seed,"
            << "views (Three-quarter added from its suggestion, edited from the chosen Face with the unused picture input removed, picked"
            << "pictures in order with a third refused, kept with the view), a workflow for both uses (its prompt node, <Picture n> learned"
            << "from the node, no pictures and two pictures with the rest removed, a required picture kept), a composed turnaround sheet,"
            << "from approved pictures (uploads in order), failure with Try again and Dismiss, cancel from the queue, resume after"
            << "reopening, ⌘Z, a location in 16:9, agents cannot mark pictures generated.";
    return 0;
}
