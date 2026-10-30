#include "ai_cloud.h"
#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

namespace {
struct Environment {
    QMap<QByteArray, QByteArray> saved;
    QSet<QByteArray> present;
    void set(const QByteArray &name, const QByteArray &value) {
        if (!saved.contains(name)) {
            saved[name] = qgetenv(name.constData());
            if (qEnvironmentVariableIsSet(name.constData()))
                present.insert(name);
        }
        qputenv(name.constData(), value);
    }
    ~Environment() {
        for (auto it = saved.cbegin(); it != saved.cend(); ++it)
            if (present.contains(it.key()))
                qputenv(it.key().constData(), it.value());
            else
                qunsetenv(it.key().constData());
    }
};

QByteArray generatedPng() {
    QImage image(48, 48, QImage::Format_RGBA8888);
    image.fill(QColor(20, 180, 70));
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}
bool writeFile(const QString &path, const QByteArray &bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
}
QByteArray readFile(const QString &path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

// The cloud adapter launches this same test executable as its local Codex CLI.
// No model or network is involved: the fixture exposes a partial PNG, completes
// it later, then keeps the CLI alive to simulate a delayed final text response.
int runCliFixture() {
    const QString output = qEnvironmentVariable("FORNAIO_CLOUD_STUB_OUTPUT");
    if (output.isEmpty())
        return 2;
    const QByteArray bytes = generatedPng();
    if (!writeFile(output, bytes.left(bytes.size() / 2)) || !writeFile(output + ".partial", "ready"))
        return 3;
    if (qEnvironmentVariable("FORNAIO_CLOUD_STUB_MODE") == "partial_only")
        return 0;
    QThread::msleep(2500);
    if (!writeFile(output, bytes) || !writeFile(output + ".complete", "ready"))
        return 4;
    QThread::msleep(5000);
    return writeFile(output + ".child_finished", "finished") ? 0 : 5;
}

QJsonObject eventOfType(const QSignalSpy &events, const QString &type) {
    for (const auto &arguments : events) {
        const auto event = arguments.first().value<QJsonObject>();
        if (event["event"] == type)
            return event;
    }
    return {};
}
int terminalEvents(const QSignalSpy &events) {
    int count = 0;
    for (const auto &arguments : events) {
        const QString type = arguments.first().value<QJsonObject>()["event"].toString();
        count += type == "result" || type == "error";
    }
    return count;
}
void configureStub(Environment &environment, const QString &output) {
    environment.set("FORNAIO_GPT_BACKEND", "codex");
    environment.set("FORNAIO_CODEX", QCoreApplication::applicationFilePath().toUtf8());
    environment.set("FORNAIO_CLOUD_STUB", "1");
    environment.set("FORNAIO_CLOUD_STUB_OUTPUT", output.toUtf8());
    environment.set("FORNAIO_CLOUD_STUB_MODE", "");
}
QJsonObject request(const QString &id, const QString &output, const QString &workDir) {
    return {{"id", id},
            {"kind", "uv_repair"},
            {"images", QJsonArray{}},
            {"prompt", "Local test fixture; no generation"},
            {"output", output},
            {"workDir", workDir}};
}
} // namespace

class CloudImageTests : public QObject {
    Q_OBJECT
  private slots:
    void completeImageReturnsBeforeCliTailAndRemainsFrozen() {
        Environment environment;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString output = directory.filePath("candidate.png");
        configureStub(environment, output);
        AiCloudImage cloud;
        QCOMPARE(cloud.backend(), AiCloudImage::Backend::Codex);
        QSignalSpy events(&cloud, &AiCloudImage::event);
        cloud.generate(request("ready", output, directory.path()));
        QVERIFY(cloud.busy());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(output + ".partial"), 3000);
        QTest::qWait(1000);
        QVERIFY(cloud.busy());
        QCOMPARE(terminalEvents(events), 0); // A stable but truncated PNG is never consumed.
        QTRY_VERIFY_WITH_TIMEOUT(!eventOfType(events, "result").isEmpty(), 6000);
        const auto result = eventOfType(events, "result");
        QCOMPARE(result["id"].toString(), QString("ready"));
        QCOMPARE(terminalEvents(events), 1);
        QVERIFY(eventOfType(events, "error").isEmpty());
        QVERIFY(!QFileInfo::exists(output + ".child_finished"));
        QVERIFY(!cloud.busy());
        const QString frozen = result["output"].toString();
        QVERIFY(frozen != output && frozen.endsWith(".ready.png"));
        QCOMPARE(readFile(frozen), generatedPng());
        QCOMPARE(QImage(frozen).pixelColor(0, 0), QColor(20, 180, 70));
        QVERIFY(writeFile(output, "a later writer replaced the original file"));
        QCOMPARE(readFile(frozen), generatedPng());
        const auto timings = result["phaseTimings"].toObject();
        QVERIFY(timings.contains("provider") && timings.contains("validating_output"));
        QVERIFY(timings["provider"].toDouble() > 1);
        for (const auto &arguments : events) {
            const auto event = arguments.first().value<QJsonObject>();
            if (event["event"] != "progress")
                continue;
            QVERIFY(!event["indeterminate"].toBool());
            QVERIFY(event["fraction"].toDouble(-1) >= 0 && event["fraction"].toDouble() <= .95);
            QVERIFY(event["elapsedSeconds"].toDouble(-1) >= 0);
            QVERIFY(event["phaseSeconds"].toDouble(-1) >= 0);
            QVERIFY(!event["phase"].toString().isEmpty());
        }
        const auto process = cloud.findChild<QProcess *>();
        QVERIFY(process);
        QTRY_COMPARE_WITH_TIMEOUT(process->state(), QProcess::NotRunning, 2000);
    }

