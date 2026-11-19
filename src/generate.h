#pragma once
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QVariantList>

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
public:
    explicit Generator(Editor* editor);
    QVariantList jobs() const;
    // What can be generated for a character, look or location: {prompt, text: {ready, workflow,
    // why}, edit: {ready, workflow, pictures, why}}.
    Q_INVOKABLE QVariantMap options(const QString& owner, int ownerId) const;
    // use "ref-text" or "ref-edit". Returns "" when queued, else why not.
    Q_INVOKABLE QString generate(const QString& owner, int ownerId, const QString& use);
    Q_INVOKABLE void cancel(int id);
    // Forgets a failed job; retry queues the same request again.
    Q_INVOKABLE void dismiss(int id);
    Q_INVOKABLE void retry(int id);
    // Called by the editor when a project opens or closes.
    void resume();
    void detach();
signals:
    void changed();
private:
    struct Job {
        int id = 0;
        QString owner; int ownerId = 0;
        QString use, workflow, workflowName, prompt, promptId, state, text;
        qint64 seed = 0;
        QJsonObject graph;
        int misses = 0, waiting = 0;
    };
    Editor* editor;
    QNetworkAccessManager net;
    QList<Job> list;
    int serial = 0, epoch = 0;
    QString clientId;
    QTimer poll;
    Job* find(int id);
    void say(Job& job, const QString& state, const QString& text);
    QStringList picturesFor(const QString& owner, int ownerId) const;
    void upload(int id, QStringList files, QStringList names);
    void submit(int id, const QStringList& names);
    void check();
    void collect(int id, const QJsonObject& outputs);
    void failJob(int id, const QString& why);
    void record(const Job& job, const QString& state, const QString& detail = {});
    bool ensureTable() const;
};
