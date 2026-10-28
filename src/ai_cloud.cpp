#include "ai_cloud.h"
#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>

namespace {
constexpr int timeoutMs = 6 * 60 * 1000;
const char *rules =
    "Rules: keep image 1's exact geometry, silhouette, creases, proportions, framing and camera pixel for pixel (do "
    "not move, resize, rotate, crop or redraw the shapes); output an unlit albedo texture render: perfectly flat, even "
    "lighting with no shadows, no shading, no highlights, no reflections and no ambient occlusion; pure white "
    "background; same 1:1 size as image 1.";
} // namespace

QString aiGptViewDescription(double turn, double pitch, bool known) {
    if (!known)
        return "identical to image 1";
    if (pitch >= 50)
        return "top-down view from above, identical to image 1; the front of the object is at the lower edge";
    if (pitch <= -50)
        return "view from below, identical to image 1; the front of the object is at the upper edge";
    if (turn >= 110)
        return "rear view, identical to image 1: the camera looks at the back of the object; its front is on the "
               "hidden far side, so features that belong on the front are not visible";
    if (turn >= 45)
        return "side view, identical to image 1; front features appear only where image 1 shows the front";
    return "front view, identical to image 1";
}

QString aiGptPrompt(bool fresh, bool hasReference, const QString &userPrompt, const QString &view) {
    const QString prompt = userPrompt.trimmed();
    const QString camera = " Camera: " + view + ".";
    const QString extra = hasReference && !prompt.isEmpty() ? " Additional instructions: " + prompt + "." : QString();
    // A reference with a strong pose (a face seen from below) pulls the model toward its layout; the reference
    // supplies appearance only, the model's shape alone places every feature.
    const QString appearanceOnly =
        hasReference ? QString(" Use image 2 only for what things look like, never for where they are: ignore its "
                               "pose, tilt, camera angle, perspective, framing and the positions and proportions of "
                               "its features. Image 1 alone decides where every feature is: paint each feature "
                               "exactly on the matching sculpted shape of image 1, following image 1's shapes even "
                               "where they differ from image 2.")
                     : QString();
    if (fresh) {
        if (hasReference)
            return "Image 1 is an untextured gray render of a 3D model. Image 2 is an appearance reference only. "
                   "Repaint image 1 so the 3D model has the appearance of image 2: its colors, materials, patterns "
                   "and fine surface details." +
                   appearanceOnly + extra + " " + rules + camera;
        return "Image 1 is an untextured gray render of a 3D model. Repaint it so the 3D model looks like " + prompt +
               ", with every detail placed exactly on the matching shape of the model. " + rules + camera;
    }
    const QString look = hasReference ? QString("the appearance of image 2") : prompt;
    return "Image 1 is a render of a partially textured 3D model: the colored areas are final paint, the flat gray "
           "areas are unpainted clay. Paint ONLY the flat gray clay areas, with " +
           look +
           ", continuing the existing paint seamlessly across the boundary with the same colors, patterns, features "
           "and detail scale. Every already painted pixel is locked: reproduce it exactly, do not repaint, move or "
           "restyle it." +
           appearanceOnly + extra + " " + rules + camera;
}

AiCloudImage::AiCloudImage(QObject *parent) : QObject(parent), process_(this), network_(this), ticker_(this) {
    ticker_.setInterval(1000);
    connect(&ticker_, &QTimer::timeout, this, [this] {
        if (!busy())
            return;
        if (elapsed_.elapsed() > timeoutMs) {
            finish("GPT Image 2 did not answer within 6 minutes.");
            return;
        }
        const double seconds = elapsed_.elapsed() / 1000.;
        emit event({{"event", "progress"},
                    {"id", request_["id"]},
                    {"fraction", std::min(.95, seconds / 70)},
                    {"message", QString("Generating with GPT Image 2 (%1) · %2 s").arg(backendName_).arg(int(seconds))}});
    });
    connect(&process_, &QProcess::finished, this, [this] {
        if (!busy())
            return;
        if (QFileInfo::exists(request_["output"].toString())) {
            finish();
            return;
        }
        QString log = QString::fromUtf8(process_.readAllStandardOutput() + process_.readAllStandardError()).trimmed();
        if (log.size() > 700)
            log = "…" + log.right(700);
        finish("Codex did not return an image." + (log.isEmpty() ? QString() : "\n" + log));
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (busy() && e == QProcess::FailedToStart)
            finish("Cannot start Codex: " + process_.errorString());
    });
}
AiCloudImage::~AiCloudImage() {
    if (reply_)
        reply_->abort();
    process_.kill();
    process_.waitForFinished(3000);
}

