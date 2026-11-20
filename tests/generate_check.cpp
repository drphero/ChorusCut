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
#include <QJSValue>
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
#include <QWheelEvent>
#include <QPointingDevice>
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

// --live-take URL PROJECT_COPY WORKFLOW SHOT OUT_DIR: a real take for a planned shot. Its prompt is
// written from what an agent is given (pictures, cut points, lyric timings) in H3's
// reference-to-video sections; the take is copied to OUT_DIR. Uses its own workflow library.
static int liveTake(int argc, char** argv) {
    if (argc != 7) { qCritical() << "usage: ChorusCutGenerateCheck --live-take URL PROJECT_COPY WORKFLOW SHOT OUT_DIR"; return 1; }
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
    const auto added = flows->importFile(QUrl::fromLocalFile(QFileInfo(argv[4]).absoluteFilePath()));
    const QString workflow = added["id"].toString();
    qInfo().noquote() << "imported" << workflow << flows->get(workflow)["use"].toString() << "ready" << flows->get(workflow)["ready"].toBool()
                      << flows->get(workflow)["problems"].toStringList().join(" ");
    const int shot = QString(argv[5]).toInt();
    gen->fetchChoices(workflow);
    if (!waitFor([&] { return (gen->takeOptions(shot)["frames"].toInt() - 5) % 17 == 0 && gen->pictureLabel(workflow, 1) == "<Picture 1>"; }, 20000))
        return fail(210, "ComfyUI's node details did not arrive: " + json(gen->takeOptions(shot)));
    const auto ctx = editor.requests()->takeContext(shot);
    const auto f = plan->shot(shot)["fields"].toMap();
    // The prompt, in H3's reference-to-video sections.
    QStringList subjects, story;
    for (const auto& p : ctx["pictures"].toArray())
        subjects << p.toObject()["calledInPrompt"].toString() + ": " + p.toObject()["of"].toString() + " (" + p.toObject()["view"].toString() + ")";
    const double cutIn = ctx["cutIn"].toDouble(), cutOut = ctx["cutOut"].toDouble(), total = ctx["seconds"].toDouble();
    auto t = [](double v) { return QString::number(v, 'f', 1); };
    story << QString("0.0-%1 s: handheld close-up of the singer from %2, red light flaring across her face; she draws a breath, eyes on the lens.")
                 .arg(t(cutIn)).arg(subjects.value(0).section(':', 0, 0));
    for (const auto& l : ctx["lyrics"].toArray()) {
        const auto o = l.toObject();
        story << QString("%1-%2 s: she sings \"%3\" straight to camera, her lips forming every word in time with the song, %4.")
                     .arg(t(qMax(0.0, o["start"].toDouble()))).arg(t(qMin(total, o["end"].toDouble()))).arg(o["text"].toString())
                     .arg(f["emotion"].toString().isEmpty() ? "with feeling" : f["emotion"].toString());
    }
    story << QString("%1-%2 s: she holds the moment as the camera keeps drifting by hand, the CRT glow pulsing behind her.").arg(t(cutOut)).arg(t(total));
    const QString prompt =
        "subject_definitions: " + subjects.join("; ") + ".\n" +
        "summary: A handheld close-up from a music video: the singer performs the line to camera on a dark soundstage in front of a wall of old CRT televisions, red light flaring across her face.\n"
        "retention_analysis: Keep the singer's face, hair and stage-leather outfit exactly as in the pictures; keep the stage and its wall of CRT televisions as in the location picture.\n"
        "detailed_description: " + story.join(" ") + "\n"
        "overall_soundscape: A woman's sung lead vocal, close and dry, over a pulsing synth track.\n"
        "non_diegetic_music: The song itself.";
    auto prompts = f["prompts"].toMap();
    prompts["h3-lipsync"] = prompt;
    plan->run({{"op", "set"}, {"kind", "shot"}, {"id", shot}, {"fields", QVariantMap{{"prompts", prompts}}}});
    qInfo().noquote() << "prompt:\n" + prompt;
    qInfo().noquote() << "take options" << json(gen->takeOptions(shot));
    QElapsedTimer clock;
    clock.start();
    const int before = plan->takesFor(shot).size();
    if (const QString why = gen->generateTake(shot); !why.isEmpty()) return fail(211, why);
    QString last;
    while (clock.elapsed() < 45 * 60000) {
        QTest::qWait(1000);
        if (plan->takesFor(shot).size() > before && gen->jobs().isEmpty()) break;
        for (const auto& j : gen->jobs()) {
            const QString text = j.toMap()["state"].toString() + ": " + j.toMap()["text"].toString();
            if (text != last) { qInfo().noquote() << int(clock.elapsed() / 1000) << "s" << text; last = text; }
            if (j.toMap()["state"] == "failed") return fail(212, j.toMap()["text"].toString());
        }
    }
    if (plan->takesFor(shot).size() <= before) return fail(213, "the take did not arrive in time");
    const auto take = plan->takesFor(shot).last().toMap();
    const QDir out(QFileInfo(argv[6]).absoluteFilePath());
    QDir().mkpath(out.path());
    const QString source = QUrl(take["url"].toString()).toLocalFile();
    const QString target = out.filePath(QString("shot%1-take%2.%3").arg(shot).arg(take["number"].toInt()).arg(QFileInfo(source).suffix()));
    QFile::remove(target);
    QFile::copy(source, target);
    qInfo().noquote() << "take arrived after" << clock.elapsed() / 1000.0 << "s:" << take["frames"].toInt() << "frames at" << take["fps"].toDouble()
                      << take["width"].toInt() << "x" << take["height"].toInt() << "covers frames" << take["from"].toInt() << "to" << take["to"].toInt()
                      << "lip-sync" << take["lipSync"].toBool() << "->" << target;
    qInfo() << "PASS live take";
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1 && QString(argv[1]) == "--live") return live(argc, argv);
    if (argc > 1 && QString(argv[1]) == "--live-take") return liveTake(argc, argv);
    if (argc != 4) { qCritical() << "usage: ChorusCutGenerateCheck PROJECT_COPY STUB_SCRIPT FIXTURES_DIR"; return 1; }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    qputenv("SDL_AUDIODRIVER", "disk"); qputenv("SDL_AUDIO_DRIVER", "disk");
    QTemporaryDir scratch;
    qputenv("SDL_AUDIO_DISK_OUTPUT_FILE", QFile::encodeName(scratch.filePath("capture.raw")));
    // The test's own workflow library, never the user's.
    qputenv("CHORUS_WORKFLOWS_DIR", QFile::encodeName(scratch.filePath("Workflows")));
    // Notifications go to a file, never to macOS.
    const QString notifyLog = scratch.filePath("notify.log");
    qputenv("CHORUS_NOTIFY_LOG", QFile::encodeName(notifyLog));
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
    // Picking without generating: a click outside keeps the dialog, Save keeps the picks on the view.
    {
        const int jobsBefore = stubLog()["prompts"].toArray().size();
        click(quarterTile);
        if (!clickMenuItem(quarterTile, "genOptions") || !waitFor([&] { return dialog->property("opened").toBool(); }, 1500))
            return fail(44, "With options… did not open the dialog again");
        click(find(window, QString("genPick_%1").arg(sampleRef)));
        QTest::mouseClick(window, Qt::LeftButton, {}, QPoint(6, 6));
        QTest::qWait(200);
        if (!dialog->property("opened").toBool()) return fail(44, "a click outside the dialog closed it and dropped the picks");
        click(find(window, "genSave"));
        QTest::qWait(200);
        if (dialog->property("opened").toBool() || plan->item("view", quarterView)["fields"].toMap()["inputs"].toList() != QVariantList{faceChosen})
            return fail(44, "Save did not keep the picks on the view: " + json(plan->item("view", quarterView)["fields"]));
        if (stubLog()["prompts"].toArray().size() != jobsBefore || !gen->jobs().isEmpty()) return fail(44, "Save generated something");
        editor.undo();
        if (plan->item("view", quarterView)["fields"].toMap()["inputs"].toList() != QVariantList{faceChosen, sampleRef})
            return fail(44, "undo did not bring the earlier picks back");
        // Ask from the dialog: the picks are saved first, so the agent can see them, and the
        // suggestions are about looking at them.
        click(quarterTile);
        clickMenuItem(quarterTile, "genOptions");
        waitFor([&] { return dialog->property("opened").toBool(); }, 1500);
        click(find(window, QString("genPick_%1").arg(sampleRef)));
        click(find(window, QString("genPick_%1").arg(sampleRef)));
        if (dialog->property("inputs").toList() != QVariantList{faceChosen, sampleRef}) return fail(45, "re-picking changed the order");
        click(find(window, QString("genPick_%1").arg(faceChosen)));
        click(find(window, QString("genPick_%1").arg(faceChosen)));
        click(find(window, "genAsk"));
        auto ask = window->findChild<QObject*>("askPopup");
        if (!ask || !waitFor([&] { return ask->property("opened").toBool(); }, 1500) || dialog->property("opened").toBool())
            return fail(45, "Ask in the Generate dialog did not open the request");
        if (ask->property("kind") != "view" || ask->property("itemId").toInt() != quarterView || ask->property("field") != "framing")
            return fail(45, "the request is not about the view's instruction: " + ask->property("label").toString());
        if (plan->item("view", quarterView)["fields"].toMap()["inputs"].toList() != QVariantList{sampleRef, faceChosen})
            return fail(45, "Ask did not save the picks first");
        const QString first = ask->property("suggestions").toList().value(0).toString();
        if (!first.startsWith("Look at the 2 picked pictures") || !first.contains(gen->pictureLabel(gen->viewOptions(quarterView)["workflow"].toString(), 2)))
            return fail(45, "the first suggestion is not about the picked pictures: " + first);
        capture("generate-ask-instruction");
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::qWait(150);
        // What the agent reads about the view: the pictures in order, with what the workflow calls them.
        const auto context = editor.requests()->viewContext(quarterView);
        const auto pics = context["pictures"].toArray();
        if (pics.size() != 2 || pics[0].toObject()["id"].toInt() != sampleRef || pics[1].toObject()["id"].toInt() != faceChosen ||
            pics[1].toObject()["calledInPrompt"] != gen->pictureLabel(gen->viewOptions(quarterView)["workflow"].toString(), 2) ||
            !context["pictureNote"].toString().contains("picked these") || context["appearance"] != portrait)
            return fail(45, "the agent's view of the request is wrong: " + QString::fromUtf8(QJsonDocument(context).toJson(QJsonDocument::Compact)));
        editor.undo();
        if (plan->item("view", quarterView)["fields"].toMap()["inputs"].toList() != QVariantList{faceChosen, sampleRef})
            return fail(45, "undo did not bring the earlier picks back after Ask");
    }
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
        if (slotsOf(anyId) != "10/value=prompt 20/image=picture0 21/image=picture1 22/image=picture2 35/aspect_ratio=setting 35/megapixels=setting 40/cfg=setting 40/denoise=setting "
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
        // An aspect-ratio choice (a resolution node like H3's): automatic for the kind of picture
        // until the user chooses one in the dialog, which Save keeps with the workflow.
        {
            const QString key = "35/aspect_ratio";
            gen->fetchChoices(anyId);
            if (!waitFor([&] { return gen->choices().contains("ResolutionSelector/aspect_ratio"); }, 3000))
                return fail(105, "the aspect choices did not arrive: " + json(gen->choices()));
            auto sentAspect = [&] { return input(lastGraph(), "35", "aspect_ratio").toString(); };
            auto settled = [&](int prompts) {
                return waitFor([&] { return stubLog()["prompts"].toArray().size() == prompts + 1 && gen->jobs().isEmpty(); });
            };
            auto quick = [&] {
                const int n = stubLog()["prompts"].toArray().size();
                return gen->generateWith("character", mara, "ref-text", {{"workflow", anyId}}).isEmpty() && settled(n);
            };
            auto openDialog = [&] {
                const QVariant prefill = QVariantMap{{"workflow", anyId}};
                QMetaObject::invokeMethod(window, "openGenerate", Q_ARG(QVariant, "character"), Q_ARG(QVariant, mara), Q_ARG(QVariant, prefill));
                return waitFor([&] { return dialog->property("opened").toBool(); }, 1500) && dialog->property("workflow") == anyId;
            };
            auto shownAspect = [&] { return dialog->property("values").toMap()[key].toString(); };
            if (!quick() || sentAspect() != "3:4 (Portrait Standard)") return fail(105, "a person was not made at 3:4 automatically: " + sentAspect());
            if (!openDialog() || shownAspect() != "3:4 (Portrait Standard)") return fail(105, "the dialog does not show the automatic 3:4: " + shownAspect());
            auto row = find(window, "genSetting_35_aspect_ratio");
            auto combo = row ? findItem(row, "genSettingChoice") : nullptr;
            if (!combo || !combo->isVisible()) return fail(105, "no aspect ratio choice in the dialog");
            // Only items on screen are found, so a missing one is hidden.
            auto inRow = [&](const char* name) { auto r = find(window, "genSetting_35_aspect_ratio"); return r ? findItem(r, name) : nullptr; };
            if (!inRow("genSettingAuto") || inRow("genSettingReset"))
                return fail(105, "the automatic aspect is not marked as automatic");
            scrollIntoView(combo);
            click(combo);
            QTest::qWait(200);
            QTest::keyClick(window, Qt::Key_Down);
            QTest::keyClick(window, Qt::Key_Return);
            QTest::qWait(150);
            if (shownAspect() != "4:3 (Landscape Standard)") return fail(106, "choosing 4:3 in the dialog: " + shownAspect());
            if (inRow("genSettingAuto") || !inRow("genSettingReset")) return fail(106, "a chosen aspect is still marked automatic");
            capture("generate-aspect");
            click(find(window, "genSave"));
            QTest::qWait(150);
            QString kept;
            for (const auto& v : flows->settings(anyId)) if (v.toMap()["key"] == key) kept = v.toMap()["value"].toString();
            if (dialog->property("opened").toBool() || kept != "4:3 (Landscape Standard)") return fail(106, "Save did not keep the aspect ratio with the workflow: " + kept);
            if (!quick() || sentAspect() != "4:3 (Landscape Standard)") return fail(106, "the kept 4:3 was replaced by the automatic one: " + sentAspect());
            if (!openDialog() || shownAspect() != "4:3 (Landscape Standard)") return fail(107, "the dialog lost the kept 4:3: " + shownAspect());
            // Reset is the workflow's own value, chosen for this picture; afterwards it is automatic again.
            scrollIntoView(inRow("genSettingReset"));
            click(inRow("genSettingReset"));
            if (shownAspect() != "1:1 (Square)") return fail(107, "Reset did not go back to the workflow's value: " + shownAspect());
            const int n = stubLog()["prompts"].toArray().size();
            click(find(window, "genGo"));
            if (!settled(n) || sentAspect() != "1:1 (Square)") return fail(107, "the 1:1 chosen in the dialog was not used: " + sentAspect());
            if (!quick() || sentAspect() != "3:4 (Portrait Standard)") return fail(107, "after Reset the aspect is not automatic again: " + sentAspect());
            // A prop is drawn square, from its appearance, and arrives in its Object view.
            const int radio = plan->run({{"op", "add"}, {"kind", "prop"}, {"fields", QVariantMap{{"name", "Radio"}, {"prompt", "A cream transistor radio"}}}})["id"].toInt();
            const int before = stubLog()["prompts"].toArray().size();
            if (const QString why = gen->generateWith("prop", radio, "ref-text", {{"workflow", anyId}}); !why.isEmpty()) return fail(108, "a prop: " + why);
            if (!settled(before) || sentAspect() != "1:1 (Square)" || !input(lastGraph(), "10", "value").toString().contains("A cream transistor radio"))
                return fail(108, "a prop was not drawn square from its appearance: " + sentAspect());
            const auto radioPics = plan->referencesFor("prop", radio);
            if (radioPics.size() != 1 || radioPics[0].toMap()["view"].toInt() != plan->viewsFor("prop", radio)[0].toMap()["id"].toInt())
                return fail(108, "the prop's picture did not arrive in its Object view: " + json(radioPics));
            editor.undo();
            plan->run({{"op", "remove"}, {"kind", "prop"}, {"id", radio}});
        }
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

    // ---- M. Takes: a lip-sync shot sung to the song, and a reference-to-video shot
    {
        auto syncAdded = flows->importFile(QUrl::fromLocalFile(fixtures.filePath("take_sync_api.json")));
        auto refAdded = flows->importFile(QUrl::fromLocalFile(fixtures.filePath("take_ref_api.json")));
        if (!syncAdded["ok"].toBool() || !refAdded["ok"].toBool()) return fail(120, "importing the take workflows: " + json(syncAdded) + json(refAdded));
        const QString syncId = syncAdded["id"].toString(), refId = refAdded["id"].toString();
        const auto syncFlow = flows->get(syncId), refFlow = flows->get(refId);
        if (syncFlow["use"] != "take-sync" || refFlow["use"] != "take-ref" || !syncFlow["ready"].toBool() || !refFlow["ready"].toBool())
            return fail(120, "the take workflows' uses: " + json(syncFlow["problems"]) + json(refFlow["problems"]));
        if (!slotsOf(syncId).contains("132/value=seconds") || !slotsOf(syncId).contains("200/audio=audio") || !slotsOf(syncId).contains("138/value=prompt")
            || !slotsOf(syncId).contains("137/image=picture0") || !slotsOf(syncId).contains("92/filename_prefix=prefix"))
            return fail(120, "the lip-sync workflow's adapter: " + slotsOf(syncId));
        if (flows->pictureCount(refId) != 9) return fail(120, "the growing reference list should offer nine pictures: " + QString::number(flows->pictureCount(refId)));
        // A lip-sync shot over the first sung line, with Mara in frame and a prompt.
        const auto firstLine = editor.lyrics()->lines()[0].toMap();
        const int start = int(std::lround(firstLine["start"].toInt() / 1000.0 * editor.fps())), end = int(std::lround(firstLine["end"].toInt() / 1000.0 * editor.fps()));
        auto made = plan->run({{"op", "shot.add"}, {"track", 1}, {"start", start}, {"end", end}});
        const int shotId = made["id"].toInt();
        const QString syncPrompt = "subject_definitions: <Picture 1> is the woman. detailed_description: 0.0-0.5 s she breathes in; then she sings the line.";
        plan->run({{"op", "set"}, {"kind", "shot"}, {"id", shotId}, {"fields", QVariantMap{{"type", "lipsync"}, {"route", "h3-lipsync"},
                   {"who", QVariantList{QVariantMap{{"character", mara}}}}, {"prompts", QVariantMap{{"h3-lipsync", syncPrompt}}}}}});
        gen->fetchChoices(syncId);
        if (!waitFor([&] { return gen->takeOptions(shotId)["ready"].toBool() && (gen->takeOptions(shotId)["frames"].toInt() - 5) % 17 == 0; }, 3000))
            return fail(121, "the lip-sync take is not ready: " + json(gen->takeOptions(shotId)));
        auto topts = gen->takeOptions(shotId);
        const int frames = topts["frames"].toInt();
        const double shotSeconds = (end - start) / editor.fps();
        if (topts["use"] != "take-sync" || topts["workflow"] != syncId || frames < std::ceil((shotSeconds + 1.0) * 24 - 1e-6) || frames - 17 >= (shotSeconds + 1.0) * 24
            || topts["pictures"].toList().isEmpty() || topts["pictures"].toList()[0].toMap()["label"] != "<Picture 1>")
            return fail(121, "the lip-sync take's options: " + json(topts));
        // Generate it from the inspector, by real clicks.
        click(find(window, "workspaceEdit"));
        window->setProperty("selection", QVariantMap{{"kind", "shot"}, {"id", shotId}, {"start", firstLine["start"]}, {"end", firstLine["end"]}});
        QTest::qWait(150);
        auto genTake = find(window, "shotGenerateTake");
        if (!genTake) return fail(122, "no Generate take in the shot inspector");
        scrollIntoView(genTake);
        const int uploadsBefore = stubLog()["uploads"].toArray().size();
        mode("finish");
        click(genTake);
        if (!waitFor([&] { return plan->takesFor(shotId).size() == 1 && gen->jobs().isEmpty(); }, 20000))
            return fail(122, "the lip-sync take did not arrive: " + json(gen->jobs()));
        QJsonObject graph = lastGraph();
        const auto uploads = stubLog()["uploads"].toArray();
        const auto wav = uploads.last().toObject();
        const int pictureCountSent = topts["pictures"].toList().size();
        if (uploads.size() != uploadsBefore + pictureCountSent + 1 || !wav["name"].toString().endsWith(".wav")
            || wav["wavBytes"].toInt() != 44 + int(std::llround(frames / 24.0 * 48000)) * 4)
            return fail(123, "the song excerpt was not uploaded at the take's length: " + QString::fromUtf8(QJsonDocument(wav).toJson(QJsonDocument::Compact)));
        if (input(graph, "200", "audio") != wav["name"] || std::abs(input(graph, "132", "value").toDouble() - frames / 24.0) > 1e-9
            || input(graph, "138", "value") != syncPrompt || input(graph, "92", "filename_prefix") != "ChorusCut/take"
            || input(graph, "137", "image") != uploads[uploadsBefore].toObject()["name"])
            return fail(123, "the lip-sync graph: " + QString::fromUtf8(QJsonDocument(graph).toJson(QJsonDocument::Compact)));
        auto take = plan->takesFor(shotId)[0].toMap();
        const int handle = int(std::lround(0.5 * editor.fps()));
        if (!take["chosen"].toBool() || !take["lipSync"].toBool() || take["from"].toInt() != start - handle || take["frames"].toInt() != frames
            || take["thumbUrl"].toString().isEmpty() || take["missing"].toBool())
            return fail(124, "the first take: " + json(take));
        // It plays in the cut: the shot's frames show the take, not black.
        QImage shown;
        QObject::connect(&editor, &Editor::imageReady, &editor, [&](const QImage& img) { shown = img; });
        auto colourAt = [&](int frame) { shown = {}; editor.seek(frame); QTest::qWait(50); return shown.isNull() ? QColor() : shown.pixelColor(shown.width() / 2, shown.height() / 2); };
        const QColor inShot = colourAt((start + end) / 2);
        if (!inShot.isValid() || (inShot.red() + inShot.green() + inShot.blue()) < 30) return fail(125, "the chosen take does not play in the cut");
        capture("take-inspector");
        if (!find(window, "shotTake1")) return fail(125, "the inspector does not show the take");
        auto zoom = find(window, "inspectorZoomTo");
        if (zoom) { scrollIntoView(zoom); click(zoom); QTest::qWait(150); }
        auto takesLabel = find(window, QString("shotTakes%1").arg(shotId));
        if (!takesLabel || takesLabel->property("text") != "1 take") return fail(125, "the timeline's shot does not show its take");
        auto monitor = find(window, "monitor");
        if (!monitor) return fail(125, "the monitor is hidden though a take plays");
        // The monitor follows the editor by itself (its editor property), as in the app.
        editor.seek((start + end) / 2);
        QTest::qWait(100);
        capture("take-timeline");
        // A second take through the dialog; choosing it changes what plays.
        click(find(window, "shotTakeOptions"));
        auto takeDialog = window->findChild<QObject*>("takePopup");
        if (!takeDialog || !waitFor([&] { return takeDialog->property("opened").toBool(); }, 1500)) return fail(126, "With options… did not open the take dialog");
        if (takeDialog->property("prompt") != syncPrompt || !find(window, "takeLength")->property("text").toString().contains("of the song, locked"))
            return fail(126, "the take dialog does not show the shot: " + find(window, "takeLength")->property("text").toString());
        capture("take-dialog");
        click(find(window, "takeGo"));
        if (!waitFor([&] { return plan->takesFor(shotId).size() == 2 && gen->jobs().isEmpty(); }, 20000))
            return fail(126, "the second take did not arrive: " + json(gen->jobs()));
        if (plan->takesFor(shotId)[1].toMap()["chosen"].toBool()) return fail(126, "a later take was chosen without asking");
        scrollIntoView(find(window, "shotTake2"));
        click(find(window, "shotTake2"));
        if (!plan->takesFor(shotId)[1].toMap()["chosen"].toBool() || plan->takesFor(shotId)[0].toMap()["chosen"].toBool())
            return fail(127, "clicking take 2 did not choose it");
        const QColor second = colourAt((start + end) / 2);
        if (second == inShot) return fail(127, "the cut still plays the first take");
        editor.undo();
        if (!plan->takesFor(shotId)[0].toMap()["chosen"].toBool() || colourAt((start + end) / 2) != inShot) return fail(127, "undo did not bring the first take back");
        // Moving a lip-sync shot keeps its take on the song: moved far, it is too short.
        plan->run({{"op", "shot.retime"}, {"id", shotId}, {"start", start + (end - start)}, {"end", end + (end - start)}});
        bool shortCheck = false;
        for (const auto& c : plan->checks()) shortCheck |= c.toMap()["code"] == "takeShort" && c.toMap()["shots"].toList().contains(shotId);
        if (plan->takesFor(shotId)[0].toMap()["from"].toInt() != start - handle || !shortCheck) return fail(128, "a moved lip-sync take left the song, or no short-take check");
        editor.undo();
        // An agent can't choose a take.
        auto agentChoice = plan->run({{"op", "set"}, {"kind", "take"}, {"id", plan->takesFor(shotId)[1].toMap()["id"]}, {"fields", QVariantMap{{"approved", true}}}, {"by", "agent"}});
        if (agentChoice["ok"].toBool()) return fail(128, "an agent chose a take");
        // What the agent reads to write the prompt.
        const auto ctx = editor.requests()->takeContext(shotId);
        if (ctx["pictures"].toArray().isEmpty() || ctx["pictures"].toArray()[0].toObject()["calledInPrompt"] != "<Picture 1>" || ctx["lyrics"].toArray().isEmpty()
            || std::abs(ctx["cutIn"].toDouble() - 0.5) > 0.02 || !ctx["lipSync"].toBool())
            return fail(129, "the agent's take context: " + QString::fromUtf8(QJsonDocument(ctx).toJson(QJsonDocument::Compact)));
        // The song is locked in by the sync node, never a reference: the agent is told not to tag
        // it, and to quote the sung lines, with no per-word times.
        if (!ctx["references"].toString().contains("never write <Audio n>") || !ctx["song"].toString().contains("audio-sync node")
            || !ctx["song"].toString().contains("name the lines") || ctx["format"].toString().contains("song locked in")
            || ctx["lyrics"].toArray()[0].toObject().contains("words") || ctx["lyrics"].toArray()[0].toObject()["text"].toString() != firstLine["text"].toString())
            return fail(129, "the lip-sync take context should quote lines and name no audio reference: " + QString::fromUtf8(QJsonDocument(ctx).toJson(QJsonDocument::Compact)));
        // Picking the shot's pictures by hand, in order: an alternative of Mara's, then a place
        // from the rest of the plan.
        {
            int alt = 0, place = 0;
            for (const auto& r : plan->referencesFor("character", mara)) if (!r.toMap()["approved"].toBool() && !r.toMap()["missing"].toBool()) alt = r.toMap()["id"].toInt();
            for (const auto& r : plan->references()) if (r.toMap()["owner"] == "location" && !r.toMap()["missing"].toBool()) place = r.toMap()["id"].toInt();
            if (!alt || !place) return fail(135, "expected an alternative of Mara's and a location picture");
            auto pickLink = find(window, "shotPicPick");
            if (!pickLink) return fail(135, "no Pick… for the shot's pictures");
            scrollIntoView(pickLink);
            click(pickLink);
            if (!waitFor([&] { return takeDialog->property("opened").toBool(); }, 1500)) return fail(135, "Pick… did not open the picker");
            for (const auto& v : takeDialog->property("picks").toList()) {
                auto tile = find(window, "takePick_" + v.toString());
                if (!tile) return fail(135, "the picker does not show the picked picture " + v.toString() + ": " + json(takeDialog->property("groups").value<QJSValue>().toVariant()) + " picks " + json(takeDialog->property("picks").value<QJSValue>().toVariant()));
                scrollIntoView(tile); click(tile);
            }
            if (!takeDialog->property("picks").toList().isEmpty()) return fail(135, "clicking the picked pictures did not take them out");
            auto altTile = find(window, QString("takePick_%1").arg(alt));
            if (!altTile) return fail(135, QString("the picker does not offer Mara%1s alternative %2: ").arg(QChar(0x2019)).arg(alt) + json(takeDialog->property("groups").value<QJSValue>().toVariant()) + " picks " + json(takeDialog->property("picks").value<QJSValue>().toVariant()));
            scrollIntoView(altTile);
            click(altTile);
            auto more = find(window, "takeMorePictures");
            if (!more) return fail(135, "no More pictures from the plan");
            scrollIntoView(more);
            click(more);
            auto placeTile = find(window, QString("takePick_%1").arg(place));
            if (!placeTile) return fail(135, "More pictures from the plan did not show the location's");
            scrollIntoView(placeTile);
            click(placeTile);
            if (takeDialog->property("picks").toList() != QVariantList{alt, place}) return fail(135, "the picks: " + json(takeDialog->property("picks")));
            capture("take-pick");
            auto save = find(window, "takeSave");
            click(save);
            QStringList order;
            for (const auto& r : plan->shotPictures(shotId)["picked"].toList()) order << r.toMap()["id"].toString();
            const auto labels = gen->takeOptions(shotId)["pictures"].toList();
            if (plan->shot(shotId)["fields"].toMap()["pictures"].toList() != QVariantList{alt, place} || order != QStringList{QString::number(alt), QString::number(place)}
                || labels.size() != 2 || labels[1].toMap()["id"].toInt() != place || labels[1].toMap()["label"] != "<Picture 2>")
                return fail(136, "the shot does not send the picked pictures in order: " + order.join(","));
            plan->run({{"op", "set"}, {"kind", "shot"}, {"id", shotId}, {"fields", QVariantMap{{"pictures", QVariantList{}}}}});
        }
        // Reference-to-video with three pictures: the workflow's two Load Image nodes grow a third.
        // A third view of Mara with a chosen picture, so the shot has three to send.
        {
            const auto addedView = plan->run({{"op", "add"}, {"kind", "view"}, {"owner", "character"}, {"ownerId", mara},
                                              {"fields", QVariantMap{{"name", "Profile"}, {"role", "other"}, {"from", "prompt"}}}});
            int spare = 0;
            for (const auto& r : plan->referencesFor("character", mara)) if (!r.toMap()["approved"].toBool() && !r.toMap()["missing"].toBool()) spare = r.toMap()["id"].toInt();
            plan->run({{"op", "ref.move"}, {"id", spare}, {"view", addedView["id"]}});
            plan->run({{"op", "set"}, {"kind", "reference"}, {"id", spare}, {"fields", QVariantMap{{"approved", true}}}});
            // A view of the user's own kind named Profile counts as a face, so the shot sends it.
            bool sent = false;
            for (const auto& r : plan->shotPictures(shotId)["picked"].toList()) sent |= r.toMap()["id"].toInt() == spare;
            if (!sent) return fail(130, "a view named Profile was not sent: " + json(plan->shotPictures(shotId)["picked"]));
        }
        QVariantList three;
        for (const auto& r : plan->shotPictures(shotId)["candidates"].toList()) if (three.size() < 3) three << r.toMap()["id"];
        if (three.size() < 3) return fail(130, "expected three of Mara's views to choose from: " + json(plan->shotPictures(shotId)));
        plan->run({{"op", "set"}, {"kind", "shot"}, {"id", shotId}, {"fields", QVariantMap{{"route", "ref2v"}, {"pictures", three},
                   {"prompts", QVariantMap{{"h3-lipsync", syncPrompt}, {"ref2v", "<Picture 1>, <Picture 2> and <Picture 3> show the woman."}}}}}});
        gen->fetchChoices(refId);
        if (!waitFor([&] { return gen->takeOptions(shotId)["ready"].toBool(); }, 3000)) return fail(130, "the reference take is not ready: " + json(gen->takeOptions(shotId)));
        const int beforeRef = stubLog()["uploads"].toArray().size();
        if (const QString why = gen->generateTake(shotId); !why.isEmpty()) return fail(130, why);
        if (!waitFor([&] { return plan->takesFor(shotId).size() == 3 && gen->jobs().isEmpty(); }, 20000)) return fail(130, "the reference take did not arrive: " + json(gen->jobs()));
        graph = lastGraph();
        const auto refUploads = stubLog()["uploads"].toArray();
        const auto cond = graph.value("136").toObject().value("inputs").toObject();
        QString grown;
        for (auto it = graph.begin(); it != graph.end(); ++it) if (it.key().toInt() > 146 && it.value().toObject()["class_type"] == "LoadImage") grown = it.key();
        if (refUploads.size() != beforeRef + 3 || graph.contains("200") || grown.isEmpty() || cond.value("ref_images.ref_image_2") != QJsonArray{grown, 0}
            || input(graph, grown.toUtf8().constData(), "image") != refUploads.last().toObject()["name"])
            return fail(131, "the three-picture reference graph: " + QString::fromUtf8(QJsonDocument(graph).toJson(QJsonDocument::Compact)));
        const auto refTake = plan->takesFor(shotId)[2].toMap();
        if (refTake["lipSync"].toBool()) return fail(131, "a reference take was locked to the song");
        // A reference take moves with its shot.
        plan->run({{"op", "shot.retime"}, {"id", shotId}, {"start", start + 10}, {"end", end + 10}});
        if (plan->takesFor(shotId)[2].toMap()["from"].toInt() != refTake["from"].toInt() + 10 || plan->takesFor(shotId)[0].toMap()["from"].toInt() != start - handle)
            return fail(132, "moving the shot: the reference take should follow and the lip-sync take stay");
        editor.undo();
        // Removing the shot takes its takes; undo brings them back.
        plan->run({{"op", "remove"}, {"kind", "shot"}, {"id", shotId}});
        if (!plan->takesFor(shotId).isEmpty()) return fail(133, "removing the shot left its takes");
        editor.undo();
        if (plan->takesFor(shotId).size() != 3) return fail(133, "undo did not bring the takes back");
        // The excerpt itself: a WAV of the requested length, silence before the song starts.
        const QString excerpt = QDir(QDir::tempPath()).filePath("chorus-excerpt-check.wav");
        if (!editor.writeExcerpt(-0.25, 1.0, excerpt).isEmpty() || QFileInfo(excerpt).size() != 44 + 48000 * 4) return fail(134, "the song excerpt's size");
        QFile::remove(excerpt);

        // ---- N. What is generating, being told when it finishes, the take dialog's room, the
        // viewer's size and pop-out, and track switches.
        {
            auto notices = [&] {
                QStringList out;
                QFile f(notifyLog);
                if (f.open(QIODevice::ReadOnly)) for (const auto& line : f.readAll().split('\n')) if (!line.isEmpty()) out << QString::fromUtf8(line);
                return out;
            };
            const int toldBefore = notices().size();
            auto selectedShot = [&] { const auto sel = window->property("selection").toMap(); return sel["kind"] == "shot" ? sel["id"].toInt() : 0; };
            const QString shotName = "Shot " + plan->shot(shotId)["number"].toString();
            window->setProperty("selection", QVariantMap{{"kind", "shot"}, {"id", shotId}, {"start", firstLine["start"]}, {"end", firstLine["end"]}});
            QTest::qWait(150);
            // A take held running on ComfyUI: the shot's card and the Jobs button say so.
            mode("hold");
            auto again = find(window, "shotGenerateTake");
            if (!again) return fail(140, "no Generate take for the shot");
            click(again);
            if (!waitFor([&] { return gen->jobs().size() == 1 && gen->jobs()[0].toMap()["state"] == "running"; })) return fail(140, "the held take is not running: " + json(gen->jobs()));
            auto marker = find(window, QString("shotGenerating%1").arg(shotId));
            auto jobsButton = find(window, "jobsButton");
            if (!marker || !jobsButton || jobsButton->property("text") != "Generating · 1" || find(window, QString("shotTakes%1").arg(shotId))->property("text") != "generating 1")
                return fail(140, "the timeline or the Jobs button does not show the take generating");
            window->setProperty("selection", QVariant::fromValue(nullptr));
            QTest::qWait(100);
            click(jobsButton);
            auto jobsPopup = window->findChild<QObject*>("jobsPopup");
            if (!jobsPopup || !waitFor([&] { return jobsPopup->property("opened").toBool(); }, 1500)) return fail(141, "the Jobs button did not open the list");
            auto row = find(window, QString("jobRow_shot%1").arg(shotId));
            if (!row) return fail(141, "the list does not show the take");
            QTest::qWait(1100);
            auto rowText = findItem(row, "jobText");
            if (!rowText || !rowText->property("text").toString().contains("Generating") || !rowText->property("text").toString().contains("0:0"))
                return fail(141, "the take's row: " + (rowText ? rowText->property("text").toString() : QString("none")));
            // Notify even while the app is in front, so the check sees it.
            auto background = find(window, "notifyBackground");
            click(background);
            if (editor.notifier()->onlyInBackground()) return fail(142, "the background-only switch did not turn off");
            capture("jobs");
            click(findItem(row, "jobShow"));
            if (selectedShot() != shotId || jobsPopup->property("opened").toBool()) return fail(141, "Show did not open the shot");
            // It arrives: the banner and the notification name the shot.
            call("/_stub/release", {}, true);
            if (!waitFor([&] { return plan->takesFor(shotId).size() == 4 && gen->jobs().isEmpty(); }, 20000)) return fail(142, "the held take did not arrive");
            auto banner = find(window, "noticeBanner");
            if (!banner || find(window, "noticeTitle")->property("text") != shotName + ": take ready") return fail(142, "no banner for the finished take");
            const auto told = notices().mid(toldBefore);
            if (told.size() != 2 || !told[0].contains("\"title\":\"" + shotName + ": take ready\"") || !told[0].contains("\"ownerId\":" + QString::number(shotId))
                || !told[0].contains("\"sound\":true") || !told[1].contains("\"dock\""))
                return fail(142, "the notification: " + told.join(" | "));
            capture("notice");
            window->setProperty("selection", QVariant::fromValue(nullptr));
            QTest::qWait(100);
            click(find(window, "noticeShow"));
            if (selectedShot() != shotId || find(window, "noticeBanner")) return fail(142, "Show on the banner did not open the shot");
            // A click on the macOS notification does the same.
            window->setProperty("selection", QVariant::fromValue(nullptr));
            editor.notifier()->activate("shot", shotId);
            QTest::qWait(100);
            if (selectedShot() != shotId) return fail(142, "clicking the notification did not open the shot");
            // Pictures are off by default; failures are on. Turn the sound off first.
            click(jobsButton);
            waitFor([&] { return jobsPopup->property("opened").toBool(); }, 1500);
            click(find(window, "notifySound"));
            if (editor.notifier()->sound()) return fail(143, "the sound switch did not turn off");
            QTest::keyClick(window, Qt::Key_Escape);
            waitFor([&] { return !jobsPopup->property("visible").toBool(); }, 1500);
            mode("error");
            if (const QString why = gen->generateTake(shotId); !why.isEmpty()) return fail(143, why);
            if (!waitFor([&] { return gen->jobs().size() == 1 && gen->jobs()[0].toMap()["state"] == "failed"; }, 10000)) return fail(143, "the take did not fail");
            mode("finish");
            const auto failedTold = notices().mid(toldBefore);
            if (failedTold.size() != 4 || !failedTold[2].contains(shotName + ": take failed") || !failedTold[2].contains("\"sound\":false"))
                return fail(143, "the failure's notification: " + failedTold.join(" | "));
            if (find(window, QString("shotTakes%1").arg(shotId))->property("text") != "take failed" || jobsButton->property("text") != "Jobs · 1 failed")
                return fail(143, "the failed take is not shown on the card and the button");
            click(jobsButton);
            waitFor([&] { return jobsPopup->property("opened").toBool(); }, 1500);
            row = find(window, QString("jobRow_shot%1").arg(shotId));
            if (!row) return fail(143, "the failed take is not listed");
            click(findItem(row, "jobDismiss"));
            if (!gen->jobs().isEmpty() || jobsButton->property("text") != "Jobs") return fail(143, "Dismiss did not clear the failed take");
            QTest::keyClick(window, Qt::Key_Escape);
            waitFor([&] { return !jobsPopup->property("visible").toBool(); }, 1500);
            {
                QSettings kept;
                if (kept.value("notify/sound").toBool() || kept.value("notify/onlyInBackground").toBool()) return fail(143, "the notification choices were not kept");
            }

            // The take dialog on a normal window: the prompt fills the left, the options scroll on the right.
            window->setProperty("selection", QVariantMap{{"kind", "shot"}, {"id", shotId}, {"start", firstLine["start"]}, {"end", firstLine["end"]}});
            QTest::qWait(150);
            click(find(window, "shotTakeOptions"));
            if (!waitFor([&] { return takeDialog->property("opened").toBool(); }, 1500)) return fail(144, "With options… did not open the take dialog");
            QTest::qWait(200);
            auto promptBox = find(window, "takePromptScroll"), optionsBox = find(window, "takeOptions");
            if (!promptBox || !optionsBox) return fail(144, "the take dialog's parts are missing");
            const QRectF pr = promptBox->mapRectToScene(QRectF(0, 0, promptBox->width(), promptBox->height()));
            const QRectF orr = optionsBox->mapRectToScene(QRectF(0, 0, optionsBox->width(), optionsBox->height()));
            if (orr.left() < pr.right() || pr.height() < 450 || pr.width() < 500)
                return fail(144, QString("the prompt should fill the left at full height: prompt %1x%2 at %3, options at %4").arg(pr.width()).arg(pr.height()).arg(pr.left()).arg(orr.left()));
            // A long prompt, typed: it scrolls in its own box, smoothly to its end, and the options stay put.
            click(find(window, "takePrompt"));
            QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
            QTest::keyClick(window, Qt::Key_Backspace);
            for (int line = 0; line < 45; ++line) {
                for (const QChar ch : QString("Second %1: the camera holds on Picture 1 as she sings, light from the left.").arg(line)) QTest::keyClick(window, ch.toLatin1());
                QTest::keyClick(window, Qt::Key_Return);
            }
            QTest::qWait(200);
            const QString words = find(window, "takePromptWords")->property("text").toString();
            if (!words.endsWith("words") || words.split(' ')[0].toInt() < 45 * 14) return fail(145, "the word count: " + words);
            auto promptFlick = promptBox->property("contentItem").value<QQuickItem*>();
            auto optionsFlick = optionsBox->property("contentItem").value<QQuickItem*>();
            if (!promptFlick || !optionsFlick) return fail(145, "no scrolling areas");
            promptFlick->setProperty("contentY", 0);
            optionsFlick->setProperty("contentY", 0);
            QTest::qWait(100);
            const QPoint over = pr.center().toPoint();
            QPointingDevice trackpad("trackpad", 2, QInputDevice::DeviceType::TouchPad, QPointingDevice::PointerType::Finger,
                                     QInputDevice::Capability::Position | QInputDevice::Capability::PixelScroll, 1, 3);
            double last = 0;
            bool steady = true;
            for (int k = 0; k < 30; ++k) {
                const auto phase = k == 0 ? Qt::ScrollBegin : Qt::ScrollUpdate;
                QWheelEvent e(over, window->mapToGlobal(over), QPoint(0, -30), QPoint(0, -90), Qt::NoButton, Qt::NoModifier, phase, false, Qt::MouseEventNotSynthesized, &trackpad);
                QGuiApplication::sendEvent(window, &e);
                QTest::qWait(16);
                const double y = promptFlick->property("contentY").toDouble();
                if (y < last - 0.5) steady = false;
                last = y;
            }
            { QWheelEvent e(over, window->mapToGlobal(over), QPoint(), QPoint(), Qt::NoButton, Qt::NoModifier, Qt::ScrollEnd, false, Qt::MouseEventNotSynthesized, &trackpad);
              QGuiApplication::sendEvent(window, &e); }
            QTest::qWait(300);
            const double maxY = promptFlick->property("contentHeight").toDouble() - promptFlick->height();
            const double endY = promptFlick->property("contentY").toDouble();
            if (!steady || maxY < 100 || endY < maxY - 1 || endY > maxY + 1 || optionsFlick->property("contentY").toDouble() != 0)
                return fail(145, QString("scrolling the prompt: steady %1, at %2 of %3, options at %4").arg(steady).arg(endY).arg(maxY).arg(optionsFlick->property("contentY").toDouble()));
            capture("take-dialog-long");
            click(find(window, "takeCancel"));
            if (plan->shot(shotId)["fields"].toMap()["prompts"].toMap()["h3-lipsync"] != syncPrompt) return fail(145, "Cancel kept the typed prompt");

            // A workflow chosen in the dialog stays with the shot when saved, and Generate take uses it.
            {
                const QString use = gen->takeOptions(shotId)["use"].toString();
                auto again = flows->importFile(QUrl::fromLocalFile(fixtures.filePath(use == "take-sync" ? "take_sync_api.json" : "take_ref_api.json")));
                const QString other = again["id"].toString();
                const QString usual = gen->takeOptions(shotId)["workflow"].toString();
                if (!again["ok"].toBool() || other == usual) return fail(150, "a second take workflow: " + json(again));
                click(find(window, "shotTakeOptions"));
                if (!waitFor([&] { return takeDialog->property("opened").toBool(); }, 1500) || takeDialog->property("workflow") != usual)
                    return fail(150, "the dialog did not open on the default workflow");
                QStringList ids;
                for (const auto& w : gen->takeOptions(shotId)["workflows"].toList()) ids << w.toMap()["id"].toString();
                choose(find(window, "takeWorkflow"), ids.indexOf(other));
                if (takeDialog->property("workflow") != other) return fail(150, "choosing the other workflow in the dialog");
                click(find(window, "takeSave"));
                if (takeDialog->property("opened").toBool() || plan->shot(shotId)["fields"].toMap()["workflow"] != other)
                    return fail(150, "Save did not keep the workflow on the shot: " + json(plan->shot(shotId)["fields"]));
                if (gen->takeOptions(shotId)["workflow"] != other) return fail(150, "the shot's takes do not use its saved workflow");
                click(find(window, "shotTakeOptions"));
                if (!waitFor([&] { return takeDialog->property("opened").toBool(); }, 1500) || takeDialog->property("workflow") != other)
                    return fail(150, "the dialog reopened on another workflow: " + takeDialog->property("workflow").toString());
                // Back to the default: the shot follows the library's default again.
                choose(find(window, "takeWorkflow"), ids.indexOf(usual));
                click(find(window, "takeSave"));
                if (plan->shot(shotId)["fields"].toMap().contains("workflow") || gen->takeOptions(shotId)["workflow"] != usual)
                    return fail(150, "choosing the default did not clear the shot's own workflow");
                editor.undo();
                if (plan->shot(shotId)["fields"].toMap()["workflow"] != other) return fail(150, "undo did not bring the saved workflow back");
                editor.undo();
                flows->remove(other);
                if (gen->takeOptions(shotId)["workflow"] != usual) return fail(150, "a removed workflow is still used for the shot");
            }

            // The viewer's size: drag the divider under it, and the one beside the inspector.
            window->setProperty("selection", QVariant::fromValue(nullptr));
            auto pane = find(window, "viewerPane");
            auto timelineItem = find(window, "timeline");
            auto divider = find(window, "viewerDivider");
            if (!pane || !timelineItem || !divider) return fail(146, "the viewer, the timeline or the divider between them is missing");
            auto drag = [&](QQuickItem* handle, QPoint by) {
                const QPoint from = center(handle);
                QTest::mousePress(window, Qt::LeftButton, {}, from);
                for (int k = 1; k <= 10; ++k) { QTest::mouseMove(window, from + by * k / 10); QTest::qWait(16); }
                QTest::mouseRelease(window, Qt::LeftButton, {}, from + by);
                QTest::qWait(150);
            };
            const double paneBefore = pane->height(), timelineBefore = timelineItem->height();
            drag(divider, QPoint(0, 120));
            if (std::abs(pane->height() - paneBefore - 120) > 4 || std::abs(timelineBefore - timelineItem->height() - 120) > 4)
                return fail(146, QString("dragging the divider: viewer %1 -> %2, timeline %3 -> %4").arg(paneBefore).arg(pane->height()).arg(timelineBefore).arg(timelineItem->height()));
            auto side = find(window, "sidePanel");
            const double sideBefore = side->width();
            drag(find(window, "panelDivider"), QPoint(-100, 0));
            if (std::abs(side->width() - sideBefore - 100) > 4) return fail(146, QString("dragging the inspector's divider: %1 -> %2").arg(sideBefore).arg(side->width()));
            {
                QSettings kept;
                waitFor([&] { kept.sync(); return std::abs(kept.value("viewer/height").toInt() - pane->height()) <= 1; }, 3000);
                if (std::abs(kept.value("viewer/height").toInt() - pane->height()) > 1 || std::abs(kept.value("viewer/panelWidth").toInt() - side->width()) > 1)
                    return fail(146, "the viewer's size was not kept: " + kept.value("viewer/height").toString() + " " + kept.value("viewer/panelWidth").toString());
            }
            capture("viewer-larger");
            // Pop out: the viewer moves to its own window and the timeline takes the room.
            const QColor withTakeColour = colourAt((start + end) / 2);
            {
                // The page's own viewer shows the frame too (no test wiring: the app's).
                const QImage page = window->grabWindow();
                const QPointF mid = find(window, "monitor")->mapToScene(QPointF(find(window, "monitor")->width() / 2, find(window, "monitor")->height() / 2));
                const QColor c = page.pixelColor((mid * page.devicePixelRatio()).toPoint());
                if (std::abs(c.red() - withTakeColour.red()) + std::abs(c.green() - withTakeColour.green()) + std::abs(c.blue() - withTakeColour.blue()) > 24)
                    return fail(147, "the Edit page's viewer does not show the take: " + c.name() + " where the cut is " + withTakeColour.name());
            }
            click(find(window, "viewerPopOut"));
            auto viewerWin = window->findChild<QQuickWindow*>("viewerWindow");
            if (!viewerWin || !waitFor([&] { return viewerWin->isVisible(); }, 2000)) return fail(147, "Pop out did not open the viewer window");
            if (!QTest::qWaitForWindowExposed(viewerWin)) return fail(147, "the viewer window is not shown");
            if (find(window, "monitor") || pane->height() > 60 || timelineItem->height() < timelineBefore + 100)
                return fail(147, QString("popped out, the Edit page should give the room to the timeline: viewer %1, timeline %2").arg(pane->height()).arg(timelineItem->height()));
            QTest::qWait(200);
            {
                const QImage seen = viewerWin->grabWindow();
                const QColor c = seen.pixelColor(seen.width() / 2, seen.height() / 2);
                const QColor inline_ = withTakeColour;
                if (std::abs(c.red() - inline_.red()) + std::abs(c.green() - inline_.green()) + std::abs(c.blue() - inline_.blue()) > 24)
                    return fail(147, "the viewer window does not show the take: " + c.name() + " where the cut shows " + inline_.name());
                if (!shots.isEmpty()) seen.save(shots + "/viewer-window.png");
            }
            capture("viewer-popped");
            auto back = findItem(viewerWin->contentItem(), "viewerWindowBack");
            if (!back) return fail(147, "no Back to Edit in the viewer window");
            QTest::mouseClick(viewerWin, Qt::LeftButton, {}, back->mapToScene(QPointF(back->width() / 2, back->height() / 2)).toPoint());
            if (!waitFor([&] { return !viewerWin->isVisible(); }, 1500) || !waitFor([&] { return find(window, "monitor") != nullptr; }, 1500))
                return fail(147, "Back to Edit did not bring the viewer back");
            QTest::qWait(200);
            if (std::abs(pane->height() - paneBefore - 120) > 6) return fail(147, QString("the viewer came back at %1, not its size %2").arg(pane->height()).arg(paneBefore + 120));

            // Track switches: hide V1 and the take leaves the viewer; hide the lyrics and the line goes.
            const QColor withTake = colourAt((start + end) / 2);
            auto v1Switch = find(window, "trackSwitch_v1");
            if (!v1Switch) return fail(148, "no eye on V1");
            click(v1Switch);
            const QColor hiddenV1 = colourAt((start + end) / 2);
            if (!editor.hiddenTracks().contains("v1") || hiddenV1 == withTake || hiddenV1.red() + hiddenV1.green() + hiddenV1.blue() > 30)
                return fail(148, "hiding V1 did not take its take out of the viewer");
            capture("track-hidden");
            click(find(window, "trackSwitch_v1"));
            if (editor.hiddenTracks().contains("v1") || colourAt((start + end) / 2) != withTake) return fail(148, "showing V1 did not bring the take back");
            editor.seekLyric(firstLine["start"].toInt() + 200);
            QTest::qWait(100);
            if (!find(window, "monitorLyric")) return fail(148, "the lyric line is not in the viewer");
            click(find(window, "trackSwitch_lyrics"));
            if (find(window, "monitorLyric") || !editor.hiddenTracks().contains("lyrics")) return fail(148, "hiding the lyrics did not leave the line off");
            // A1's mute: nothing reaches the output while it is on.
            QFile captureFile(scratch.filePath("capture.raw"));
            auto loudest = [&](qint64 from) {
                float peak = 0;
                if (!captureFile.open(QIODevice::ReadOnly)) return -1.0f;
                captureFile.seek(from);
                const QByteArray b = captureFile.readAll();
                captureFile.close();
                const float* f = reinterpret_cast<const float*>(b.constData());
                for (qsizetype i = 0; i < b.size() / 4; ++i) peak = std::max(peak, std::abs(f[i]));
                return b.size() < 4 ? -1.0f : peak;
            };
            // SDL's disk driver starts the file over each time playback starts.
            auto playFor = [&](int ms) {
                const qint64 from = 0;
                editor.seekLyric(firstLine["start"].toInt());
                editor.togglePlay();
                QTest::qWait(ms);
                if (editor.playing()) editor.togglePlay();
                QTest::qWait(200);
                return loudest(from);
            };
            click(find(window, "trackSwitch_a1"));
            if (!editor.mutedTracks().contains("a1")) return fail(149, "the A1 switch did not mute the master");
            const float mutedPeak = playFor(700);
            click(find(window, "trackSwitch_a1"));
            const float openPeak = playFor(700);
            if (mutedPeak != 0 || openPeak <= 0.001f) return fail(149, QString("A1's mute at the output: muted peak %1, unmuted %2").arg(mutedPeak).arg(openPeak));
            // The switches stay with the project.
            click(find(window, "trackSwitch_v2"));
            if (!editor.openProject(project)) return fail(149, "reopening: " + editor.status());
            QTest::qWait(300);
            if (editor.hiddenTracks() != QStringList{"lyrics", "v2"} || !editor.mutedTracks().isEmpty())
                return fail(149, "after reopening, hidden " + editor.hiddenTracks().join(",") + " muted " + editor.mutedTracks().join(","));
            editor.setTrackHidden("lyrics", false);
            editor.setTrackHidden("v2", false);
        }
        flows->remove(syncId);
        flows->remove(refId);
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
