#include "render.h"
#include "editor.h"
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QUrl>
#include <cmath>
#include <mlt++/Mlt.h>

namespace {
// Standard sizes for each of the plan's aspect ratios, smallest first.
const QList<QPair<QString, QList<QSize>>> kSizes = {
    {"16:9", {QSize(1280, 720), QSize(1920, 1080), QSize(3840, 2160)}},
    {"9:16", {QSize(720, 1280), QSize(1080, 1920)}},
    {"1:1", {QSize(720, 720), QSize(1080, 1080)}},
    {"4:5", {QSize(864, 1080), QSize(1080, 1350)}},
    {"2.39:1", {QSize(1280, 536), QSize(1920, 804)}},
};
double ratioOf(const QString& aspect) {
    const auto parts = aspect.split(':');
    return parts.size() == 2 && parts[1].toDouble() > 0 ? parts[0].toDouble() / parts[1].toDouble() : 0;
}
QString clean(QString name) {
    for (const QChar c : QStringLiteral("/\\:*?\"<>|")) name.replace(c, '-');
    return name.simplified();
}
}

Renderer::Renderer(Editor* editor) : QObject(editor), editor(editor) {
    quality = qBound(0, QSettings().value("render/crf", 19).toInt(), 51);
    poll.setInterval(200);
    connect(&poll, &QTimer::timeout, this, &Renderer::check);
}

Renderer::~Renderer() {
    poll.stop();
    if (consumer && !consumer->is_stopped()) consumer->stop();
    consumer.reset();
    tractor.reset();
    if (!part.isEmpty()) QFile::remove(part);
}

void Renderer::setCrf(int value) {
    value = qBound(0, value, 51);
    if (value == quality) return;
    quality = value;
    QSettings().setValue("render/crf", value);
    emit crfChanged();
}

QPair<int, int> Renderer::frames(double fps, int length, int startMs, int endMs) {
    if (startMs < 0 || endMs < 0) return {0, length};
    const int a = qBound(0, int(std::floor(startMs * fps / 1000.0 + 1e-6)), length);
    const int b = qBound(0, int(std::ceil(endMs * fps / 1000.0 - 1e-6)), length);
    return {a, b};
}

QVariantMap Renderer::options() const {
    Plan* plan = editor->plan();
    QSize native;
    QSet<int> covered;
    int upscaled = 0;
    for (const auto& v : plan->timelineTakes({}, true)) {
        const auto t = v.toMap();
        covered.insert(t["shot"].toInt());
        // Upscaled versions count: they are what renders.
        const QSize size(t["width"].toInt(), t["height"].toInt());
        if (size.width() * size.height() > native.width() * native.height()) native = size;
        if (t["upscaled"].toBool() || plan->item("take", t["take"].toInt())["hd"].toBool()) ++upscaled;
    }
    QString aspect = plan->concept()["fields"].toMap()["aspect"].toString();
    if (native.isEmpty()) native = editor->pictureSize();
    if (native.isEmpty()) {
        // No takes and no footage yet: the plan's shape at 1080 lines (or 1920 wide).
        const double r = ratioOf(aspect) > 0 ? ratioOf(aspect) : 16.0 / 9;
        native = r >= 1 ? QSize(int(std::round(1080 * r)) & ~1, 1080) : QSize(1080, int(std::round(1080 / r)) & ~1);
    }
    native = QSize(native.width() & ~1, native.height() & ~1);
    if (ratioOf(aspect) <= 0) {
        // Not chosen in the plan: the standard shape nearest the native size.
        const double r = double(native.width()) / native.height();
        double best = 1e9;
        for (const auto& [name, _] : kSizes)
            if (std::abs(std::log(ratioOf(name) / r)) < best) { best = std::abs(std::log(ratioOf(name) / r)); aspect = name; }
    }
    QVariantList sizes;
    for (const auto& [name, list] : kSizes)
        if (name == aspect)
            for (const QSize& s : list) sizes.append(QVariantMap{{"label", QString("%1 × %2").arg(s.width()).arg(s.height())}, {"width", s.width()}, {"height", s.height()}});
    QVariantList missing;
    int shots = 0;
    for (const auto& v : plan->shots()) {
        const auto s = v.toMap();
        ++shots;
        if (!covered.contains(s["id"].toInt())) missing.append(s["number"].toInt());
    }
    return {{"native", QVariantMap{{"width", native.width()}, {"height", native.height()}}}, {"aspect", aspect}, {"sizes", sizes},
            {"missing", missing}, {"shots", shots}, {"name", editor->projectName()}, {"frames", editor->length()},
            {"upscaled", upscaled}, {"withTakes", covered.size()}};
}

QString Renderer::defaultPath(const QString& part) const {
    const QDir dir(QDir(editor->projectDir()).filePath("Renders"));
    const QString base = clean(editor->projectName() + (part.isEmpty() ? QString() : " – " + part));
    QString path = dir.filePath(base + ".mp4");
    for (int n = 2; QFileInfo::exists(path); ++n) path = dir.filePath(QString("%1 %2.mp4").arg(base).arg(n));
    return path;
}

