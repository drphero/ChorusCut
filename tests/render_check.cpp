// Rendering the cut, with real input: makes two takes with ffmpeg (a 1376×768 grey ramp whose
// level gives away each frame, and an 864×480 test pattern with a red left and a green top
// stripe), puts them on two shots with a third shot left without a take, then renders through
// the Render dialog by clicks and typing and inspects the files with ffprobe and ffmpeg:
// codecs, size, length and audio; the frames on each side of the cuts are the takes' right
// frames; the smaller take is scaled smoothly (closer to a bicubic reference than a
// nearest-neighbour one); Fill, Fit and Stretch each place the pattern as they should; the shot
// without a take is a card; a selection renders exactly its frames; Cancel leaves no file.
// usage: ChorusCutRenderCheck PROJECT_COPY ALIGNMENT_JSON   (ffmpeg and ffprobe on PATH)
#include "editor.h"
#include "waveform.h"
#include "library.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>
#include <QXmlStreamReader>
#include <QDebug>
#include <cmath>
#include <functional>

static int fail(int code, const QString& why) { qCritical().noquote() << "FAIL" << code << why; return code; }
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
static QString run(const QString& program, const QStringList& args, bool* ok = nullptr) {
    QProcess p;
    p.start(program, args);
    const bool done = p.waitForFinished(120000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    if (ok) *ok = done;
    if (!done) qWarning().noquote() << program << args.join(' ') << QString::fromUtf8(p.readAllStandardError()).right(800);
    return QString::fromUtf8(p.readAllStandardOutput());
}
// Frame n of a video as a picture, optionally through an ffmpeg filter afterwards.
static QImage frameOf(const QString& video, int n, const QString& dir, const QString& then = QString()) {
    static int serial = 0;
    const QString out = QDir(dir).filePath(QString("frame-%1.png").arg(++serial));
    QString filter = QString("select=eq(n\\,%1)").arg(n);
    if (!then.isEmpty()) filter += "," + then;
    bool ok = false;
    run("ffmpeg", {"-v", "error", "-y", "-i", video, "-vf", filter, "-fps_mode", "passthrough", "-frames:v", "1", out}, &ok);
    return ok ? QImage(out).convertToFormat(QImage::Format_RGB32) : QImage();
}
static double psnr(const QImage& a, const QImage& b) {
    if (a.size() != b.size() || a.isNull()) return 0;
    double sum = 0;
    for (int y = 0; y < a.height(); ++y)
        for (int x = 0; x < a.width(); ++x) {
            const QRgb p = a.pixel(x, y), q = b.pixel(x, y);
            const double dr = qRed(p) - qRed(q), dg = qGreen(p) - qGreen(q), db = qBlue(p) - qBlue(q);
            sum += dr * dr + dg * dg + db * db;
        }
    const double mse = sum / (3.0 * a.width() * a.height());
    return mse <= 0 ? 99 : 10 * std::log10(255.0 * 255.0 / mse);
}
static double meanLuma(const QImage& img, QRect r) {
    double s = 0; int n = 0;
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) { s += qGray(img.pixel(x, y)); ++n; }
    return n ? s / n : 0;
}
// An XML file as a tree, for reading the exported timeline.
struct Node {
    QString name, text;
    QXmlStreamAttributes attrs;
    QList<Node> children;
    const Node* child(const QString& n) const { for (const auto& c : children) if (c.name == n) return &c; return nullptr; }
    QList<const Node*> all(const QString& n) const { QList<const Node*> out; for (const auto& c : children) if (c.name == n) out << &c; return out; }
    QString at(const QString& path) const {
        const Node* n = this;
        for (const auto& part : path.split('/')) if (!(n = n->child(part))) return QString();
        return n->text.trimmed();
    }
};
static bool readNode(QXmlStreamReader& r, Node& node) {
    node.name = r.name().toString(); node.attrs = r.attributes();
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement()) { node.children.append(Node()); if (!readNode(r, node.children.last())) return false; }
        else if (r.isCharacters()) node.text += r.text();
        else if (r.isEndElement()) return true;
    }
    return false;
}
static Node readXml(const QString& path) {
    QFile f(path);
    Node root;
    if (!f.open(QIODevice::ReadOnly)) return root;
    QXmlStreamReader r(&f);
    while (!r.atEnd() && !r.isStartElement()) r.readNext();
    if (r.isStartElement() && (!readNode(r, root) || r.hasError())) return Node();
    return root;
}

