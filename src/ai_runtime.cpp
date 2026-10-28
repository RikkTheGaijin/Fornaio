#include "ai_runtime.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QUrl>
#include <QUuid>
#include <stdexcept>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace {
void error(const QString &s) {
    throw std::runtime_error(s.toStdString());
}
bool safeRelative(const QString &p) {
    return !p.isEmpty() && !QDir::isAbsolutePath(p) && !p.contains(':') && !p.contains('\\') &&
           !p.split('/').contains("..") && !p.split('/').contains(".") && !p.contains("//");
}
QString componentName(const QJsonObject &file) {
    const QString path = file["path"].toString();
    if (path.startsWith("runtime/"))
        return "AI runtime";
    if (path.startsWith("model/text_encoder/") || path.startsWith("model/tokenizer/"))
        return "Text encoder";
    if (path.startsWith("model/vae/"))
        return "Image decoder";
    return "Image generation model";
}
void writeAtomic(const QString &path, const QByteArray &data) {
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(data) != data.size() || !f.commit())
        error("Cannot write " + path);
}
void hidden(QProcess &process) {
#ifdef Q_OS_WIN
    process.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_NO_WINDOW; });
#endif
}
bool removeChild(const QString &root, const QString &path) {
    QString base = QDir(root).absolutePath() + "/", target = QFileInfo(path).absoluteFilePath();
    if (!target.startsWith(base, Qt::CaseInsensitive) || QFileInfo(path).isSymLink())
        return false;
    return QDir(path).removeRecursively();
}
} // namespace
QString aiAssetsPath(const QString &relative) {
    return QCoreApplication::applicationDirPath() + "/ai/" + relative;
}
AiManifest AiManifest::load(const QString &path, bool tests) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        error("AI download catalogue is missing: " + path);
    return parse(f.readAll(), tests);
}
AiManifest AiManifest::parse(const QByteArray &bytes, bool tests) {
    QJsonParseError e;
    auto j = QJsonDocument::fromJson(bytes, &e).object();
    if (e.error != QJsonParseError::NoError || j["format"] != "FornaioAI" || j["version"] != 1)
        error("Invalid AI download catalogue");
    AiManifest m;
    m.json = j;
    m.id = j["id"].toString();
    if (!QRegularExpression("^[A-Za-z0-9_-]{1,80}$").match(m.id).hasMatch())
        error("Invalid AI release identifier");
    if (!safeRelative(j["python"].toString()))
        error("Invalid AI Python path");
    QSet<QString> paths;
    for (const auto &v : j["files"].toArray()) {
        auto f = v.toObject();
        QString path = f["path"].toString(), extract = f["extract"].toString();
        QUrl url(f["url"].toString());
        qint64 n = f["bytes"].toInteger();
        if (!safeRelative(path) || paths.contains(path.toLower()) ||
            (!extract.isEmpty() && !safeRelative(extract)) || n < 1 ||
            !QRegularExpression("^[a-f0-9]{64}$").match(f["sha256"].toString()).hasMatch() ||
            !url.isValid() || (!tests && url.scheme() != "https") || !url.userInfo().isEmpty())
            error("Unsafe AI catalogue file: " + path);
        paths.insert(path.toLower());
        m.downloadBytes += n;
    }
    if (paths.empty())
        error("AI catalogue contains no files");
    return m;
}
AiHardware aiHardware() {
    AiHardware h;
    QProcess p;
    hidden(p);
    QString smi = QStandardPaths::findExecutable("nvidia-smi");
#ifdef Q_OS_WIN
    if (smi.isEmpty())
        smi = qEnvironmentVariable("SystemRoot") + "/System32/nvidia-smi.exe";
#endif
    p.start(smi,
            {"--query-gpu=name,memory.total,driver_version,compute_cap", "--format=csv,noheader,nounits"});
    if (!p.waitForFinished(5000) || p.exitCode() != 0) {
        p.kill();
        h.error = "An NVIDIA RTX GPU and compatible driver are required.";
        return h;
    }
    for (const auto &line : p.readAllStandardOutput().split('\n')) {
        auto f = line.trimmed().split(',');
        if (f.size() != 4)
            continue;
        int mem = f[1].trimmed().toInt();
        const QString name = QString::fromUtf8(f[0].trimmed());
        int driver = f[2].trimmed().split('.')[0].toInt();
        bool compatible = name.contains("RTX", Qt::CaseInsensitive) && mem >= 12000 && driver >= 590 &&
                          f[3].trimmed().toFloat() >= 8;
        if ((!compatible && h.compatible) || (compatible == h.compatible && mem <= h.vramMiB))
            continue;
        h.name = name;
        h.vramMiB = mem;
        h.driver = driver;
        h.compatible = compatible;
    }
    if (!h.compatible)
        h.error = "AI Texturing requires an Ampere-or-newer RTX GPU (RTX 30 series or newer), at least 12 GB "
                  "VRAM, and driver R590 or newer.";
    return h;
}