// A stand-in for a shot with no take yet: its number, what it is, who and where, and its purpose.
QString Renderer::card(int shotId, QSize size) {
    Plan* plan = editor->plan();
    const auto s = plan->shot(shotId);
    const auto f = s["fields"].toMap();
    QStringList who;
    for (const auto& e : f["who"].toList()) {
        const auto c = plan->item("character", e.toMap()["character"].toInt());
        if (!c.isEmpty()) who << c["fields"].toMap()["name"].toString();
    }
    const auto where = plan->item("location", f["where"].toInt());
    QStringList about;
    if (!s["typeLabel"].toString().isEmpty()) about << s["typeLabel"].toString();
    if (!who.isEmpty()) about << who.join(", ");
    if (!where.isEmpty()) about << where["fields"].toMap()["name"].toString();

    QImage image(size, QImage::Format_RGB32);
    image.fill(QColor("#141c20"));
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    const int unit = qMax(8, qMin(size.width(), size.height()) / 18);
    const QRect area = image.rect().adjusted(unit * 2, unit * 2, -unit * 2, -unit * 2);
    p.setPen(QPen(QColor("#34434a"), qMax(1, unit / 6)));
    p.drawRoundedRect(image.rect().adjusted(unit, unit, -unit, -unit), unit / 2, unit / 2);
    QFont title; title.setPixelSize(unit * 2); title.setWeight(QFont::DemiBold);
    QFont body; body.setPixelSize(qMax(8, unit));
    QFont small; small.setPixelSize(qMax(8, unit * 3 / 4));
    p.setFont(title); p.setPen(QColor("#edf1ef"));
    QRect line = p.boundingRect(area, Qt::AlignHCenter | Qt::TextWordWrap, "Shot " + QString::number(s["number"].toInt()));
    p.setFont(body);
    const QString aboutText = about.join(" · ");
    const QString purpose = f["purpose"].toString();
    QRect aboutRect = p.boundingRect(area, Qt::AlignHCenter | Qt::TextWordWrap, aboutText);
    QRect purposeRect = p.boundingRect(area, Qt::AlignHCenter | Qt::TextWordWrap, purpose);
    const int gap = unit / 2;
    const int total = line.height() + (aboutText.isEmpty() ? 0 : gap + aboutRect.height()) + (purpose.isEmpty() ? 0 : gap + purposeRect.height()) + gap + unit;
    int y = area.top() + qMax(0, (area.height() - total) / 2);
    p.setFont(title);
    p.drawText(QRect(area.left(), y, area.width(), line.height()), Qt::AlignHCenter | Qt::TextWordWrap, "Shot " + QString::number(s["number"].toInt()));
    y += line.height() + gap;
    p.setFont(body);
    if (!aboutText.isEmpty()) {
        p.setPen(QColor("#87dec7"));
        p.drawText(QRect(area.left(), y, area.width(), aboutRect.height()), Qt::AlignHCenter | Qt::TextWordWrap, aboutText);
        y += aboutRect.height() + gap;
    }
    if (!purpose.isEmpty()) {
        p.setPen(QColor("#c3cdce"));
        p.drawText(QRect(area.left(), y, area.width(), purposeRect.height()), Qt::AlignHCenter | Qt::TextWordWrap, purpose);
        y += purposeRect.height() + gap;
    }
    p.setFont(small); p.setPen(QColor("#8fa0a4"));
    p.drawText(QRect(area.left(), y, area.width(), unit * 2), Qt::AlignHCenter, "No take yet");
    p.end();
    const QString path = cardDir->filePath(QString("shot-%1.png").arg(shotId));
    return image.save(path) ? path : QString();
}