    void cancellationStopsPartialProducerAndRetryUsesNewRequest() {
        Environment environment;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString cancelled = directory.filePath("cancelled.png");
        configureStub(environment, cancelled);
        AiCloudImage cloud;
        QSignalSpy events(&cloud, &AiCloudImage::event);
        cloud.generate(request("cancelled", cancelled, directory.path()));
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(cancelled + ".partial"), 3000);
        cloud.cancel();
        QVERIFY(!cloud.busy());
        const auto process = cloud.findChild<QProcess *>();
        QVERIFY(process);
        QTRY_COMPARE_WITH_TIMEOUT(process->state(), QProcess::NotRunning, 2000);
        QCOMPARE(terminalEvents(events), 0);
        QVERIFY(!QFileInfo::exists(directory.filePath("cancelled.ready.png")));
        const QString retry = directory.filePath("retry.png");
        environment.set("FORNAIO_CLOUD_STUB_OUTPUT", retry.toUtf8());
        cloud.generate(request("retry", retry, directory.path()));
        QTRY_VERIFY_WITH_TIMEOUT(!eventOfType(events, "result").isEmpty(), 7000);
        QCOMPARE(eventOfType(events, "result")["id"].toString(), QString("retry"));
        QCOMPARE(terminalEvents(events), 1);
        QVERIFY(!QFileInfo::exists(cancelled + ".complete"));
        QVERIFY(!QFileInfo::exists(cancelled + ".child_finished"));
    }

    void cliExitWithPartialPngReportsError() {
        Environment environment;
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString output = directory.filePath("partial.png");
        configureStub(environment, output);
        environment.set("FORNAIO_CLOUD_STUB_MODE", "partial_only");
        AiCloudImage cloud;
        QSignalSpy events(&cloud, &AiCloudImage::event);
        cloud.generate(request("partial", output, directory.path()));
        QTRY_VERIFY_WITH_TIMEOUT(!eventOfType(events, "error").isEmpty(), 4000);
        QCOMPARE(terminalEvents(events), 1);
        QVERIFY(eventOfType(events, "result").isEmpty());
        QVERIFY(eventOfType(events, "error")["message"].toString().contains("incomplete"));
        QVERIFY(!QFileInfo::exists(directory.filePath("partial.ready.png")));
        QVERIFY(!cloud.busy());
    }
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariable("FORNAIO_CLOUD_STUB") == "1" && argc > 1 && QByteArray(argv[1]) == "exec")
        return runCliFixture();
    CloudImageTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "cloud_image_tests.moc"
