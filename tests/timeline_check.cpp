// Drives the timeline with real mouse and keyboard events at on-screen positions,
// so a control that is hidden, zero-sized or covered fails the check.
// usage: ChorusCutTimelineCheck PROJECT_COPY ALIGNMENT_JSON DISK_CAPTURE
#include "editor.h"
#include "waveform.h"
#include "library.h"
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QStyleHints>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>
#include <QDebug>
#include <cmath>
#include <functional>

static QQuickItem* find(QQuickItem* root, const QString& name) {
    if (root->objectName() == name && root->isVisible()) return root;
    for (auto child : root->childItems())
        if (auto found = find(child, name)) return found;
    return nullptr;
}
static bool waitFor(const std::function<bool()>& ready, int timeout = 10000) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < timeout) { if (ready()) return true; QTest::qWait(20); }
    return ready();
}
static int fail(int code, const QString& why) { qCritical().noquote() << "FAIL" << code << why; return code; }

int main(int argc, char** argv) {
    if (argc != 4) { qCritical() << "usage: ChorusCutTimelineCheck PROJECT_COPY ALIGNMENT_JSON DISK_CAPTURE"; return 1; }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    qputenv("SDL_AUDIODRIVER", "disk"); qputenv("SDL_AUDIO_DRIVER", "disk");
    qputenv("SDL_AUDIO_DISK_OUTPUT_FILE", QFile::encodeName(argv[3]));
    QGuiApplication app(argc, argv);
    // Own settings, so remembered track heights never touch the real app's preferences.
    app.setOrganizationName("Chorus Cut Tests");
    app.setApplicationName("Timeline Check");
    Mlt::Factory::init();
    const QUrl project = QUrl::fromLocalFile(QFileInfo(argv[1]).absoluteFilePath());
    Editor editor;
    if (!editor.openProject(project)) return fail(2, editor.status());
    auto lyrics = editor.lyrics();

    // Fixture: the GPU alignment with one line flagged for review.
    QFile in(argv[2]);
    if (!in.open(QIODevice::ReadOnly)) return fail(3, "cannot read alignment");
    auto doc = QJsonDocument::fromJson(in.readAll()).object();
    auto words = doc["words"].toArray();
    const int flaggedLine = words[40].toObject()["line"].toInt();
    for (int i = 0; i < words.size(); ++i) {
        auto w = words[i].toObject();
        w["flag"] = w["line"].toInt() == flaggedLine;
        words[i] = w;
    }
    doc["words"] = words;
    QTemporaryDir tmp;
    QFile fixture(tmp.filePath("alignment.json"));
    if (!fixture.open(QIODevice::WriteOnly) || fixture.write(QJsonDocument(doc).toJson()) <= 0) return fail(3, "fixture");
    fixture.close();
    if (!lyrics->importAlignment(QUrl::fromLocalFile(fixture.fileName()))) return fail(4, lyrics->message());
    if (lyrics->flaggedLines() != 1) return fail(4, QString("expected 1 flagged line, got %1").arg(lyrics->flaggedLines()));

    qmlRegisterType<Monitor>("Chorus", 1, 0, "VideoMonitor");
    qmlRegisterType<WaveformView>("Chorus", 1, 0, "WaveformView");
    QQmlApplicationEngine engine;
    ProjectLibrary library(&editor);
    engine.rootContext()->setContextProperty("editor", &editor);
    engine.rootContext()->setContextProperty("library", &library);
    engine.load(QUrl("qrc:/src/Main.qml"));
    if (engine.rootObjects().isEmpty()) return fail(5, "QML did not load");
    auto window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    window->resize(1280, 820);
    if (!QTest::qWaitForWindowExposed(window)) return fail(5, "window not exposed");
    auto root = window->contentItem();
    auto timeline = find(root, "timeline");
    if (!timeline || !waitFor([&] { return editor.peaksReady(); })) return fail(6, "timeline or waveform missing");
    editor.seek(0);

    auto lines = [&] { return lyrics->lines(); };
    auto line = [&](int row) { return lines()[row].toMap(); };
    auto center = [&](QQuickItem* item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); };
    auto zoomTo = [&](int a, int b) {
        QMetaObject::invokeMethod(timeline, "zoomTo", Q_ARG(QVariant, a), Q_ARG(QVariant, b));
        QTest::qWait(150);
    };
    auto drag = [&](QPoint from, QPoint to) {
        QTest::mousePress(window, Qt::LeftButton, {}, from);
        for (int k = 1; k <= 8; ++k) QTest::mouseMove(window, from + (to - from) * k / 8, 10);
        QTest::mouseRelease(window, Qt::LeftButton, {}, to, 10);
        QTest::qWait(100);
    };
    auto selectionKind = [&] {
        const auto s = window->property("selection").toMap();
        return s.value("kind").toString();
    };

    // 1. Drag a whole line to the right, then undo and redo it with the keyboard.
    const int row = 9;
    auto before = line(row);
    zoomTo(before["start"].toInt() - 4000, before["end"].toInt() + 4000);
    auto clip = find(root, QString("lyricClip%1").arg(row));
    if (!clip || clip->width() < 20) return fail(7, "line clip not visible on screen");
    const double pps = timeline->property("pps").toDouble();
    const auto limits = lyrics->moveLimits(before["first"].toInt(), before["last"].toInt());
    const QPoint start = center(clip);
    // Words are visible at this zoom, so grab the line by its title strip above the words.
    const QPoint grip = clip->mapToScene(QPointF(clip->width() / 2, 8)).toPoint();
    drag(grip, grip + QPoint(40, 0));
    const int expected = qBound(limits["min"].toInt(), int(std::lround(40 / pps * 1000)), limits["max"].toInt());
    auto moved = line(row);
    if (std::abs(moved["start"].toInt() - before["start"].toInt() - expected) > 2 ||
        moved["end"].toInt() - moved["start"].toInt() != before["end"].toInt() - before["start"].toInt())
        return fail(8, QString("line move: start %1 -> %2, expected delta %3").arg(before["start"].toInt()).arg(moved["start"].toInt()).arg(expected));
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    if (line(row)["start"] != before["start"]) return fail(9, "undo did not restore the line");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier | Qt::ShiftModifier);
    if (line(row)["start"] != moved["start"]) return fail(10, "redo did not reapply the move");
    Q_UNUSED(start);

    // 2. Click a word to select it, then trim its end by dragging the edge.
    // Use the line's longest word, so the trim stays clear of the 20 ms minimum length.
    QVariantMap w;
    for (const auto& v : line(row)["words"].toList())
        if (w.isEmpty() || v.toMap()["end"].toInt() - v.toMap()["start"].toInt() > w["end"].toInt() - w["start"].toInt()) w = v.toMap();
    const double viewStart = timeline->property("viewStart").toDouble();
    const double header = timeline->property("header").toDouble();
    const double lyricY = timeline->property("lyricY").toDouble();
    const double lyricH = timeline->property("lyricH").toDouble();
    auto at = [&](int ms) { return timeline->mapToScene(QPointF(header + (ms / 1000.0 - viewStart) * pps, lyricY + lyricH - 16)).toPoint(); };
    QTest::mouseClick(window, Qt::LeftButton, {}, at((w["start"].toInt() + w["end"].toInt()) / 2));
    QTest::qWait(100);
    if (selectionKind() != "word") return fail(11, "clicking a word did not select it (got " + selectionKind() + ")");
    QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 100);  // not a double-click
    if (editor.playing()) return fail(11, "a single click started playback");
    const int trimPx = int((w["end"].toInt() - w["start"].toInt()) / 1000.0 * pps / 4);
    const QPoint edge = at(w["end"].toInt()) - QPoint(1, 0);
    drag(edge, edge - QPoint(trimPx, 0));
    QVariantMap trimmed;
    for (const auto& v : line(row)["words"].toList()) if (v.toMap()["index"] == w["index"]) trimmed = v.toMap();
    const int expectEnd = w["end"].toInt() - int(std::lround(trimPx / pps * 1000));
    if (std::abs(trimmed["end"].toInt() - expectEnd) > 25 || trimmed["start"] != w["start"])
        return fail(12, QString("word trim: end %1 -> %2, expected about %3").arg(w["end"].toInt()).arg(trimmed["end"].toInt()).arg(expectEnd));

    // 3. Select the flagged line and mark it checked with the inspector button; undo brings the flag back.
    int flaggedRow = -1;
    for (int r = 0; r < lines().size(); ++r) if (line(r)["flag"].toBool()) flaggedRow = r;
    if (flaggedRow < 0) return fail(13, "no flagged row");
    zoomTo(line(flaggedRow)["start"].toInt() - 4000, line(flaggedRow)["end"].toInt() + 4000);
    auto flaggedClip = find(root, QString("lyricClip%1").arg(flaggedRow));
    QTest::mouseClick(window, Qt::LeftButton, {}, flaggedClip->mapToScene(QPointF(flaggedClip->width() / 2, 8)).toPoint());
    QTest::qWait(150);
    auto mark = find(root, "markChecked");
    if (selectionKind() != "line" || !mark) return fail(14, "flagged line not selected or no Mark checked button");
    QTest::mouseClick(window, Qt::LeftButton, {}, center(mark));
    if (lyrics->flaggedLines() != 0) return fail(15, "Mark checked did not clear the flag");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    if (lyrics->flaggedLines() != 1) return fail(16, "undo did not restore the flag");

    // 4. Return plays the selected line through the disk audio driver and stops at its end.
    auto sel = line(flaggedRow);
    QTest::keyClick(window, Qt::Key_Return);
    if (!waitFor([&] { return editor.playing(); }, 3000)) return fail(17, "Return did not start playback");
    if (!waitFor([&] { return !editor.playing(); }, 30000)) return fail(18, "playback did not stop");
    const int endFrame = int(std::ceil(sel["end"].toInt() * editor.fps() / 1000.)) - 1;
    if (editor.frame() != endFrame) return fail(19, QString("stopped at frame %1, expected %2").arg(editor.frame()).arg(endFrame));

    // 5. Resize tracks from their header dividers with the mouse.
    auto divider = [&](const QString& key) { return find(root, "trackResize_" + key); };
    auto lyricDivider = divider("lyricH"), audioDivider = divider("audioH");
    if (!lyricDivider || !audioDivider || lyricDivider->height() < 4) return fail(21, "no track divider on screen");
    QTest::mouseDClick(window, Qt::LeftButton, {}, center(lyricDivider));   // start from the default height
    QTest::mouseDClick(window, Qt::LeftButton, {}, center(audioDivider));
    QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 100);
    const int lyricBefore = timeline->property("lyricH").toInt();
    drag(center(lyricDivider), center(lyricDivider) + QPoint(0, 60));
    if (timeline->property("lyricH").toInt() != lyricBefore + 60)
        return fail(22, QString("lyric track %1 -> %2, expected +60").arg(lyricBefore).arg(timeline->property("lyricH").toInt()));
    auto bar = find(root, "trackScrollBar");
    drag(center(audioDivider), center(audioDivider) + QPoint(0, 400));
    QTest::qWait(100);
    bar = find(root, "trackScrollBar");
    if (!bar || timeline->property("contentH").toInt() <= timeline->property("trackViewH").toInt())
        return fail(23, "tall tracks did not show the vertical scrollbar");
    QTest::mouseDClick(window, Qt::LeftButton, {}, center(divider("lyricH")));
    QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 100);
    if (timeline->property("lyricH").toInt() != lyricBefore) return fail(24, "double-click did not reset the track height");
    const int remembered = timeline->property("audioH").toInt();
    QTest::qWait(1500);  // QtCore Settings writes changes on a short timer
    engine.clearComponentCache();
    QQmlApplicationEngine second;
    second.rootContext()->setContextProperty("editor", &editor);
    second.rootContext()->setContextProperty("library", &library);
    second.load(QUrl("qrc:/src/Main.qml"));
    auto secondTimeline = second.rootObjects().isEmpty() ? nullptr : second.rootObjects().first()->findChild<QObject*>("timeline");
    if (!secondTimeline || secondTimeline->property("audioH").toInt() != remembered)
        return fail(25, "track height was not remembered");
    QMetaObject::invokeMethod(secondTimeline, "resizeTrack", Q_ARG(QVariant, "audioH"), Q_ARG(QVariant, 180));

    // 6. Edits persist: reopen the project and compare.
    const auto saved = lines();
    if (!editor.openProject(project) || lyrics->lines() != saved || editor.canUndo())
        return fail(20, "reopened project differs from the edited one");
    qInfo() << "PASS: line drag, undo/redo, word select and trim, mark checked, play selection, track resize/scroll/reset/remember, reopen.";
    return 0;
}
