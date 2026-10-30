#include "ai_cloud.h"
#include "ai_smart_fill.h"
#include <QBuffer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTest>

namespace {
Scene mesh(bool inset = false) {
    Scene s;
    s.uvNames = {"UV1"};
    s.materials = {"skin"};
    s.objects = {"head"};
    s.scale = 2;
    const V3 p[] = {{-.8f, -.8f, 0}, {.8f, -.8f, 0}, {.8f, .8f, 0}, {-.8f, .8f, 0}};
    const float lo = inset ? .125f : 0, hi = inset ? .875f : 1;
    const V2 uv[] = {{lo, lo}, {hi, lo}, {hi, hi}, {lo, hi}};
    for (auto indices : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}}) {
        Triangle t;
        t.material = 0;
        t.object = 0;
        std::array<V2, 3> tex;
        for (int k = 0; k < 3; ++k) {
            t.p[k] = p[indices[k]];
            t.n[k] = {0, 0, 1};
            tex[k] = uv[indices[k]];
        }
        t.uv.push_back(tex);
        s.tris.push_back(t);
    }
    return s;
}
QImage image(int size, QColor color) {
    QImage i(size, size, QImage::Format_RGBA8888);
    i.fill(color);
    return i;
}
QString encoded(const QImage &im) {
    QByteArray bytes;
    QBuffer b(&bytes);
    b.open(QIODevice::WriteOnly);
    im.save(&b, "PNG");
    return QString::fromLatin1(bytes.toBase64());
}
void paint(AiAtlases &atlases, const AiUvDomains &domains) {
    for (auto it = atlases.begin(); it != atlases.end(); ++it) {
        auto &a = it.value();
        const int n = a.color.width();
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                a.color.setPixel(x, y, qRgb(80 + x % 97, 40 + y % 101, 30 + (x + y) % 71));
                const bool inside = domains[it.key()].occupancy.constScanLine(y)[x] != 0;
                a.coverage.scanLine(y)[x] = inside ? 255 : 0;
                reinterpret_cast<quint16 *>(a.confidence.scanLine(y))[x] = inside ? 65535 : 0;
            }
    }
}
void hole(AiAtlas &a, const QRect &r) {
    for (int y = r.top(); y <= r.bottom(); ++y)
        for (int x = r.left(); x <= r.right(); ++x) {
            a.coverage.scanLine(y)[x] = 0;
            reinterpret_cast<quint16 *>(a.confidence.scanLine(y))[x] = 0;
            a.color.setPixel(x, y, qRgb(128, 128, 128));
        }
}
QJsonObject complete(QJsonObject plan) {
    const auto malicious = image(1024, QColor(0, 240, 190));
    for (int i = 0; i < plan["patches"].toArray().size(); ++i)
        if (!plan["patches"].toArray()[i].toObject()["done"].toBool())
            plan = aiSmartFillAcceptPatch(plan, i, malicious);
    return plan;
}
qint64 changedKnown(const AiAtlas &a, const AiAtlas &b) {
    qint64 n = 0;
    for (int y = 0; y < a.color.height(); ++y)
        for (int x = 0; x < a.color.width(); ++x)
            if (a.coverage.constScanLine(y)[x] && a.color.pixel(x, y) != b.color.pixel(x, y))
                ++n;
    return n;
}
qint64 changedKnownQuality(const AiAtlas &a, const AiAtlas &b) {
    qint64 n = 0;
    for (int y = 0; y < a.color.height(); ++y) {
        const auto *before = reinterpret_cast<const quint16 *>(a.confidence.constScanLine(y));
        const auto *after = reinterpret_cast<const quint16 *>(b.confidence.constScanLine(y));
        for (int x = 0; x < a.color.width(); ++x)
            if (a.coverage.constScanLine(y)[x] && before[x] != after[x])
                ++n;
    }
    return n;
}
QJsonObject planned(const Scene &s, const AiAtlases &source, const AiUvDomains &domains) {
    return aiPlanSmartFill(s, 0, source, domains, {"skin"}, aiSmartFillFingerprint(s, 0, source, {}, true));
}
QRect mapping(const QJsonValue &value) {
    const auto rect = value.toArray();
    return {rect[0].toInt(), rect[1].toInt(), rect[2].toInt(), rect[3].toInt()};
}
QPoint mappedPixel(const QRect &tile, int x, int y, int nativeSize) {
    return {tile.x() + x * tile.width() / nativeSize, tile.y() + y * tile.height() / nativeSize};
}
QJsonArray rectJson(const QRect &r) {
    return {r.x(), r.y(), r.width(), r.height()};
}
} // namespace

class SmartFillTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { aiInitializeProjectionRenderer(); }
    void cleanupTestCase() { aiShutdownProjectionRenderer(); }
    void cancelledFakeCallbackCannotFinishRetry() {
        struct RestoreBackend {
            bool wasSet = qEnvironmentVariableIsSet("FORNAIO_GPT_BACKEND");
            QByteArray value = qgetenv("FORNAIO_GPT_BACKEND");
            ~RestoreBackend() {
                if (wasSet)
                    qputenv("FORNAIO_GPT_BACKEND", value);
                else
                    qunsetenv("FORNAIO_GPT_BACKEND");
            }
        } restore;
        qputenv("FORNAIO_GPT_BACKEND", "fake");
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString originalInput = directory.filePath("cancelled-input.png");
        const QString originalOutput = directory.filePath("cancelled-output.png");
        const QString retryInput = directory.filePath("retry-input.png");
        const QString retryOutput = directory.filePath("retry-output.png");
        QVERIFY(image(32, Qt::red).save(originalInput));
        AiCloudImage cloud;
        QCOMPARE(cloud.backend(), AiCloudImage::Backend::Fake);
        QSignalSpy events(&cloud, &AiCloudImage::event);
        cloud.generate({{"id", "cancelled"},
                        {"images", QJsonArray{originalInput}},
                        {"output", originalOutput},
                        {"workDir", directory.path()}});
        events.clear(); // Provider progress is emitted immediately; only completion is checked below.
        QVERIFY(cloud.busy());
        // Let the first fake timer expire without processing events. Its callback will be
        // ready when we resume the event loop, after cancellation and immediate retry.
        QTest::qSleep(60);
        cloud.cancel();
        QVERIFY(!cloud.busy());
        QCOMPARE(events.size(), 0);
        cloud.generate({{"id", "retry"},
                        {"images", QJsonArray{retryInput}},
                        {"output", retryOutput},
                        {"workDir", directory.path()}});
        events.clear();
        QVERIFY(cloud.busy());
        // Dispatch the already-due old callback now, before the retry's new 50 ms timer.
        // Create its input synchronously afterward, avoiding timer delivery-order assumptions.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
        QVERIFY(cloud.busy());
        QCOMPARE(events.size(), 0);
        QVERIFY(!QFileInfo::exists(retryOutput));
        QVERIFY(image(32, Qt::green).save(retryInput));
        QTRY_COMPARE_WITH_TIMEOUT(events.size(), 1, 2000);
        const QJsonObject result = events.front().front().value<QJsonObject>();
        QCOMPARE(result["event"].toString(), QString("result"));
        QCOMPARE(result["id"].toString(), QString("retry"));
        QCOMPARE(result["output"].toString(), retryOutput);
        const QImage output(retryOutput);
        QVERIFY(!output.isNull());
        QCOMPARE(output.pixelColor(0, 0), QColor(Qt::green));
        QVERIFY(!cloud.busy());
        QVERIFY(!QFileInfo::exists(originalOutput));
    }
    void fakeProviderProgressIsIndeterminate() {
        const bool wasSet = qEnvironmentVariableIsSet("FORNAIO_GPT_BACKEND");
        const QByteArray old = qgetenv("FORNAIO_GPT_BACKEND");
        qputenv("FORNAIO_GPT_BACKEND", "fake");
        struct Restore {
            bool set;
            QByteArray value;
            ~Restore() {
                if (set)
                    qputenv("FORNAIO_GPT_BACKEND", value);
                else
                    qunsetenv("FORNAIO_GPT_BACKEND");
            }
        } restore{wasSet, old};
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString input = directory.filePath("input.png"), output = directory.filePath("output.png");
        QVERIFY(image(32, Qt::green).save(input));
        AiCloudImage cloud;
        QSignalSpy events(&cloud, &AiCloudImage::event);
        cloud.generate({{"id", "progress"},
                        {"images", QJsonArray{input}},
                        {"output", output},
                        {"workDir", directory.path()}});
        QVERIFY(!events.isEmpty());
        const auto progress = events.first().first().value<QJsonObject>();
        QCOMPARE(progress["event"].toString(), QString("progress"));
        QVERIFY(progress["indeterminate"].toBool());
        QVERIFY(progress["fraction"].toDouble() < 0);
        QVERIFY(progress.contains("elapsedSeconds"));
        QVERIFY(progress.contains("phaseSeconds"));
        QTRY_VERIFY_WITH_TIMEOUT(!cloud.busy(), 2000);
        const auto result = events.last().first().value<QJsonObject>();
        QCOMPARE(result["event"].toString(), QString("result"));
        QVERIFY(result["phaseTimings"].isObject());
        QCOMPARE(QImage(output).pixelColor(0, 0), QColor(Qt::green));
    }
    void gradientDomainRemovesCastAndIgnoresRedrawnKnownPixels() {
        constexpr int n = 192;
        const auto scene = mesh();
        const auto domains = aiUvDomains(scene, 0, n, {"skin"});
        auto source = aiEmptyAtlases(scene, n);
        const QColor accepted(178, 112, 84);
        source["skin"].color.fill(accepted);
        source["skin"].coverage.fill(255);
        source["skin"].confidence.fill(60000);
        const QRect gap(24, 24, 144, 144);
        hole(source["skin"], gap);
        const auto plan = planned(scene, source, domains);
        QImage generated = image(n, QColor(20, 220, 190));
        for (int y = gap.top(); y <= gap.bottom(); ++y)
            for (int x = gap.left(); x <= gap.right(); ++x)
                generated.setPixelColor(x, y, QColor(250, 40, 20));
        const auto completed = aiSmartFillAcceptPatch(plan, 0, generated);
        QCOMPARE(completed["matchingVersion"].toInt(), 2);
        const auto preview = aiSmartFillPreview(source, completed);
        QCOMPARE(changedKnown(source["skin"], preview["skin"]), 0);
        QCOMPARE(changedKnownQuality(source["skin"], preview["skin"]), 0);
        for (int y = gap.top(); y <= gap.bottom(); ++y)
            for (int x = gap.left(); x <= gap.right(); ++x) {
                const auto color = preview["skin"].color.pixelColor(x, y);
                QVERIFY(std::abs(color.red() - accepted.red()) <= 1);
                QVERIFY(std::abs(color.green() - accepted.green()) <= 1);
                QVERIFY(std::abs(color.blue() - accepted.blue()) <= 1);
            }
        // The previous 32-pixel fade reintroduced a strong cast here and at the center.
        for (int x : {gap.left(), gap.left() + 31, gap.left() + 32, gap.left() + 33, gap.center().x()})
            QCOMPARE(preview["skin"].color.pixelColor(x, gap.center().y()), accepted);
    }
    void gradientDomainMatchesSpatialDriftAndPreservesInteriorTexture() {
        constexpr int n = 160;
        const auto scene = mesh();
        const auto domains = aiUvDomains(scene, 0, n, {"skin"});
        auto source = aiEmptyAtlases(scene, n);
        source["skin"].coverage.fill(255);
        source["skin"].confidence.fill(55000);
        const QRect gap(20, 16, 120, 128);
        QImage expected = image(n, Qt::black), generated = image(n, Qt::magenta);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const int r = 90 + x / 4, g = 80 + y / 4, b = 60 + (x + y) / 8;
                source["skin"].color.setPixel(x, y, qRgb(r, g, b));
                const bool interior = gap.adjusted(8, 8, -8, -8).contains(x, y);
                const int detail = interior ? ((x / 2 + y / 2) % 2 ? 6 : -6) : 0;
                expected.setPixelColor(x, y, QColor(r + detail, g + detail, b + detail));
                if (gap.contains(x, y))
                    generated.setPixelColor(
                        x, y,
                        QColor(r + detail + 35 + x / 8, g + detail - 25 + y / 8, b + detail + 45 - x / 8));
            }
        hole(source["skin"], gap);
        const auto plan = planned(scene, source, domains);
        const auto completed = aiSmartFillAcceptPatch(plan, 0, generated);
        const auto preview = aiSmartFillPreview(source, completed);
        int maximum = 0;
        for (int y = gap.top(); y <= gap.bottom(); ++y)
            for (int x = gap.left(); x <= gap.right(); ++x) {
                const auto actual = preview["skin"].color.pixelColor(x, y),
                           wanted = expected.pixelColor(x, y);
                maximum = std::max({maximum, std::abs(actual.red() - wanted.red()),
                                    std::abs(actual.green() - wanted.green()),
                                    std::abs(actual.blue() - wanted.blue())});
            }
        QVERIFY2(maximum <= 3, qPrintable(QString("Maximum harmonic reconstruction error: %1").arg(maximum)));
        QCOMPARE(changedKnown(source["skin"], preview["skin"]), 0);
        const int contrast =
            preview["skin"].color.pixelColor(80, 80).red() - preview["skin"].color.pixelColor(82, 80).red();
        QVERIFY(std::abs(contrast) >= 9); // Candidate grain survives; a flat diffusion fill would fail.
        auto retained = plan;
        auto patches = retained["patches"].toArray();
        auto patch = patches.first().toObject();
        patch["generated"] = encoded(generated);
        patches[0] = patch;
        retained["patches"] = patches;
        const auto reprocessed = aiSmartFillReprocess(retained);
        QCOMPARE(aiSmartFillPreview(source, reprocessed)["skin"].color, preview["skin"].color);
        auto oldMatched = completed;
        oldMatched["matchingVersion"] = 1;
        oldMatched["repairs"] = QJsonObject{{"skin", encoded(image(n, Qt::green))}};
        const auto upgraded = aiSmartFillReprocess(oldMatched);
        QCOMPARE(upgraded["matchingVersion"].toInt(), 2);
        QCOMPARE(aiSmartFillPreview(source, upgraded)["skin"].color, preview["skin"].color);
        QCOMPARE(oldMatched["matchingVersion"].toInt(), 1);
        QCOMPARE(retained["repairs"].toObject().size(), 0);
        QVERIFY(!retained["patches"].toArray().first().toObject()["done"].toBool());
        std::atomic_bool cancel = true;
        QVERIFY_EXCEPTION_THROWN(aiSmartFillReprocess(retained, &cancel), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(aiSmartFillPreview(source, completed, &cancel), std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(aiSmartFillFingerprint(scene, 0, source, {}, true, &cancel),
                                 std::runtime_error);
        QCOMPARE(retained["repairs"].toObject().size(), 0);
    }
    void gradientDomainHandlesOnePixelUvEdgesAndSeparateCharts() {
        constexpr int n = 64;
        const auto scene = mesh();
        auto domains = aiUvDomains(scene, 0, n, {"skin"});
        auto source = aiEmptyAtlases(scene, n);
        source["skin"].coverage.fill(255);
        source["skin"].confidence.fill(50000);
        const QColor left(170, 80, 40), right(30, 70, 190);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                domains["skin"].charts[y * n + x] = x < n / 2 ? 0 : 1;
                source["skin"].color.setPixelColor(x, y, x < n / 2 ? left : right);
            }
        for (const auto &gap : {QRect(0, 0, 1, 1), QRect(31, 20, 1, 1), QRect(32, 20, 1, 1),
                                QRect(0, 30, 8, 12), QRect(56, 45, 8, 12)})
            hole(source["skin"], gap);
        const auto plan = planned(scene, source, domains);
        const auto preview = aiSmartFillPreview(source, aiSmartFillAcceptPatch(plan, 0, image(n, Qt::green)));
        QCOMPARE(changedKnown(source["skin"], preview["skin"]), 0);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (!source["skin"].coverage.constScanLine(y)[x])
                    QCOMPARE(preview["skin"].color.pixelColor(x, y), x < n / 2 ? left : right);
    }
    void gradientDomainMatchingScalesAtNative2048And4096() {
        for (int n : {2048, 4096}) {
            QElapsedTimer elapsed;
            elapsed.start();
            const auto scene = mesh();
            const auto domains = aiUvDomains(scene, 0, n, {"skin"});
            auto source = aiEmptyAtlases(scene, n);
            source["skin"].color.fill(QColor(150, 100, 70));
            source["skin"].coverage.fill(255);
            source["skin"].confidence.fill(50000);
            const QRect gap(n / 2 - 160, n / 2 - 160, 320, 320);
            hole(source["skin"], gap);
            const auto plan = planned(scene, source, domains);
            QImage generated = image(n, QColor(0, 250, 250));
            for (int y = gap.top(); y <= gap.bottom(); ++y)
                for (int x = gap.left(); x <= gap.right(); ++x)
                    generated.setPixelColor(
                        x, y, QColor(175 + (x - gap.left()) / 8, 55 + (y - gap.top()) / 8, 130));
            const auto completed = aiSmartFillAcceptPatch(plan, 0, generated);
            const auto preview = aiSmartFillPreview(source, completed);
            QCOMPARE(changedKnown(source["skin"], preview["skin"]), 0);
            for (const auto &position : {gap.topLeft(), gap.center(), gap.bottomRight()}) {
                const auto color = preview["skin"].color.pixelColor(position);
                QVERIFY(std::abs(color.red() - 150) <= 2);
                QVERIFY(std::abs(color.green() - 100) <= 2);
                QVERIFY(std::abs(color.blue() - 70) <= 1);
            }
            qInfo("Smart fill native %d atlas boundary solve and preview: %lld ms", n, elapsed.elapsed());
            QVERIFY(elapsed.elapsed() < 30000);
        }
    }
    void eligibilityAndExactPreservation() {
        const auto s = mesh(true);
        const auto domains = aiUvDomains(s, 0, 64, {"skin"});
        QCOMPARE(domains["skin"].occupancy.size(), QSize(64, 64));
        QCOMPARE(domains["skin"].charts.size(), 4096);
        QVERIFY(domains["skin"].occupancy.constScanLine(32)[32]);
        QCOMPARE(domains["skin"].charts[0], -1);
        auto source = aiEmptyAtlases(s, 64);
        paint(source, domains);
        hole(source["skin"], QRect(31, 31, 1, 1));
        hole(source["skin"], QRect(8, 18, 4, 8));
        const auto plan = planned(s, source, domains);
        QCOMPARE(plan["version"].toInt(), 2);
        QCOMPARE(aiSmartFillRemaining(plan), 1);
        const auto preview = aiSmartFillPreview(source, complete(plan));
        QCOMPARE(changedKnown(source["skin"], preview["skin"]), 0);
        QVERIFY(preview["skin"].coverage.constScanLine(31)[31]);
        QCOMPARE(preview["skin"].coverage.constScanLine(0)[0], uchar(0));
        QCOMPARE(aiSmartFillRemaining(planned(s, preview, domains)), 0);
    }
    void oneRequestCoversFarGapsAndNativeMasks() {
        const auto s = mesh();
        const auto domains = aiUvDomains(s, 0, 2048, {"skin"});
        auto source = aiEmptyAtlases(s, 2048);
        paint(source, domains);
        hole(source["skin"], QRect(890, 400, 20, 20));
        hole(source["skin"], QRect(1800, 1800, 2, 2));
        const auto plan = planned(s, source, domains);
        QCOMPARE(plan["version"].toInt(), 2);
        QCOMPARE(aiSmartFillRemaining(plan), 1);
        QCOMPARE(plan["patches"].toArray().size(), 1);
        QCOMPARE(plan["patches"].toArray().first().toObject()["kind"].toString(), QString("uv_board"));
        const QImage frozenMask = QImage::fromData(
            QByteArray::fromBase64(plan["eligibleMasks"].toObject()["skin"].toString().toLatin1()), "PNG");
        QCOMPARE(frozenMask.size(), QSize(2048, 2048));
        QCOMPARE(qGray(frozenMask.pixel(890, 400)), 255);
        QCOMPARE(qGray(frozenMask.pixel(1800, 1800)), 255);
        QCOMPARE(qGray(frozenMask.pixel(100, 100)), 0);
        QTemporaryDir dir;
        const auto request = aiSmartFillRequest(plan, 0, dir.path(), image(256, Qt::red));
        QCOMPARE(request["purpose"].toString(), QString("uv_repair"));
        QCOMPARE(request["images"].toArray().size(), 3);
        for (const auto &p : request["images"].toArray())
            QVERIFY(QFileInfo::exists(p.toString()));
        QImage mask(request["mask"].toString());
        const int boardSize = plan["boardSize"].toInt();
        QVERIFY(boardSize > 0 && boardSize <= 4096);
        QCOMPARE(mask.size(), QSize(boardSize, boardSize));
        const QRect skin = mapping(plan["layout"].toObject()["skin"]);
        QCOMPARE(skin.size(), QSize(2048, 2048));
        QCOMPARE(mask.pixelColor(skin.x() + 890, skin.y() + 400).alpha(), 0);
        QCOMPARE(mask.pixelColor(skin.x() + 1800, skin.y() + 1800).alpha(), 0);
        QCOMPARE(mask.pixelColor(skin.x() + 100, skin.y() + 100).alpha(), 255);
        bool edit = false, protect = false;
        for (int y = 0; y < mask.height(); ++y)
            for (int x = 0; x < mask.width(); ++x) {
                edit |= mask.pixelColor(x, y).alpha() == 0;
                protect |= mask.pixelColor(x, y).alpha() == 255;
            }
        QVERIFY(edit && protect);
        auto completed = aiSmartFillAcceptPatch(plan, 0, image(1024, Qt::green));
        QCOMPARE(aiSmartFillRemaining(completed), 0);
        auto restored = QJsonDocument::fromJson(QJsonDocument(completed).toJson()).object();
        QCOMPARE(aiSmartFillRemaining(restored), 0);
        QCOMPARE(restored["eligibleMasks"], plan["eligibleMasks"]);
        QCOMPARE(restored["layout"], plan["layout"]);
        QVERIFY_EXCEPTION_THROWN(aiSmartFillSetMasks(completed, {{"skin", image(2048, Qt::white)}}),
                                 std::runtime_error);
        const auto excluded = aiSmartFillSetMasks(plan, {{"skin", image(2048, Qt::black)}});
        QCOMPARE(aiSmartFillRemaining(excluded), 0);
        QCOMPARE(aiSmartFillRemaining(aiSmartFillSetMasks(excluded, {{"skin", image(2048, Qt::white)}})), 1);
        const auto preview = aiSmartFillPreview(source, complete(restored));
        QCOMPARE(changedKnown(source["skin"], preview["skin"]), 0);
        for (int y = 400; y < 420; ++y)
            for (int x = 890; x < 910; ++x)
                QVERIFY(preview["skin"].coverage.constScanLine(y)[x]);
        QVERIFY(preview["skin"].coverage.constScanLine(1800)[1800]);
    }
    void legacyCroppedPartialCandidateRemainsReadable() {
        const auto s = mesh();
        const auto domains = aiUvDomains(s, 0, 2048, {"skin"});
        auto source = aiEmptyAtlases(s, 2048);
        paint(source, domains);
        hole(source["skin"], QRect(890, 400, 20, 20));
        hole(source["skin"], QRect(1800, 1800, 2, 2));
        auto legacy = planned(s, source, domains);
        legacy["version"] = 1;
        legacy["requestSize"] = 1024;
        legacy["coreSize"] = 896;
        legacy["contextBorder"] = 64;
        legacy.remove("layout");
        legacy.remove("boardSize");
        const int chart = domains["skin"].charts[400 * 2048 + 890];
        QJsonArray patches;
        for (const auto &core : {QRect(0, 0, 896, 896), QRect(896, 0, 896, 896), QRect(1792, 1792, 256, 256)})
            patches.append(QJsonObject{{"material", "skin"},
                                       {"chart", chart},
                                       {"core", rectJson(core)},
                                       {"crop", rectJson(QRect(core.x() - 64, core.y() - 64, 1024, 1024))},
                                       {"done", false}});
        legacy["patches"] = patches;
        QCOMPARE(aiSmartFillRemaining(legacy), 3);
        auto partial = aiSmartFillAcceptPatch(legacy, 0, image(1024, Qt::green));
        QCOMPARE(aiSmartFillRemaining(partial), 2);
        const auto partialPreview = aiSmartFillPreview(source, partial);
        QCOMPARE(changedKnown(source["skin"], partialPreview["skin"]), 0);
        QVERIFY(partialPreview["skin"].coverage.constScanLine(400)[890]);
        QCOMPARE(partialPreview["skin"].coverage.constScanLine(400)[900], uchar(0));
        QCOMPARE(partialPreview["skin"].coverage.constScanLine(1800)[1800], uchar(0));
        const auto applied = aiComposeLayerStack(source, {aiSmartFillLayer(partial)}, true);
        QCOMPARE(applied["skin"].coverage, partialPreview["skin"].coverage);
        const auto restored = QJsonDocument::fromJson(QJsonDocument(partial).toJson()).object();
        QCOMPARE(aiSmartFillRemaining(restored), 2);
        QVERIFY_EXCEPTION_THROWN(aiSmartFillSetMasks(restored, {{"skin", image(2048, Qt::white)}}),
                                 std::runtime_error);
        QTemporaryDir dir;
        QVERIFY_EXCEPTION_THROWN(aiSmartFillRequest(restored, 1, dir.path(), {}), std::runtime_error);
        const auto finished = aiSmartFillPreview(source, complete(restored));
        QCOMPARE(changedKnown(source["skin"], finished["skin"]), 0);
        QVERIFY(finished["skin"].coverage.constScanLine(400)[900]);
        QVERIFY(finished["skin"].coverage.constScanLine(1800)[1800]);
    }
    void fallbackLayersNeverCompeteWithProjectionPaint() {
        const auto s = mesh();
        const auto domains = aiUvDomains(s, 0, 64, {"skin"});
        auto source = aiEmptyAtlases(s, 64);
        paint(source, domains);
        hole(source["skin"], QRect(30, 30, 4, 4));
        auto repair = aiSmartFillLayer(complete(planned(s, source, domains)));
        QVERIFY(aiIsRepairLayer(repair));
        const auto filled = aiComposeLayerStack(source, {repair}, true);
        QCOMPARE(changedKnown(source["skin"], filled["skin"]), 0);
        QVERIFY(filled["skin"].coverage.constScanLine(32)[32]);
        QVERIFY(filled["skin"].repair.constScanLine(32)[32]);
        const auto quality =
            reinterpret_cast<const quint16 *>(filled["skin"].confidence.constScanLine(32))[32];
        QVERIFY(quality > 0 && quality < 6554);
        auto ordinary = image(64, Qt::transparent);
        ordinary.setPixelColor(32, 32, Qt::blue);
        AiLayerContribution c;
        c.materials["skin"] = ordinary;
        QJsonObject projection{{"visible", true},
                               {"blending", QJsonObject{{"mode", "blend"}, {"strength", 1}}},
                               {"uvLayer", aiEncodeProjectionLayer(c, 64)}};
        const auto latest = aiComposeLayerStack(source, {repair, projection}, true);
        QCOMPARE(latest["skin"].color.pixel(32, 32), qRgb(0, 0, 255));
        QCOMPARE(latest["skin"].repair.constScanLine(32)[32], uchar(0));
        QCOMPARE(aiComposeLayerStack(source, {projection, repair}, true)["skin"].color, latest["skin"].color);
        repair["visible"] = false;
        QCOMPARE(aiComposeLayerStack(source, {repair}, true)["skin"].coverage, source["skin"].coverage);
        repair["visible"] = true;
        repair["uvMasks"] = QJsonObject{{"skin", encoded(image(64, Qt::black))}};
        QCOMPARE(aiComposeLayerStack(source, {repair}, true)["skin"].coverage, source["skin"].coverage);
        QCOMPARE(aiWithoutRepairs(source, {repair}, true)["skin"].color, source["skin"].color);
    }
    void pendingRoundTripFingerprintAndCancellation() {
        const auto s = mesh();
        auto source = aiEmptyAtlases(s, 64);
        const auto domains = aiUvDomains(s, 0, 64, {"skin"});
        paint(source, domains);
        hole(source["skin"], QRect(20, 20, 2, 2));
        const auto fingerprint = aiSmartFillFingerprint(s, 0, source, {}, true);
        const auto plan = complete(planned(s, source, domains));
        const auto repair = aiSmartFillLayer(plan);
        QTemporaryDir dir;
        const auto path = dir.filePath("smart.fornaioai");
        const auto filled = aiComposeLayerStack(source, {repair}, true);
        aiSaveProject(path, {{"projectionBackend", "modddif-v1"}, {"pendingSmartFill", plan}}, filled,
                      {repair});
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object()["version"].toInt(), 3);
        AiAtlases restored;
        QList<QJsonObject> records;
        const auto settings = aiLoadProject(path, restored, records);
        QCOMPARE(restored["skin"].repair, filled["skin"].repair);
        QCOMPARE(aiComposeLayerStack(source, records, true)["skin"].color, filled["skin"].color);
        QCOMPARE(aiSmartFillRemaining(settings["pendingSmartFill"].toObject()), 0);
        QCOMPARE(aiSmartFillFingerprint(s, 0, aiDeserializeAtlases(aiSerializeAtlases(source)), {}, true),
                 fingerprint);
        auto changed = source;
        changed["skin"].color.setPixelColor(0, 0, Qt::blue);
        QVERIFY(aiSmartFillFingerprint(s, 0, changed, {}, true) != fingerprint);
        std::atomic_bool cancel = true;
        QVERIFY_EXCEPTION_THROWN(aiPlanSmartFill(s, 0, source, domains, {"skin"}, fingerprint, &cancel),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(aiSmartFillAcceptPatch(plan, 0, image(1024, Qt::red), &cancel),
                                 std::runtime_error);
    }
    void materialAndChartIsolation() {
        auto s = mesh();
        s.materials << "other";
        auto copy = s.tris;
        for (auto &t : copy) {
            t.material = 1;
            t.object = 1;
            s.tris.push_back(t);
        }
        const auto domains = aiUvDomains(s, 0, 64, {"skin", "other"});
        auto source = aiEmptyAtlases(s, 64);
        paint(source, domains);
        hole(source["skin"], QRect(31, 31, 1, 1));
        hole(source["other"], QRect(31, 31, 1, 1));
        const auto preview = aiSmartFillPreview(source, complete(planned(s, source, domains)));
        QCOMPARE(preview["other"].color, source["other"].color);
        QCOMPARE(preview["other"].coverage, source["other"].coverage);
        auto split = mesh();
        split.tris[1].object = 2;
        const auto charts = aiUvDomains(split, 0, 64, {"skin"});
        const int first = charts["skin"].charts[48 * 64 + 48], second = charts["skin"].charts[16 * 64 + 16];
        QVERIFY(first >= 0 && second >= 0 && first != second);
    }
    void oneGenerationMapsAllSelectedMaterials() {
        auto s = mesh();
        s.materials << "other";
        auto copy = s.tris;
        for (auto &t : copy) {
            t.material = 1;
            t.object = 1;
            s.tris.push_back(t);
        }
        auto domains = aiUvDomains(s, 0, 256, {"skin", "other"});
        auto source = aiEmptyAtlases(s, 256);
        paint(source, domains);
        hole(source["skin"], QRect(80, 80, 96, 96));
        hole(source["other"], QRect(80, 80, 96, 96));
        // The central gaps are independent, completely unpainted charts. Their candidate
        // colors must survive, without anchors borrowed from a different chart.
        for (auto &domain : domains)
            for (int y = 80; y < 176; ++y)
                for (int x = 80; x < 176; ++x)
                    domain.charts[y * 256 + x] = 1;
        // Separate one-pixel gaps exercise all selected materials without adding requests.
        hole(source["skin"], QRect(4, 250, 1, 1));
        hole(source["other"], QRect(250, 4, 1, 1));
        const auto plan = aiPlanSmartFill(s, 0, source, domains, {"skin", "other"},
                                          aiSmartFillFingerprint(s, 0, source, {}, true));
        QCOMPARE(aiSmartFillRemaining(plan), 1);
        QCOMPARE(plan["patches"].toArray().size(), 1);
        QCOMPARE(plan["materials"].toArray().size(), 2);
        const auto layout = plan["layout"].toObject();
        const QRect skin = mapping(layout["skin"]), other = mapping(layout["other"]);
        QVERIFY(!skin.isEmpty() && skin.size() == other.size());
        QVERIFY(!skin.intersects(other));
        const int boardSize = plan["boardSize"].toInt();
        const QRect board(0, 0, boardSize, boardSize);
        QVERIFY(board.contains(skin) && board.contains(other));
        QTemporaryDir directory;
        const auto request = aiSmartFillRequest(plan, 0, directory.path(), image(256, Qt::gray));
        const QImage texture(request["images"].toArray().first().toString());
        const QImage mask(request["mask"].toString());
        QCOMPARE(texture.size(), board.size());
        QCOMPARE(mask.size(), board.size());
        for (const auto &tile : {skin, other}) {
            QCOMPARE(mask.pixelColor(mappedPixel(tile, 128, 128, 256)).alpha(), 0);
            QCOMPARE(mask.pixelColor(mappedPixel(tile, 32, 32, 256)).alpha(), 255);
        }
        const QColor skinRepair(220, 20, 10), otherRepair(10, 30, 230);
        QImage generated(board.size(), QImage::Format_RGBA8888);
        generated.fill(Qt::yellow);
        for (const auto &pair : {qMakePair(skin, skinRepair), qMakePair(other, otherRepair)})
            for (int y = pair.first.top(); y <= pair.first.bottom(); ++y)
                for (int x = pair.first.left(); x <= pair.first.right(); ++x)
                    generated.setPixelColor(x, y, pair.second);
        const auto completed = aiSmartFillAcceptPatch(plan, 0, generated);
        QCOMPARE(aiSmartFillRemaining(completed), 0);
        const auto preview = aiSmartFillPreview(source, completed);
        for (const QString material : {QString("skin"), QString("other")}) {
            QCOMPARE(changedKnown(source[material], preview[material]), 0);
            QCOMPARE(changedKnownQuality(source[material], preview[material]), 0);
        }
        // Unanchored charts retain each material's returned color without taking pixels
        // from its neighboring tile.
        QCOMPARE(preview["skin"].color.pixelColor(128, 128), skinRepair);
        QCOMPARE(preview["other"].color.pixelColor(128, 128), otherRepair);
        QVERIFY(preview["skin"].coverage.constScanLine(250)[4]);
        QVERIFY(preview["other"].coverage.constScanLine(4)[250]);
    }
    void combinedBoardKeepsSubpixelGapsAndMaterialEdges() {
        constexpr int n = 4096;
        auto s = mesh();
        s.materials << "other";
        auto copy = s.tris;
        for (auto &t : copy) {
            t.material = 1;
            t.object = 1;
            s.tris.push_back(t);
        }
        auto domains = aiUvDomains(s, 0, n, {"skin"});
        domains["other"] = domains["skin"];
        auto source = aiEmptyAtlases(s, n);
        for (auto &atlas : source) {
            atlas.color.fill(QColor(80, 100, 120));
            atlas.coverage.fill(255);
            atlas.confidence.fill(60000);
            // Odd one-pixel locations would disappear under ordinary nearest-neighbor
            // reduction. Edge patches reveal interpolation from the adjacent material cell.
            hole(atlas, QRect(101, 103, 1, 1));
            hole(atlas, QRect(4000, 3999, 1, 1));
            hole(atlas, QRect(0, 1000, 96, 96));
            hole(atlas, QRect(n - 96, 1000, 96, 96));
        }
        // Keep the material-cell edge tests independent of seam matching: these two
        // wholly blank charts intentionally have no accepted same-chart boundary.
        for (auto &domain : domains)
            for (int y = 1000; y < 1096; ++y) {
                for (int x = 0; x < 96; ++x)
                    domain.charts[y * n + x] = 1;
                for (int x = n - 96; x < n; ++x)
                    domain.charts[y * n + x] = 2;
            }
        const auto plan = aiPlanSmartFill(s, 0, source, domains, {"skin", "other"},
                                          aiSmartFillFingerprint(s, 0, source, {}, true));
        QCOMPARE(aiSmartFillRemaining(plan), 1);
        QCOMPARE(plan["boardSize"].toInt(), 4096);
        const auto layout = plan["layout"].toObject();
        QTemporaryDir directory;
        const auto request = aiSmartFillRequest(plan, 0, directory.path(), image(256, Qt::gray));
        const QImage mask(request["mask"].toString());
        const QImage guide(request["images"].toArray()[1].toString());
        const QColor skinRepair(225, 20, 10), otherRepair(10, 30, 235);
        QImage generated = image(1024, Qt::yellow);
        for (const auto &pair :
             {qMakePair(QString("skin"), skinRepair), qMakePair(QString("other"), otherRepair)}) {
            const QRect tile = mapping(layout[pair.first]);
            QVERIFY(tile.width() < n && tile.height() < n);
            for (const auto &native : {QPoint(101, 103), QPoint(4000, 3999)}) {
                const QPoint boardPixel = mappedPixel(tile, native.x(), native.y(), n);
                QCOMPARE(mask.pixelColor(boardPixel).alpha(), 0);
                QCOMPARE(qGray(guide.pixel(boardPixel)), 255);
            }
            const QRect returned(tile.x() * 1024 / 4096, tile.y() * 1024 / 4096, tile.width() * 1024 / 4096,
                                 tile.height() * 1024 / 4096);
            for (int y = returned.top(); y <= returned.bottom(); ++y)
                for (int x = returned.left(); x <= returned.right(); ++x)
                    generated.setPixelColor(x, y, pair.second);
        }
        const auto completed = aiSmartFillAcceptPatch(plan, 0, generated);
        QCOMPARE(aiSmartFillRemaining(completed), 0);
        const auto preview = aiSmartFillPreview(source, completed);
        for (const auto &pair :
             {qMakePair(QString("skin"), skinRepair), qMakePair(QString("other"), otherRepair)}) {
            const auto &atlas = preview[pair.first];
            QCOMPARE(changedKnown(source[pair.first], atlas), 0);
            QCOMPARE(changedKnownQuality(source[pair.first], atlas), 0);
            QVERIFY(atlas.coverage.constScanLine(103)[101]);
            QVERIFY(atlas.coverage.constScanLine(3999)[4000]);
            QCOMPARE(atlas.color.pixelColor(0, 1048), pair.second);
            QCOMPARE(atlas.color.pixelColor(n - 1, 1048), pair.second);
        }
    }
    void legacyCompositionAndCameraRebuildPreserveRepairs() {
        const auto s = mesh();
        const auto base = aiEmptyAtlases(s, 64);
        AiCamera camera;
        camera.size = 128;
        camera.orbit.distance = 3;
        camera.orbit.yaw = camera.orbit.pitch = 0;
        camera.orbit.orthographic = true;
        camera.view.translate(0, 0, -3);
        camera.projection.ortho(-1, 1, -1, 1, .1f, 10);
        const auto capture = aiCapture(s, 0, base, camera);
        auto mask = image(128, Qt::white);
        for (int y = 60; y < 68; ++y)
            for (int x = 60; x < 68; ++x)
                mask.setPixelColor(x, y, Qt::black);
        const auto candidate = image(128, QColor(175, 85, 45));
        AiProjectionOptions options;
        options.overlay = true;
        options.recordLayer = true;
        const auto projected =
            aiProject(s, 0, base, capture, candidate, mask, {"skin"}, false, 8, nullptr, options);
        QJsonObject projection{{"kind", "projection"},
                               {"visible", true},
                               {"camera", aiCameraJson(camera)},
                               {"candidate", encoded(candidate)},
                               {"mask", encoded(mask)},
                               {"materials", QJsonArray{"skin"}},
                               {"blending", QJsonObject{{"mode", "overlay"}}},
                               {"uvLayer", projected.layer}};
        const auto domains = aiUvDomains(s, 0, 64, {"skin"});
        const auto repair = aiSmartFillLayer(complete(planned(s, projected.atlases, domains)));
        const auto result = aiComposeLayerStack(base, {projection, repair}, false);
        QCOMPARE(changedKnown(projected.atlases["skin"], result["skin"]), 0);
        QVERIFY(result["skin"].coverage.constScanLine(32)[32]);
        QCOMPARE(aiComposeLayers(base, {projection, repair})["skin"].color, result["skin"].color);
        const auto prepared = aiPrepareLayers(s, 0, base, {projection, repair});
        QCOMPARE(prepared[1]["uvLayer"], repair["uvLayer"]);
        const auto rebuilt = aiRebuildProjectionLayers(s, 0, 64, {projection, repair});
        QVERIFY(aiIsRepairLayer(rebuilt[1]));
        QCOMPARE(rebuilt[1]["uvLayer"], repair["uvLayer"]);
        auto hidden = projection;
        hidden["visible"] = false;
        const auto exposed = aiComposeLayerStack(base, {hidden, repair}, false);
        QVERIFY(exposed["skin"].coverage.constScanLine(32)[32]);
        QCOMPARE(exposed["skin"].coverage.constScanLine(16)[16], uchar(0));
    }
    void oneGenerationPreservesNativeAtlasSizesAndBoundaryGaps() {
        for (int n : {1024, 2048, 4096}) {
            const auto s = mesh();
            const auto domains = aiUvDomains(s, 0, n, {"skin"});
            auto source = aiEmptyAtlases(s, n);
            paint(source, domains);
            hole(source["skin"], QRect(n / 2, n / 2, 1, 1));
            hole(source["skin"], QRect(0, 0, 1, 1));
            hole(source["skin"], QRect(n - 1, n - 1, 1, 1));
            const auto plan = planned(s, source, domains);
            QCOMPARE(aiSmartFillRemaining(plan), 1);
            QCOMPARE(plan["patches"].toArray().size(), 1);
            const QImage mask = QImage::fromData(
                QByteArray::fromBase64(plan["masks"].toObject()["skin"].toString().toLatin1()), "PNG");
            QCOMPARE(mask.size(), QSize(n, n));
            QCOMPARE(qGray(mask.pixel(0, 0)), 255);
            QCOMPARE(qGray(mask.pixel(n - 1, n - 1)), 255);
            const auto preview = aiSmartFillPreview(source, complete(plan));
            QCOMPARE(preview["skin"].color.size(), QSize(n, n));
            QVERIFY(preview["skin"].coverage.constScanLine(n / 2)[n / 2]);
            QVERIFY(preview["skin"].coverage.constScanLine(0)[0]);
            QVERIFY(preview["skin"].coverage.constScanLine(n - 1)[n - 1]);
            QCOMPARE(changedKnown(source["skin"], preview["skin"]), 0);
            QCOMPARE(changedKnownQuality(source["skin"], preview["skin"]), 0);
            QCOMPARE(preview["skin"].projectionWeight, source["skin"].projectionWeight);
        }
    }
};

int main(int argc, char **argv) {
    QSurfaceFormat f;
    f.setVersion(3, 3);
    f.setProfile(QSurfaceFormat::CoreProfile);
    f.setSamples(0);
    QSurfaceFormat::setDefaultFormat(f);
    QGuiApplication app(argc, argv);
    SmartFillTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "smart_fill_tests.moc"
