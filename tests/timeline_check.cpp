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
#include <QWheelEvent>
#include <QNativeGestureEvent>
#include <QPointingDevice>
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
    app.setOrganizationName("ChorusCut Tests");
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

    // 0. Zoomed out, lines wrap onto several rows and no word clips are shown.
    const int row = 9;
    auto anyWords = [&] {
        for (const auto& l : lines())
            for (const auto& v : l.toMap()["words"].toList())
                if (find(root, QString("lyricWord%1").arg(v.toMap()["index"].toInt()))) return true;
        return false;
    };
    zoomTo(line(row)["start"].toInt() - 15000, line(row)["end"].toInt() + 15000);
    int wrapped = 0;
    for (int i = 0; i < lines().size(); ++i)
        if (auto text = find(root, QString("lyricText%1").arg(i)); text && text->property("lineCount").toInt() > 1) ++wrapped;
    if (!wrapped) return fail(7, "zoomed out, no lyric line wrapped onto a second row");
    if (anyWords()) return fail(7, "zoomed out, word clips should be hidden");

    // 1. Drag a whole line to the right, then undo and redo it with the keyboard.
    auto before = line(row);
    zoomTo(before["start"].toInt() - 1500, before["end"].toInt() + 1500);
    auto clip = find(root, QString("lyricClip%1").arg(row));
    if (!clip || clip->width() < 20) return fail(7, "line clip not visible on screen");
    if (!find(root, QString("lyricWord%1").arg(before["first"].toInt())) || find(root, QString("lyricText%1").arg(row)))
        return fail(7, QString("zoomed in, the line should show word clips (pps %1, showWords %2, lyricH %3)")
                       .arg(timeline->property("pps").toDouble()).arg(clip->property("showWords").toBool()).arg(timeline->property("lyricH").toDouble()));
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

    // 6. Trackpad: two-finger swipes pan, a pinch zooms around the pointer, ⌘-scroll zooms too.
    {
        static QPointingDevice trackpad("Test trackpad", 4242, QInputDevice::DeviceType::TouchPad, QPointingDevice::PointerType::Finger,
                                        QInputDevice::Capability::Position | QInputDevice::Capability::Scroll | QInputDevice::Capability::PixelScroll,
                                        5, 0);
        auto lanes = find(root, "timelineLanes");
        if (!lanes) return fail(26, "no lanes");
        zoomTo(line(row)["start"].toInt() - 4000, line(row)["end"].toInt() + 4000);
        const QPoint at = lanes->mapToScene(QPointF(lanes->width() / 2, timeline->property("lyricY").toDouble() + 10)).toPoint();
        auto swipe = [&](QPoint pixels, Qt::KeyboardModifiers mods = {}) {
            const Qt::ScrollPhase phases[] = {Qt::ScrollBegin, Qt::ScrollUpdate, Qt::ScrollUpdate, Qt::ScrollEnd};
            for (auto phase : phases) {
                const QPoint d = phase == Qt::ScrollBegin || phase == Qt::ScrollEnd ? QPoint() : pixels / 2;
                QWheelEvent e(at, window->mapToGlobal(at), d, d * 2, Qt::NoButton, mods, phase, false, Qt::MouseEventNotSynthesized, &trackpad);
                QGuiApplication::sendEvent(window, &e);
                QTest::qWait(20);
            }
            QTest::qWait(80);
        };
        auto state = [&] { return QString("viewStart %1, pps %2").arg(timeline->property("viewStart").toDouble()).arg(timeline->property("pps").toDouble()); };
        const double v0 = timeline->property("viewStart").toDouble(), p0 = timeline->property("pps").toDouble();
        swipe(QPoint(-120, 0));  // fingers move left: later in the song
        const double v1 = timeline->property("viewStart").toDouble();
        if (!(std::abs(v1 - v0 - 120 / p0) < 0.05)) return fail(27, "horizontal two-finger swipe did not pan: " + state());
        swipe(QPoint(0, -90));
        const double v2 = timeline->property("viewStart").toDouble();
        if (!(std::abs(v2 - v1 - 90 / p0) < 0.05)) return fail(28, "vertical two-finger swipe did not pan: " + state());
        // Pinch: the time under the pointer stays put while the scale grows by about half.
        const double anchor = v2 + (at.x() - lanes->mapToScene(QPointF(0, 0)).x()) / p0;
        const QPointF local = window->contentItem()->mapFromScene(at);
        auto gesture = [&](Qt::NativeGestureType type, double value) {
            QNativeGestureEvent e(type, &trackpad, 2, local, at, window->mapToGlobal(at), value, QPointF(), 7);
            QGuiApplication::sendEvent(window, &e);
            QTest::qWait(20);
        };
        gesture(Qt::BeginNativeGesture, 0);
        for (int k = 0; k < 5; ++k) gesture(Qt::ZoomNativeGesture, 0.0845);  // 1.0845^5 ≈ 1.5
        gesture(Qt::EndNativeGesture, 0);
        QTest::qWait(80);
        const double p3 = timeline->property("pps").toDouble(), v3 = timeline->property("viewStart").toDouble();
        if (!(p3 > p0 * 1.35 && p3 < p0 * 1.65)) return fail(29, "pinch did not zoom in by about half: " + state());
        const double anchorAfter = v3 + (at.x() - lanes->mapToScene(QPointF(0, 0)).x()) / p3;
        if (!(std::abs(anchorAfter - anchor) < 0.05)) return fail(29, QString("pinch moved the time under the pointer: %1 s -> %2 s").arg(anchor).arg(anchorAfter));
        // ⌘-scroll on a mouse wheel zooms out around the pointer and keeps the view sane.
        QWheelEvent wheel(at, window->mapToGlobal(at), QPoint(), QPoint(0, -240), Qt::NoButton, Qt::ControlModifier, Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(window, &wheel);
        QTest::qWait(80);
        const double p4 = timeline->property("pps").toDouble(), v4 = timeline->property("viewStart").toDouble();
        if (!(p4 < p3 * 0.9) || !std::isfinite(v4)) return fail(30, "⌘-scroll did not zoom out: " + state());
        const double anchor4 = v4 + (at.x() - lanes->mapToScene(QPointF(0, 0)).x()) / p4;
        if (!(std::abs(anchor4 - anchorAfter) < 0.05)) return fail(30, QString("⌘-scroll moved the time under the pointer: %1 s -> %2 s").arg(anchorAfter).arg(anchor4));
    }

    // 6b. Selecting, by real input: a hint while nothing is selected; a drag across the waveform
    // (an I-beam says it will select); the bar in the ruler with its name and length, edges that
    // drag, and × to clear; I and O at the playhead; X for the shot, then the section, under it.
    {
        const QString shots = qEnvironmentVariable("CHORUS_SHOTS");
        auto capture = [&](const QString& name) { if (!shots.isEmpty()) window->grabWindow().save(shots + "/" + name + ".png"); };
        auto sel = [&] { return window->property("selection").toMap(); };
        auto lanes = find(root, "timelineLanes");
        // Section 5 opened a second window; shortcuts go to the active one.
        window->requestActivate();
        if (!QTest::qWaitForWindowActive(window)) return fail(31, "the window could not be made active again");
        timeline->setProperty("scrollY", 0);
        QTest::qWait(50);
        QTest::mouseMove(window, QPoint(5, 5));
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::qWait(50);
        if (!sel().isEmpty() || !find(root, "selectHint"))
            return fail(31, "with nothing selected, the inspector does not say how to select (selection " + QString::fromUtf8(QJsonDocument::fromVariant(sel()).toJson(QJsonDocument::Compact)) + ")");
        capture("select-hint");
        zoomTo(line(row)["start"].toInt() - 6000, line(row)["end"].toInt() + 6000);
        const double pps = timeline->property("pps").toDouble(), viewStart = timeline->property("viewStart").toDouble();
        const double waveY = timeline->property("audioY").toDouble() + std::min(40.0, timeline->property("audioH").toDouble() / 2);
        auto at = [&](double seconds, double y) { return lanes->mapToScene(QPointF((seconds - viewStart) * pps, y)).toPoint(); };
        const double a = viewStart + 2, b = viewStart + 7;
        QTest::mouseMove(window, at(a, waveY));
        QTest::qWait(50);
        if (window->cursor().shape() != Qt::IBeamCursor) return fail(31, QString("the pointer over the waveform is not an I-beam (%1)").arg(window->cursor().shape()));
        drag(at(a, waveY), at(b, waveY));
        auto near = [&](int ms, double seconds) { return std::abs(ms / 1000.0 - seconds) < 2.0 / pps + 0.01; };
        if (sel()["kind"] != "range" || !near(sel()["start"].toInt(), a) || !near(sel()["end"].toInt(), b))
            return fail(31, "dragging across the waveform did not select that range: " + QString::fromUtf8(QJsonDocument::fromVariant(sel()).toJson(QJsonDocument::Compact)));
        auto label = find(root, "selectionLabel");
        if (!find(root, "selectionBar") || !label || label->property("text").toString() != "Range · 5.0 s")
            return fail(32, "the ruler does not show the selection's name and length: " + (label ? label->property("text").toString() : QString("no label")));
        // Its edges drag in the ruler; what is left is a plain range.
        drag(center(find(root, "selectionEnd")), center(find(root, "selectionEnd")) + QPoint(int(2 * pps), 0));
        if (!near(sel()["end"].toInt(), b + 2) || !near(sel()["start"].toInt(), a)) return fail(32, "dragging the selection's end did not move it");
        drag(center(find(root, "selectionStart")), center(find(root, "selectionStart")) - QPoint(int(1 * pps), 0));
        if (!near(sel()["start"].toInt(), a - 1) || !near(sel()["end"].toInt(), b + 2)) return fail(32, "dragging the selection's start did not move it");
        capture("selection-bar");
        QTest::mouseClick(window, Qt::LeftButton, {}, center(find(root, "selectionClear")));
        QTest::qWait(100);
        if (!sel().isEmpty() || find(root, "selectionBar")) return fail(32, "× did not clear the selection");
        // I and O at the playhead.
        const int fps = int(std::round(editor.fps()));
        const int songMs = int(std::round(editor.length() * 1000.0 / editor.fps()));
        auto ms = [&](int frame) { return int(std::round(frame * 1000.0 / editor.fps())); };
        const int f1 = int((a + 1) * fps), f2 = f1 + 3 * fps;
        editor.seek(f1);
        QTest::keyClick(window, Qt::Key_I);
        if (sel()["start"].toInt() != ms(f1) || sel()["end"].toInt() != songMs) return fail(33, "I did not select from the playhead to the end of the song");
        editor.seek(f2);
        QTest::keyClick(window, Qt::Key_O);
        if (sel()["start"].toInt() != ms(f1) || sel()["end"].toInt() != ms(f2)) return fail(33, "O did not end the selection at the playhead");
        editor.seek(f1 - fps);
        QTest::keyClick(window, Qt::Key_O);
        if (sel()["start"].toInt() != 0 || sel()["end"].toInt() != ms(f1 - fps)) return fail(33, "O before the start did not select from the beginning");
        // X: plan a shot over the line by double-clicking V1, then X selects it, and X again its section.
        const double mid = (line(row)["start"].toInt() + line(row)["end"].toInt()) / 2000.0;
        QTest::mouseDClick(window, Qt::LeftButton, {}, at(mid, timeline->property("v1Y").toDouble() + 10));
        QTest::qWait(150);
        if (sel()["kind"] != "shot") return fail(34, "double-clicking V1 did not plan a shot");
        const int shotId = sel()["id"].toInt();
        QTest::keyClick(window, Qt::Key_Escape);
        editor.seek(int(mid * editor.fps()));
        QTest::keyClick(window, Qt::Key_X);
        if (sel()["kind"] != "shot" || sel()["id"].toInt() != shotId) return fail(34, "X did not select the shot under the playhead");
        QTest::keyClick(window, Qt::Key_X);
        const int t = ms(editor.frame());
        if (sel()["kind"] != "region" || sel()["start"].toInt() > t || sel()["end"].toInt() <= t)
            return fail(34, "X again did not select the section under the playhead: " + QString::fromUtf8(QJsonDocument::fromVariant(sel()).toJson(QJsonDocument::Compact)));
        if (!find(root, "selectionLabel")->property("text").toString().startsWith(sel()["label"].toString() + " · "))
            return fail(34, "the ruler does not name the section");
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);  // the planned shot
        QTest::keyClick(window, Qt::Key_Escape);
    }

    // 7. Edits persist: reopen the project and compare.
    const auto saved = lines();
    if (!editor.openProject(project) || lyrics->lines() != saved || editor.canUndo())
        return fail(20, "reopened project differs from the edited one");
    qInfo() << "PASS: wrap when zoomed out, words when zoomed in, line drag, undo/redo, word select and trim, mark checked, play selection, track resize/scroll/reset/remember, trackpad pan and pinch, ⌘-scroll zoom, selecting (hint, waveform drag, ruler bar edges and ×, I, O, X), reopen.";
    return 0;
}