QString AiCloudImage::codexPath() {
    const QString configured = qEnvironmentVariable("FORNAIO_CODEX");
    if (!configured.isEmpty())
        return QFileInfo(configured).isExecutable() ? configured : QString();
    const QString onPath = QStandardPaths::findExecutable("codex");
    if (!onPath.isEmpty())
        return onPath;
    // The ChatGPT/Codex extension for VS Code ships the CLI; use the newest installed version.
    QStringList candidates;
    for (const QString &editor : {".vscode", ".vscode-insiders", ".cursor", ".windsurf"}) {
        QDir extensions(QDir::homePath() + "/" + editor + "/extensions");
        for (const QString &dir : extensions.entryList({"openai.chatgpt-*"}, QDir::Dirs)) {
            const QString exe = extensions.filePath(dir + "/bin/windows-x86_64/codex.exe");
            if (QFileInfo(exe).isExecutable())
                candidates << exe;
        }
    }
    QCollator natural;
    natural.setNumericMode(true);
    std::sort(candidates.begin(), candidates.end(),
              [&](const QString &a, const QString &b) { return natural.compare(a, b) < 0; });
    return candidates.isEmpty() ? QString() : candidates.last();
}

AiCloudImage::Backend AiCloudImage::backend() const {
    const QString forced = qEnvironmentVariable("FORNAIO_GPT_BACKEND").toLower();
    if (forced == "none")
        return Backend::None;
    if (forced == "fake")
        return Backend::Fake;
    if (forced == "api")
        return qEnvironmentVariableIsEmpty("OPENAI_API_KEY") ? Backend::None : Backend::Api;
    if (!codexPath().isEmpty())
        return Backend::Codex;
    if (forced != "codex" && !qEnvironmentVariableIsEmpty("OPENAI_API_KEY"))
        return Backend::Api;
    return Backend::None;
}

QString AiCloudImage::backendDescription() const {
    switch (backend()) {
    case Backend::Codex:
        return "GPT Image 2 through Codex with your ChatGPT sign-in (your plan's image limits apply).";
    case Backend::Api:
        return "GPT Image 2 through the OpenAI API (OPENAI_API_KEY, billed per image).";
    case Backend::Fake:
        return "GPT Image 2 test backend.";
    case Backend::None:
        break;
    }
    return "GPT Image 2 needs Codex signed in with ChatGPT (the VS Code ChatGPT extension or the Codex CLI), or an "
           "OPENAI_API_KEY environment variable.";
}

void AiCloudImage::generate(const QJsonObject &request) {
    if (busy())
        return;
    request_ = request;
    QFile::remove(request_["output"].toString());
    elapsed_.start();
    ticker_.start();
    switch (backend()) {
    case Backend::Codex:
        backendName_ = "Codex";
        runCodex();
        break;
    case Backend::Api:
        backendName_ = "API";
        runApi();
        break;
    case Backend::Fake:
        backendName_ = "test";
        QTimer::singleShot(50, this, [this] {
            if (!busy())
                return;
            const QString first = request_["images"].toArray().first().toString();
            QFile::copy(first, request_["output"].toString());
            finish();
        });
        break;
    case Backend::None:
        finish(backendDescription());
        break;
    }
}

