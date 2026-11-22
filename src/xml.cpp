#include "editor.h"
#include <QSaveFile>
#include <QFileInfo>
#include <QXmlStreamWriter>
#include <cmath>

// The cut as Final Cut Pro 7 XML (xmeml 5), which DaVinci Resolve, Premiere and others import.
// The picture tracks are what assemble() plays, bottom first: the base performance and the
// cutaway when the project has footage, then each shot's chosen take on V1 and V2 (upscaled
// versions where they exist, as a render uses). The master song is the only sound. Shots without
// a take are left empty; the render's cards are not exported.
bool Editor::exportTimeline(const QUrl& destination) {
    if (!ready()) return false;
    QSaveFile file(destination.toLocalFile());
    if (!file.open(QIODevice::WriteOnly)) { info(file.errorString()); return false; }
    QXmlStreamWriter x(&file);
    x.setAutoFormatting(true);
    auto open=[&](const QString& tag){ x.writeStartElement(tag); };
    auto close=[&](){ x.writeEndElement(); };
    auto text=[&](const QString& tag,const QString& val){ x.writeTextElement(tag,val); };
    auto number=[&](const QString& tag,int val){ text(tag,QString::number(val)); };
    const bool ntsc = project->fpsDen == 1001;
    auto rateOf=[&](double fps, bool isNtsc){ open("rate"); number("timebase",qRound(fps)); text("ntsc",isNtsc?"TRUE":"FALSE"); close(); };
    auto rate=[&](){ rateOf(fps(), ntsc); };
    auto pictureFormat=[&](QSize size){
        open("samplecharacteristics"); rate(); number("width",size.width()); number("height",size.height());
        text("anamorphic","FALSE"); text("pixelaspectratio","square"); text("fielddominance","none"); close();
    };
    auto audioFormat=[&](){ open("samplecharacteristics"); if (project->master.bits > 0) number("depth",project->master.bits); number("samplerate",project->master.sampleRate); close(); };

    // A file is described in full the first time a clip uses it, then referred to by its id.
    struct Source { QString path; int frames; double fps; bool isNtsc; QSize size; bool audio; int channels; };
    QHash<QString, QString> fileIds;
    int clipSerial = 0;
    auto clip=[&](const QString& name, const Source& s, int start, int end, int in, int length, int channel = 0){
        open("clipitem"); x.writeAttribute("id",QString("clipitem-%1").arg(++clipSerial));
        text("name",name); text("enabled","TRUE"); number("duration",length); rate();
        number("start",start); number("end",end); number("in",in); number("out",in+end-start);
        open("file");
        const bool known = fileIds.contains(s.path);
        if (!known) fileIds.insert(s.path, QString("file-%1").arg(fileIds.size()+1));
        x.writeAttribute("id",fileIds[s.path]);
        if (!known) {
            text("name",QFileInfo(s.path).fileName()); text("pathurl",QUrl::fromLocalFile(s.path).toString(QUrl::FullyEncoded));
            rateOf(s.fps, s.isNtsc); number("duration",s.frames); open("media");
            if (s.audio) { open("audio"); audioFormat(); number("channelcount",s.channels); close(); }
            else { open("video"); pictureFormat(s.size); close(); }
            close();
        }
        close();
        if (s.audio) { open("sourcetrack"); text("mediatype","audio"); number("trackindex",channel); close(); }
        close();
    };
    auto endTrack=[&](){ text("enabled","TRUE"); text("locked","FALSE"); close(); };
    auto footage=[&](const Media& m, bool audio){
        return Source{project->file(m), m.frames, fps(), ntsc, QSize(m.width, m.height), audio, m.channels};
    };

    const QVariantMap native = render->options()["native"].toMap();
    const QSize frameSize(native["width"].toInt(), native["height"].toInt());
    const QVariantList takes = planState->timelineTakes({}, true);
    QList<int> takeIds;
    for (const auto& v : takes) takeIds << v.toMap()["take"].toInt();
    QHash<int, QVariantMap> placed;  // take -> {shotNumber, number}
    for (const auto& v : planState->placeTakes(takeIds)) placed.insert(v.toMap()["take"].toInt(), v.toMap());

    x.writeStartDocument(); x.writeDTD("<!DOCTYPE xmeml>"); open("xmeml"); x.writeAttribute("version","5");
    open("sequence"); x.writeAttribute("id","sequence-1");
    text("name",projectName()); number("duration",length()); rate();
    open("timecode"); rate(); text("string","00:00:00:00"); number("frame",0); text("displayformat","NDF"); close();
    open("media");
    open("video"); open("format"); pictureFormat(frameSize); close();
    if (sourceLength()) {
        open("track");
        clip(performanceName(), footage(project->performance,false), 0, baseLength(), 0, project->performance.frames);
        endTrack();
        open("track");
        clip(cutawayName(), footage(project->cutaway,false), trim.start, trim.end, trim.source, project->cutaway.frames);
        endTrack();
    }
    for (int track : {1, 2}) {
        open("track");
        for (const auto& v : takes) {
            const auto t = v.toMap();
            if (t["track"].toInt() != track) continue;
            const double rateOfTake = t["fps"].toDouble();
            const bool takeNtsc = std::abs(rateOfTake - qRound(rateOfTake)) > 0.001;
            const Source s{t["file"].toString(), t["frames"].toInt(), rateOfTake, takeNtsc, QSize(t["width"].toInt(), t["height"].toInt()), false, 0};
            const auto p = placed.value(t["take"].toInt());
            const QString name = QString("Shot %1, take %2").arg(p["shotNumber"].toInt()).arg(p["number"].toInt());
            clip(name, s, t["start"].toInt(), t["end"].toInt(), t["in"].toInt(), t["length"].toInt());
        }
        endTrack();
    }
    close();
    open("audio"); number("numOutputChannels",project->master.channels); open("format"); audioFormat(); close();
    for (int ch = 1; ch <= project->master.channels; ++ch) {
        open("track"); clip(masterName(), footage(project->master,true), 0, length(), 0, project->master.frames, ch); endTrack();
    }
    close(); close(); close(); close(); x.writeEndDocument();
    if (x.hasError() || !file.commit()) { info("Could not save XML"); return false; }
    info("Exported " + file.fileName()); return true;
}
