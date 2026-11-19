#include "editor.h"
#include "waveform.h"
#include "library.h"
#include <QQuickWindow>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QTimer>
#include <QFileInfo>
#include <QDebug>
#include <QQuickStyle>

int main(int argc,char** argv) {
    QGuiApplication app(argc,argv);
    app.setApplicationName("Chorus Cut");
    app.setOrganizationName("Chorus Cut");  // QSettings: remembered track heights
    QQuickStyle::setStyle("Basic");
    QStringList args=app.arguments();
    if (args.contains("--check-playback")) {
        // Playback checks must never open a physical audio device.
        qputenv("SDL_AUDIODRIVER", "disk");
        qputenv("SDL_AUDIO_DRIVER", "disk");
    }
    Mlt::Factory::init();
    // An interactive launch opens the Project Manager. Scripted checks open a project directly:
    // --project PATH, or the Example beside a development build.
    const bool scripted = args.contains("--check") || args.contains("--export") || args.contains("--check-playback") ||
                          args.contains("--import-alignment");
    QString project = scripted ? QDir(QCoreApplication::applicationDirPath()).absoluteFilePath("../../../Example") : QString();
    for (int i=1;i+1<args.size();++i)
        if (args[i]=="--project") project=QFileInfo(args[i+1]).absoluteFilePath();
    Editor editor;
    if (!project.isEmpty() && !editor.openProject(QUrl::fromLocalFile(QDir::cleanPath(project)))) {
        if (args.contains("--project") || scripted) { qCritical().noquote()<<editor.status(); return 1; }
    }
    ProjectLibrary library(&editor);
    const int importAt = args.indexOf("--import-alignment");
    if (importAt >= 0) {
        // Load a GPU alignment (alignment/experiments/align_v2.py output) into the project.
        const bool ok = editor.lyrics()->importAlignment(QUrl::fromLocalFile(QFileInfo(args.value(importAt + 1)).absoluteFilePath()));
        qInfo().noquote() << editor.lyrics()->message();
        if (!ok) return 11;
        if (!args.contains("--screenshot") && !args.contains("--check-playback")) return 0;
    }
    if (args.contains("--export")) return editor.exportTimeline(editor.defaultExport()) ? 0 : 8;
    if (args.contains("--check")) {
        // Run on a disposable copy of the example project.
        if (!editor.checkMedia()) { qCritical()<<"Preview frame or audio mismatch"; return 9; }
        int start=editor.cutIn(), end=editor.cutOut();
        QImage before=editor.imageAt(end);
        editor.change("out",8);
        if (editor.cutOut()!=end+8 || before==editor.imageAt(end) || before.isNull()) return 2;
        editor.undo();
        if (editor.cutOut()!=end || before!=editor.imageAt(end)) return 3;
        editor.redo(); editor.undo();
        if (editor.cutIn()!=start || editor.cutOut()!=end || !editor.exportTimeline(editor.defaultExport())) return 4;
        return 0;
    }
    qmlRegisterType<Monitor>("Chorus",1,0,"VideoMonitor");
    qmlRegisterType<WaveformView>("Chorus",1,0,"WaveformView");
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("editor",&editor);
    engine.rootContext()->setContextProperty("library",&library);
    engine.load(QUrl("qrc:/src/Main.qml"));
    if (engine.rootObjects().isEmpty()) return 5;
    auto monitor=engine.rootObjects().first()->findChild<Monitor*>("monitor");
    if (!monitor) return 6;
    QObject::connect(&editor,&Editor::imageReady,monitor,&Monitor::showImage);
    // Agents (Claude Code, Codex) reach the open project through choruscut-mcp; not in scripted runs.
    if (!args.contains("--screenshot") && !args.contains("--check-playback")) editor.agent()->start();
    editor.seek(0);
    const int shotAt = args.indexOf("--screenshot");
    if (shotAt >= 0) {
        // Render the window offscreen and save it, for checking the layout without a display.
        const QString path = QFileInfo(args.value(shotAt + 1)).absoluteFilePath();
        const int seekAt = args.indexOf("--seek-ms");
        if (seekAt >= 0) editor.seekLyric(args.value(seekAt + 1).toInt());
        auto window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
        const int delayAt = args.indexOf("--screenshot-delay");
        const int delay = delayAt >= 0 ? args.value(delayAt + 1).toInt() : 4000;
        const int zoomAt = args.indexOf("--zoom-ms");
        if (zoomAt >= 0 && window) {
            // "--zoom-ms START,END" frames the timeline on a passage before the capture.
            const auto range = args.value(zoomAt + 1).split(',');
            QTimer::singleShot(300, window, [window, range] {
                if (auto timeline = window->findChild<QObject*>("timeline"))
                    QMetaObject::invokeMethod(timeline, "zoomTo", Q_ARG(QVariant, range.value(0).toInt()), Q_ARG(QVariant, range.value(1).toInt()));
            });
        }
        // "--workspace plan", "--plan-page arc" and "--plan-link section:3" open the Plan workspace
        // on a page with a card's links showing.
        if (window) {
            const int workspaceAt = args.indexOf("--workspace"), pageAt = args.indexOf("--plan-page"), linkAt = args.indexOf("--plan-link");
            if (workspaceAt >= 0) window->setProperty("workspace", args.value(workspaceAt + 1));
            if (auto plan = window->findChild<QObject*>("planWorkspace")) {
                if (pageAt >= 0) plan->setProperty("page", args.value(pageAt + 1));
                const auto link = args.value(linkAt + 1).split(':');
                if (linkAt >= 0 && link.size() == 2)
                    plan->setProperty("linked", QVariantMap{{"kind", link[0]}, {"id", link[1].toInt()}});
            }
        }
        QTimer::singleShot(delay, &app, [window, path, &app] {
            app.exit(window && window->grabWindow().save(path) ? 0 : 12);
        });
        return app.exec();
    }
    if (args.contains("--check-playback")) {
        int option = args.indexOf("--check-monitor-db");
        if (option >= 0) {
            bool ok = false;
            int db = args.value(option + 1).toInt(&ok);
            if (!ok || db < -60 || db > 0) return 10;
            editor.setMonitorDb(db);
            editor.setMonitorMuted(false);
        }
        if (args.contains("--check-mute")) editor.setMonitorMuted(true);
        QObject::connect(&editor,&Editor::cursorChanged,&app,[&]{
            if (editor.frame()==editor.length()-1) app.exit(0);
        });
        QTimer::singleShot(300,&editor,&Editor::togglePlay);
        QTimer::singleShot(15000,&app,[&]{app.exit(7);});
    }
    return app.exec();
}