AiWorker::AiWorker(QObject *p) : QObject(p) {
    hidden(process_);
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        process_.kill();
        emit failed("AI worker timed out; the accepted texture is unchanged.");
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        diagnostics_ += process_.readAllStandardError();
        if (diagnostics_.size() > 16384)
            diagnostics_ = diagnostics_.right(16384);
    });
    connect(&process_, &QProcess::readyReadStandardOutput, this, [this] {
        buffer_ += process_.readAllStandardOutput();
        if (buffer_.size() > 1024 * 1024) {
            stop();
            emit failed("Invalid AI worker response");
            return;
        }
        while (buffer_.contains('\n')) {
            int i = buffer_.indexOf('\n');
            QByteArray line = buffer_.left(i);
            buffer_.remove(0, i + 1);
            QJsonParseError e;
            auto j = QJsonDocument::fromJson(line, &e).object();
            if (e.error != QJsonParseError::NoError || j["protocol"] != 1) {
                stop();
                emit failed("AI worker protocol mismatch");
                return;
            }
            if (j["event"] == "progress")
                deadline_.start(600000);
            else
                deadline_.stop();
            emit event(j);
        }
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (!stopping_ && e == QProcess::FailedToStart)
            emit failed("Cannot start the private AI runtime: " + process_.errorString());
    });
    connect(&process_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                deadline_.stop();
                if (!stopping_)
                    emit failed(QString("AI worker exited (%1). %2")
                                    .arg(code)
                                    .arg(QString::fromUtf8(diagnostics_.right(2000))));
            });
}
AiWorker::~AiWorker() {
    stop();
}
void AiWorker::start(const QString &python, const QString &model, const QString &profile, bool fake) {
    stop();
    stopping_ = false;
    buffer_.clear();
    diagnostics_.clear();
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove("PYTHONPATH");
    env.remove("PYTHONHOME");
    for (const auto &key : env.keys())
        if (key.startsWith("CUDA_PATH", Qt::CaseInsensitive))
            env.remove(key);
#ifdef Q_OS_WIN
    QString runtime = QFileInfo(python).absolutePath(),
            windows = qEnvironmentVariable("SystemRoot", "C:/Windows");
    env.insert("PATH", QDir::toNativeSeparators(runtime) + ";" +
                           QDir::toNativeSeparators(runtime + "/Lib/site-packages/torch/lib") + ";" +
                           windows + "/System32;" + windows);
#endif
    env.insert("HF_HUB_OFFLINE", "1");
    env.insert("TRANSFORMERS_OFFLINE", "1");
    env.insert("HF_HUB_DISABLE_TELEMETRY", "1");
    process_.setProcessEnvironment(env);
    QStringList args{"-I", aiAssetsPath("ai_worker.py"), "--model", model, "--profile", profile};
    if (fake)
        args << "--fake";
    process_.start(python, args);
    deadline_.start(30000);
}
void AiWorker::send(QJsonObject command) {
    command["protocol"] = 1;
    process_.write(QJsonDocument(command).toJson(QJsonDocument::Compact) + '\n');
    if (command["command"] == "generate" || command["command"] == "health")
        deadline_.start(600000);
}
void AiWorker::cancel() {
    if (!active())
        return;
    send({{"command", "cancel"}});
    deadline_.start(5000);
}
void AiWorker::stop() {
    stopping_ = true;
    deadline_.stop();
    if (active()) {
        send({{"command", "shutdown"}});
        process_.closeWriteChannel();
        if (!process_.waitForFinished(1000)) {
            process_.kill();
            process_.waitForFinished(3000);
        }
    }
}

