// Song-only intake with real input: open the lyrics editor, paste with the keyboard,
// align on this Mac, click a lyric line on the timeline, play the selection to disk, reopen.
// usage: ChorusCutLyricsCheck NEW_PROJECT SONG LYRICS_TEXT DISK_CAPTURE [PERFORMANCE CUTAWAY]
#include "editor.h"
#include "waveform.h"
#include "library.h"
#include <QClipboard>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTest>
#include <QDebug>
#include <cmath>
#include <functional>

static bool waitFor(const std::function<bool()>& ready, int timeout = 10000) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < timeout) { if (ready()) return true; QTest::qWait(20); }
    return ready();
}
// Searches the visual item tree (Repeater delegates live only there) and the window's
// object tree (popup contents live in the overlay).
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
static int fail(int code, const QString& why) { qCritical().noquote() << "FAIL" << code << why; return code; }

int main(int argc, char** argv) {
    if (argc != 5 && argc != 7) { qCritical() << "Usage: LyricsCheck NEW_PROJECT SONG LYRICS_TEXT DISK_CAPTURE [PERFORMANCE CUTAWAY]"; return 1; }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    qputenv("SDL_AUDIO_DRIVER", "disk"); qputenv("SDL_AUDIODRIVER", "disk");
    qputenv("SDL_AUDIO_DISK_OUTPUT_FILE", QFile::encodeName(argv[4]));
    QGuiApplication app(argc, argv);
    Mlt::Factory::init();
    Editor editor;
    QFileInfo dest(argv[1]);
    auto url = [](QString path) { return QUrl::fromLocalFile(QFileInfo(path).absoluteFilePath()); };
    editor.createProject(url(dest.absolutePath()), dest.fileName(), url(argv[2]),
                         argc == 7 ? url(argv[5]) : QUrl(), argc == 7 ? url(argv[6]) : QUrl());
    if (!waitFor([&] { return !editor.importing(); }) || !editor.ready()) return fail(2, editor.status());
    if (editor.monitorMuted() || editor.monitorDb() != 0) return fail(2, "a new project should play at unity");
    qmlRegisterType<Monitor>("Chorus", 1, 0, "VideoMonitor");
    qmlRegisterType<WaveformView>("Chorus", 1, 0, "WaveformView");
    QQmlApplicationEngine engine;
    ProjectLibrary library(&editor);
    engine.rootContext()->setContextProperty("editor", &editor);
    engine.rootContext()->setContextProperty("library", &library);
    engine.load(QUrl("qrc:/src/Main.qml"));
    if (engine.rootObjects().empty()) return fail(3, "QML did not load");
    auto window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    window->resize(1280, 820);
    if (!QTest::qWaitForWindowExposed(window)) return fail(3, "window not exposed");
    QQuickWindow* root = window;
    auto click = [&](QQuickItem* item) {
        QTest::mouseClick(window, Qt::LeftButton, {}, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        QTest::qWait(150);
    };
    if (argc == 7) { editor.change("in", 3); editor.change("slip", 5); }
    const int in = editor.cutIn(), out = editor.cutOut(), offset = editor.sourceIn();

    // Open the lyrics editor and paste with the keyboard.
    auto open = find(root, "openLyrics");
    if (!open) return fail(4, "no Lyrics… button");
    click(open);
    auto draft = find(root, "lyricDraft");
    QFile file(argv[3]);
    if (!draft || !file.open(QIODevice::ReadOnly)) return fail(4, "lyrics editor did not open");
    const auto lyrics = QString::fromUtf8(file.readAll());
    app.clipboard()->setText(lyrics);
    click(draft);
    QTest::keySequence(window, QKeySequence::Paste);
    if (draft->property("text").toString() != lyrics) return fail(5, "paste did not fill the lyrics");
    auto align = find(root, "alignWords");
    if (!align || !align->isEnabled()) return fail(6, "Align button missing or disabled");
    click(align);
    if (!waitFor([&] { return !editor.lyrics()->busy(); }, 300000)) return fail(7, "alignment did not finish");
    const auto lines = editor.lyrics()->lines();
    if (lines.size() < 2) return fail(8, editor.lyrics()->message());

    // Click the second lyric line on the timeline: it is selected and the playhead jumps to it.
    if (!waitFor([&] { return find(root, "lyricClip1") != nullptr && !find(root, "lyricsEditor"); })) return fail(9, "no clip for line 2");
    auto timeline = find(root, "timeline");
    auto line = lines[1].toMap();
    QMetaObject::invokeMethod(timeline, "zoomTo", Q_ARG(QVariant, line["start"].toInt() - 3000), Q_ARG(QVariant, line["end"].toInt() + 3000));
    QTest::qWait(150);
    auto clip = find(root, "lyricClip1");
    QTest::mouseClick(window, Qt::LeftButton, {}, clip->mapToScene(QPointF(clip->width() / 2, 8)).toPoint());
    QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 100);
    if (editor.frame() != int(std::floor(line["start"].toInt() * editor.fps() / 1000.))) return fail(10, "click did not seek to the line");

    // Play the selection from the inspector; it stops on the line's last frame.
    auto play = find(root, "playSelection");
    if (!play) return fail(11, "no Play button for the selection");
    click(play);
    if (!editor.playing()) return fail(11, "Play did not start");
    if (!waitFor([&] { return !editor.playing(); }, 30000) ||
        editor.frame() != int(std::ceil(line["end"].toInt() * editor.fps() / 1000.)) - 1) return fail(12, "playback did not stop at the line end");
    if (!editor.openProject(url(argv[1])) || editor.lyrics()->lines() != lines || editor.lyrics()->text() != lyrics) return fail(13, "reopen differs");
    if (argc == 5) { qInfo() << "PASS: song-only paste, local alignment, line click, play selection and reopen."; return 0; }

    // With picture: a word correction keeps the cut and its source offset.
    auto w = line["words"].toList().first().toMap();
    int index = w["index"].toInt(), start = w["start"].toInt(), end = w["end"].toInt();
    if (editor.lyrics()->correct(index, end, start) || !editor.lyrics()->correct(index, start + 1, end) ||
        !editor.openProject(url(argv[1]))) return fail(14, "word correction");
    auto saved = editor.lyrics()->lines()[1].toMap()["words"].toList().first().toMap();
    if (saved["start"].toInt() != start + 1 || !saved["manual"].toBool() ||
        editor.cutIn() != in || editor.cutOut() != out || editor.sourceIn() != offset) return fail(15, "cut changed");
    if (!editor.lyrics()->saveText(lyrics + "\nA different line") || !editor.lyrics()->lines().empty() ||
        !editor.lyrics()->saveText(lyrics) || editor.lyrics()->lines().empty()) return fail(16, "text change/restore");
    qInfo() << "PASS: paste, align, line click, play selection, reopen; correction kept the cut" << lines.size() << "lines.";
    return 0;
}
