// Drives the Project Manager with real mouse and keyboard events at on-screen positions:
// launch into the manager, search, open by double-click, ⌘O, rename the open project,
// duplicate, trash (redirected away from the user's Trash), create, Esc and Open.
// usage: ChorusCutProjectsCheck WORK_DIR SONG PERFORMANCE CUTAWAY
#include "editor.h"
#include "library.h"
#include "waveform.h"
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
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
// QTest::keyClicks only takes widgets; type into a window one key at a time.
static void typeText(QWindow* window, const char* text) {
    for (const char* c = text; *c; ++c) QTest::keyClick(window, *c);
}
static int fail(int code, const QString& why) { qCritical().noquote() << "FAIL" << code << why; return code; }

int main(int argc, char** argv) {
    if (argc != 5) { qCritical() << "usage: ChorusCutProjectsCheck WORK_DIR SONG PERFORMANCE CUTAWAY"; return 1; }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    qputenv("SDL_AUDIODRIVER", "disk"); qputenv("SDL_AUDIO_DRIVER", "disk");
    const QDir work(QFileInfo(argv[1]).absoluteFilePath());
    const QString lib = work.filePath("Library"), trash = work.filePath("Trash");
    if (!QDir().mkpath(lib) || !QDir().mkpath(trash)) return fail(1, "cannot prepare the work folder");
    qputenv("CHORUS_TRASH_DIR", QFile::encodeName(trash));
    qputenv("SDL_AUDIO_DISK_OUTPUT_FILE", QFile::encodeName(work.filePath("audio.raw")));
    QGuiApplication app(argc, argv);
    // Own settings, so the test never changes the real app's library list.
    app.setOrganizationName("ChorusCut Tests");
    app.setApplicationName("Projects Check");
    { QSettings s; s.clear(); s.setValue("projects/libraries", QStringList{lib}); s.setValue("projects/current", 0); }
    Mlt::Factory::init();
    const QString song = QFileInfo(argv[2]).absoluteFilePath();
    QString error;
    if (!Project::create(QDir(lib).filePath("Alpha"), song, {}, {}, error) ||
        !Project::create(QDir(lib).filePath("Beta"), song, QFileInfo(argv[3]).absoluteFilePath(),
                         QFileInfo(argv[4]).absoluteFilePath(), error)) return fail(1, error);

    Editor editor;
    ProjectLibrary library(&editor);
    qmlRegisterType<Monitor>("Chorus", 1, 0, "VideoMonitor");
    qmlRegisterType<WaveformView>("Chorus", 1, 0, "WaveformView");
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty("editor", &editor);
    engine.rootContext()->setContextProperty("library", &library);
    engine.load(QUrl("qrc:/src/Main.qml"));
    if (engine.rootObjects().empty()) return fail(2, "QML did not load");
    auto window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    window->resize(1280, 820);
    if (!QTest::qWaitForWindowExposed(window)) return fail(2, "window not exposed");
    const int doubleClickGap = QGuiApplication::styleHints()->mouseDoubleClickInterval() + 100;
    auto centre = [&](QQuickItem* item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); };
    auto click = [&](QQuickItem* item, Qt::MouseButton button = Qt::LeftButton) {
        QTest::mouseClick(window, button, {}, centre(item));
        QTest::qWait(doubleClickGap);
    };
    auto clickNamed = [&](const QString& name) {
        if (!waitFor([&] { return find(window, name) != nullptr; }, 3000)) return false;
        click(find(window, name));
        return true;
    };
    // Popups are not items; ask them directly.
    auto popupOpen = [&](const char* name) { auto p = window->findChild<QObject*>(name); return p && p->property("visible").toBool(); };
    // CHORUS_SHOTS=DIR saves window captures along the way, for looking at the layout.
    auto shot = [&](const char* name) {
        const QString dir = qEnvironmentVariable("CHORUS_SHOTS");
        if (!dir.isEmpty()) { QTest::qWait(400); window->grabWindow().save(QDir(dir).filePath(QString(name) + ".png")); }
    };
    auto managerShown = [&] { return find(window, "projectManager") != nullptr; };
    auto exists = [&](const QString& name) { return QFileInfo::exists(QDir(lib).filePath(name) + "/project.sqlite"); };

    // Launch: no project is open, so the manager covers the window and lists both projects.
    if (!managerShown()) return fail(3, "the manager is not shown at launch");
    if (!waitFor([&] { return find(window, "projectCard_Alpha") && find(window, "projectCard_Beta"); }))
        return fail(3, "both project cards should be listed");
    if (find(window, "closeManager")) return fail(3, "there is no project to go back to");
    shot("1-launch");

    // Search narrows the cards; Esc in the field clears it.
    if (!clickNamed("projectSearch")) return fail(4, "no search field");
    typeText(window, "bet");
    if (!waitFor([&] { return !find(window, "projectCard_Alpha") && find(window, "projectCard_Beta"); }, 2000))
        return fail(4, "search did not filter to Beta");
    QTest::keyClick(window, Qt::Key_Escape);
    if (!waitFor([&] { return find(window, "projectCard_Alpha") != nullptr; }, 2000)) return fail(4, "Esc did not clear the search");

    // Double-click opens a project and returns to the editor.
    QTest::mouseDClick(window, Qt::LeftButton, {}, centre(find(window, "projectCard_Alpha")));
    if (!waitFor([&] { return editor.ready() && editor.projectName() == "Alpha" && !managerShown(); }))
        return fail(5, "double-click did not open Alpha");
    const QString thumb = ProjectLibrary::thumbnailPath(QDir(lib).filePath("Alpha"));
    if (!waitFor([&] { return QFileInfo::exists(thumb); })) return fail(5, "no thumbnail was written");

    // ⌘O brings the manager back with the open project marked.
    QTest::keySequence(window, QKeySequence::Open);
    if (!waitFor(managerShown, 2000)) return fail(6, "⌘O did not show the manager");
    bool marked = false;
    for (const auto& p : library.projects()) {
        const auto card = p.toMap();
        if (card["name"] == "Alpha") marked = card["open"].toBool() && !card["thumbnail"].toUrl().isEmpty();
    }
    if (!marked || !find(window, "closeManager")) return fail(6, "Alpha should be marked open with a thumbnail");

    // Rename the open project from the card's menu: it reopens under the new name.
    click(find(window, "projectCard_Alpha"));
    if (!clickNamed("cardMenu_Alpha") || !clickNamed("menuRename")) return fail(7, "no Rename in the card menu");
    if (!waitFor([&] { return find(window, "renameField") != nullptr; }, 2000)) return fail(7, "rename sheet did not open");
    QTest::keySequence(window, QKeySequence::SelectAll);
    typeText(window, "Alpha Mix");
    QTest::keyClick(window, Qt::Key_Return);
    if (!waitFor([&] { return exists("Alpha Mix") && !exists("Alpha") && editor.projectName() == "Alpha Mix"; }))
        return fail(7, "rename did not move the folder and reopen it: " + library.message());

    // A name already in the library cannot be used.
    if (!waitFor([&] { return find(window, "projectCard_Beta") != nullptr; })) return fail(8, "Beta disappeared");
    click(find(window, "projectCard_Beta"), Qt::RightButton);
    if (!clickNamed("menuRename")) return fail(8, "right-click menu has no Rename");
    if (!waitFor([&] { return find(window, "renameField") != nullptr; }, 2000)) return fail(8, "rename sheet did not open");
    QTest::keySequence(window, QKeySequence::SelectAll);
    typeText(window, "alpha mix");
    if (find(window, "renameAccept")->isEnabled()) return fail(8, "a duplicate name should disable Rename");
    QTest::keyClick(window, Qt::Key_Return);
    QTest::qWait(200);
    if (!exists("Beta")) return fail(8, "Return renamed despite the clash");
    QTest::keyClick(window, Qt::Key_Escape);
    if (!waitFor([&] { return !popupOpen("renameSheet"); }, 2000)) return fail(8, "Esc did not close the sheet");

    // Duplicate copies the whole folder under a free name.
    click(find(window, "projectCard_Beta"), Qt::RightButton);
    if (!clickNamed("menuDuplicate")) return fail(9, "no Duplicate");
    if (!waitFor([&] { return !library.busy() && exists("Beta copy") && find(window, "projectCard_Beta copy"); }, 20000))
        return fail(9, "duplicate did not appear: " + library.message());
    if (QFileInfo(QDir(lib).filePath("Beta copy/Media/Performance/") + QFileInfo(argv[3]).fileName()).size() !=
        QFileInfo(argv[3]).size()) return fail(9, "duplicate is missing its media");

    // ⌘⌫ asks before trashing; Cancel keeps it, Move to Trash removes it.
    click(find(window, "projectCard_Beta copy"));
    QTest::keyClick(window, Qt::Key_Backspace, Qt::ControlModifier);
    if (!clickNamed("trashCancel") || !exists("Beta copy")) return fail(10, "Cancel should keep the project");
    QTest::keyClick(window, Qt::Key_Backspace, Qt::ControlModifier);
    if (!clickNamed("trashAccept")) return fail(10, "no confirmation for ⌘⌫");
    if (!waitFor([&] { return !exists("Beta copy") && !find(window, "projectCard_Beta copy"); }))
        return fail(10, "Beta copy was not trashed");
    if (QDir(trash).entryList(QDir::Dirs | QDir::NoDotAndDotDot).size() != 1) return fail(10, "trash redirect missing");

    // Trashing the open project closes it; the manager stays up with nothing to return to.
    if (!waitFor([&] { return find(window, "projectCard_Alpha Mix") != nullptr; })) return fail(11, "no Alpha Mix card");
    click(find(window, "projectCard_Alpha Mix"));
    if (!clickNamed("cardMenu_Alpha Mix")) return fail(11, "no menu on Alpha Mix");
    if (!clickNamed("menuTrash") || !clickNamed("trashAccept")) return fail(11, "could not trash the open project");
    if (!waitFor([&] { return !editor.ready() && !exists("Alpha Mix") && managerShown() && !find(window, "closeManager"); }))
        return fail(11, "the open project should close when trashed");

    // New project: the name is prefilled; the song is set as the file dialog would (it cannot be driven).
    if (!clickNamed("newProject")) return fail(12, "no New Project button");
    auto field = find(window, "newProjectName");
    if (!field || field->property("text").toString() != "Untitled Project") return fail(12, "name not prefilled");
    if (!find(window, "chooseSong")) return fail(12, "no Song… button");
    if (find(window, "createProject")->isEnabled()) return fail(12, "Create needs a song");
    // Choosing the song names the project after it while the default name is untouched.
    window->findChild<QObject*>("newProjectSheet")->setProperty("master", QUrl::fromLocalFile(song));
    if (!waitFor([&] { return field->property("text").toString() == QFileInfo(song).completeBaseName(); }, 2000))
        return fail(12, "the project was not named after the song: " + field->property("text").toString());
    shot("4-new-project");
    click(field);
    QTest::keySequence(window, QKeySequence::SelectAll);
    typeText(window, "Gamma");
    if (!clickNamed("createProject")) return fail(12, "Create did not enable");
    if (!waitFor([&] { return editor.ready() && editor.projectName() == "Gamma" && !managerShown(); }, 20000))
        return fail(12, "Gamma was not created and opened: " + library.message());
    if (!exists("Gamma")) return fail(12, "Gamma is not in the library");

    // Esc returns to the open project; the toolbar's Projects button and Open switch projects.
    QTest::keySequence(window, QKeySequence::Open);
    if (!waitFor(managerShown, 2000)) return fail(13, "⌘O after create");
    QTest::keyClick(window, Qt::Key_Escape);
    if (!waitFor([&] { return !managerShown(); }, 2000) || editor.projectName() != "Gamma") return fail(13, "Esc should return to Gamma");
    if (!clickNamed("openManager") || !waitFor(managerShown, 2000)) return fail(14, "Projects button");
    click(find(window, "projectCard_Beta"));
    if (!clickNamed("openSelected")) return fail(14, "no Open button");
    if (!waitFor([&] { return editor.projectName() == "Beta" && editor.sourceLength() > 0 && !managerShown(); }))
        return fail(14, "Open did not switch to Beta");
    // Return opens the selected card from the keyboard.
    QTest::keySequence(window, QKeySequence::Open);
    if (!waitFor(managerShown, 2000)) return fail(15, "⌘O");
    click(find(window, "projectCard_Gamma"));
    shot("2-thumbnails");
    click(find(window, "projectCard_Beta"), Qt::RightButton);
    shot("3-menu");
    QTest::keyClick(window, Qt::Key_Escape);
    QTest::qWait(200);
    click(find(window, "projectCard_Gamma"));
    QTest::keyClick(window, Qt::Key_Return);
    if (!waitFor([&] { return editor.projectName() == "Gamma" && !managerShown(); })) return fail(15, "Return did not open Gamma");

    // The window's size is remembered for the next launch.
    window->resize(1111, 777);
    QTest::qWait(1600);  // The window saves 0.4 s after a change; Settings writes it shortly after.
    window->close();
    QQmlApplicationEngine second;
    second.rootContext()->setContextProperty("editor", &editor);
    second.rootContext()->setContextProperty("library", &library);
    second.load(QUrl("qrc:/src/Main.qml"));
    auto reopened = qobject_cast<QQuickWindow*>(second.rootObjects().value(0));
    if (!reopened || !QTest::qWaitForWindowExposed(reopened) || reopened->width() != 1111 || reopened->height() != 777)
        return fail(16, QString("window size not remembered: %1x%2").arg(reopened ? reopened->width() : 0).arg(reopened ? reopened->height() : 0));

    // The library list is remembered.
    ProjectLibrary again(&editor);
    if (again.libraries().size() != 1 || again.libraries()[0].toMap()["path"] != lib || again.projects().size() != 2)
        return fail(17, "library not remembered");
    qInfo() << "PASS: launch, search, double-click, ⌘O, rename open project, name clash, duplicate, trash, trash open, create (song only, named after it), Esc, Open, Return, window size.";
    return 0;
}
