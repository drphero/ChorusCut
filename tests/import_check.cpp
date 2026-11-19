#include "editor.h"
#include <QGuiApplication>
#include <QDebug>
#include <QEventLoop>
#include <cmath>

// import_project.py supplies an isolated folder and generated media.
int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);
    if (argc != 2) return 1;
    Mlt::Factory::init();
    QDir work(argv[1]);
    auto url = [&](const QString& path) { return QUrl::fromLocalFile(work.absoluteFilePath(path)); };
    Editor editor;
    auto create = [&](const QString& name, const QString& footage) {
        QEventLoop loop;
        bool success = false;
        QObject::connect(&editor, &Editor::importFinished, &loop, [&](bool ok) { success = ok; loop.quit(); });
        editor.createProject(url("."), name, url("Song & voice.wav"),
                                    url("Performance/take.mp4"), url(footage));
        if (editor.importing()) loop.exec();
        return success;
    };
    if (create("Broken","Song & voice.wav") || work.exists("Broken")) return 2;
    if (!create("Imported cut","Cutaway/take.mp4")) { qCritical() << editor.status(); return 3; }
    if (create("Imported cut","Cutaway/take.mp4") || editor.projectName() != "Imported cut") return 4;
    if (editor.length()!=300 || editor.baseLength()!=270 || editor.sourceLength()!=90 ||
        std::abs(editor.fps()-30000./1001)>0.000001 || editor.monitorMuted() || editor.monitorDb()!=0) return 5;
    if (editor.cutIn()!=100 || editor.cutOut()!=160 || !editor.checkPictures()) return 6;
    editor.change("slip",12);
    editor.change("out",8);
    editor.undo();
    if (editor.cutOut()!=160 || !editor.canRedo()) return 7;
    editor.redo();
    if (editor.cutOut()!=168 || editor.sourceIn()!=12 || !editor.checkPictures()) return 8;
    editor.change("out",1000);
    if (editor.cutOut()!=178) return 9;
    editor.undo();
    editor.change("move",1000);
    if (editor.cutOut()!=300 || !editor.checkPictures()) return 10;
    editor.undo();
    if (!editor.openProject(url("Imported cut")) || editor.canUndo() || editor.frame()!=0 ||
        editor.length()!=300 || editor.cutIn()!=100 || editor.cutOut()!=168 || editor.sourceIn()!=12 ||
        !editor.checkPictures() || !editor.exportTimeline(editor.defaultExport())) return 11;
    return 0;
}