AiInstaller::AiInstaller(QObject *p) : QObject(p) {
    hidden(extract_);
    retryTimer_.setSingleShot(true);
    connect(&retryTimer_, &QTimer::timeout, this, &AiInstaller::request);
    connect(&extract_, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus) {
                if (!busy_)
                    return;
                if (cancelled_) {
                    fail("Download cancelled; verified files are retained for resume.");
                    return;
                }
                if (code) {
                    fail("Cannot extract AI archive: " +
                         QString::fromUtf8(extract_.readAllStandardError()).right(1200));
                    return;
                }
                done_ += files_[index_]["bytes"].toInteger();
                ++index_;
                next();
            });
    connect(&extract_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && busy_)
            fail("Cannot start Windows archive extraction");
    });
    connect(&health_, &AiWorker::event, this, [this](const QJsonObject &e) {
        if (!busy_)
            return;
        if (e["event"] == "ready")
            health_.send({{"command", "health"}, {"id", "setup"}});
        else if (e["event"] == "health") {
            health_.stop();
            complete();
        } else if (e["event"] == "error")
            fail("AI engine check failed: " + e["message"].toString());
        else if (e["event"] == "progress")
            emit progress(done_, manifest_.downloadBytes, e["message"].toString());
    });
    connect(&health_, &AiWorker::failed, this, [this](const QString &e) {
        if (busy_)
            fail(e);
    });
}
AiInstaller::~AiInstaller() {
    cancel();
    extract_.kill();
    extract_.waitForFinished(3000);
    health_.stop();
}
QString AiInstaller::defaultRoot() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/Fornaio/AI";
}
void AiInstaller::setRoot(const QString &path) {
    if (!busy_)
        root_ = QDir(path).absolutePath();
}
QString AiInstaller::activePath() const {
    QFile f(root_ + "/active.json");
    if (!f.open(QIODevice::ReadOnly))
        return {};
    auto j = QJsonDocument::fromJson(f.readAll()).object();
    QString id = j["id"].toString();
    if (!QRegularExpression("^[A-Za-z0-9_-]{1,80}$").match(id).hasMatch())
        return {};
    return root_ + "/engines/" + id;
}
bool AiInstaller::installed() const {
    return !activePath().isEmpty() && QFileInfo::exists(activePath() + "/runtime/python.exe") &&
           QFileInfo::exists(activePath() + "/ready.json");
}
QString AiInstaller::objectPath(const QJsonObject &f) const {
    return root_ + "/objects/" + f["sha256"].toString();
}
qint64 AiInstaller::missingBytes() const {
    qint64 n = 0;
    for (auto v : manifest_.json["files"].toArray()) {
        auto f = v.toObject();
        QFileInfo ready(objectPath(f)), part(objectPath(f) + ".part");
        if (ready.size() == f["bytes"].toInteger())
            continue;
        n += std::max(qint64(0), f["bytes"].toInteger() - part.size());
    }
    return n;
}
qint64 AiInstaller::requiredBytes() const {
    return missingBytes() + qint64(12) * 1024 * 1024 * 1024;
}
void AiInstaller::install(const QString &offline, bool repair) {
    if (busy_)
        return;
    if (root_.isEmpty() || manifest_.id.isEmpty()) {
        emit finished(false, "AI download catalogue is unavailable.");
        return;
    }
    QDir().mkpath(root_ + "/objects");
    lock_ = std::make_unique<QLockFile>(root_ + "/install.lock");
    if (!lock_->tryLock(0)) {
        lock_.reset();
        emit finished(false, "Another AI setup is using this storage location.");
        return;
    }
    QStorageInfo storage(root_);
    if (storage.isValid() && storage.bytesAvailable() < requiredBytes()) {
        emit finished(false, "Insufficient disk space for AI installation and extraction.");
        lock_.reset();
        return;
    }
    offline_ = offline;
    cancelled_ = false;
    busy_ = true;
    index_ = 0;
    done_ = 0;
    retry_ = 0;
    corruptRetries_ = 0;
    transferred_ = 0;
    lastReceived_ = 0;
    waitingForGpu_ = false;
    elapsed_.start();
    files_.clear();
    for (auto v : manifest_.json["files"].toArray())
        files_ << v.toObject();
    stage_ = root_ + "/staging-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    QDir().mkpath(stage_);
    // Always rehash cached files, including Repair. A partial download alone is never trusted.
    Q_UNUSED(repair);
    next();
}
void AiInstaller::next() {
    if (!busy_)
        return;
    if (cancelled_) {
        fail("Download cancelled; verified files are retained for resume.");
        return;
    }
    if (index_ == files_.size()) {
        if (!gpuAvailable_) {
            waitingForGpu_ = true;
            emit progress(
                done_, manifest_.downloadBytes,
                "Downloads complete. Waiting for the current GPU job before checking the AI engine.");
            return;
        }
        waitingForGpu_ = false;
        try {
            for (const QString &dll :
                 {QString("msvcp140.dll"), QString("msvcp140_1.dll"), QString("msvcp140_2.dll"),
                  QString("vcruntime140.dll"), QString("vcruntime140_1.dll"), QString("concrt140.dll")}) {
                QString source = QCoreApplication::applicationDirPath() + "/" + dll;
                if (QFileInfo::exists(source)) {
                    QFile::remove(stage_ + "/runtime/" + dll);
                    if (!QFile::copy(source, stage_ + "/runtime/" + dll))
                        error("Cannot prepare the private Microsoft runtime");
                }
            }
            writeAtomic(stage_ + "/runtime/python312._pth",
                        "python312.zip\n.\nLib/site-packages\nimport site\n");
            writeAtomic(stage_ + "/manifest.json", QJsonDocument(manifest_.json).toJson());
            emit checkingEngine(true);
            emit progress(done_, manifest_.downloadBytes, "Checking the installed AI engine");
            health_.start(stage_ + "/" + manifest_.json["python"].toString(), stage_ + "/model",
                          healthProfile_);
        } catch (const std::exception &e) {
            fail(QString::fromUtf8(e.what()));
        }
        return;
    }
    retry_ = 0;
    corruptRetries_ = 0;
    const auto f = files_[index_];
    QString object = objectPath(f);
    if (QFileInfo(object).size() == f["bytes"].toInteger()) {
        verifyCurrent();
        return;
    }
    if (QFileInfo::exists(object) && !QFile::remove(object)) {
        fail("Cannot replace a damaged cached AI component.");
        return;
    }
    if (!offline_.isEmpty()) {
        QString source = offline_ + "/objects/" + f["sha256"].toString();
        offlineSource_.setFileName(source);
        part_.setFileName(object + ".part");
        if (!offlineSource_.open(QIODevice::ReadOnly) || !part_.open(QIODevice::ReadWrite)) {
            fail("Offline pack is missing " + f["path"].toString());
            return;
        }
        qint64 resume = part_.size();
        if (resume > f["bytes"].toInteger()) {
            part_.resize(0);
            resume = 0;
        }
        part_.seek(resume);
        offlineSource_.seek(resume);
        QTimer::singleShot(0, this, &AiInstaller::copyOfflineChunk);
        return;
    }
    request();
}
void AiInstaller::copyOfflineChunk() {
    if (!busy_)
        return;
    if (cancelled_) {
        fail("Copy cancelled; verified components are retained.");
        return;
    }
    QByteArray bytes = offlineSource_.read(4 * 1024 * 1024);
    if (bytes.isEmpty()) {
        offlineSource_.close();
        part_.close();
        verifyCurrent();
        return;
    }
    if (part_.size() + bytes.size() > files_[index_]["bytes"].toInteger() ||
        part_.write(bytes) != bytes.size()) {
        fail("Cannot copy AI components; check available disk space.");
        return;
    }
    emit progress(done_ + part_.size(), manifest_.downloadBytes, "Copying " + componentName(files_[index_]));
    QTimer::singleShot(0, this, &AiInstaller::copyOfflineChunk);
}
void AiInstaller::request() {
    if (!busy_ || cancelled_)
        return;
    const auto f = files_[index_];
    QString path = objectPath(f) + ".part";
    part_.setFileName(path);
    if (!part_.open(QIODevice::ReadWrite)) {
        fail(part_.errorString());
        return;
    }
    offset_ = part_.size();
    if (offset_ > f["bytes"].toInteger()) {
        part_.resize(0);
        offset_ = 0;
    }
    part_.seek(offset_);
    if (offset_ == f["bytes"].toInteger()) {
        part_.close();
        verifyCurrent();
        return;
    }
    QNetworkRequest request(QUrl(f["url"].toString()));
    request.setTransferTimeout(60000);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setRawHeader("User-Agent", "Fornaio/" FORNAIO_VERSION);
    request.setRawHeader("Accept-Encoding", "identity");
    // Gated Hugging Face models (Fibo) need the user's token; it is sent to huggingface.co only.
    if (request.url().host() == "huggingface.co") {
        const QString token = huggingFaceToken();
        if (!token.isEmpty())
            request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    }
    if (offset_)
        request.setRawHeader("Range", "bytes=" + QByteArray::number(offset_) + "-");
    lastReceived_ = 0;
    reply_ = network_.get(request);
    auto reply = reply_;
    connect(reply, &QNetworkReply::metaDataChanged, this, [this, reply] {
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 200 && offset_) {
            part_.resize(0);
            part_.seek(0);
            offset_ = 0;
        }
        if (status == 206 &&
            !reply->rawHeader("Content-Range").startsWith("bytes " + QByteArray::number(offset_) + "-")) {
            reply->abort();
            fail("Download server returned an invalid resume range");
        }
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply, f] {
        QByteArray data = reply->readAll();
        if (!busy_ || !part_.isOpen())
            return;
        if (part_.size() + data.size() > f["bytes"].toInteger() || part_.write(data) != data.size()) {
            reply->abort();
            fail("AI download exceeds its expected size or the disk is full.");
        }
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, f](qint64 bytes, qint64) {
        if (!busy_)
            return;
        qint64 received = done_ + offset_ + std::max(qint64(0), bytes);
        transferred_ += std::max(qint64(0), bytes - lastReceived_);
        lastReceived_ = bytes;
        double seconds = std::max(.1, elapsed_.elapsed() / 1000.0),
               speed = std::max(1.0, transferred_ / seconds);
        emit progress(received, manifest_.downloadBytes,
                      QString("%1 / %2 GB • %3 MB/s • ~%4 min remaining\n%5")
                          .arg(received / 1e9, 0, 'f', 2)
                          .arg(manifest_.downloadBytes / 1e9, 0, 'f', 2)
                          .arg(speed / 1e6, 0, 'f', 1)
                          .arg((manifest_.downloadBytes - received) / speed / 60, 0, 'f', 0)
                          .arg(componentName(f)));
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        if (reply_ == reply)
            reply_ = nullptr;
        part_.close();
        auto code = reply->error();
        QString message = reply->errorString();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool huggingFace = reply->request().url().host() == "huggingface.co";
        reply->deleteLater();
        if (!busy_)
            return;
        if (cancelled_) {
            fail("Download cancelled; partial files are retained for resume.");
            return;
        }
        if (huggingFace && (status == 401 || status == 403)) {
            fail(QString("Hugging Face refused the model download (HTTP %1). Accept the model license on "
                         "huggingface.co/%2 with your account, then enter a Hugging Face access token or log in "
                         "with the Hugging Face CLI.")
                     .arg(status)
                     .arg(manifest_.json["model"].toString()));
            return;
        }
        if (code != QNetworkReply::NoError) {
            if (++retry_ <= 3) {
                retryTimer_.start(1000 * (1 << retry_));
                return;
            }
            fail("Download failed; retry to resume. " + message);
            return;
        }
        verifyCurrent();
    });
}
QString AiInstaller::huggingFaceToken() const {
    if (!hfToken_.isEmpty())
        return hfToken_;
    const QString environment = qEnvironmentVariable("HF_TOKEN").trimmed();
    if (!environment.isEmpty())
        return environment;
    // The token file written by `hf auth login` / `huggingface-cli login`.
    const QString home = qEnvironmentVariable("HF_HOME", QDir::homePath() + "/.cache/huggingface");
    QFile file(home + "/token");
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()).trimmed() : QString();
}
void AiInstaller::setGpuAvailable(bool available) {
    gpuAvailable_ = available;
    if (available && waitingForGpu_ && busy_)
        QTimer::singleShot(0, this, &AiInstaller::next);
}
void AiInstaller::verifyCurrent() {
    QString object = objectPath(files_[index_]);
    verify_.setFileName(QFileInfo::exists(object) ? object : object + ".part");
    if (!verify_.open(QIODevice::ReadOnly)) {
        fail("Cannot read downloaded component");
        return;
    }
    hash_ = std::make_unique<QCryptographicHash>(QCryptographicHash::Sha256);
    verifying_ = true;
    emit progress(done_, manifest_.downloadBytes, "Verifying " + componentName(files_[index_]));
    QTimer::singleShot(0, this, &AiInstaller::verifyChunk);
}
void AiInstaller::verifyChunk() {
    if (!busy_)
        return;
    if (cancelled_) {
        fail("Installation cancelled; downloads are retained.");
        return;
    }
    QByteArray chunk = verify_.read(4 * 1024 * 1024);
    if (!chunk.isEmpty()) {
        hash_->addData(chunk);
        QTimer::singleShot(0, this, &AiInstaller::verifyChunk);
        return;
    }
    qint64 size = verify_.size();
    QString path = verify_.fileName();
    verify_.close();
    verifying_ = false;
    const auto f = files_[index_];
    bool valid =
        size == f["bytes"].toInteger() && hash_->result().toHex() == f["sha256"].toString().toLatin1();
    hash_.reset();
    if (!valid) {
        QFile::remove(path);
        if (offline_.isEmpty() && corruptRetries_++ < 1) {
            emit progress(done_, manifest_.downloadBytes, "Repairing a damaged AI component");
            request();
            return;
        }
        fail("Checksum mismatch: " + f["path"].toString() + ". Retry to download this component again.");
        return;
    }
    QString object = objectPath(f);
    if (path != object && !QFile::rename(path, object)) {
        fail("Cannot finalize downloaded component");
        return;
    }
    materialize();
}
void AiInstaller::materialize() {
    const auto f = files_[index_];
    QString object = objectPath(f), extract = f["extract"].toString();
    if (!extract.isEmpty()) {
        QString destination = stage_ + "/" + extract;
        QDir().mkpath(destination);
        extract_.start("powershell.exe",
                       {"-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File",
                        aiAssetsPath("ai_extract.ps1"), "-Archive", object, "-Destination", destination});
        return;
    }
    QString target = stage_ + "/" + f["path"].toString();
    QDir().mkpath(QFileInfo(target).absolutePath());
    bool linked = false;
#ifdef Q_OS_WIN
    linked = CreateHardLinkW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(target).utf16()),
                             reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(object).utf16()), nullptr);
