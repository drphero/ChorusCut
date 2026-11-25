#include "lyrics.h"
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QUndoStack>
#include <cmath>

namespace {
const QRegularExpression wordPattern(QStringLiteral("[\\p{L}\\p{N}]+(?:['’][\\p{L}\\p{N}]+)*"));
bool heading(const QString& line) {
    static const QRegularExpression pattern("^\\s*\\[[^\\]]+\\]\\s*$");
    return pattern.match(line).hasMatch();
}
QString headingLabel(const QString& line) {
    QString label = line.trimmed();
    return label.mid(1, label.size() - 2).trimmed();
}
constexpr int kMinWordMs = 20;

// Where align.py lives: beside the exe on Windows, in Resources on macOS, in the source
// tree beside a development build.
QString alignScript() {
    const QString custom = qEnvironmentVariable("CHORUS_ALIGNMENT_SCRIPT");
    if (!custom.isEmpty()) return custom;
    const QDir appDir(QCoreApplication::applicationDirPath());
    for (const char* rel : {"align.py", "../Resources/align.py", "../alignment/align.py",
                            "../../alignment/align.py", "../../../alignment/align.py",
                            "../../../../alignment/align.py"}) {
        const QString path = appDir.absoluteFilePath(rel);
        if (QFileInfo::exists(path)) return path;
    }
    return appDir.absoluteFilePath("align.py");
}

// One undoable change to a contiguous run of words.
class WordsCommand : public QUndoCommand {
public:
    WordsCommand(Lyrics* lyrics, int first, QJsonArray before, QJsonArray after, const QString& label)
        : QUndoCommand(label), lyrics(lyrics), first(first), before(std::move(before)), after(std::move(after)) {}
    void redo() override {
        if (skip) { skip = false; return; }
        lyrics->applyWords(first, after, "Redid " + text().toLower() + ".");
    }
    void undo() override { lyrics->applyWords(first, before, "Undid " + text().toLower() + "."); }
    bool skip = true;  // The edit was already applied before the command was pushed.
private:
    Lyrics* lyrics;
    int first;
    QJsonArray before, after;
};
}

void Lyrics::say(const QString& value) { notice = value; emit messageChanged(); }
Lyrics::~Lyrics() { cancel(); }
void Lyrics::cancel() {
    if (!process) return;
    auto worker = process;
    process = nullptr;
    worker->disconnect(this);
    worker->kill();
    worker->waitForFinished(3000);
    worker->deleteLater();
    say("Alignment cancelled.");
    emit busyChanged(); emit finished(false);
}

int Lyrics::songMs() const {
    return project ? qRound(1000. * project->master.frames * project->fpsDen / project->fpsNum) : 0;
}

bool Lyrics::load(Project& next, QString& error) {
    QSqlQuery q(next.db);
    if (!q.exec("CREATE TABLE IF NOT EXISTS lyrics(id INTEGER PRIMARY KEY CHECK(id=1), "
                "revision INTEGER NOT NULL, draft TEXT NOT NULL, aligned TEXT NOT NULL, words TEXT NOT NULL)") ||
        !q.exec("INSERT OR IGNORE INTO lyrics(id,revision,draft,aligned,words) VALUES(1,0,'','','[]')")) {
        error = q.lastError().text(); return false;
    }
    // Instrumental sections arrived with GPU alignment; older projects gain the column.
    bool hasSections = false;
    if (!q.exec("PRAGMA table_info(lyrics)")) { error = q.lastError().text(); return false; }
    while (q.next()) hasSections |= q.value(1).toString() == "sections";
    if (!hasSections && !q.exec("ALTER TABLE lyrics ADD COLUMN sections TEXT NOT NULL DEFAULT '[]'")) {
        error = q.lastError().text(); return false;
    }
    if (!q.exec("SELECT revision,draft,aligned,words,sections FROM lyrics WHERE id=1") || !q.next()) {
        error = q.lastError().text(); return false;
    }
    auto parsed = QJsonDocument::fromJson(q.value(3).toByteArray());
    auto gaps = QJsonDocument::fromJson(q.value(4).toByteArray());
    if (!parsed.isArray() || !gaps.isArray()) { error = "The project contains unreadable lyric timings"; return false; }
    cancel();
    project = &next;
    revision = q.value(0).toInt(); draft = q.value(1).toString();
    alignedText = q.value(2).toString(); words = parsed.array(); sections = gaps.array();
    say({}); emit changed();
    return true;
}

