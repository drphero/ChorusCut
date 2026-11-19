#include "editor.h"
#include <QSaveFile>
#include <QFileInfo>
#include <QXmlStreamWriter>

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
    auto rate=[&](){ open("rate"); number("timebase",qRound(fps())); text("ntsc",project->fpsDen==1001?"TRUE":"FALSE"); close(); };
    auto pictureFormat=[&](const Media& media){
        open("samplecharacteristics"); rate(); number("width",media.width); number("height",media.height);
        text("anamorphic","FALSE"); text("pixelaspectratio","square"); text("fielddominance","none"); close();
    };
    auto audioFormat=[&](){ open("samplecharacteristics"); if (project->master.bits > 0) number("depth",project->master.bits); number("samplerate",project->master.sampleRate); close(); };
    auto track=[&](QString name,const Media& media,int start,int end,int source,bool audio,int channel=0){
        const QString filename = QFileInfo(media.path).fileName();
        open("track"); open("clipitem"); x.writeAttribute("id",name+QString::number(channel));
        text("name",filename); number("duration",media.frames); rate();
        number("start",start); number("end",end); number("in",source); number("out",source+end-start);
        open("file"); x.writeAttribute("id","file-"+name);
        if (channel<2) {
            text("name",filename); text("pathurl",QUrl::fromLocalFile(project->file(media)).toString(QUrl::FullyEncoded));
            rate(); number("duration",media.frames); open("media"); open(audio?"audio":"video");
            if (audio) { audioFormat(); number("channelcount",media.channels); } else pictureFormat(media);
            close(); close();
        }
        close();
        if (audio) { open("sourcetrack"); text("mediatype","audio"); number("trackindex",channel); close(); }
        close(); text("enabled","TRUE"); text("locked","FALSE"); close();
    };
    x.writeStartDocument(); x.writeDTD("<!DOCTYPE xmeml>"); open("xmeml"); x.writeAttribute("version","5");
    open("sequence"); x.writeAttribute("id","chorus"); text("name",projectName()); number("duration",length()); rate();
    open("media");
    if (sourceLength()) {
        open("video"); open("format"); pictureFormat(project->performance); close();
        track("performance",project->performance,0,baseLength(),0,false);
        track("cutaway",project->cutaway,trim.start,trim.end,trim.source,false); close();
    }
    open("audio"); number("numOutputChannels",project->master.channels); open("format"); audioFormat(); close();
    for (int ch = 1; ch <= project->master.channels; ++ch) track("master",project->master,0,length(),0,true,ch);
    close(); close(); close(); close(); x.writeEndDocument();
    if (x.hasError() || !file.commit()) { info("Could not save XML"); return false; }
    info("Exported " + file.fileName()); return true;
}