bool Renderer::start(const QVariantMap& r) {
    auto refuse = [&](const QString& why) { note = why; emit stateChanged(); return false; };
    if (running) return refuse("A render is already running.");
    if (!editor->ready()) return refuse("Open a project first.");
    const int width = r.value("width").toInt(), height = r.value("height").toInt();
    if (width < 16 || height < 16 || width > 7680 || height > 4320 || width % 2 || height % 2)
        return refuse("Width and height must be even numbers from 16 to 7680 × 4320.");
    const QString fit = r.value("fit", "fill").toString();
    if (fit != "fill" && fit != "fit" && fit != "stretch") return refuse("Choose Fill, Fit or Stretch.");
    const int crf = r.value("crf", quality).toInt();
    if (crf < 0 || crf > 51) return refuse("CRF runs from 0 to 51.");
    target = r.value("path").toString();
    if (target.startsWith("file:")) target = QUrl(target).toLocalFile();
    if (target.isEmpty()) return refuse("Choose where to save the video.");
    if (!target.endsWith(".mp4", Qt::CaseInsensitive)) target += ".mp4";
    if (!QDir().mkpath(QFileInfo(target).absolutePath())) return refuse("Cannot create " + QFileInfo(target).absolutePath() + ".");
    const auto [a, b] = frames(editor->fps(), editor->length(), r.value("startMs", -1).toInt(), r.value("endMs", -1).toInt());
    if (b <= a) return refuse("The part to render is empty.");
    first = a; last = b;
    setCrf(crf);

    profile = std::make_unique<Mlt::Profile>("atsc_720p_24");
    profile->set_width(width);
    profile->set_height(height);
    profile->set_sample_aspect(1, 1);
    profile->set_display_aspect(width, height);
    profile->set_frame_rate(editor->fpsNum(), editor->fpsDen());
    profile->set_progressive(1);
    profile->set_explicit(1);

    // A card for each shot with no take, drawn at the output size.
    cardDir = std::make_unique<QTemporaryDir>();
    Editor::Cut cut;
    cut.fit = fit;
    cut.upscaled = true;
    QSet<int> covered;
    for (const auto& v : editor->plan()->timelineTakes()) covered.insert(v.toMap()["shot"].toInt());
    for (const auto& v : editor->plan()->shots()) {
        const auto s = v.toMap();
        const int id = s["id"].toInt();
        if (covered.contains(id) || s["end"].toInt() <= first || s["start"].toInt() >= last) continue;
        const QString path = card(id, QSize(width, height));
        if (!path.isEmpty()) cut.cards.insert(id, path);
    }
    tractor = std::make_unique<Mlt::Tractor>(*profile);
    editor->assemble(*tractor, *profile, cut);
    tractor->set_in_and_out(first, last - 1);
    // A part of the song starts and ends with a one-frame fade, so it never clicks.
    if (first > 0 || last < editor->length()) {
        Mlt::Filter fade(*profile, "volume");
        fade.set("level", QString("%1=-60;%2=0;%3=0;%4=-60").arg(first).arg(first + 1).arg(qMax(first + 1, last - 2)).arg(last - 1).toUtf8().constData());
        tractor->attach(fade);
    }

    part = target + ".part";
    QFile::remove(part);
    consumer = std::make_unique<Mlt::Consumer>(*profile, "avformat", part.toUtf8().constData());
    if (!consumer->is_valid()) return refuse("MLT cannot write video files here (no avformat consumer).");
    consumer->set("f", "mp4");
    consumer->set("vcodec", "libx264");
    consumer->set("crf", crf);
    consumer->set("preset", "medium");
    consumer->set("pix_fmt", "yuv420p");
    consumer->set("acodec", "aac");
    consumer->set("ab", "256k");
    consumer->set("frequency", 48000);
    consumer->set("channels", 2);
    consumer->set("movflags", "+faststart");
    consumer->set("rescale", "bicubic");   // never MLT's nearest-neighbour: scaled takes would shimmer
    consumer->set("real_time", -1);        // every frame, in order
    consumer->set("terminate_on_pause", 1);
    consumer->connect(*tractor);
    tractor->seek(0);  // positions count from the in point
    tractor->set_speed(1);
    if (consumer->start() != 0) { consumer.reset(); tractor.reset(); return refuse("The render could not start."); }
    running = true;
    done = 0;
    note = QString("Rendering %1 × %2…").arg(width).arg(height);
    poll.start();
    emit stateChanged();
    emit progressChanged();
    return true;
}

void Renderer::check() {
    if (!running || !consumer) return;
    const double now = qBound(0.0, double(tractor->position()) / qMax(1, last - first), 1.0);
    if (now != done) { done = now; emit progressChanged(); }
    if (!consumer->is_stopped()) return;
    consumer->stop();
    const bool ok = QFileInfo(part).size() > 0;
    if (ok) {
        QFile::remove(target);
        if (!QFile::rename(part, target)) { finish(false, "Rendered, but could not save " + target + "."); return; }
    }
    finish(ok, ok ? "Saved " + QFileInfo(target).fileName() + "." : "The render failed: nothing was written.");
}

void Renderer::finish(bool ok, const QString& text) {
    poll.stop();
    consumer.reset();
    tractor.reset();
    cardDir.reset();
    if (!ok) QFile::remove(part);
    part.clear();
    running = false;
    note = text;
    if (ok) { finishedFile = target; done = 1; emit progressChanged(); }
    emit stateChanged();
    emit finished(ok, ok ? target : QString());
}

void Renderer::cancel() {
    if (!running) return;
    if (consumer) consumer->stop();
    finish(false, "Render cancelled.");
}

void Renderer::reveal() const {
    if (finishedFile.isEmpty()) return;
#if defined(Q_OS_MACOS)
    QProcess::startDetached("/usr/bin/open", {"-R", finishedFile});
#elif defined(Q_OS_WIN)
    QProcess::startDetached("explorer.exe", {"/select," + QDir::toNativeSeparators(finishedFile)});
#else
    // File managers differ on Linux; open the folder that contains it.
    QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(finishedFile).absolutePath()));
#endif
}
