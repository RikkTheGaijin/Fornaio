#pragma once
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
    explicit AiCloudImage(QObject *parent = nullptr);
    ~AiCloudImage();
    static QString codexPath();
    Backend backend() const;
    QString backendDescription() const;
    bool busy() const { return !request_.isEmpty(); }
    // request: id, images (array of PNG paths; the first is the camera guide), prompt, output, workDir.
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
    void runCodex();
    void runApi();
    void finish(const QString &error = {});
};

// Prompts for GPT Image 2. image 1 is the camera guide (locked paint and gray clay), image 2 the reference.
// viewTurn/viewPitch name the camera relative to the front view so front features are not painted on the back.
QString aiGptViewDescription(double viewTurn, double viewPitch, bool known);
QString aiGptPrompt(bool fresh, bool hasReference, const QString &userPrompt, const QString &view);
