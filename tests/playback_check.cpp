// Playback picture check: plays a project through the app's own playback path (the SDL consumer
// MLT runs in real time, audio to SDL's disk driver) and compares every frame the viewer is sent
// with the same position decoded on its own by a seek, which is known to be clean.
//
// Then checks sharpness: the preview holds at least the pixels of the largest take, and the
// viewer, drawn at Retina density (2 device pixels per point), matches a smooth scale of it.
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
    qputenv("QT_SCALE_FACTOR", "2");
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

    // The preview must not throw away the takes' own pixels.
    QSize largest;
    {
        Mlt::Profile probe;
        for (const QFileInfo& f : QDir(QFileInfo(argv[1]).absoluteFilePath() + "/Media/Takes").entryInfoList({"*.mp4", "*.mov", "*.webm"}, QDir::Files)) {
            Mlt::Producer take(probe, "avformat", f.absoluteFilePath().toUtf8());
            const QSize size(take.get_int("width"), take.get_int("height"));
            if (size.width() * size.height() > largest.width() * largest.height()) largest = size;
        }
    }
    const QSize preview = editor.imageAt(start).size();
    qInfo().noquote() << QString("preview %1x%2, largest take %3x%4").arg(preview.width()).arg(preview.height()).arg(largest.width()).arg(largest.height());
    if (largest.isValid() && (preview.width() < largest.width() * 0.97 || preview.height() < largest.height() * 0.97)) {
        qCritical() << "FAIL: the preview is smaller than the takes it shows"; return 7;
    }
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
    // In device pixels: the viewer must use the display's density, not draw at points and stretch.
    const QImage drawn = window->grabWindow().convertToFormat(QImage::Format_RGB888);
    const qreal dpr = window->devicePixelRatio();
    QSize fit = frame.size(); fit.scale(drawn.size(), Qt::KeepAspectRatio);
    const QRect area(QPoint((drawn.width() - fit.width()) / 2, (drawn.height() - fit.height()) / 2), fit);
    const QImage painted = drawn.copy(area.adjusted(3, 3, -3, -3));
    auto as = [&](const QImage& from, Qt::TransformationMode mode) { return from.scaled(fit, Qt::IgnoreAspectRatio, mode).copy(QRect(QPoint(3, 3), fit - QSize(6, 6))); };
    const QImage points = frame.scaled(fit / dpr, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    const double smooth = psnr(painted, as(frame, Qt::SmoothTransformation)), blocky = psnr(painted, as(frame, Qt::FastTransformation));
    const double stretched = psnr(painted, as(points, Qt::SmoothTransformation));
    qInfo().noquote() << QString("viewer at %1x%2 device pixels (x%3): %4 dB from a smooth scale, %5 dB from nearest-neighbour, %6 dB from drawing at points and stretching")
                         .arg(fit.width()).arg(fit.height()).arg(dpr).arg(smooth, 0, 'f', 1).arg(blocky, 0, 'f', 1).arg(stretched, 0, 'f', 1);
    if (!out.isEmpty()) painted.save(out + "/viewer.png");
    if (dpr < 2) { qCritical() << "FAIL: the viewer window isn't at Retina density"; return 6; }
    if (smooth < blocky + 1) { qCritical() << "FAIL: the viewer scales the frame by dropping pixels"; return 6; }
    if (smooth < stretched + 1) { qCritical() << "FAIL: the viewer draws at points and stretches to the display"; return 6; }
    qInfo() << "PASS: every played frame matches its clean decode, the preview keeps the takes' pixels, and the viewer scales it smoothly at Retina density";
    return 0;
}
