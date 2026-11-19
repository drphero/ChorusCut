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
#include <cmath>

namespace {
struct Named { const char* value; const char* label; const char* hint; };
const QList<Named>& useTable() {
    static const QList<Named> table = {
        {"ref-text", "Reference picture from a prompt", "Text to image: a character, look or location from its picture prompt."},
        {"ref-edit", "Reference picture from pictures", "Image editing: a new picture from pictures you pick and an instruction, such as a look on the character's face."},
        {"ref-any", "Reference picture, from a prompt or from pictures", "Both: a model that draws from text alone or edits from any number of pictures (its picture inputs must be optional)."},
    };
    return table;
}
const QList<Named>& slotTable() {
    static const QList<Named> table = {
        {"prompt", "Prompt", ""}, {"negative", "Negative prompt", ""}, {"picture", "Picture", "indexed"},
        {"seed", "Seed", ""}, {"width", "Width", ""}, {"height", "Height", ""}, {"prefix", "File name prefix", ""},
        {"setting", "Adjust when generating", ""},
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
        // A prompt typed into a string primitive (as ComfyUI's own templates do).
        if (classOf(graph, node).startsWith("PrimitiveString") && inputs["value"].isString()) return {node, "value"};
        // Never into a prompt enhancer's instructions: the user's words are elsewhere.
        for (auto it = inputs.begin(); it != inputs.end(); ++it)
            if (isLink(it.value()) && it.key() != "model" && it.key() != "vae" && it.key() != "system_prompt" && it.key() != "string")
                queue << it.value().toArray()[0].toString();
        // A template's own words (StringReplace's string) last, so a user's text is found first.
        if (isLink(inputs["string"])) queue << inputs["string"].toArray()[0].toString();
    }
    return {};
}
// "sampler_name" → "Sampler name"; a few short names read better in capitals.
QString pretty(const QString& input) {
    static const QMap<QString, QString> known = {{"cfg", "CFG"}, {"ckpt_name", "Checkpoint"}, {"unet_name", "Model"},
                                                 {"lora_name", "LoRA"}, {"strength_model", "LoRA strength"}};
    if (known.contains(input)) return known[input];
    QString s = input;
    s.replace('_', ' ');
    if (!s.isEmpty()) s[0] = s[0].toUpper();
    return s;
}
// Inputs worth adjusting from one picture to the next, offered without asking on import.
bool usualSetting(const QString& cls, const QString& input, const QJsonValue& value) {
    if (isLink(value) || value.isArray() || value.isObject()) return false;
    if (cls.startsWith("KSampler") && cls != "KSamplerSelect")
        return input == "steps" || input == "cfg" || input == "sampler_name" || input == "scheduler" || input == "denoise";
    if (cls == "KSamplerSelect") return input == "sampler_name";
    if (cls == "BasicScheduler") return input == "steps" || input == "scheduler" || input == "denoise";
    if (cls == "CFGGuider") return input == "cfg";
    if (cls == "FluxGuidance") return input == "guidance";
    // Shape and size pickers: aspect ratio and megapixel choices, whatever node offers them.
    if (input == "aspect_ratio" || input == "megapixels") return true;
    return false;
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
    for (const QString& node : order) {
        const auto inputs = inputsOf(graph, node);
        for (auto it = inputs.begin(); it != inputs.end(); ++it)
            if (usualSetting(classOf(graph, node), it.key(), it.value())) add("setting", node, it.key());
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
    if ((use == "ref-edit" || use == "ref-any") && !pictures) out << "Choose the Load Image input that takes the first picture.";
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
    const QJsonObject keptValues = adapter["values"].toObject();
    auto shown = [](const QJsonValue& v) {
        QString value = v.isString() ? v.toString() : v.isBool() ? (v.toBool() ? "true" : "false") : QString::number(v.toDouble(), 'g', 12);
        value = value.simplified();
        return value.size() > 90 ? value.left(88) + "…" : value;
    };
    QMap<QString, QJsonObject> mapped;
    for (const auto& s : adapter["slots"].toArray()) mapped[s.toObject()["node"].toString() + "/" + s.toObject()["input"].toString()] = s.toObject();
    QVariantList inputs;
    for (const QString& node : nodeOrder(graph)) {
        const auto n = graph[node].toObject();
        const auto in = n["inputs"].toObject();
        for (auto it = in.begin(); it != in.end(); ++it) {
            if (isLink(it.value()) || it.value().isArray() || it.value().isObject()) continue;
            const QString key = node + "/" + it.key();
            const auto s = mapped.value(key);
            const bool kept = s["slot"] == "setting" && keptValues.contains(key);
            QString value = shown(kept ? keptValues[key] : it.value());
            if (kept) value += "  (workflow: " + shown(it.value()) + ")";
            inputs.append(QVariantMap{{"key", key}, {"node", node}, {"classType", n["class_type"].toString()},
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
            {"outputs", outs}, {"inputs", inputs}, {"pictures", pictureCount(id)},
            {"pictureName", adapter["pictureName"].toString()}, {"notes", adapter["notes"].toString()}};
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
    if (!ids().contains(id) || !serves(id, use)) return false;
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

bool Workflows::serves(const QString& id, const QString& use) const {
    const QString own = adapterOf(id)["use"].toString();
    return own == use || (own == "ref-any" && (use == "ref-text" || use == "ref-edit"));
}

QString Workflows::forUse(const QString& use) const {
    const QString chosen = library()["defaults"].toObject()[use].toString();
    if (ids().contains(chosen) && serves(chosen, use) && problems(chosen).isEmpty()) return chosen;
    // A workflow made for this use first, then one that does both.
    QStringList own, both;
    for (const QString& id : ids()) {
        if (!problems(id).isEmpty()) continue;
        if (adapterOf(id)["use"].toString() == use) own << id;
        else if (serves(id, use)) both << id;
    }
    if (own.size() == 1) return own[0];
    if (own.isEmpty() && both.size() == 1) return both[0];
    return {};
}

int Workflows::pictureCount(const QString& id) const {
    int n = 0;
    for (const auto& s : adapterOf(id)["slots"].toArray())
        if (s.toObject()["slot"] == "picture") n = qMax(n, s.toObject()["index"].toInt() + 1);
    return n;
}

QVariantList Workflows::ready(const QString& use) const {
    QVariantList out;
    const QString chosen = forUse(use);
    for (const QString& id : ids())
        if (serves(id, use) && problems(id).isEmpty())
            out.append(QVariantMap{{"id", id}, {"name", nameOf(id)}, {"isDefault", id == chosen}});
    return out;
}

bool Workflows::hasSlot(const QString& id, const QString& slot) const {
    for (const auto& s : adapterOf(id)["slots"].toArray())
        if (s.toObject()["slot"] == slot) return true;
    return false;
}

QVariantList Workflows::settings(const QString& id) const {
    const auto graph = graphOf(id);
    const auto adapter = adapterOf(id);
    const auto kept = adapter["values"].toObject();
    QVariantList out;
    QMap<QString, int> names;
    for (const auto& s : adapter["slots"].toArray())
        if (s.toObject()["slot"] == "setting") names[s.toObject()["input"].toString()]++;
    for (const auto& s : adapter["slots"].toArray()) {
        const auto o = s.toObject();
        if (o["slot"] != "setting") continue;
        const QString node = o["node"].toString(), input = o["input"].toString(), key = node + "/" + input;
        const auto inputs = inputsOf(graph, node);
        if (!inputs.contains(input)) continue;
        const QJsonValue original = inputs[input];
        const QString type = original.isBool() ? "switch" : original.isDouble() ? "number" : "text";
        // Two samplers' steps are told apart by their node's title.
        QString label = pretty(input);
        if (names[input] > 1) label += " · " + (graph[node].toObject()["_meta"].toObject()["title"].toString().isEmpty()
                                                    ? classOf(graph, node) : graph[node].toObject()["_meta"].toObject()["title"].toString());
        out.append(QVariantMap{{"key", key}, {"node", node}, {"input", input}, {"classType", classOf(graph, node)}, {"label", label},
                               {"type", type}, {"value", (kept.contains(key) ? kept[key] : original).toVariant()},
                               {"original", original.toVariant()}});
    }
    return out;
}

QJsonObject Workflows::checkValues(const QString& id, const QVariantMap& values, QString& error) const {
    QJsonObject out;
    QMap<QString, QVariantMap> known;
    for (const auto& v : settings(id)) known[v.toMap()["key"].toString()] = v.toMap();
    for (auto it = values.begin(); it != values.end(); ++it) {
        if (!known.contains(it.key())) { error = QString("%1 has no setting “%2”.").arg(nameOf(id), it.key()); return {}; }
        const auto& s = known[it.key()];
        const QVariant v = it.value();
        if (s["type"] == "number") {
            bool ok = false;
            const double d = v.toString().trimmed().toDouble(&ok);
            if (!ok || !std::isfinite(d)) { error = s["label"].toString() + " must be a number."; return {}; }
            out[it.key()] = d;
        } else if (s["type"] == "switch") {
            out[it.key()] = v.typeId() == QMetaType::Bool ? v.toBool() : v.toString() == "true";
        } else {
            out[it.key()] = v.toString();
        }
    }
    return out;
}

QString Workflows::setValues(const QString& id, const QJsonObject& values) {
    if (!ids().contains(id)) return "The workflow is no longer in the library.";
    auto adapter = adapterOf(id);
    auto kept = adapter["values"].toObject();
    const auto graph = graphOf(id);
    const auto before = kept;
    for (auto it = values.begin(); it != values.end(); ++it) {
        const QString node = it.key().section('/', 0, 0), input = it.key().section('/', 1);
        if (inputsOf(graph, node)[input] == it.value()) kept.remove(it.key());
        else kept[it.key()] = it.value();
    }
    if (kept == before) return {};
    if (kept.isEmpty()) adapter.remove("values"); else adapter["values"] = kept;
    return saveAdapter(id, adapter) ? QString() : "Could not save the workflow's settings.";
}

QString Workflows::prune(QJsonObject& graph, const QSet<QString>& start, const QJsonArray& outs, const OptionalInputs& optional) {
    // Nodes that cannot run without the removed ones join them, unless the link they lose goes
    // into an optional input (a growing list like images.image_3 counts by its base name).
    QSet<QString> gone = start;
    bool grew = true;
    while (grew) {
        grew = false;
        for (auto it = graph.begin(); it != graph.end(); ++it) {
            if (gone.contains(it.key())) continue;
            const QString cls = it.value().toObject()["class_type"].toString();
            QJsonObject inputs = it.value().toObject()["inputs"].toObject();
            for (auto in = inputs.begin(); in != inputs.end(); ++in) {
                if (!isLink(in.value()) || !gone.contains(in.value().toArray()[0].toString())) continue;
                // "mode.images.image_0" is optional when it, "mode.images" or "mode" is.
                const QSet<QString> opt = optional.value(cls);
                bool isOptional = false;
                for (QString key = in.key(); !key.isEmpty() && !isOptional; key = key.contains('.') ? key.section('.', 0, -2) : QString())
                    isOptional = opt.contains(key);
                if (!isOptional) { gone.insert(it.key()); grew = true; break; }
            }
        }
    }
    for (const auto& o : outs)
        if (gone.contains(o.toString())) return "a picture it needs would be left out.";
    for (const QString& node : gone) graph.remove(node);
    for (auto it = graph.begin(); it != graph.end(); ++it) {
        QJsonObject n = it.value().toObject();
        QJsonObject inputs = n["inputs"].toObject();
        for (const QString& key : inputs.keys())
            if (isLink(inputs[key]) && gone.contains(inputs[key].toArray()[0].toString())) inputs.remove(key);
        n["inputs"] = inputs;
        *it = n;
    }
    return {};
}

int Workflows::minPictures(const QString& id, const OptionalInputs& optional) const {
    const int most = pictureCount(id);
    for (int n = 0; n <= most; ++n) {
        QJsonObject graph = graphOf(id);
        QSet<QString> unused;
        for (const auto& s : adapterOf(id)["slots"].toArray())
            if (s.toObject()["slot"] == "picture" && s.toObject()["index"].toInt() >= n) unused << s.toObject()["node"].toString();
        if (unused.isEmpty() || prune(graph, unused, outputs(id), optional).isEmpty()) return n;
    }
    return most;
}

QStringList Workflows::classes(const QString& id) const {
    QSet<QString> out;
    const auto graph = graphOf(id);
    for (auto it = graph.begin(); it != graph.end(); ++it) out.insert(it.value().toObject()["class_type"].toString());
    return QStringList(out.begin(), out.end());
}

QString Workflows::pictureName(const QString& id) const { return adapterOf(id)["pictureName"].toString(); }
QString Workflows::notes(const QString& id) const { return adapterOf(id)["notes"].toString(); }

bool Workflows::setConventions(const QString& id, const QString& pictureName, const QString& notes) {
    if (!ids().contains(id)) return false;
    auto adapter = adapterOf(id);
    if (pictureName.trimmed().isEmpty()) adapter.remove("pictureName"); else adapter["pictureName"] = pictureName.trimmed();
    if (notes.trimmed().isEmpty()) adapter.remove("notes"); else adapter["notes"] = notes.trimmed();
    return saveAdapter(id, adapter);
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
    if (v.pictures.size() > pictures) {
        error = QString("%1 takes at most %2 %3.").arg(nameOf(id)).arg(pictures).arg(pictures == 1 ? "picture" : "pictures");
        return {};
    }
    QJsonObject graph = graphOf(id);
    // Fewer pictures than the workflow has Load Image inputs: the unused ones come out, with what
    // only they feed, where ComfyUI says the inputs they reach are optional.
    if (v.pictures.size() < pictures) {
        QSet<QString> unused;
        for (const auto& s : adapterOf(id)["slots"].toArray())
            if (s.toObject()["slot"] == "picture" && s.toObject()["index"].toInt() >= v.pictures.size()) unused << s.toObject()["node"].toString();
        const QString why = prune(graph, unused, outputs(id), v.optional);
        if (!why.isEmpty()) {
            const int need = minPictures(id, v.optional);
            error = need > v.pictures.size()
                ? QString("%1 needs at least %2 %3 here.").arg(nameOf(id)).arg(need).arg(need == 1 ? "picture" : "pictures")
                : nameOf(id) + ": " + why;
            return {};
        }
    }
    auto put = [&](const QString& node, const QString& input, const QJsonValue& value) {
        auto n = graph[node].toObject();
        auto inputs = n["inputs"].toObject();
        inputs[input] = value;
        n["inputs"] = inputs;
        graph[node] = n;
    };
    // Settings: the values kept with the workflow, then this generation's.
    QJsonObject chosen = adapterOf(id)["values"].toObject();
    for (auto it = v.settings.begin(); it != v.settings.end(); ++it) chosen[it.key()] = it.value();
    QSet<QString> adjustable;
    for (const auto& s : adapterOf(id)["slots"].toArray())
        if (s.toObject()["slot"] == "setting") adjustable << s.toObject()["node"].toString() + "/" + s.toObject()["input"].toString();
    for (auto it = chosen.begin(); it != chosen.end(); ++it) {
        const QString node = it.key().section('/', 0, 0), input = it.key().section('/', 1);
        if (adjustable.contains(it.key()) && graph.contains(node) && inputsOf(graph, node).contains(input)) put(node, input, it.value());
    }
    for (const auto& s : adapterOf(id)["slots"].toArray()) {
        const auto o = s.toObject();
        const QString node = o["node"].toString(), input = o["input"].toString(), slot = o["slot"].toString();
        QJsonValue value;
        if (slot == "prompt") value = v.prompt;
        else if (slot == "negative") { if (v.negative.isNull()) continue; value = v.negative; }
        else if (slot == "picture") { if (o["index"].toInt() >= v.pictures.size()) continue; value = v.pictures.value(o["index"].toInt()); }
        else if (slot == "seed") { if (v.seed < 0) continue; value = double(v.seed); }
        else if (slot == "width") { if (v.width <= 0) continue; value = v.width; }
        else if (slot == "height") { if (v.height <= 0) continue; value = v.height; }
        else if (slot == "prefix") { if (v.prefix.isEmpty()) continue; value = v.prefix; }
        else continue;
        if (!graph.contains(node)) continue;  // pruned with an unused picture
        put(node, input, value);
    }
    return graph;
}