void Lyrics::unload() {
    cancel();
    project = nullptr;
    revision = 0; draft.clear(); alignedText.clear(); words = {}; sections = {};
    say({}); emit changed();
}

bool Lyrics::store(const QString& text, const QString& aligned, const QJsonArray& spans, const QJsonArray& gaps) {
    if (!project) return false;
    QSqlQuery q(project->db);
    q.prepare("UPDATE lyrics SET revision=revision+1,draft=?,aligned=?,words=?,sections=? WHERE id=1 AND revision=?");
    q.addBindValue(text.isNull() ? QStringLiteral("") : text);
    q.addBindValue(aligned.isNull() ? QStringLiteral("") : aligned);
    q.addBindValue(QString::fromUtf8(QJsonDocument(spans).toJson(QJsonDocument::Compact)));
    q.addBindValue(QString::fromUtf8(QJsonDocument(gaps).toJson(QJsonDocument::Compact)));
    q.addBindValue(revision);
    if (!q.exec()) { say(q.lastError().text()); return false; }
    if (q.numRowsAffected()!=1) { say("Lyrics changed in another window. Reopen the project."); return false; }
    ++revision; draft = text; alignedText = aligned; words = spans; sections = gaps;
    emit changed(); return true;
}

bool Lyrics::saveText(const QString& text) {
    if (text == draft) return true;
    if (busy()) cancel();
    if (!store(text,alignedText,words,sections)) return false;
    emit historyInvalidated();
    say("Lyrics saved."); return true;
}

QVariantList Lyrics::lines() const {
    QVariantList result;
    if (draft != alignedText || words.isEmpty()) return result;
    const QStringList raw = draft.split('\n');
    int i = 0;
    QString section;
    for (int n=0; n<raw.size(); ++n) {
        if (heading(raw[n])) { section = headingLabel(raw[n]); continue; }
        QVariantList tokens;
        int start = 0, end = 0, first = i;
        bool flagged = false, manual = false;
        while (i<words.size() && words[i].toObject()["line"].toInt()==n) {
            auto m = words[i].toObject().toVariantMap();
            m["index"] = i++;
            if (tokens.empty()) start=m["start"].toInt();
            end=m["end"].toInt();
            flagged |= m.value("flag").toBool();
            manual |= m.value("manual").toBool();
            tokens.append(m);
        }
        if (!tokens.empty()) {
            result.append(QVariantMap{{"text",raw[n].trimmed()},{"section",section},{"start",start},
                                      {"end",end},{"words",tokens},{"first",first},{"last",i-1},
                                      {"flag",flagged},{"manual",manual},{"row",result.size()}});
            section.clear();
        }
    }
    return result;
}

QVariantList Lyrics::regions() const {
    // Song sections from the [Heading] lines, spanning their sung lines.
    QVariantList result;
    if (draft != alignedText || words.isEmpty()) return result;
    const QStringList raw = draft.split('\n');
    QString label;
    int start = -1, end = -1, i = 0, firstRow = 0, row = 0;
    auto flush = [&] {
        if (!label.isEmpty() && start >= 0)
            result.append(QVariantMap{{"label",label},{"start",start},{"end",end},{"firstRow",firstRow},{"lastRow",row-1}});
    };
    for (int n=0; n<raw.size(); ++n) {
        if (heading(raw[n])) { flush(); label = headingLabel(raw[n]); start = -1; firstRow = row; continue; }
        bool any = false;
        while (i<words.size() && words[i].toObject()["line"].toInt()==n) {
            const auto w = words[i++].toObject();
            if (start < 0) start = w["start"].toInt();
            end = w["end"].toInt();
            any = true;
        }
        if (any) ++row;
    }
    flush();
    return result;
}

QVariantList Lyrics::instrumental() const {
    QVariantList result;
    if (draft != alignedText) return result;
    for (const auto& s : sections) result.append(s.toObject().toVariantMap());
    return result;
}

int Lyrics::flaggedLines() const {
    int count = 0;
    for (const auto& l : lines()) count += l.toMap()["flag"].toBool();
    return count;
}