static bool isRed(QRgb c) { return qRed(c) > 150 && qGreen(c) < 90 && qBlue(c) < 90; }
static bool isGreen(QRgb c) { return qGreen(c) > 150 && qRed(c) < 90 && qBlue(c) < 90; }
static bool isBlack(QRgb c) { return qRed(c) < 24 && qGreen(c) < 24 && qBlue(c) < 24; }

int main(int argc, char** argv) {
    if (argc != 3) { qCritical() << "usage: ChorusCutRenderCheck PROJECT_COPY ALIGNMENT_JSON"; return 1; }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    qputenv("SDL_AUDIODRIVER", "disk"); qputenv("SDL_AUDIO_DRIVER", "disk");
    QTemporaryDir scratch;
    qputenv("SDL_AUDIO_DISK_OUTPUT_FILE", QFile::encodeName(scratch.filePath("capture.raw")));
    QGuiApplication app(argc, argv);
    app.setOrganizationName("ChorusCut Tests");
    app.setApplicationName("Render Check");
    QSettings().remove("render/crf");
    Mlt::Factory::init();
    const QUrl project = QUrl::fromLocalFile(QFileInfo(argv[1]).absoluteFilePath());
    Editor editor;
    if (!editor.openProject(project)) return fail(2, editor.status());
    if (!editor.lyrics()->importAlignment(QUrl::fromLocalFile(QFileInfo(argv[2]).absoluteFilePath()))) return fail(2, editor.lyrics()->message());
    Plan* plan = editor.plan();
    if (std::abs(editor.fps() - 24) > 0.01) return fail(2, QString("expected a 24 fps project, got %1").arg(editor.fps()));

    // Takes: A, a grey ramp (Y = 16 + 3n), and B, a test pattern with a red left and a green top stripe.
    const QString takeA = scratch.filePath("takeA.mp4"), takeB = scratch.filePath("takeB.mp4");
    bool ok = false;
    run("ffmpeg", {"-v", "error", "-y", "-f", "lavfi", "-i", "color=c=black:s=1376x768:r=24:d=4,format=yuv420p,geq=lum='16+3*N':cb=128:cr=128",
                   "-frames:v", "90", "-c:v", "libx264", "-qp", "0", "-pix_fmt", "yuv420p", takeA}, &ok);
    if (!ok) return fail(3, "ffmpeg could not make take A");
    run("ffmpeg", {"-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=s=864x480:r=24:d=4,drawbox=x=0:y=0:w=40:h=ih:color=red:t=fill,drawbox=x=0:y=0:w=iw:h=40:color=lime:t=fill",
                   "-frames:v", "96", "-c:v", "libx264", "-qp", "0", "-pix_fmt", "yuv420p", takeB}, &ok);
    if (!ok) return fail(3, "ffmpeg could not make take B");
    // Shots: 1 [24, 96) with take A from frame 6 (anchored at 18), 2 [96, 168) with take B, 3 [168, 216) with none.
    auto addShot = [&](int start, int end) { return plan->run({{"op", "shot.add"}, {"track", 1}, {"start", start}, {"end", end}})["id"].toInt(); };
    const int s1 = addShot(24, 96), s2 = addShot(96, 168), s3 = addShot(168, 216);
    if (!s1 || !s2 || !s3) return fail(4, "could not plan the three shots");
    plan->run({{"op", "set"}, {"kind", "shot"}, {"id", s3}, {"fields", QVariantMap{{"purpose", "The radio flickers on"}}}});
    auto addTake = [&](int shot, const QString& path, int frames, int anchor, int w, int h) {
        const auto r = plan->run({{"op", "take.add"}, {"shot", shot}, {"path", path}, {"frames", frames}, {"fps", 24}, {"anchor", anchor}, {"width", w}, {"height", h}});
        if (!r["ok"].toBool()) return false;
        return plan->run({{"op", "set"}, {"kind", "take"}, {"id", r["id"].toInt()}, {"fields", QVariantMap{{"approved", true}}}})["ok"].toBool();
    };
    if (!addTake(s1, takeA, 90, 18, 1376, 768) || !addTake(s2, takeB, 96, 96, 864, 480)) return fail(4, "could not add and choose the takes");
    if (plan->timelineTakes().size() != 2) return fail(4, "the cut does not play both takes");

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
    QTest::qWait(300);
    auto root = window->contentItem();
    const QString shots = qEnvironmentVariable("CHORUS_SHOTS");
    auto capture = [&](const QString& name) { if (!shots.isEmpty()) window->grabWindow().save(shots + "/" + name + ".png"); };
    auto center = [&](QQuickItem* item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); };
    auto click = [&](QQuickItem* item) {
        if (!item) { qCritical() << "nothing to click"; return; }
        QTest::qWait(50);  // let layouts settle after blocking ffmpeg runs
        QTest::mouseClick(window, Qt::LeftButton, {}, center(item));
        QTest::qWait(120);
    };
    auto typeInto = [&](QQuickItem* field, const QString& text) {
        click(field);
        QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
        for (const QChar ch : text) QTest::keyClick(window, ch.toLatin1());
        QTest::qWait(80);
    };
    Renderer* renderer = editor.renderer();
    auto popup = window->findChild<QObject*>("renderPopup");
    auto openDialog = [&] {
        click(find(root, "renderButton"));
        return waitFor([&] { return popup->property("opened").toBool(); }, 1500);
    };
    auto render = [&](int timeout) {
        click(find(root, "renderGo"));
        if (!waitFor([&] { return renderer->busy(); }, 3000)) return false;
        return waitFor([&] { return !renderer->busy(); }, timeout) && renderer->message().startsWith("Saved");
    };
    auto probe = [&](const QString& file) {
        return QJsonDocument::fromJson(run("ffprobe", {"-v", "error", "-count_frames", "-show_streams", "-show_format", "-of", "json", file}).toUtf8()).object();
    };
    auto stream = [&](const QJsonObject& info, const QString& type) {
        for (const auto& v : info["streams"].toArray()) if (v.toObject()["codec_type"] == type) return v.toObject();
        return QJsonObject();
    };

    // 1. A selection by I and O, frames [0, 240): native size (take A's, the largest), Fill, CRF 19.
    window->requestActivate();
    QTest::qWaitForWindowActive(window);
    editor.seek(0);
    QTest::keyClick(window, Qt::Key_I);
    editor.seek(240);
    QTest::keyClick(window, Qt::Key_O);
    if (window->property("selection").toMap()["end"].toInt() != 10000) return fail(6, "I and O did not select the first ten seconds");
    if (!openDialog()) return fail(6, "Render… did not open the dialog");
    if (popup->property("part") != "selection" || !find(root, "renderPartSelection")->property("text").toString().contains("Range · 10.0 s"))
        return fail(6, "the dialog did not offer the selection: " + find(root, "renderPartSelection")->property("text").toString());
    if (!find(root, "renderSizeNative")->property("text").toString().endsWith("1376 × 768") || !find(root, "renderSize1280x720") || !find(root, "renderSize1920x1080"))
        return fail(6, "the sizes are not the largest take's and the standard 16:9 ones");
    if (popup->property("fit") != "fill" || popup->property("crf").toInt() != 19) return fail(6, "Fill and CRF 19 are not the defaults");
    auto missing = find(root, "renderMissing");
    if (!missing || !missing->childItems().value(0) || !missing->childItems()[0]->property("text").toString().startsWith("Shot 3 has no take yet"))
        return fail(6, "the dialog does not say which shot has no take");
    const QString path1 = popup->property("path").toString();
    if (QFileInfo(path1).fileName() != QFileInfo(argv[1]).fileName() + " – Range.mp4" || QFileInfo(path1).dir().dirName() != "Renders")
        return fail(6, "the default file is not in Renders named after the project and the part: " + path1);
    capture("render-dialog");
    const QPoint closeBefore = center(find(root, "renderClose")), goBefore = center(find(root, "renderGo"));
    if (!render(180000)) return fail(7, "the first render did not finish: " + renderer->message());
    QTest::qWait(100);
    if (center(find(root, "renderClose")) != closeBefore || center(find(root, "renderGo")) != goBefore)
        return fail(7, "Close or Render moved when the render finished, where a click could start it again");
    capture("render-done");
    if (!find(root, "renderReveal")) return fail(7, "no Show in Finder after the render");
    {
        const auto info = probe(path1);
        const auto v = stream(info, "video"), a = stream(info, "audio");
        if (v["codec_name"] != "h264" || v["width"].toInt() != 1376 || v["height"].toInt() != 768 || v["pix_fmt"] != "yuv420p" || v["nb_read_frames"].toString().toInt() != 240)
            return fail(8, "the video stream is wrong: " + QString::fromUtf8(QJsonDocument(v).toJson(QJsonDocument::Compact)));
        if (a["codec_name"] != "aac" || std::abs(a["duration"].toString().toDouble() - 10.0) > 0.08)
            return fail(8, "the audio is not AAC of the selection's length: " + QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact)));
        const QRect middle(600, 300, 176, 168);
        auto grey = [&](int n) { return meanLuma(frameOf(takeA, n, scratch.path()), middle); };
        // Before shot 1: nothing planned, black. Shot 1 starts on take A's frame 6 and ends on 77.
        if (meanLuma(frameOf(path1, 12, scratch.path()), middle) > 6) return fail(9, "the frames before the first shot are not black");
        const double r24 = meanLuma(frameOf(path1, 24, scratch.path()), middle), r95 = meanLuma(frameOf(path1, 95, scratch.path()), middle);
        if (std::abs(r24 - grey(6)) > 1.5 || std::abs(r95 - grey(77)) > 1.5)
            return fail(9, QString("shot 1's first and last frames are not take A's frames 6 and 77: %1 vs %2, %3 vs %4").arg(r24).arg(grey(6)).arg(r95).arg(grey(77)));
        // Shot 2 starts on take B's frame 0, filled to 1376×768 (1382.4×768, 3 px off each side), smoothly.
        const QImage cut = frameOf(path1, 96, scratch.path());
        const QImage smooth = frameOf(takeB, 0, scratch.path(), "scale=1383:768:flags=bicubic,crop=1376:768");
        const QImage nearest = frameOf(takeB, 0, scratch.path(), "scale=1383:768:flags=neighbor,crop=1376:768");
        const double ps = psnr(cut, smooth), pn = psnr(cut, nearest);
        if (ps < 28 || ps < pn + 0.5)
            return fail(10, QString("shot 2's first frame is not take B scaled smoothly: %1 dB from bicubic, %2 dB from nearest").arg(ps, 0, 'f', 1).arg(pn, 0, 'f', 1));
        if (!isGreen(cut.pixel(688, 15)) || !isRed(cut.pixel(20, 400))) return fail(10, "take B's stripes are not where Fill puts them");
        // Shot 3 has no take: a card, mostly its dark background with light lettering.
        const QImage card = frameOf(path1, 190, scratch.path());
        int light = 0;
        for (int y = 0; y < card.height(); y += 2) for (int x = 0; x < card.width(); x += 2) light += qGray(card.pixel(x, y)) > 180;
        if (std::abs(meanLuma(card, QRect(4, 4, 16, 16)) - qGray(qRgb(0x14, 0x1c, 0x20))) > 4 || light < 200)
            return fail(11, QString("shot 3 is not a card (%1 light pixels)").arg(light));
        // After shot 3, black again.
        if (meanLuma(frameOf(path1, 230, scratch.path()), middle) > 6) return fail(11, "after the last shot is not black");
    }

    // 2. Shot 2 alone, clicked on the timeline, at a custom 1080 × 1080 with each treatment.
    click(find(root, "renderClose"));
    QMetaObject::invokeMethod(find(root, "timeline"), "zoomTo", Q_ARG(QVariant, 0), Q_ARG(QVariant, 10000));
    QTest::qWait(200);
    click(find(root, QString("shotCard%1").arg(s2)));
    if (window->property("selection").toMap()["kind"] != "shot") return fail(12, "clicking shot 2 did not select it");
    struct Look { QString fit, button; };
    QStringList files;
    for (const Look& look : {Look{"fit", "renderFitFit"}, Look{"stretch", "renderFitStretch"}, Look{"fill", "renderFitFill"}}) {
        if (!openDialog()) return fail(12, "Render… did not open the dialog again");
        if (!find(root, "renderPartSelection")->property("text").toString().contains("Shot 2 · 3.0 s")) return fail(12, "the dialog did not offer shot 2");
        qInfo().noquote() << "render" << look.fit << "to" << popup->property("path").toString();
        click(find(root, "renderSizeCustom"));
        typeInto(find(root, "renderWidth"), "1080");
        typeInto(find(root, "renderHeight"), "1080");
        click(find(root, look.button));
        typeInto(find(root, "renderCrf"), "23");
        if (popup->property("outW").toInt() != 1080 || popup->property("outH").toInt() != 1080 || popup->property("fit") != look.fit || popup->property("crf").toInt() != 23)
            return fail(12, "the dialog did not take the size, treatment and CRF typed and clicked");
        if (look.fit == "fit") capture("render-dialog-custom");
        if (!render(60000)) return fail(12, look.fit + " render did not finish: " + renderer->message());
        files << renderer->file();
        const auto v = stream(probe(renderer->file()), "video");
        if (v["width"].toInt() != 1080 || v["height"].toInt() != 1080 || v["nb_read_frames"].toString().toInt() != 72)
            return fail(13, look.fit + ": not 72 frames of 1080 × 1080: " + QString::fromUtf8(QJsonDocument(v).toJson(QJsonDocument::Compact)));
        const QImage f = frameOf(renderer->file(), 0, scratch.path());
        // B is 864×480. Fit: 1080×600 with bars above and below. Stretch: the whole frame. Fill: 1944×1080, the sides cropped.
        const bool right = look.fit == "fit" ? isBlack(f.pixel(540, 100)) && isRed(f.pixel(20, 540)) && isGreen(f.pixel(540, 260)) && isBlack(f.pixel(540, 980))
                         : look.fit == "stretch" ? isRed(f.pixel(20, 540)) && isGreen(f.pixel(540, 40)) && !isBlack(f.pixel(540, 1000))
                         : isGreen(f.pixel(540, 40)) && !isRed(f.pixel(20, 540)) && !isBlack(f.pixel(540, 1000));
        if (!right) return fail(14, look.fit + " did not place take B as it should");
        click(find(root, "renderClose"));
        if (!waitFor([&] { return !popup->property("visible").toBool(); }, 1500)) return fail(14, "Close did not close the dialog");
    }
    if (renderer->crf() != 23) return fail(14, "the CRF typed was not remembered");
    if (QFileInfo(files[1]).fileName() != QFileInfo(argv[1]).fileName() + " – Shot 2 2.mp4") return fail(14, "a second render of shot 2 did not get a free name: " + files[1]);

    // 3. The whole song, cancelled part way: no file left.
    if (!openDialog()) return fail(15, "Render… did not open the dialog for the whole song");
    click(find(root, "renderPartSong"));
    click(find(root, "renderSizeNative"));
    const QString whole = popup->property("path").toString();
    if (QFileInfo(whole).fileName() != QFileInfo(argv[1]).fileName() + ".mp4") return fail(15, "the whole song's file is not named after the project: " + whole);
    click(find(root, "renderGo"));
    if (!waitFor([&] { return renderer->busy() && renderer->progress() > 0.01; }, 30000)) return fail(15, "the whole-song render did not get going");
    if (!find(root, "renderButton")->property("text").toString().startsWith("Rendering ")) return fail(15, "the top bar does not show the render's progress");
    capture("render-progress");
    click(find(root, "renderCancel"));
    if (!waitFor([&] { return !renderer->busy(); }, 5000) || renderer->message() != "Render cancelled.") return fail(15, "Cancel did not stop the render");
    if (QFileInfo::exists(whole) || QFileInfo::exists(whole + ".part")) return fail(15, "a cancelled render left a file behind");

    // 4. The exported timeline (FCP 7 XML): each chosen take on its track at its frames, the
    // upscaled version where there is one, a file used twice described once, the song below.
    // A V2 shot [100, 120) plays take A again from its first frame; take B gets an upscaled version.
    const int s4 = plan->run({{"op", "shot.add"}, {"track", 2}, {"start", 100}, {"end", 120}})["id"].toInt();
    if (!s4 || !addTake(s4, takeA, 90, 100, 1376, 768)) return fail(16, "could not plan the V2 shot with take A");
    const QString takeBUp = scratch.filePath("takeB-up.mp4");
    run("ffmpeg", {"-v", "error", "-y", "-i", takeB, "-vf", "scale=1728:960", "-c:v", "libx264", "-qp", "0", "-pix_fmt", "yuv420p", takeBUp}, &ok);
    if (!ok) return fail(16, "ffmpeg could not upscale take B");
    int takeBId = 0;
    for (const auto& v : plan->timelineTakes()) if (v.toMap()["shot"].toInt() == s2) takeBId = v.toMap()["take"].toInt();
    if (!plan->run({{"op", "take.upscale"}, {"id", takeBId}, {"path", takeBUp}, {"width", 1728}, {"height", 960}, {"frames", 96}})["ok"].toBool())
        return fail(16, "could not give take B an upscaled version");
    const auto finalTakes = plan->timelineTakes({}, true);
    if (finalTakes.size() != 3) return fail(16, "the cut does not play three takes");
    QHash<int, QString> fileOf;
    for (const auto& v : finalTakes) fileOf.insert(v.toMap()["shot"].toInt(), v.toMap()["file"].toString());
    const QString xmlPath = scratch.filePath("export.xml");
    if (!editor.exportTimeline(QUrl::fromLocalFile(xmlPath))) return fail(17, "export failed: " + editor.status());
    {
        const Node doc = readXml(xmlPath);
        const Node* seq = doc.child("sequence");
        if (doc.name != "xmeml" || !seq) return fail(17, "the export is not an xmeml sequence");
        if (seq->at("duration").toInt() != editor.length() || seq->at("rate/timebase") != "24" || seq->at("rate/ntsc") != "FALSE")
            return fail(17, "the sequence's length or rate is wrong");
        if (seq->at("media/video/format/samplecharacteristics/width") != "1728" || seq->at("media/video/format/samplecharacteristics/height") != "960")
            return fail(17, "the sequence frame is not the largest take's (the upscaled 1728 × 960)");
        const auto tracks = seq->child("media")->child("video")->all("track");
        if (tracks.size() != 2) return fail(17, QString("expected V1 and V2, got %1 video tracks").arg(tracks.size()));
        QSet<QString> clipIds, defined;
        struct Want { int track, shot, start, end, in; QString width; };
        const QList<Want> wants{{0, s1, 24, 96, 6, "1376"}, {0, s2, 96, 168, 0, "1728"}, {1, s4, 100, 120, 0, QString()}};
        QList<QList<const Node*>> clips{tracks[0]->all("clipitem"), tracks[1]->all("clipitem")};
        if (clips[0].size() != 2 || clips[1].size() != 1) return fail(18, QString("expected 2 clips on V1 and 1 on V2, got %1 and %2").arg(clips[0].size()).arg(clips[1].size()));
        QList<int> next{0, 0};
        for (const Want& w : wants) {
            const Node* c = clips[w.track][next[w.track]++];
            const QString where = QString("shot %1's clip").arg(plan->shot(w.shot)["number"].toInt());
            if (c->at("start").toInt() != w.start || c->at("end").toInt() != w.end || c->at("in").toInt() != w.in || c->at("out").toInt() != w.in + w.end - w.start)
                return fail(18, where + QString(" is at %1–%2 from %3, not %4–%5 from %6").arg(c->at("start"), c->at("end"), c->at("in")).arg(w.start).arg(w.end).arg(w.in));
            if (c->at("name") != QString("Shot %1, take 1").arg(plan->shot(w.shot)["number"].toInt())) return fail(18, where + " is named " + c->at("name"));
            const Node* f = c->child("file");
            if (!f) return fail(18, where + " has no file");
            const QString id = f->attrs.value("id").toString();
            if (clipIds.contains(c->attrs.value("id").toString())) return fail(18, where + " reuses a clip id");
            clipIds << c->attrs.value("id").toString();
            if (f->children.isEmpty()) {
                if (!defined.contains(id)) return fail(18, where + " refers to a file not described before it");
                continue;
            }
            defined << id;
            const QString path = QUrl(f->at("pathurl")).toLocalFile();
            if (path != fileOf[w.shot] || !QFileInfo::exists(path)) return fail(18, where + " points at " + path + ", not " + fileOf[w.shot]);
            if (f->at("media/video/samplecharacteristics/width") != w.width) return fail(18, where + "'s file is " + f->at("media/video/samplecharacteristics/width") + " wide");
        }
        if (!clips[1][0]->child("file")->children.isEmpty()) return fail(18, "take A's file is described twice");
        const auto audio = seq->child("media")->child("audio")->all("track");
        if (audio.isEmpty() || audio[0]->at("clipitem/end").toInt() != editor.length() || !QFileInfo::exists(QUrl(audio[0]->at("clipitem/file/pathurl")).toLocalFile()))
            return fail(19, "the song is not under the whole sequence");
    }

    qInfo() << "PASS: render through the dialog: selection by I and O, native size, Fill, CRF; H.264/AAC, size, frames, audio length;"
               " the takes' right frames at each cut, smooth scaling, a card for the shot without a take; Fit, Stretch and Fill at a typed"
               " custom size; CRF remembered; free names; progress in the top bar; Cancel leaves no file; the exported XML has each"
               " chosen take on V1 and V2 at its frames, the upscaled version, shared files described once, and the song.";
    return 0;
}
