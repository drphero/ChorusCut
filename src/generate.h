#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantList>
#include <functional>
#include "workflows.h"

class Editor;

// Generates pictures through the user's ComfyUI workflows (see Workflows). Today: reference
// pictures for characters, looks and locations, from the picture prompt ("ref-text") or from
// approved pictures and the prompt ("ref-edit"). Several jobs can run at once. Each is recorded
// in the project's jobs table with the exact graph it queued, so closing the project or the app
// does not lose it; reopening picks it up. Results come back as references waiting for approval,
// each one undo step, with the workflow, prompt and seed kept on the reference.
class Generator : public QObject {
    Q_OBJECT
    // [{id, owner, ownerId, use, workflow, state ("uploading", "queued", "running", "failed"), text}]
    Q_PROPERTY(QVariantList jobs READ jobs NOTIFY changed)
    // What ComfyUI offers for text settings such as the sampler: {"ClassType/input": [choices]},
    // filled by fetchChoices for a workflow's node types (which also learns their optional
    // inputs and descriptions, for leaving pictures out and naming them).
    Q_PROPERTY(QVariantMap choices READ choices NOTIFY choicesChanged)
public:
    explicit Generator(Editor* editor);
    QVariantList jobs() const;
    // What can be generated for a character, look or location: {prompt, text: {ready, workflow,
    // why}, edit: {ready, workflow, pictures, why}}.
    Q_INVOKABLE QVariantMap options(const QString& owner, int ownerId) const;
    // use "ref-text" or "ref-edit". Returns "" when queued, else why not.
    Q_INVOKABLE QString generate(const QString& owner, int ownerId, const QString& use);
    // The Generate dialog's way in. options: {workflow (else the use's default), seed (a fixed
    // seed, else random; with a count, the next pictures take the seeds after it), count (1 to 8
    // pictures, one job each), settings: {key: value} for the workflow's adjustable inputs, kept
    // as the workflow's own for the next time, view (the view the pictures are for; its "from"
    // sets the use when use is ""), inputs (reference ids to edit from, in order; else the view's
    // own, else a first guess), instruction (else the view's framing)}. Returns "" when queued,
    // else why not.
    Q_INVOKABLE QString generateWith(const QString& owner, int ownerId, const QString& use, const QVariantMap& options);
    // For a view: {use, workflow, workflows, inputs (picked, else the first guess), guessed,
    // instruction, subject, why, ready, pictures: {min, max, known}, pictureName}.
    Q_INVOKABLE QVariantMap viewOptions(int view, const QString& use = {}, const QString& workflow = {}) const;
    // How a workflow's model names input picture n ("<Picture 2>"): the adapter's pattern, else
    // what ComfyUI's node descriptions say, else "picture n".
    Q_INVOKABLE QString pictureLabel(const QString& workflow, int n) const;
    // Why a workflow cannot make this picture now, or "".
    Q_INVOKABLE QString whyNot(const QString& owner, int ownerId, const QString& use, const QString& workflow) const;
    QVariantMap choices() const { return choiceMap; }
    Q_INVOKABLE void fetchChoices(const QString& workflow);
    // Lays the chosen pictures of the given views side by side, as one picture for a sheet view
    // (made on this computer, no ComfyUI), chosen at once; one undo step. "" or why not.
    Q_INVOKABLE QString composeSheet(int view, const QVariantList& panels);
    Q_INVOKABLE void cancel(int id);
    // Forgets a failed job; retry queues the same request again.
    Q_INVOKABLE void dismiss(int id);
    Q_INVOKABLE void retry(int id);
    // Called by the editor when a project opens or closes.
    void resume();
    void detach();
signals:
    void changed();
    void choicesChanged();
private:
    struct Job {
        int id = 0;
        QString owner; int ownerId = 0;
        QString use, workflow, workflowName, prompt, promptId, state, text;
        qint64 seed = 0;
        bool hasSeed = true;
        QJsonObject graph, settings;
        QJsonArray shown;  // [{key, label, value}], for the reference's details
        int view = 0;
        bool aspectChosen = false;  // the user set the workflow's aspect-ratio choice
        QList<int> inputs;  // reference ids, in order
        QString instruction;
        int misses = 0, waiting = 0;
    };
    Editor* editor;
    QNetworkAccessManager net;
    QList<Job> list;
    QVariantMap choiceMap;
    QSet<QString> asked;
    // Per server and node type: optional inputs and descriptions, from /object_info.
    struct NodeInfo { QSet<QString> optional; QString text; };
    QHash<QString, NodeInfo> info;
    QHash<QString, int> waiting;  // node types being fetched, by server|type
    QList<std::function<void()>> afterInfo;
    void withInfo(const QString& workflow, std::function<void()> then);
    Workflows::OptionalInputs optionalFor(const QString& workflow) const;
    bool infoKnown(const QString& workflow) const;
    QString subjectOf(const QString& owner, int ownerId) const;
    QList<int> guessInputs(int view) const;
    int serial = 0, epoch = 0;
    QString clientId;
    QTimer poll;
    Job* find(int id);
    void say(Job& job, const QString& state, const QString& text);
    void upload(int id, QStringList files, QStringList names);
    void submit(int id, const QStringList& names);
    void check();
    void collect(int id, const QJsonObject& outputs);
    void failJob(int id, const QString& why);
    void record(const Job& job, const QString& state, const QString& detail = {});
    bool ensureTable() const;
};
