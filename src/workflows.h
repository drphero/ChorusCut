#pragma once
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

// The user's ComfyUI workflows, each with an adapter that says which node input takes which
// value from Chorus Cut: the prompt, the reference pictures, the seed, the size and so on.
// One library for all projects, in plain files (Application Support/Chorus Cut/Workflows, or
// CHORUS_WORKFLOWS_DIR): a folder per workflow with workflow.json (ComfyUI's Export (API)
// format) and adapter.json, plus library.json with the default workflow for each use. A job
// stores the exact graph it queued in the project, so later edits here never change old takes.
// See docs/PLANNING.md, decision 4.
class Workflows : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList list READ list NOTIFY changed)
    Q_PROPERTY(QVariantList uses READ uses CONSTANT)
    Q_PROPERTY(QVariantList slotTypes READ slotTypes CONSTANT)
    Q_PROPERTY(QString folder READ folder CONSTANT)
public:
    // ComfyUI's optional inputs by node type, from /object_info: what may lose its link when a
    // picture is left out. Types not listed count as having none.
    using OptionalInputs = QHash<QString, QSet<QString>>;
    explicit Workflows(QObject* parent = nullptr);
    QString folder() const { return root.absolutePath(); }
    // [{id, name, use, useLabel, isDefault, ready, problems}]
    QVariantList list() const;
    // What a workflow can be used for: [{value, label, hint}].
    QVariantList uses() const;
    // The values Chorus Cut can put into a workflow: [{value, label, indexed}].
    QVariantList slotTypes() const;

    // Copies an Export (API) workflow into the library and guesses its adapter.
    // use "" guesses it too (pictures in means "ref-edit"). Result {ok, id, error}.
    Q_INVOKABLE QVariantMap importFile(const QUrl& file, const QString& use = {});
    // Everything about one workflow for the adapter sheet: {id, name, use, isDefault, ready,
    // problems, outputs, inputs: [{key, node, classType, title, input, value, slot, index}]}.
    // inputs lists every literal (unlinked) node input; slot is "" when it keeps its value.
    Q_INVOKABLE QVariantMap get(const QString& id) const;
    // Maps one node input to a slot ("" to keep the workflow's value). Pictures take an index.
    Q_INVOKABLE bool setSlot(const QString& id, const QString& node, const QString& input, const QString& slot, int index = 0);
    Q_INVOKABLE bool setUse(const QString& id, const QString& use);
    Q_INVOKABLE bool rename(const QString& id, const QString& name);
    Q_INVOKABLE bool setDefault(const QString& use, const QString& id);
    Q_INVOKABLE bool remove(const QString& id);
    // The workflow used for a use: its default, else the only ready one; "" when none is ready.
    Q_INVOKABLE QString forUse(const QString& use) const;
    // How many pictures a workflow takes (its highest picture index + 1).
    Q_INVOKABLE int pictureCount(const QString& id) const;
    // Whether a workflow can be used for a use ("ref-any" serves both picture uses).
    Q_INVOKABLE bool serves(const QString& id, const QString& use) const;
    // How the workflow's model refers to its input pictures, as a pattern such as "<Picture {n}>"
    // ("" when not set), and the user's notes on how to write for it, for the agent.
    QString pictureName(const QString& id) const;
    QString notes(const QString& id) const;
    Q_INVOKABLE bool setConventions(const QString& id, const QString& pictureName, const QString& notes);
    // Every node type the workflow uses.
    QStringList classes(const QString& id) const;
    // The fewest pictures the workflow can run with, given what is optional.
    int minPictures(const QString& id, const OptionalInputs& optional) const;
    // Takes the given nodes out of a graph with every node that then cannot run, and the links to
    // them; "" when done, else why not (an output would go). Static so tests can call it.
    static QString prune(QJsonObject& graph, const QSet<QString>& nodes, const QJsonArray& outputs, const OptionalInputs& optional);
    // The ready workflows for a use: [{id, name, isDefault}].
    Q_INVOKABLE QVariantList ready(const QString& use) const;
    Q_INVOKABLE bool hasSlot(const QString& id, const QString& slot) const;
    // The inputs marked "Adjust when generating", for the Generate dialog: [{key ("node/input"),
    // node, input, classType, label, type ("number", "text" or "switch"), value, original}].
    // value is what the next generation uses (the last one chosen), original the workflow's own.
    Q_INVOKABLE QVariantList settings(const QString& id) const;
    // Keeps chosen setting values ({key: value}) as the workflow's, for the next generation;
    // a value equal to the workflow's own clears the change. Returns "" or why not.
    QString setValues(const QString& id, const QJsonObject& values);
    // The values as the graph takes them ({key: value}), or why one is not right.
    QJsonObject checkValues(const QString& id, const QVariantMap& values, QString& error) const;

    // What to put into a workflow. Pictures are names ComfyUI gave the uploads, in order; fewer
    // than the workflow's Load Image inputs leaves the rest out (see prune).
    // settings: {key: value} for inputs marked "Adjust when generating", over the kept values.
    // For a take: audio is the uploaded song excerpt; frames the length, given to a seconds input
    // as frames / fps.
    // For a face refine: video is the uploaded take, face the picture the face is found by.
    struct Values { QString prompt, negative, prefix, audio, video, face; qint64 seed = -1; int width = 0, height = 0, frames = 0; double fps = 24;
                    QStringList pictures; QJsonObject settings; OptionalInputs optional; };
    // The graph to queue, or an empty object with the reason in error.
    QJsonObject build(const QString& id, const Values& values, QString& error) const;
    // The workflow's own width and height, when its adapter maps them; else 0.
    QSize size(const QString& id) const;
    QString nameOf(const QString& id) const;
    QJsonArray outputs(const QString& id) const;
    // A take workflow's frame rate, from its video node (CreateVideo's fps, Video Combine's
    // frame_rate); 24 when it doesn't say.
    double fps(const QString& id) const;

    // The adapter guessed for a graph: {slots: [{slot, node, input, index}], outputs: [...]}.
    static QJsonObject detect(const QJsonObject& graph);
    // "" when the JSON is an Export (API) graph, else what is wrong with it.
    static QString checkGraph(const QJsonDocument& doc);
signals:
    void changed();
private:
    QDir root;
    QJsonObject library() const;
    // H3 takes up to nine reference pictures; a take workflow's growing list offers that many.
    static constexpr int kGrownPictures = 9;
    int declaredPictures(const QString& id) const;
    QPair<QString, QString> growingInput(const QString& id) const;
    void saveLibrary(const QJsonObject& value);
    QJsonObject graphOf(const QString& id) const;
    QJsonObject adapterOf(const QString& id) const;
    bool saveAdapter(const QString& id, const QJsonObject& adapter);
    QStringList problems(const QString& id) const;
    QStringList ids() const;
};