#endif
    if (!linked && !QFile::copy(object, target)) {
        fail("Cannot install model component");
        return;
    }
    done_ += f["bytes"].toInteger();
    ++index_;
    next();
}
void AiInstaller::complete() {
    try {
        writeAtomic(stage_ + "/ready.json",
                    QJsonDocument(QJsonObject{{"id", manifest_.id}, {"protocol", 1}}).toJson());
        QDir().mkpath(root_ + "/engines");
        QString final = root_ + "/engines/" + manifest_.id + "-" +
                        QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
        if (!QDir().rename(stage_, final))
            error("Cannot activate AI engine");
        stage_.clear();
        writeAtomic(root_ + "/active.json",
                    QJsonDocument(QJsonObject{{"id", QFileInfo(final).fileName()}}).toJson());
        busy_ = false;
        lock_.reset();
        emit checkingEngine(false);
        emit finished(true, "AI Texturing is ready. Generation is now available offline.");
    } catch (const std::exception &e) {
        fail(QString::fromUtf8(e.what()));
    }
}
void AiInstaller::fail(const QString &message) {
    if (!busy_)
        return;
    busy_ = false;
    lock_.reset();
    retryTimer_.stop();
    if (reply_) {
        auto r = reply_;
        reply_ = nullptr;
        r->abort();
    }
    part_.close();
    verify_.close();
    offlineSource_.close();
    hash_.reset();
    extract_.kill();
    health_.stop();
    if (!stage_.isEmpty() && extract_.state() == QProcess::NotRunning) {
        removeChild(root_, stage_);
        stage_.clear();
    }
    emit checkingEngine(false);
    emit finished(false, message);
}
void AiInstaller::cancel() {
    if (!busy_)
        return;
    cancelled_ = true;
    retryTimer_.stop();
    if (reply_)
        reply_->abort();
    else if (extract_.state() != QProcess::NotRunning)
        extract_.kill();
    else
        fail("Installation cancelled; downloads are retained for resume.");
}
void AiInstaller::remove() {
    if (busy_)
        return;
    if (root_.isEmpty())
        return;
    QLockFile guard(root_ + "/install.lock");
    if (!guard.tryLock(0))
        error("Another AI setup is using this storage location.");
    // Delete only Fornaio-owned subdirectories after resolving them inside the chosen root.
    for (const QString &name : {QString("objects"), QString("engines")}) {
        QString path = root_ + "/" + name;
        if (QFileInfo::exists(path) && !removeChild(root_, path))
            error("Cannot remove AI components: " + path);
    }
    for (const auto &entry : QDir(root_).entryInfoList({"staging-*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (QRegularExpression("^staging-[a-f0-9-]{36}$").match(entry.fileName()).hasMatch() &&
            !removeChild(root_, entry.absoluteFilePath()))
            error("Cannot remove an incomplete AI installation");
    }
    QFile::remove(root_ + "/active.json");
}
