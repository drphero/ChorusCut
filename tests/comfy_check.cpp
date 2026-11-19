// Drives ComfyUI lyric alignment with real mouse and keyboard input against tests/comfy_stub.py:
// connect, Align, import, resume a job after closing and reopening the project, cancel,
// a failing job, and an unreachable server.
// usage: ChorusCutComfyCheck PROJECT_COPY ALIGNMENT_JSON STUB_SCRIPT
//        ChorusCutComfyCheck --live URL PROJECT_COPY [LANGUAGE]   (headless run against a real ComfyUI)
#include "editor.h"
#include "library.h"
#include "waveform.h"
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QClipboard>
#include <QSqlQuery>
#include <qt6keychain/keychain.h>
#include <QStyleHints>
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
static void typeText(QWindow* window, const QString& text) {
    for (const QChar c : text) QTest::keyClick(window, c.toLatin1());
}
static int fail(int code, const QString& why) { qCritical().noquote() << "FAIL" << code << why; return code; }

// Real ComfyUI with the node pack: align the project's saved lyrics and import the result.
static int live(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    app.setOrganizationName("Chorus Cut Tests");
    app.setApplicationName("Comfy Live");
    Mlt::Factory::init();
    Editor editor;
    if (!editor.openProject(QUrl::fromLocalFile(QFileInfo(argv[3]).absoluteFilePath()))) return fail(2, editor.status());
    auto comfy = editor.comfy();
    comfy->configure(argv[2], {}, true);
    comfy->testConnection();
    if (!waitFor([&] { return comfy->connection() != "checking" && comfy->connection() != "unknown"; }) || comfy->connection() != "ready")
        return fail(3, comfy->connectionText());
    qInfo().noquote() << comfy->connectionText();
    const auto before = editor.lyrics()->lines();
    QString last;
    bool done = false, ok = false;
    QObject::connect(comfy, &ComfyJobs::finished, [&](bool success) { done = true; ok = success; });
    comfy->alignLyrics(argc > 4 ? argv[4] : "en");
    waitFor([&] {
        if (comfy->jobText() != last) { last = comfy->jobText(); qInfo().noquote() << last; }
        return done;
    }, 30 * 60 * 1000);
    if (!ok) return fail(4, comfy->jobText());
    int moved = 0;
    const auto after = editor.lyrics()->lines();
    for (int i = 0; i < qMin(before.size(), after.size()); ++i)
        moved += std::abs(before[i].toMap()["start"].toInt() - after[i].toMap()["start"].toInt()) > 300;
    qInfo().noquote() << QString("PASS live: %1 lines, %2 to check, %3 line starts differ from before by more than 0.3 s")
                             .arg(after.size()).arg(editor.lyrics()->flaggedLines()).arg(moved);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 4 && QByteArray(argv[1]) == "--live") return live(argc, argv);
    if (argc != 4) { qCritical() << "usage: ChorusCutComfyCheck PROJECT_COPY ALIGNMENT_JSON STUB_SCRIPT"; return 1; }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    qputenv("SDL_AUDIODRIVER", "disk"); qputenv("SDL_AUDIO_DRIVER", "disk");
    qputenv("SDL_AUDIO_DISK_OUTPUT_FILE", QFile::encodeName(QDir(argv[1]).filePath("../comfy-check.raw")));
    QGuiApplication app(argc, argv);
    app.setOrganizationName("Chorus Cut Tests");
    app.setApplicationName("Comfy Check");
    { QSettings s; s.clear(); }
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    Mlt::Factory::init();

    // The stand-in ComfyUI server.
    const int port = 18000 + int(QCoreApplication::applicationPid() % 1000);
    const QString stubUrl = QString("http://127.0.0.1:%1").arg(port);
    QProcess stub;
    stub.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    stub.start("python3", {argv[3], QString::number(port), QFileInfo(argv[2]).absoluteFilePath()});
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
    if (!waitFor([&] { return !call("/system_stats").contains("unreachable"); }, 10000)) return fail(1, "stub did not start");
    // Reads this test's own keychain entry ("Comfy Check" service), as the app would.
    auto keychainToken = [&](const QString& server) {
        QKeychain::ReadPasswordJob job(QCoreApplication::applicationName());
        job.setAutoDelete(false);
        job.setKey("ComfyUI token " + server);
        QEventLoop loop;
        QObject::connect(&job, &QKeychain::Job::finished, &loop, &QEventLoop::quit);
        job.start();
        loop.exec();
        return job.error() == QKeychain::NoError ? job.textData() : QString();
    };
    // An older build left a plain-text token in the settings; it moves to the keychain.
    { QSettings s; s.setValue("comfyui/url", stubUrl); s.setValue("comfyui/token", "legacy-token"); }
    // New projects go to a scratch library beside the project copy, never the user's.
    const QString scratchLibrary = QDir(QFileInfo(argv[1]).absolutePath()).filePath("Library");
    QDir().mkpath(scratchLibrary);
    { QSettings s; s.setValue("projects/libraries", QStringList{scratchLibrary}); s.setValue("projects/current", 0); }
    auto stubLog = [&] { return call("/_stub/log"); };
    auto mode = [&](const QString& m, int shift) { call("/_stub/mode", {{"mode", m}, {"shift", shift}}, true); };

    Editor editor;
    if (!waitFor([&] { return editor.comfy()->hasToken() && keychainToken(stubUrl) == "legacy-token"; }, 5000) ||
        QSettings().contains("comfyui/token"))
        return fail(2, "the plain-text token was not moved to the keychain");
    const QUrl project = QUrl::fromLocalFile(QFileInfo(argv[1]).absoluteFilePath());
    if (!editor.openProject(project)) return fail(2, editor.status());
    auto lyrics = editor.lyrics();
    QFile in(argv[2]);
    if (!in.open(QIODevice::ReadOnly)) return fail(2, "cannot read the alignment");
    const auto fixture = QJsonDocument::fromJson(in.readAll()).object()["words"].toArray();
    auto firstStart = [&] {
        const auto lines = lyrics->lines();
        return lines.isEmpty() ? -1 : lines[0].toMap()["words"].toList()[0].toMap()["start"].toInt();
    };
    const int base = fixture[0].toObject()["start"].toInt();

    qmlRegisterType<Monitor>("Chorus", 1, 0, "VideoMonitor");
    qmlRegisterType<WaveformView>("Chorus", 1, 0, "WaveformView");
    ProjectLibrary library(&editor);  // Before the engine, so QML never outlives it.
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("editor", &editor);
    engine.rootContext()->setContextProperty("library", &library);
    engine.load(QUrl("qrc:/src/Main.qml"));
    if (engine.rootObjects().isEmpty()) return fail(3, "QML did not load");
    auto window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    window->resize(1280, 820);
    if (!QTest::qWaitForWindowExposed(window)) return fail(3, "window not exposed");
    const int gap = QGuiApplication::styleHints()->mouseDoubleClickInterval() + 100;
    auto click = [&](const QString& name) {
        if (!waitFor([&] { return find(window, name) != nullptr; }, 3000)) return false;
        auto item = find(window, name);
        if (!item->isEnabled()) return false;
        QTest::mouseClick(window, Qt::LeftButton, {}, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        QTest::qWait(gap);
        return true;
    };
    auto comfy = editor.comfy();
    // CHORUS_SHOTS=DIR saves window captures along the way, for looking at the layout.
    auto shot = [&](const char* name) {
        const QString dir = qEnvironmentVariable("CHORUS_SHOTS");
        if (!dir.isEmpty()) { QTest::qWait(400); window->grabWindow().save(QDir(dir).filePath(QString(name) + ".png")); }
    };
    auto jobState = [&] {
        QSqlQuery q(editor.current()->db);
        return q.exec("SELECT state FROM jobs ORDER BY submitted DESC, rowid DESC LIMIT 1") && q.next() ? q.value(0).toString() : QString();
    };

    // 1. An unreachable server is reported in the sheet.
    if (!click("comfyButton") || !click("comfyUrl")) return fail(4, "no ComfyUI sheet");
    QTest::keySequence(window, QKeySequence::SelectAll);
    typeText(window, "http://127.0.0.1:9");
    if (!click("comfyConnect") || !waitFor([&] { return comfy->connection() == "unreachable"; })) return fail(4, "unreachable server not reported");

    // 2. Connect to the stub with a token, by keyboard.
    click("comfyUrl");
    QTest::keySequence(window, QKeySequence::SelectAll);
    typeText(window, stubUrl);
    click("comfyToken");
    typeText(window, "secret-token");
    QTest::keyClick(window, Qt::Key_Return);
    if (!waitFor([&] { return comfy->connection() == "ready"; })) return fail(5, "did not connect: " + comfy->connectionText());
    if (!comfy->hasToken() || comfy->url() != stubUrl) return fail(5, "settings not kept");
    if (!waitFor([&] { return keychainToken(stubUrl) == "secret-token"; }, 5000) || QSettings().contains("comfyui/token"))
        return fail(5, "the token should be in the keychain and nowhere in the settings");
    shot("comfy-sheet");
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::qWait(200);

    // 3. Align: upload, queue, import. The stub shifts every word by 40 ms so the import is visible.
    const QDir jobs(QFileInfo(argv[1]).absoluteFilePath() + "/Jobs");
    const auto savedBefore = jobs.entryList({"alignment-*.json"}).size();  // the copied project may have earlier jobs
    mode("finish", 40);
    if (!click("openLyrics") || !click("alignComfy")) return fail(6, "no Align button");
    if (!waitFor([&] { return !comfy->busy() && firstStart() == base + 40; }, 15000))
        return fail(6, "alignment not imported: " + comfy->jobText());
    if (find(window, "lyricsEditor")) return fail(6, "the lyrics window should close after a successful alignment");
    auto log = stubLog();
    const auto prompt = log["prompts"].toArray().last().toObject();
    if (prompt["2"].toObject()["inputs"].toObject()["lyrics"].toString() != lyrics->text() ||
        prompt["2"].toObject()["inputs"].toObject()["language"].toString() != "en" ||
        prompt["1"].toObject()["inputs"].toObject()["audio"].toString() != log["uploads"].toArray().last().toObject()["name"].toString())
        return fail(7, "the queued graph does not match the song and lyrics");
    if (log["uploads"].toArray().last().toObject()["auth"].toString() != "Bearer secret-token") return fail(7, "token not sent");
    if (jobState() != "done" || jobs.entryList({"alignment-*.json"}).size() != savedBefore + 1)
        return fail(7, "job not recorded or result not saved in Jobs/");

    // 4. A job survives closing and reopening the project, and finishes afterwards.
    mode("hold", 80);
    if (!click("openLyrics") || !click("alignComfy")) return fail(8, "Align again");
    if (!waitFor([&] { return jobState() == "running"; })) return fail(8, "job did not start: " + comfy->jobText());
    // Live progress arrives over the WebSocket as a percentage.
    if (!waitFor([&] { return comfy->jobText().contains("%"); }, 5000)) return fail(8, "no live percentage: " + comfy->jobText());
    QTest::keyClick(window, Qt::Key_Escape);
    editor.closeProject();
    if (comfy->busy()) return fail(8, "closing the project should stop watching the job");
    if (!editor.openProject(project)) return fail(8, editor.status());
    if (!waitFor([&] { return comfy->busy(); }, 3000)) return fail(8, "reopening did not resume the job");
    // The reopened project listens with the job's own client ID, so progress keeps coming.
    if (!waitFor([&] { return comfy->jobText().contains("%"); }, 5000)) return fail(8, "no percentage after reopening: " + comfy->jobText());
    if (stubLog()["sockets"].toArray().size() < 2 ||
        stubLog()["sockets"].toArray().last().toString() != stubLog()["sockets"].toArray()[stubLog()["sockets"].toArray().size() - 2].toString())
        return fail(8, "the reopened project should listen with the same client ID");
    call("/_stub/release", {}, true);
    if (!waitFor([&] { return !comfy->busy() && firstStart() == base + 80; }, 15000)) return fail(8, "resumed job not imported: " + comfy->jobText());

    // 5. Cancel removes a running job from ComfyUI.
    mode("hold", 120);
    if (!click("openLyrics") || !click("alignComfy")) return fail(9, "Align for cancel");
    if (!waitFor([&] { return jobState() == "running"; })) return fail(9, "job did not start");
    shot("comfy-running");
    auto cancel = window->findChildren<QQuickItem*>();
    QQuickItem* cancelButton = nullptr;
    for (auto item : cancel) if (item->isVisible() && item->property("text").toString() == "Cancel alignment") cancelButton = item;
    if (!cancelButton) return fail(9, "no Cancel alignment button");
    QTest::mouseClick(window, Qt::LeftButton, {}, cancelButton->mapToScene(QPointF(cancelButton->width() / 2, cancelButton->height() / 2)).toPoint());
    if (!waitFor([&] { return !comfy->busy() && jobState() == "cancelled"; }, 3000)) return fail(9, "cancel did not stop the job");
    if (!waitFor([&] { return stubLog()["interrupted"].toInt() >= 1 && stubLog()["deleted"].toArray().size() >= 1; }, 3000))
        return fail(9, "cancel did not reach ComfyUI");
    if (firstStart() != base + 80) return fail(9, "a cancelled job changed the timings");

    // 6. A failing job shows ComfyUI's message and leaves the timings alone.
    mode("error", 160);
    if (!click("alignComfy")) return fail(10, "Align for failure");
    if (!waitFor([&] { return !comfy->busy() && jobState() == "failed"; }, 10000)) return fail(10, "failure not detected");
    if (!comfy->jobText().contains("Stub failure for testing") || !find(window, "comfyJobText")) return fail(10, "failure message not shown: " + comfy->jobText());
    if (firstStart() != base + 80) return fail(10, "a failed job changed the timings");

    // Forget token removes it from the keychain.
    auto clickText = [&](const QString& text) {
        QQuickItem* found = nullptr;
        for (auto item : window->findChildren<QQuickItem*>())
            if (item->isVisible() && item->isEnabled() && item->property("text").toString() == text) found = item;
        if (!found) return false;
        QTest::mouseClick(window, Qt::LeftButton, {}, found->mapToScene(QPointF(found->width() / 2, found->height() / 2)).toPoint());
        QTest::qWait(gap);
        return true;
    };
    if (!clickText("Done") || !waitFor([&] { return !find(window, "lyricsEditor"); }, 2000)) return fail(11, "could not close the lyrics window");

    // New Project with pasted lyrics opens the project and aligns it on ComfyUI by itself.
    const QString lyricsText = lyrics->text();
    const QString song = editor.current()->file(editor.current()->master);
    mode("finish", 200);
    QTest::keySequence(window, QKeySequence::Open);
    if (!click("newProject")) return fail(12, "no New Project button");
    window->findChild<QObject*>("newProjectSheet")->setProperty("master", QUrl::fromLocalFile(song));  // as the file dialog would
    if (!click("newProjectName")) return fail(12, "no name field");
    QTest::keySequence(window, QKeySequence::SelectAll);
    typeText(window, "Auto Aligned");
    if (!click("newProjectLyrics")) return fail(12, "no lyrics box");
    QGuiApplication::clipboard()->setText(lyricsText);
    QTest::keySequence(window, QKeySequence::Paste);
    if (find(window, "newProjectLyrics")->property("text").toString() != lyricsText) return fail(12, "paste did not fill the lyrics");
    shot("new-project-lyrics");
    if (!click("createProject")) return fail(12, "Create");
    if (!waitFor([&] { return editor.projectName() == "Auto Aligned" && !comfy->busy() && firstStart() == base + 200; }, 20000))
        return fail(12, "the new project was not aligned automatically: " + comfy->jobText() + " / " + editor.status());
    if (lyrics->text() != lyricsText) return fail(12, "lyrics not saved in the new project");

    if (!click("comfyButton")) return fail(11, "ComfyUI sheet for Forget token");
    if (!clickText("Forget token")) return fail(11, "no Forget token button");
    if (!waitFor([&] { return !comfy->hasToken() && keychainToken(stubUrl).isEmpty(); }, 5000)) return fail(11, "token not forgotten");

    stub.kill();
    stub.waitForFinished();
    qInfo() << "PASS: token moved to the keychain, unreachable, connect with token, align and import, live progress, resume after reopen"
               " with progress, cancel, failure, new project with lyrics aligns itself, forget token.";
    return 0;
}
