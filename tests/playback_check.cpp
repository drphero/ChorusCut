// Playback picture check: plays a project through the app's own playback path (the SDL consumer
// MLT runs in real time, audio to SDL's disk driver) and compares every frame the viewer is sent
// with the same position decoded on its own by a seek, which is known to be clean.
//
// usage: ChorusCutPlaybackCheck PROJECT_COPY START_FRAME FRAMES [OUT_DIR]
// Use a copy: opening a project saves its thumbnail.
#include "editor.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QDebug>
#include <cmath>

// Peak signal-to-noise ratio in dB between two RGB pictures of the same size (99 when identical).
static double psnr(const QImage& a, const QImage& b) {
    if (a.size() != b.size() || a.isNull()) return -1;
    const QImage x = a.convertToFormat(QImage::Format_RGB888), y = b.convertToFormat(QImage::Format_RGB888);
    double sum = 0;
    for (int r = 0; r < x.height(); ++r) {
        const uchar* p = x.constScanLine(r);
        const uchar* q = y.constScanLine(r);
        for (int i = 0; i < x.width() * 3; ++i) { const double d = double(p[i]) - q[i]; sum += d * d; }
    }
    const double mse = sum / (double(x.width()) * x.height() * 3);
    return mse <= 1e-9 ? 99 : 10 * std::log10(255.0 * 255.0 / mse);
}

int main(int argc, char** argv) {
    if (argc < 4) { qCritical() << "usage: ChorusCutPlaybackCheck PROJECT_COPY START_FRAME FRAMES [OUT_DIR]"; return 1; }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir scratch;
    qputenv("SDL_AUDIODRIVER", "disk"); qputenv("SDL_AUDIO_DRIVER", "disk");
    qputenv("SDL_AUDIO_DISK_OUTPUT_FILE", QFile::encodeName(scratch.filePath("capture.raw")));
    qputenv("CHORUS_WORKFLOWS_DIR", QFile::encodeName(scratch.filePath("Workflows")));
    qputenv("CHORUS_NOTIFY_LOG", QFile::encodeName(scratch.filePath("notify.log")));
    QGuiApplication app(argc, argv);
    app.setOrganizationName("Chorus Cut Tests");
    app.setApplicationName("Playback Check");
    Mlt::Factory::init();
    const int start = QString(argv[2]).toInt(), count = QString(argv[3]).toInt();
    const QString out = argc > 4 ? QFileInfo(argv[4]).absoluteFilePath() : QString();
    if (!out.isEmpty()) QDir().mkpath(out);

    Editor editor;
    if (!editor.openProject(QUrl::fromLocalFile(QFileInfo(argv[1]).absoluteFilePath()))) { qCritical() << editor.status(); return 2; }
    struct Shown { int position; QImage image; };
    QList<Shown> shown;
    QObject::connect(&editor, &Editor::imageReady, &editor, [&](const QImage& image) {
        if (editor.playing()) shown.append({editor.frame(), image});
    });
    editor.seek(start);
    editor.togglePlay();
    QElapsedTimer clock; clock.start();
    while (editor.playing() && editor.frame() < start + count && clock.elapsed() < count * 1000 / 24 + 8000) QTest::qWait(5);
    if (editor.playing()) editor.togglePlay();
    if (shown.size() < count / 2) { qCritical() << "FAIL: only" << shown.size() << "frames were shown"; return 3; }

    double worst = 99;
    int worstAt = -1, bad = 0, repeats = 0, skips = 0, last = -1;
    for (const auto& s : shown) {
        if (s.position == last) ++repeats;
        else if (last >= 0 && s.position != last + 1) skips += s.position - last - 1;
        last = s.position;
        const QImage clean = editor.imageAt(s.position);
        const double p = psnr(s.image, clean);
        if (p < 40) {
            ++bad;
            if (!out.isEmpty() && bad <= 6) {
                s.image.save(QString("%1/played-%2.png").arg(out).arg(s.position));
                clean.save(QString("%1/clean-%2.png").arg(out).arg(s.position));
            }
        }
        if (p < worst) { worst = p; worstAt = s.position; }
    }
    qInfo().noquote() << QString("%1 frames shown from %2: %3 differ from a clean decode (worst %4 dB at %5), %6 repeated, %7 skipped")
                         .arg(shown.size()).arg(start).arg(bad).arg(worst, 0, 'f', 1).arg(worstAt).arg(repeats).arg(skips);
    if (bad) { qCritical() << "FAIL: played frames differ from the clean decode"; return 4; }

    // The viewer draws the frame at its own size: smoothly, not by repeating and dropping pixels.
    qmlRegisterType<Monitor>("Chorus", 1, 0, "VideoMonitor");
    QQmlEngine engine;
    engine.rootContext()->setContextProperty("editor", &editor);
    QQmlComponent component(&engine);
    component.setData("import QtQuick\nimport Chorus\nWindow { width: 1001; height: 563; visible: true; color: 'black'\n"
                      "  readonly property var source: editor\n"
                      "  VideoMonitor { anchors.fill: parent; editor: source } }", QUrl());
    std::unique_ptr<QObject> root(component.create());
    auto window = qobject_cast<QQuickWindow*>(root.get());
    if (!window || !QTest::qWaitForWindowExposed(window)) { qCritical() << "FAIL: no viewer window" << component.errorString(); return 5; }
    editor.seek(start + count / 2);
    QTest::qWait(300);
    const QImage frame = editor.imageAt(start + count / 2);
    QImage drawn = window->grabWindow().convertToFormat(QImage::Format_RGB888);
    drawn = drawn.scaled(window->size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);  // to logical pixels
    QSize fit = frame.size(); fit.scale(window->size(), Qt::KeepAspectRatio);
    const QRect area(QPoint((window->width() - fit.width()) / 2, (window->height() - fit.height()) / 2), fit);
    const QImage painted = drawn.copy(area.adjusted(2, 2, -2, -2));
    auto as = [&](Qt::TransformationMode mode) { return frame.scaled(fit, Qt::IgnoreAspectRatio, mode).copy(QRect(QPoint(2, 2), fit - QSize(4, 4))); };
    const double smooth = psnr(painted, as(Qt::SmoothTransformation)), blocky = psnr(painted, as(Qt::FastTransformation));
    qInfo().noquote() << QString("viewer at %1x%2: %3 dB from a smooth scale, %4 dB from nearest-neighbour").arg(fit.width()).arg(fit.height()).arg(smooth, 0, 'f', 1).arg(blocky, 0, 'f', 1);
    if (!out.isEmpty()) painted.save(out + "/viewer.png");
    if (smooth < blocky + 1) { qCritical() << "FAIL: the viewer scales the frame by dropping pixels"; return 6; }
    qInfo() << "PASS: every played frame matches its clean decode, and the viewer scales it smoothly";
    return 0;
}