void AiCloudImage::runCodex() {
    const QJsonArray images = request_["images"].toArray();
    const QString intro = images.size() > 1 ? "The first attached image is image 1, the second attached image is image "
                                              "2. Pass both attached images to the image tool as input images."
                                            : "The attached image is image 1. Pass it to the image tool as the input "
                                              "image.";
    const QString output = QFileInfo(request_["output"].toString()).fileName();
    const QString task =
        "Use your built-in image generation tool to make ONE image edit. " + intro + " Image prompt: \"" +
        request_["prompt"].toString() + "\" Output a square image. Save the generated image file exactly as " + output +
        " in the current directory (copy it there if the tool saved it elsewhere). Do not write any code to edit the "
        "image yourself; only use the image generation tool. Reply with the saved path.";
    const QString work = QDir::toNativeSeparators(request_["workDir"].toString());
    QStringList args{"exec", "--skip-git-repo-check", "-s", "workspace-write", "-C", work, "-i"};
    for (const auto &image : images)
        args << QDir::toNativeSeparators(image.toString());
    args << "-";
    process_.setWorkingDirectory(work);
    process_.start(codexPath(), args);
    if (process_.waitForStarted(15000)) {
        process_.write(task.toUtf8());
        process_.closeWriteChannel();
    }
}

void AiCloudImage::runApi() {
    auto multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    auto field = [&](const QString &name, const QByteArray &value) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader, QString("form-data; name=\"%1\"").arg(name));
        part.setBody(value);
        multipart->append(part);
    };
    field("model", "gpt-image-2");
    field("prompt", request_["prompt"].toString().toUtf8());
    field("size", "1024x1024");
    field("quality", "high");
    field("n", "1");
    for (const auto &image : request_["images"].toArray()) {
        QFile file(image.toString());
        if (!file.open(QIODevice::ReadOnly)) {
            delete multipart;
            finish("Cannot read " + image.toString());
            return;
        }
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentTypeHeader, "image/png");
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QString("form-data; name=\"image[]\"; filename=\"%1\"").arg(QFileInfo(file).fileName()));
        part.setBody(file.readAll());
        multipart->append(part);
    }
    QNetworkRequest http(QUrl("https://api.openai.com/v1/images/edits"));
    http.setRawHeader("Authorization", "Bearer " + qgetenv("OPENAI_API_KEY"));
    http.setTransferTimeout(timeoutMs);
    reply_ = network_.post(http, multipart);
    multipart->setParent(reply_);
    connect(reply_, &QNetworkReply::finished, this, [this, reply = reply_] {
        reply->deleteLater();
        if (!busy() || reply != reply_)
            return;
        const auto json = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError) {
            const QString message = json["error"].toObject()["message"].toString();
            finish("OpenAI API: " + (message.isEmpty() ? reply->errorString() : message));
            return;
        }
        const QByteArray png =
            QByteArray::fromBase64(json["data"].toArray().first().toObject()["b64_json"].toString().toLatin1());
        QFile out(request_["output"].toString());
        if (png.isEmpty() || !out.open(QIODevice::WriteOnly) || out.write(png) != png.size()) {
            finish("The OpenAI API returned no image.");
            return;
        }
        out.close();
        finish();
    });
}

void AiCloudImage::cancel() {
    if (!busy())
        return;
    request_ = {}; // Cleared first: the abort and kill below must not report a result or an error.
    ticker_.stop();
    if (reply_)
        reply_->abort();
    process_.kill();
}

void AiCloudImage::finish(const QString &error) {
    const QJsonObject request = request_;
    request_ = {};
    ticker_.stop();
    if (process_.state() != QProcess::NotRunning)
        process_.kill();
    if (!error.isEmpty()) {
        emit event({{"event", "error"}, {"id", request["id"]}, {"message", error}});
        return;
    }
    emit event({{"event", "result"},
                {"id", request["id"]},
                {"output", request["output"]},
                {"seconds", elapsed_.elapsed() / 1000.}});
}
