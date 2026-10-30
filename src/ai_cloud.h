#pragma once
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QPointer>
#include <QProcess>
#include <QTimer>

// GPT Image 2 generation for AI Texturing. Two backends, chosen automatically:
//  - Codex: the Codex CLI with the user's ChatGPT sign-in (its built-in image tool, no API key);
//  - API: the OpenAI Images edit endpoint with OPENAI_API_KEY.
// Events use the local worker's protocol: {"event": "progress" | "result" | "error", "id", ...}.
// FORNAIO_GPT_BACKEND=none|codex|api|fake overrides the choice (fake copies the first image, for tests).
class AiCloudImage : public QObject {
    Q_OBJECT
  public:
    enum class Backend { None, Codex, Api, Fake };
    enum class RequestKind { Projection, UvRepair };
    explicit AiCloudImage(QObject *parent = nullptr);
    ~AiCloudImage();
    static QString codexPath();
    Backend backend() const;
    QString backendDescription() const;
    bool busy() const { return !request_.isEmpty(); }
    // request: id, kind/purpose (projection or uv_repair), images (numbered PNG paths), prompt,
    // output, workDir. UV repair may supply mask (alpha-zero editable PNG, applied to image 1 by API).
    static RequestKind requestKind(const QJsonObject &request);
    void generate(const QJsonObject &request);
    void cancel(); // Silent: the caller reports the cancellation.
  signals:
    void event(const QJsonObject &);

  private:
    QProcess process_;
    QNetworkAccessManager network_;
    QPointer<QNetworkReply> reply_;
    QJsonObject request_;
    QElapsedTimer elapsed_;
    QTimer ticker_;
    QString backendName_;
    Backend activeBackend_ = Backend::None;
    QString phase_, phaseMessage_;
    qint64 phaseStartedMs_ = 0;
    QJsonObject phaseTimings_;
    QByteArray codexTask_;
    qint64 outputSize_ = -1, outputObservedMs_ = 0;
    QDateTime outputModified_;
    bool outputChecked_ = false;
    QString outputValidationError_;
    void setPhase(const QString &, const QString &message);
    void reportProgress();
    QJsonObject phaseTimings() const;
    bool tryCodexOutput(bool processFinished = false);
    void runCodex();
    void runApi();
    void finish(const QString &error = {}, const QString &readyOutput = {});
};

// Prompts for GPT Image 2. image 1 is the camera guide (locked paint and gray clay), image 2 the reference.
// viewTurn/viewPitch name the camera relative to the front view so front features are not painted on the
// back.
QString aiGptViewDescription(double viewTurn, double viewPitch, bool known);
QString aiGptPrompt(bool fresh, bool hasReference, const QString &userPrompt, const QString &view);
QString aiGptUvRepairPrompt();
