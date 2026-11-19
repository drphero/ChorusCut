#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QUrl>

class Editor;
class QNetworkReply;
class QWebSocket;

// Runs Chorus Cut jobs on a ComfyUI server: the user's own machine or a rented GPU.
// Jobs are recorded in the project (table "jobs"), so closing the project or the app,
// or losing the connection, does not lose them: reopening the project picks them up.
// Today the only job is lyric alignment (the ChorusCutAlignLyrics node).
class ComfyJobs : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString url READ url NOTIFY settingsChanged)
    Q_PROPERTY(bool hasToken READ hasToken NOTIFY settingsChanged)
    Q_PROPERTY(QString tokenNote READ tokenNote NOTIFY settingsChanged)   // where the token is kept
    Q_PROPERTY(QString connection READ connection NOTIFY connectionChanged)   // unknown, checking, ready, unreachable, missingNode
    Q_PROPERTY(QString connectionText READ connectionText NOTIFY connectionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY jobChanged)
    Q_PROPERTY(QString jobText READ jobText NOTIFY jobChanged)
    Q_PROPERTY(QString language READ language NOTIFY jobChanged)
public:
    explicit ComfyJobs(Editor* editor);
    QString url() const { return server; }
    bool hasToken() const { return !token.isEmpty(); }
    QString tokenNote() const { return tokenWhere; }
    QString connection() const { return state; }
    QString connectionText() const { return stateText; }
    bool busy() const { return !promptId.isEmpty() || uploading; }
    QString jobText() const { return status; }
    QString language() const { return lastLanguage; }

    // An empty token keeps the saved one; pass clearToken to remove it.
    Q_INVOKABLE void configure(const QString& url, const QString& token, bool clearToken = false);
    Q_INVOKABLE void testConnection();
    // Uploads the project's song and queues ChorusCutAlignLyrics for the saved lyrics.
    Q_INVOKABLE void alignLyrics(const QString& language);
    Q_INVOKABLE void cancel();
    // Called by the editor when a project opens or closes.
    void resume();
    void detach();
signals:
    void settingsChanged();
    void connectionChanged();
    void jobChanged();
    void finished(bool success);
private:
    Editor* editor;
    QNetworkAccessManager net;
    QString server, token, state = "unknown", stateText, status, lastLanguage = "en";
    QString promptId, jobLyrics;
    // ComfyUI streams progress over a WebSocket, only to the client ID that queued the job,
    // so each job keeps its client ID and a reopened project listens with the same one.
    QString clientId;
    QWebSocket* socket = nullptr;
    int percent = -1;
    void listen();
    void stopListening();
    void showRunning();
    // The access token lives in the system keychain (macOS Keychain, Windows Credential
    // Manager, Secret Service on Linux), one entry per server. Without a keychain it is
    // kept for this session only, never written to disk.
    QString tokenWhere;
    bool tokenLoading = false, testWaiting = false, keychainMissing = false;
    QString keychainService() const;
    void loadToken();
    void storeToken(const QString& value);
    bool uploading = false;
    int serial = 0;          // Replies from an earlier project or job are ignored.
    int probe = 0;           // Likewise for connection tests.
    int misses = 0;          // Polls in a row where the server could not be reached.
    QTimer poll;
    QElapsedTimer clock;
    QNetworkRequest request(const QString& path) const;
    void setConnection(const QString& value, const QString& text);
    void say(const QString& text);
    bool ensureTable();
    void record(const QString& state, const QString& detail = {});
    void submit(const QString& uploaded, const QString& language);
    void check();
    void complete(const QByteArray& alignment);
    void fail(const QString& why);
    void forget();
    static QString errorOf(QNetworkReply* reply);
};
