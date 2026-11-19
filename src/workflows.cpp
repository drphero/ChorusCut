#include "workflows.h"
#include <QCollator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSize>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <algorithm>

namespace {
struct Named { const char* value; const char* label; const char* hint; };
const QList<Named>& useTable() {
    static const QList<Named> table = {
        {"ref-text", "Reference picture from a prompt", "Text to image: a character, look or location from its picture prompt."},
        {"ref-edit", "Reference picture from pictures", "Image editing: a new picture from approved ones and the prompt, such as a look on the character's face."},
    };
    return table;
}
const QList<Named>& slotTable() {
    static const QList<Named> table = {
        {"prompt", "Prompt", ""}, {"negative", "Negative prompt", ""}, {"picture", "Picture", "indexed"},
        {"seed", "Seed", ""}, {"width", "Width", ""}, {"height", "Height", ""}, {"prefix", "File name prefix", ""},
    };
    return table;
}
QString useLabel(const QString& use) {
    for (const auto& u : useTable()) if (use == u.value) return u.label;
    return use;
}
bool isLink(const QJsonValue& v) {
    const auto a = v.toArray();
    return v.isArray() && a.size() == 2 && a[0].isString();
}
// Node ids in numeric order where they are numbers ("2" before "10").
QStringList nodeOrder(const QJsonObject& graph) {
    QStringList keys = graph.keys();
    QCollator c; c.setNumericMode(true);
    std::sort(keys.begin(), keys.end(), [&](const QString& a, const QString& b) { return c.compare(a, b) < 0; });
    return keys;
}
QString classOf(const QJsonObject& graph, const QString& node) { return graph[node].toObject()["class_type"].toString(); }
QJsonObject inputsOf(const QJsonObject& graph, const QString& node) { return graph[node].toObject()["inputs"].toObject(); }

// From a conditioning link, upstream to the first node with literal prompt text.
QPair<QString, QString> textFor(const QJsonObject& graph, const QJsonValue& link) {
    QList<QString> queue{link.toArray()[0].toString()};
    QSet<QString> seen;
    while (!queue.isEmpty()) {
        const QString node = queue.takeFirst();
        if (seen.contains(node) || !graph.contains(node)) continue;
        seen.insert(node);
        const auto inputs = inputsOf(graph, node);
        for (const char* name : {"text", "prompt", "text_g"})
            if (inputs[name].isString()) return {node, name};
        for (auto it = inputs.begin(); it != inputs.end(); ++it)
            if (isLink(it.value()) && it.key() != "model" && it.key() != "vae") queue << it.value().toArray()[0].toString();
    }
    return {};
}
QString slug(const QString& name) {
    QString s = name.toLower();
    s.replace(QRegularExpression("[^a-z0-9]+"), "-");
    s = s.mid(0, 40).remove(QRegularExpression("^-+|-+$"));
    return s.isEmpty() ? "workflow" : s;
}
}

