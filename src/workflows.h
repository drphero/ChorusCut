#pragma once
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
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

    // What to put into a workflow. Pictures are names ComfyUI gave the uploads, in order.
    // settings: {key: value} for inputs marked "Adjust when generating", over the kept values.
    struct Values { QString prompt, negative, prefix; qint64 seed = -1; int width = 0, height = 0; QStringList pictures; QJsonObject settings; };
    // The graph to queue, or an empty object with the reason in error.
    QJsonObject build(const QString& id, const Values& values, QString& error) const;
    // The workflow's own width and height, when its adapter maps them; else 0.
    QSize size(const QString& id) const;
    QString nameOf(const QString& id) const;
    QJsonArray outputs(const QString& id) const;

    // The adapter guessed for a graph: {slots: [{slot, node, input, index}], outputs: [...]}.
    static QJsonObject detect(const QJsonObject& graph);
    // "" when the JSON is an Export (API) graph, else what is wrong with it.
    static QString checkGraph(const QJsonDocument& doc);
signals:
    void changed();
private:
    QDir root;
    QJsonObject library() const;
    void saveLibrary(const QJsonObject& value);
    QJsonObject graphOf(const QString& id) const;
    QJsonObject adapterOf(const QString& id) const;
    bool saveAdapter(const QString& id, const QJsonObject& adapter);
    QStringList problems(const QString& id) const;
    QStringList ids() const;
};