bool Lyrics::accept(const QByteArray& result) {
    auto data = QJsonDocument::fromJson(result).object();
    QJsonArray spans = data["words"].toArray();
    const int limit = songMs() + qCeil(500.*project->fpsDen/project->fpsNum);
    auto invalid = [&] { say("Alignment returned incomplete or invalid word timings."); return false; };
    int i=0, previousEnd=0;
    const auto lines = draft.split('\n');
    for (int n=0; n<lines.size(); ++n) {
        if (heading(lines[n])) continue;
        auto matches = wordPattern.globalMatch(lines[n]);
        while (matches.hasNext()) {
            auto match=matches.next();
            if (i>=spans.size()) return invalid();
            auto w=spans[i++].toObject();
            int start=w["start"].toInt(-1), end=w["end"].toInt(-1);
            if (w["line"].toInt(-1)!=n || w["word"].toString()!=match.captured() ||
                start<previousEnd || end<=start || end>limit) return invalid();
            previousEnd=end;
        }
    }
    if (!i || i!=spans.size()) return invalid();
    if (!store(draft,draft,spans,QJsonArray{})) return false;
    emit historyInvalidated();
    say(QString("Aligned %1 words offline.").arg(i)); return true;
}

bool Lyrics::importAlignment(const QUrl& file) {
    // GPU alignment output (alignment/experiments/align_v2.py): words with optional
    // review flags, plus instrumental sections. The words must match the saved lyrics.
    if (!project || busy()) return false;
    QFile in(file.toLocalFile());
    if (!in.open(QIODevice::ReadOnly)) { say("Cannot read " + file.toLocalFile()); return false; }
    const auto data = QJsonDocument::fromJson(in.readAll()).object();
    QJsonArray source = data["words"].toArray(), spans;
    const int limit = songMs() + 500;
    const auto rawLines = draft.split('\n');
    int i = 0;
    for (int n=0; n<rawLines.size(); ++n) {
        if (heading(rawLines[n])) continue;
        auto matches = wordPattern.globalMatch(rawLines[n]);
        while (matches.hasNext()) {
            const auto match = matches.next();
            if (i >= source.size()) { say("The alignment has fewer words than the lyrics."); return false; }
            const auto w = source[i++].toObject();
            if (w["line"].toInt(-1) != n || w["word"].toString() != match.captured()) {
                say(QString("Word %1 (“%2”) does not match the saved lyrics.").arg(i).arg(match.captured()));
                return false;
            }
            QJsonObject out{{"line",n},{"word",match.captured()},{"start",w["start"].toInt()},
                            {"end",w["end"].toInt()},{"score",w["score"].toDouble()}};
            if (w["flag"].toBool()) out["flag"] = true;
            spans.append(out);
        }
    }
    if (i != source.size() || spans.isEmpty()) { say("The alignment has more words than the lyrics."); return false; }
    // Voting can leave tiny overlaps between neighbours; trim ends rather than reject.
    for (int k=0; k<spans.size(); ++k) {
        auto w = spans[k].toObject();
        int start = qBound(0, w["start"].toInt(), limit - kMinWordMs);
        int end = qBound(start + kMinWordMs, w["end"].toInt(), limit);
        if (k + 1 < spans.size()) {
            const int next = spans[k+1].toObject()["start"].toInt();
            if (next < start) { say("The alignment's word times go backwards."); return false; }
            end = qMax(start + 1, qMin(end, next));
        }
        w["start"] = start; w["end"] = end; spans[k] = w;
    }
    QJsonArray gaps;
    for (const auto& s : data["sections"].toArray()) {
        const auto o = s.toObject();
        gaps.append(QJsonObject{{"start",o["start"].toInt()},{"end",o["end"].toInt()},{"kind",o["kind"].toString()}});
    }
    if (!store(draft, draft, spans, gaps)) return false;
    emit historyInvalidated();
    int flagged = 0;
    for (const auto& l : lines()) flagged += l.toMap()["flag"].toBool();
    say(QString("Imported %1 words; %2 lines to check.").arg(spans.size()).arg(flagged));
    return true;
}