Workflows::Workflows(QObject* parent) : QObject(parent) {
    const QString custom = qEnvironmentVariable("CHORUS_WORKFLOWS_DIR");
    root = QDir(custom.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/Workflows" : custom);
}

QVariantList Workflows::uses() const {
    QVariantList out;
    for (const auto& u : useTable()) out.append(QVariantMap{{"value", u.value}, {"label", u.label}, {"hint", u.hint}});
    return out;
}

QVariantList Workflows::slotTypes() const {
    QVariantList out;
    for (const auto& s : slotTable()) out.append(QVariantMap{{"value", s.value}, {"label", s.label}, {"indexed", QString(s.hint) == "indexed"}});
    return out;
}

QStringList Workflows::ids() const {
    QStringList out;
    for (const auto& info : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        if (QFileInfo::exists(info.filePath() + "/workflow.json")) out << info.fileName();
    return out;
}

QJsonObject Workflows::library() const {
    QFile f(root.filePath("library.json"));
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject{};
}

void Workflows::saveLibrary(const QJsonObject& value) {
    root.mkpath(".");
    QFile f(root.filePath("library.json"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) f.write(QJsonDocument(value).toJson());
}

QJsonObject Workflows::graphOf(const QString& id) const {
    QFile f(root.filePath(id + "/workflow.json"));
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject{};
}

QJsonObject Workflows::adapterOf(const QString& id) const {
    QFile f(root.filePath(id + "/adapter.json"));
    return f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll()).object() : QJsonObject{};
}

bool Workflows::saveAdapter(const QString& id, const QJsonObject& adapter) {
    QFile f(root.filePath(id + "/adapter.json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(adapter).toJson());
    f.close();
    emit changed();
    return true;
}

QString Workflows::nameOf(const QString& id) const {
    const QString name = adapterOf(id)["name"].toString();
    return name.isEmpty() ? id : name;
}

QJsonArray Workflows::outputs(const QString& id) const { return adapterOf(id)["outputs"].toArray(); }

QString Workflows::checkGraph(const QJsonDocument& doc) {
    if (!doc.isObject()) return "This file is not a ComfyUI workflow.";
    const auto o = doc.object();
    if (o.contains("nodes") && o.contains("links"))
        return "This is a saved ComfyUI workflow. In ComfyUI, open it and choose Workflow ▸ Export (API), then import that file.";
    if (o.isEmpty()) return "This workflow has no nodes.";
    for (auto it = o.begin(); it != o.end(); ++it) {
        const auto node = it.value().toObject();
        if (!node["class_type"].isString() || !node["inputs"].isObject())
            return "This file is not a ComfyUI workflow in the Export (API) format.";
    }
    return {};
}

QJsonObject Workflows::detect(const QJsonObject& graph) {
    QJsonArray mapping, outs;
    QSet<QString> taken;
    auto add = [&](const QString& slot, const QString& node, const QString& input, int index = 0) {
        const QString key = node + "/" + input;
        if (node.isEmpty() || taken.contains(key)) return;
        taken.insert(key);
        QJsonObject s{{"slot", slot}, {"node", node}, {"input", input}};
        if (slot == "picture") s["index"] = index;
        mapping.append(s);
    };
    const QStringList order = nodeOrder(graph);
    // The prompt and negative prompt: traced from what samplers and guiders take as conditioning.
    QPair<QString, QString> positive, negative;
    for (const QString& node : order) {
        const auto inputs = inputsOf(graph, node);
        const QJsonValue pos = inputs.contains("positive") ? inputs["positive"] : inputs["conditioning"];
        if (positive.first.isEmpty() && isLink(pos) && (inputs.contains("positive") || classOf(graph, node).contains("Guider")))
            positive = textFor(graph, pos);
        if (negative.first.isEmpty() && isLink(inputs["negative"])) negative = textFor(graph, inputs["negative"]);
    }
    if (positive.first.isEmpty())
        for (const QString& node : order)
            if (classOf(graph, node).contains("Text") && inputsOf(graph, node)["text"].isString()) { positive = {node, "text"}; break; }
    add("prompt", positive.first, positive.second);
    if (negative != positive) add("negative", negative.first, negative.second);
    int picture = 0;
    QString saver, preview;
    for (const QString& node : order) {
        const QString cls = classOf(graph, node);
        const auto inputs = inputsOf(graph, node);
        if (cls.contains("LoadImage") && inputs["image"].isString()) add("picture", node, "image", picture++);
        for (const char* name : {"seed", "noise_seed"})
            if (inputs[name].isDouble()) add("seed", node, name);
        if (cls.contains("Latent") && inputs["width"].isDouble() && inputs["height"].isDouble()) {
            add("width", node, "width");
            add("height", node, "height");
        }
        if (cls.contains("SaveImage")) {
            if (saver.isEmpty()) saver = node;
            outs.append(node);
            if (inputs["filename_prefix"].isString()) add("prefix", node, "filename_prefix");
        }
        if (cls == "PreviewImage" && preview.isEmpty()) preview = node;
    }
    if (outs.isEmpty() && !preview.isEmpty()) outs.append(preview);
    return {{"slots", mapping}, {"outputs", outs}};
}

QVariantMap Workflows::importFile(const QUrl& file, const QString& use) {
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {{"ok", false}, {"error", "Cannot read " + QFileInfo(path).fileName() + "."}};
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(f.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError) return {{"ok", false}, {"error", QFileInfo(path).fileName() + " is not valid JSON."}};
    const QString problem = checkGraph(doc);
    if (!problem.isEmpty()) return {{"ok", false}, {"error", problem}};
    QJsonObject adapter = detect(doc.object());
    bool pictures = false;
    for (const auto& s : adapter["slots"].toArray()) pictures |= s.toObject()["slot"] == "picture";
    const QString chosen = use.isEmpty() ? (pictures ? "ref-edit" : "ref-text") : use;
    const QString name = QFileInfo(path).completeBaseName();
    QString id = slug(name);
    for (int n = 2; root.exists(id); ++n) id = slug(name) + "-" + QString::number(n);
    if (!root.mkpath(id)) return {{"ok", false}, {"error", "Cannot create the workflow folder in " + root.absolutePath() + "."}};
    QFile out(root.filePath(id + "/workflow.json"));
    if (!out.open(QIODevice::WriteOnly) || out.write(doc.toJson()) < 0) return {{"ok", false}, {"error", "Cannot save the workflow."}};
    out.close();
    adapter["version"] = 1;
    adapter["name"] = name;
    adapter["use"] = chosen;
    adapter["source"] = QFileInfo(path).fileName();
    saveAdapter(id, adapter);
    auto lib = library();
    auto defaults = lib["defaults"].toObject();
    if (!defaults.contains(chosen)) { defaults[chosen] = id; lib["defaults"] = defaults; saveLibrary(lib); }
    emit changed();
    return {{"ok", true}, {"id", id}};
}

QStringList Workflows::problems(const QString& id) const {
    QStringList out;
    const auto adapter = adapterOf(id);
    const QString use = adapter["use"].toString();
    int prompts = 0, pictures = 0;
    for (const auto& s : adapter["slots"].toArray()) {
        prompts += s.toObject()["slot"] == "prompt";
        pictures += s.toObject()["slot"] == "picture";
    }
    if (!prompts) out << "Choose the input that takes the prompt.";
    if (use == "ref-edit" && !pictures) out << "Choose the Load Image input that takes the first picture.";
    if (use == "ref-text" && pictures) out << "This workflow takes pictures: use it for “Reference picture from pictures”, or let its Load Image inputs keep their values.";
    if (adapter["outputs"].toArray().isEmpty()) out << "Add a Save Image node, so Chorus Cut can collect the result.";
    const auto graph = graphOf(id);
    for (const auto& s : adapter["slots"].toArray()) {
        const auto o = s.toObject();
        if (!inputsOf(graph, o["node"].toString()).contains(o["input"].toString()))
            out << QString("Node %1 has no input “%2”.").arg(o["node"].toString(), o["input"].toString());
    }
    return out;
}

QVariantList Workflows::list() const {
    QVariantList out;
    const auto defaults = library()["defaults"].toObject();
    for (const QString& id : ids()) {
        const auto adapter = adapterOf(id);
        const QString use = adapter["use"].toString();
        const auto issues = problems(id);
        out.append(QVariantMap{{"id", id}, {"name", nameOf(id)}, {"use", use}, {"useLabel", useLabel(use)},
                               {"isDefault", defaults[use].toString() == id}, {"ready", issues.isEmpty()}, {"problems", issues}});
    }
    return out;
}

QVariantMap Workflows::get(const QString& id) const {
    if (!ids().contains(id)) return {};
    const auto graph = graphOf(id);
    const auto adapter = adapterOf(id);
    QMap<QString, QJsonObject> mapped;
    for (const auto& s : adapter["slots"].toArray()) mapped[s.toObject()["node"].toString() + "/" + s.toObject()["input"].toString()] = s.toObject();
    QVariantList inputs;
    for (const QString& node : nodeOrder(graph)) {
        const auto n = graph[node].toObject();
        const auto in = n["inputs"].toObject();
        for (auto it = in.begin(); it != in.end(); ++it) {
            if (isLink(it.value()) || it.value().isArray() || it.value().isObject()) continue;
            QString value = it.value().isString() ? it.value().toString() : it.value().isBool() ? (it.value().toBool() ? "true" : "false")
                                                                                              : QString::number(it.value().toDouble(), 'g', 12);
            value = value.simplified();
            if (value.size() > 90) value = value.left(88) + "…";
            const auto s = mapped.value(node + "/" + it.key());
            inputs.append(QVariantMap{{"key", node + "/" + it.key()}, {"node", node}, {"classType", n["class_type"].toString()},
                                      {"title", n["_meta"].toObject()["title"].toString()}, {"input", it.key()}, {"value", value},
                                      {"slot", s["slot"].toString()}, {"index", s["index"].toInt()}});
        }
    }
    const QString use = adapter["use"].toString();
    const auto issues = problems(id);
    QVariantList outs;
    for (const auto& o : adapter["outputs"].toArray()) outs << QString("%1 (%2)").arg(o.toString(), classOf(graph, o.toString()));
    return {{"id", id}, {"name", nameOf(id)}, {"use", use}, {"useLabel", useLabel(use)}, {"source", adapter["source"].toString()},
            {"isDefault", library()["defaults"].toObject()[use].toString() == id}, {"ready", issues.isEmpty()}, {"problems", issues},
            {"outputs", outs}, {"inputs", inputs}, {"pictures", pictureCount(id)}};
}

bool Workflows::setSlot(const QString& id, const QString& node, const QString& input, const QString& slot, int index) {
    if (!ids().contains(id) || !inputsOf(graphOf(id), node).contains(input)) return false;
    bool known = slot.isEmpty();
    for (const auto& s : slotTable()) known |= slot == s.value;
    if (!known) return false;
    auto adapter = adapterOf(id);
    QJsonArray mapping;
    for (const auto& s : adapter["slots"].toArray()) {
        const auto o = s.toObject();
        if (o["node"].toString() == node && o["input"].toString() == input) continue;
        // A picture index or the prompt goes to one input at a time.
        if (!slot.isEmpty() && o["slot"].toString() == slot && (slot == "prompt" || slot == "negative" || slot == "prefix" ||
                                                                 (slot == "picture" && o["index"].toInt() == index))) continue;
        mapping.append(o);
    }
    if (!slot.isEmpty()) {
        QJsonObject s{{"slot", slot}, {"node", node}, {"input", input}};
        if (slot == "picture") s["index"] = qMax(0, index);
        mapping.append(s);
    }
    adapter["slots"] = mapping;
    return saveAdapter(id, adapter);
}

bool Workflows::setUse(const QString& id, const QString& use) {
    bool known = false;
    for (const auto& u : useTable()) known |= use == u.value;
    if (!known || !ids().contains(id)) return false;
    auto adapter = adapterOf(id);
    const QString old = adapter["use"].toString();
    adapter["use"] = use;
    auto lib = library();
    auto defaults = lib["defaults"].toObject();
    if (defaults[old].toString() == id) defaults.remove(old);
    if (!defaults.contains(use)) defaults[use] = id;
    lib["defaults"] = defaults;
    saveLibrary(lib);
    return saveAdapter(id, adapter);
}

bool Workflows::rename(const QString& id, const QString& name) {
    if (!ids().contains(id) || name.trimmed().isEmpty()) return false;
    auto adapter = adapterOf(id);
    adapter["name"] = name.trimmed();
    return saveAdapter(id, adapter);
}

bool Workflows::setDefault(const QString& use, const QString& id) {
    if (!ids().contains(id) || adapterOf(id)["use"].toString() != use) return false;
    auto lib = library();
    auto defaults = lib["defaults"].toObject();
    defaults[use] = id;
    lib["defaults"] = defaults;
    saveLibrary(lib);
    emit changed();
    return true;
}

bool Workflows::remove(const QString& id) {
    if (!ids().contains(id)) return false;
    QDir dir(root.filePath(id));
    if (!dir.removeRecursively()) return false;
    auto lib = library();
    auto defaults = lib["defaults"].toObject();
    for (const QString& use : defaults.keys())
        if (defaults[use].toString() == id) defaults.remove(use);
    lib["defaults"] = defaults;
    saveLibrary(lib);
    emit changed();
    return true;
}

QString Workflows::forUse(const QString& use) const {
    const QString chosen = library()["defaults"].toObject()[use].toString();
    if (ids().contains(chosen) && adapterOf(chosen)["use"].toString() == use && problems(chosen).isEmpty()) return chosen;
    QStringList ready;
    for (const QString& id : ids())
        if (adapterOf(id)["use"].toString() == use && problems(id).isEmpty()) ready << id;
    return ready.size() == 1 ? ready[0] : QString();
}

int Workflows::pictureCount(const QString& id) const {
    int n = 0;
    for (const auto& s : adapterOf(id)["slots"].toArray())
        if (s.toObject()["slot"] == "picture") n = qMax(n, s.toObject()["index"].toInt() + 1);
    return n;
}

QSize Workflows::size(const QString& id) const {
    const auto graph = graphOf(id);
    int w = 0, h = 0;
    for (const auto& s : adapterOf(id)["slots"].toArray()) {
        const auto o = s.toObject();
        const auto v = inputsOf(graph, o["node"].toString())[o["input"].toString()];
        if (o["slot"] == "width") w = v.toInt();
        if (o["slot"] == "height") h = v.toInt();
    }
    return {w, h};
}

QJsonObject Workflows::build(const QString& id, const Values& v, QString& error) const {
    if (!ids().contains(id)) { error = "The workflow is no longer in the library."; return {}; }
    const auto issues = problems(id);
    if (!issues.isEmpty()) { error = nameOf(id) + ": " + issues.first(); return {}; }
    const int pictures = pictureCount(id);
    if (v.pictures.size() < pictures) {
        error = QString("%1 takes %2 pictures; %3 %4 approved.").arg(nameOf(id)).arg(pictures).arg(v.pictures.size())
                    .arg(v.pictures.size() == 1 ? "is" : "are");
        return {};
    }
    QJsonObject graph = graphOf(id);
    for (const auto& s : adapterOf(id)["slots"].toArray()) {
        const auto o = s.toObject();
        const QString node = o["node"].toString(), input = o["input"].toString(), slot = o["slot"].toString();
        QJsonValue value;
        if (slot == "prompt") value = v.prompt;
        else if (slot == "negative") { if (v.negative.isNull()) continue; value = v.negative; }
        else if (slot == "picture") value = v.pictures.value(o["index"].toInt());
        else if (slot == "seed") { if (v.seed < 0) continue; value = double(v.seed); }
        else if (slot == "width") { if (v.width <= 0) continue; value = v.width; }
        else if (slot == "height") { if (v.height <= 0) continue; value = v.height; }
        else if (slot == "prefix") { if (v.prefix.isEmpty()) continue; value = v.prefix; }
        else continue;
        auto n = graph[node].toObject();
        auto inputs = n["inputs"].toObject();
        inputs[input] = value;
        n["inputs"] = inputs;
        graph[node] = n;
    }
    return graph;
}
