#include "ai_cloud.h"
#include <QBuffer>
#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSaveFile>
#include <QStandardPaths>
#include <QtEndian>
#include <algorithm>
#include <cmath>

namespace {
constexpr int timeoutMs = 6 * 60 * 1000;
constexpr qint64 maximumOutputBytes = 128ll * 1024 * 1024;
// A decoder can sometimes recover pixels from a truncated PNG. Require its
// complete chunk stream before treating an in-flight CLI file as finished.
bool completePng(const QByteArray &bytes) {
    if (!bytes.startsWith(QByteArray::fromHex("89504e470d0a1a0a")))
        return false;
    bool header = false, pixels = false;
    for (qsizetype offset = 8; offset <= bytes.size() - 12;) {
        const quint32 length = qFromBigEndian<quint32>(bytes.constData() + offset);
        if (quint64(length) + 12 > quint64(bytes.size() - offset))
            return false;
        const QByteArray type = bytes.mid(offset + 4, 4);
        if (!header) {
            if (type != "IHDR" || length != 13)
                return false;
            header = true;
        }
        pixels |= type == "IDAT";
        offset += qsizetype(length) + 12;
        if (type == "IEND")
            return header && pixels && length == 0 && offset == bytes.size();
    }
    return false;
}
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

QString aiGptUvRepairPrompt() {
    return "Image 1 is a complete UV albedo texture atlas board, not a photograph or a camera view. "
           "Image 2 is the matching repair map: pure white marks the only missing texels to fill; black is locked. "
           "Image 3 is a rendering of the accepted textured object and supplies appearance context only. "
           "Make ONE image edit that fills ALL white regions of image 2 across the whole board in image 1. "
           "Each separate panel is one material's complete UV atlas; keep every panel in its exact position "
           "and never merge, rearrange, crop, enlarge or omit panels. Continue adjacent accepted color, material, "
           "texture grain, pattern and detail scale seamlessly through the gaps. Keep the exact UV island "
           "layout, orientation, boundaries and pixel positions. The gray missing areas within an island need "
           "texture continuation; white exterior space is not part of the surface. Keep every existing painted "
           "texel unchanged. Do not invent or redraw anatomy, eyes, nostrils, ears, hairlines or other features; "
           "continue only the local surface appearance. Do not add lighting, shadows, highlights, ambient "
           "occlusion, labels, text or borders. Return only ONE repaired square 1024 by 1024 albedo board, "
           "preserving the full image 1 framing and relative positions of every atlas panel and UV island. "
           "Do not output the repair map or the rendering.";
}

AiCloudImage::RequestKind AiCloudImage::requestKind(const QJsonObject &request) {
    return request["purpose"].toString() == "uv_repair" || request["kind"].toString() == "uv_repair"
               ? RequestKind::UvRepair
               : RequestKind::Projection;
}

AiCloudImage::AiCloudImage(QObject *parent) : QObject(parent), process_(this), network_(this), ticker_(this) {
    ticker_.setInterval(1000);
    connect(&ticker_, &QTimer::timeout, this, [this] {
        if (!busy())
            return;
        if (activeBackend_ == Backend::Codex && tryCodexOutput())
            return;
        if (elapsed_.elapsed() > timeoutMs) {
            finish("GPT Image 2 did not answer within 6 minutes.");
            return;
        }
        reportProgress();
    });
    connect(&process_, &QProcess::started, this, [this] {
        if (!busy() || activeBackend_ != Backend::Codex)
            return;
        process_.write(codexTask_);
        process_.closeWriteChannel();
        codexTask_.clear();
        setPhase("provider", "Waiting for GPT Image 2 (Codex)");
    });
    connect(&process_, &QProcess::finished, this, [this] {
        if (!busy() || activeBackend_ != Backend::Codex)
            return;
        if (tryCodexOutput(true))
            return;
        QString log = QString::fromUtf8(process_.readAllStandardOutput() + process_.readAllStandardError()).trimmed();
        if (log.size() > 700)
            log = "…" + log.right(700);
        const QString validation = outputValidationError_.isEmpty() ? QString() : " " + outputValidationError_;
        finish("Codex did not return a complete readable image." + validation +
               (log.isEmpty() ? QString() : "\n" + log));
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (busy() && e == QProcess::FailedToStart)
            finish("Cannot start Codex: " + process_.errorString());
    });
}
QJsonObject AiCloudImage::phaseTimings() const {
    auto timings = phaseTimings_;
    if (!phase_.isEmpty() && elapsed_.isValid())
        timings[phase_] = timings[phase_].toDouble() +
                          std::max<qint64>(0, elapsed_.elapsed() - phaseStartedMs_) / 1000.;
    return timings;
}

void AiCloudImage::setPhase(const QString &phase, const QString &message) {
    if (phase != phase_) {
        phaseTimings_ = phaseTimings();
        phase_ = phase;
        phaseStartedMs_ = elapsed_.elapsed();
    }
    phaseMessage_ = message;
    reportProgress();
}

void AiCloudImage::reportProgress() {
    if (!busy())
        return;
    const double seconds = elapsed_.elapsed() / 1000.;
    // The provider reports no progress; estimate from a typical ~70 s generation and hold below done.
    emit event({{"event", "progress"},
                {"id", request_["id"]},
                {"fraction", std::min(.95, seconds / 70)},
                {"phase", phase_},
                {"phaseSeconds", std::max<qint64>(0, elapsed_.elapsed() - phaseStartedMs_) / 1000.},
                {"elapsedSeconds", seconds},
                {"phaseTimings", phaseTimings()},
                {"message", phaseMessage_ + QString(" - %1 s elapsed").arg(int(seconds))}});
}

bool AiCloudImage::tryCodexOutput(bool processFinished) {
    const auto requestId = request_.value("id"), requestedOutput = request_.value("output");
    const QFileInfo output(request_["output"].toString());
    if (!output.isFile() || output.size() <= 0)
        return false;
    if (output.size() != outputSize_ || output.lastModified() != outputModified_) {
        outputSize_ = output.size();
        outputModified_ = output.lastModified();
        outputObservedMs_ = elapsed_.elapsed();
        outputChecked_ = false;
        outputValidationError_.clear();
    }
    // Read/decode each stable file version at most once. A still-growing output
    // is never decoded every second, and a completed process needs no settling.
    if (outputChecked_ || (!processFinished && elapsed_.elapsed() - outputObservedMs_ < 750))
        return false;
    outputChecked_ = true;
    if (output.size() > maximumOutputBytes) {
        outputValidationError_ = "The returned image exceeds the supported file size.";
        return false;
    }
    QFile file(output.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        outputChecked_ = false; // A writer may hold an exclusive handle briefly.
        return false;
    }
    QByteArray bytes = file.readAll();
    file.close();
    const QFileInfo after(output.absoluteFilePath());
    if (bytes.size() != outputSize_ || after.size() != outputSize_ || after.lastModified() != outputModified_) {
        outputChecked_ = false;
        return false;
    }
    if (!completePng(bytes)) {
        outputValidationError_ = "The returned PNG is incomplete.";
        return false;
    }
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "PNG");
    const QSize size = reader.size();
    if (size.isEmpty() || size.width() > 8192 || size.height() > 8192) {
        outputValidationError_ = "The returned image dimensions are invalid.";
        return false;
    }
    setPhase("validating_output", "Verifying the completed GPT Image 2 image");
    if (!busy() || request_["id"] != requestId || request_["output"] != requestedOutput)
        return true; // Progress handlers may synchronously cancel or start another request.
    const QImage image = reader.read();
    if (image.isNull()) {
        outputValidationError_ = "The returned PNG could not be decoded.";
        return false;
    }
    // Freeze the verified bytes before stopping the CLI's final reply. Its
    // descendants can no longer change the image consumed by the caller.
    const QString readyPath = output.absolutePath() + "/" + output.completeBaseName() + ".ready.png";
    QSaveFile ready(readyPath);
    if (!ready.open(QIODevice::WriteOnly) || ready.write(bytes) != bytes.size() || !ready.commit()) {
        finish("Cannot preserve the completed GPT Image 2 image: " + ready.errorString());
        return true;
    }
    finish({}, readyPath);
    return true;
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
    // A killed Codex process may still be delivering its final signal. Drain it while
    // no request is active, before its finished handler can observe the next request.
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        if (!process_.waitForFinished(5000)) {
            emit event({{"event", "error"},
                        {"id", request["id"]},
                        {"message", "The previous Codex process is still stopping. Retry shortly."}});
            return;
        }
    }
    request_ = request;
    QFile::remove(request_["output"].toString());
    elapsed_.start();
    phase_.clear();
    phaseMessage_.clear();
    phaseStartedMs_ = 0;
    phaseTimings_ = {};
    codexTask_.clear();
    outputSize_ = -1;
    outputModified_ = {};
    outputChecked_ = false;
    outputValidationError_.clear();
    ticker_.start();
    activeBackend_ = backend();
    const auto startedRequest = request_;
    switch (activeBackend_) {
    case Backend::Codex:
        backendName_ = "Codex";
        setPhase("starting_provider", "Starting GPT Image 2 (Codex)");
        if (request_ == startedRequest)
            runCodex();
        break;
    case Backend::Api:
        backendName_ = "API";
        setPhase("uploading", "Sending images to GPT Image 2 (API)");
        if (request_ == startedRequest)
            runApi();
        break;
    case Backend::Fake:
        backendName_ = "test";
        setPhase("provider", "Waiting for GPT Image 2 (test)");
        if (request_ != startedRequest)
            break;
        QTimer::singleShot(50, this, [this, id = request_.value("id"), output = request_.value("output")] {
            if (!busy() || request_["id"] != id || request_["output"] != output)
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
    QString intro =
        images.size() > 1
            ? "The first attached image is image 1, the second attached image is image 2. Pass both attached "
              "images to the image tool as input images."
            : "The attached image is image 1. Pass it to the image tool as the input image.";
    if (images.size() > 2) {
        intro.clear();
        for (int i = 0; i < images.size(); ++i)
            intro += QString("Attachment %1 is image %1. ").arg(i + 1);
        intro += "Pass all " + QString::number(images.size()) + " attached images to the image tool in this order. ";
    }
    if (requestKind(request_) == RequestKind::UvRepair)
        intro += " Use GPT Image 2. The visible repair map in image 2 is guidance; this route has no structured "
                 "edit mask. ";
    const QString output = QFileInfo(request_["output"].toString()).fileName();
    const QString task =
        "Use your built-in image generation tool to make ONE image edit. " + intro + " Image prompt: \"" +
        request_["prompt"].toString() + "\" Output a square image. Save the generated image file exactly as " + output +
        " in the current directory (copy it there if the tool saved it elsewhere). Do not write any code to edit the "
        "image yourself; only use the image generation tool. Do not delegate or launch helper agents. "
        "Make exactly one image-tool call, copy its generated file to the requested path, then reply with the saved path.";
    const QString work = QDir::toNativeSeparators(request_["workDir"].toString());
    QStringList args{"exec", "--skip-git-repo-check", "-s", "workspace-write", "-C", work, "-i"};
    for (const auto &image : images)
        args << QDir::toNativeSeparators(image.toString());
    args << "-";
    process_.setWorkingDirectory(work);
    codexTask_ = task.toUtf8();
    process_.start(codexPath(), args);
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
    if (requestKind(request_) == RequestKind::UvRepair && !request_["mask"].toString().isEmpty()) {
        QFile file(request_["mask"].toString());
        if (!file.open(QIODevice::ReadOnly) || file.size() >= 4 * 1024 * 1024) {
            delete multipart;
            finish("Cannot read Smart fill alpha mask (a PNG smaller than 4 MB is required).");
            return;
        }
        const QByteArray maskBytes = file.readAll();
        const QImage mask = QImage::fromData(maskBytes, "PNG");
        const QImage first(request_["images"].toArray().first().toString());
        if (mask.isNull() || first.isNull() || !mask.hasAlphaChannel() || mask.size() != first.size()) {
            delete multipart;
            finish("Smart fill alpha mask must match the first input image's dimensions.");
            return;
        }
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentTypeHeader, "image/png");
        part.setHeader(QNetworkRequest::ContentDispositionHeader,
                       QString("form-data; name=\"mask\"; filename=\"%1\"").arg(QFileInfo(file).fileName()));
        part.setBody(maskBytes);
        multipart->append(part);
    }
    QNetworkRequest http(QUrl("https://api.openai.com/v1/images/edits"));
    http.setRawHeader("Authorization", "Bearer " + qgetenv("OPENAI_API_KEY"));
    http.setTransferTimeout(timeoutMs);
    reply_ = network_.post(http, multipart);
    multipart->setParent(reply_);
    connect(reply_, &QNetworkReply::uploadProgress, this, [this, reply = reply_](qint64 sent, qint64 total) {
        if (busy() && reply == reply_ && total > 0 && sent >= total)
            setPhase("provider", "Waiting for GPT Image 2 (API)");
    });
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
    const auto reply = reply_;
    reply_ = nullptr; // A delayed reply callback must not see a subsequent request as its own.
    if (reply)
        reply->abort();
    if (process_.state() != QProcess::NotRunning) {
        process_.kill();
        process_.waitForFinished(5000);
    }
}

void AiCloudImage::finish(const QString &error, const QString &readyOutput) {
    if (!busy())
        return;
    const QJsonObject request = request_;
    const auto timings = phaseTimings();
    request_ = {};
    ticker_.stop();
    const auto reply = reply_;
    reply_ = nullptr;
    if (reply && !reply->isFinished())
        reply->abort();
    if (process_.state() != QProcess::NotRunning)
        process_.kill();
    if (!error.isEmpty()) {
        emit event({{"event", "error"}, {"id", request["id"]}, {"message", error},
                    {"seconds", elapsed_.elapsed() / 1000.}, {"phaseTimings", timings}, {"timings", timings}});
        return;
    }
    emit event({{"event", "result"},
                {"id", request["id"]},
                {"output", readyOutput.isEmpty() ? request["output"] : QJsonValue(readyOutput)},
                {"phaseTimings", timings},
                {"timings", timings},
                {"seconds", elapsed_.elapsed() / 1000.}});
}