void Lyrics::align() {
    if (busy() || !project) return;
    if (draft.trimmed().isEmpty()) { say("Paste the lyrics first."); emit finished(false); return; }
    QString python=qEnvironmentVariable("CHORUS_ALIGNMENT_PYTHON",CHORUS_ALIGNMENT_PYTHON);
    QString script=alignScript();
    auto worker = new QProcess(this);
    process = worker;
    auto finish = [this,worker](bool normal) {
        if (process!=worker) return;
        const auto last=worker->readAllStandardError().trimmed();
        if (!last.isEmpty()) say(QString::fromUtf8(last).section('\n',-1));
        bool success=normal && accept(worker->readAllStandardOutput());
        if (!normal && !notice.startsWith("Alignment failed"))
            say("Alignment failed. " + notice);
        process=nullptr; worker->deleteLater(); emit busyChanged(); emit finished(success);
    };
    connect(worker,&QProcess::readyReadStandardError,this,[this,worker] {
        const auto msg=QString::fromUtf8(worker->readAllStandardError()).trimmed();
        if (!msg.isEmpty()) say(msg.section('\n',-1));
    });
    connect(worker,&QProcess::finished,this,[finish](int code,QProcess::ExitStatus state) {
        finish(state==QProcess::NormalExit && code==0);
    });
    connect(worker,&QProcess::errorOccurred,this,[this,worker,finish](QProcess::ProcessError error) {
        if (error==QProcess::FailedToStart) { say("Cannot start the alignment Python: "+worker->errorString()); finish(false); }
    });
    connect(worker,&QProcess::started,this,[this,worker] {
        worker->write(draft.toUtf8()); worker->closeWriteChannel();
    });
    say("Loading the local English model…"); emit busyChanged();
    worker->start(python, {"-B","-u",script,project->file(project->master),"--stream",QString::number(project->master.stream)});
}

bool Lyrics::applyWords(int first, const QJsonArray& replacement, const QString& done) {
    if (first < 0 || first + replacement.size() > words.size()) return false;
    auto updated = words;
    for (int k=0; k<replacement.size(); ++k) updated[first + k] = replacement[k];
    if (!store(draft, alignedText, updated, sections)) return false;
    say(done);
    return true;
}

bool Lyrics::edit(int first, const QJsonArray& replacement, const QString& label, const QString& done) {
    QJsonArray before;
    for (int k=0; k<replacement.size(); ++k) before.append(words[first + k]);
    if (before == replacement) return true;
    if (!applyWords(first, replacement, done)) return false;
    if (history) history->push(new WordsCommand(this, first, before, replacement, label));
    return true;
}

QVariantMap Lyrics::wordLimits(int i) const {
    if (i < 0 || i >= words.size()) return {};
    const int lo = i ? words[i-1].toObject()["end"].toInt() : 0;
    const int hi = i + 1 < words.size() ? words[i+1].toObject()["start"].toInt() : songMs();
    return {{"lo", lo}, {"hi", hi}};
}

QVariantMap Lyrics::moveLimits(int first, int last) const {
    if (first < 0 || last < first || last >= words.size()) return {};
    const int lo = first ? words[first-1].toObject()["end"].toInt() : 0;
    const int hi = last + 1 < words.size() ? words[last+1].toObject()["start"].toInt() : songMs();
    return {{"min", lo - words[first].toObject()["start"].toInt()},
            {"max", hi - words[last].toObject()["end"].toInt()}};
}

bool Lyrics::correct(int i,int start,int end) {
    if (!project || busy() || draft!=alignedText || i<0 || i>=words.size()) return false;
    const auto limits = wordLimits(i);
    if (start<limits["lo"].toInt() || end-start<1 || end>limits["hi"].toInt()) {
        say("Keep the word inside its neighbors and give it a positive duration."); return false;
    }
    auto word=words[i].toObject();
    word["start"]=start; word["end"]=end; word["manual"]=true; word.remove("flag");
    return edit(i, QJsonArray{word}, "Word timing", "Word timing saved.");
}

bool Lyrics::moveWords(int first, int last, int delta) {
    if (!project || busy() || draft!=alignedText || first<0 || last<first || last>=words.size()) return false;
    const auto limits = moveLimits(first, last);
    delta = qBound(limits["min"].toInt(), delta, limits["max"].toInt());
    if (delta == 0) return true;
    QJsonArray moved;
    for (int k=first; k<=last; ++k) {
        auto w = words[k].toObject();
        w["start"] = w["start"].toInt() + delta;
        w["end"] = w["end"].toInt() + delta;
        w["manual"] = true; w.remove("flag");
        moved.append(w);
    }
    const QString label = first == last ? "Move word" : "Move lyrics";
    return edit(first, moved, label, QString("Moved %1 ms.").arg(delta));
}

bool Lyrics::markChecked(int first, int last) {
    if (!project || draft!=alignedText || first<0 || last<first || last>=words.size()) return false;
    QJsonArray checked;
    for (int k=first; k<=last; ++k) {
        auto w = words[k].toObject();
        w.remove("flag");
        checked.append(w);
    }
    return edit(first, checked, "Mark checked", "Marked as checked.");
}
