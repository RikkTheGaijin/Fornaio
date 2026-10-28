#pragma once
#include <QElapsedTimer>
#include <QFile>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <memory>

struct AiManifest {
    QJsonObject json;
    QString id;
    qint64 downloadBytes = 0;
    static AiManifest load(const QString &path, bool testSources = false);
    static AiManifest parse(const QByteArray &, bool testSources = false);
};
struct AiHardware {
    QString name, error;
    int vramMiB = 0, driver = 0;
    bool compatible = false;
    QString profile() const { return vramMiB < 20000 ? "memory" : "standard"; }
};
AiHardware aiHardware();
QString aiAssetsPath(const QString &relative);

class AiWorker : public QObject {
    Q_OBJECT
  public:
    explicit AiWorker(QObject *parent = nullptr);
    ~AiWorker();
    void start(const QString &python, const QString &model, const QString &profile, bool fake = false);
    void send(QJsonObject command);
    void stop();
    void cancel();
    bool active() const { return process_.state() != QProcess::NotRunning; }
  signals:
    void event(const QJsonObject &);
    void failed(const QString &);

  private:
    QProcess process_;
    QByteArray buffer_, diagnostics_;
    bool stopping_ = false;
    QTimer deadline_;
};

// Native setup. Each release archive is prebuilt; no Python is needed for downloading/extraction.
class AiInstaller : public QObject {
    Q_OBJECT
  public:
    explicit AiInstaller(QObject *parent = nullptr);
    ~AiInstaller();
    void setManifest(const AiManifest &m) { manifest_ = m; }
    void setRoot(const QString &root);
    QString root() const { return root_; }
    QString activePath() const;
    bool installed() const;
    bool busy() const { return busy_; }
    qint64 missingBytes() const;
    qint64 requiredBytes() const;
    void install(const QString &offlineDirectory = {}, bool repair = false);
    void setGpuAvailable(bool);
    void setMemoryProfile(const QString &profile) { healthProfile_ = profile; }
    // Token for gated Hugging Face files; empty uses HF_TOKEN or the Hugging Face CLI login. Never stored.
    void setHuggingFaceToken(const QString &token) { hfToken_ = token.trimmed(); }
    QString huggingFaceToken() const;
    void cancel();
    void remove();
    static QString defaultRoot();
  signals:
    void progress(qint64 received, qint64 total, const QString &message);
    void finished(bool success, const QString &message);
    void checkingEngine(bool);

  private:
    AiManifest manifest_;
    QString root_, offline_, stage_;
    QList<QJsonObject> files_;
    int index_ = 0, retry_ = 0, corruptRetries_ = 0;
    qint64 done_ = 0, offset_ = 0;
    bool busy_ = false, cancelled_ = false, verifying_ = false;
    bool gpuAvailable_ = true, waitingForGpu_ = false;
    QString healthProfile_ = "standard", hfToken_;
    qint64 transferred_ = 0, lastReceived_ = 0;
    QNetworkAccessManager network_;
    QNetworkReply *reply_ = nullptr;
    QFile part_, verify_, offlineSource_;
    std::unique_ptr<class QCryptographicHash> hash_;
    std::unique_ptr<class QLockFile> lock_;
    QProcess extract_;
    AiWorker health_;
    QTimer retryTimer_;
    QElapsedTimer elapsed_;
    void next();
    void request();
    void verifyCurrent();
    void copyOfflineChunk();
    void verifyChunk();
    void materialize();
    void complete();
    void fail(const QString &);
    QString objectPath(const QJsonObject &) const;
};
