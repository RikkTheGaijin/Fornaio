#include "ai_layers.h"
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QTest>
#include <cmath>

namespace {
Scene plane() {
    Scene s;
    s.uvNames = {"UV1"};
    s.materials = {"surface"};
    s.scale = 2;
    V3 p[] = {{-.8f, -.8f, 0}, {.8f, -.8f, 0}, {.8f, .8f, 0}, {-.8f, .8f, 0}};
    V2 uv[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (auto indices : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}}) {
        Triangle t;
        t.material = 0;
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
AiCamera camera(bool ortho = true, int size = 64) {
    AiCamera c;
    c.size = size;
    c.orbit.distance = 3;
    c.orbit.yaw = c.orbit.pitch = 0;
    c.orbit.orthographic = ortho;
    c.view.translate(0, 0, -3);
    if (ortho)
        c.projection.ortho(-1, 1, -1, 1, .1f, 10);
    else
        c.projection.perspective(40, 1, .1f, 10);
    return c;
}
QImage rgba(QColor color, int size = 64) {
    QImage im(size, size, QImage::Format_RGBA8888);
    im.fill(color);
    return im;
}
QJsonObject contribution(const QImage &image, QString mode = "blend", float strength = 1) {
    AiLayerContribution layer;
    layer.materials["surface"] = image;
    return {{"visible", true},
            {"blending", QJsonObject{{"mode", mode}, {"strength", strength}}},
            {"uvLayer", aiEncodeProjectionLayer(layer, image.width())}};
}
AiAtlases base(int size = 1) {
    return aiEmptyAtlases(plane(), size);
}
} // namespace

class ProjectionTests : public QObject {
    Q_OBJECT
  private slots:
    void initTestCase() { aiInitializeProjectionRenderer(); }
    void cleanupTestCase() { aiShutdownProjectionRenderer(); }
    void browserProjectionGoldens() {
        QFile file("tests/projection_reference_golden.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        const auto cases = QJsonDocument::fromJson(file.readAll()).object()["cases"].toArray();
        int checked = 0;
        for (const auto &v : cases) {
            auto test = v.toObject();
            const auto name = test["name"].toString();
            if (!name.startsWith("orthographic_") && name != "overlay_bypasses_similarity")
                continue;
            auto s = plane();
            auto settings = test["settings"].toObject();
            auto normal = settings["normal"].toArray();
            for (auto &t : s.tris)
                for (auto &n : t.n)
                    n = {float(normal[0].toDouble()), float(normal[1].toDouble()),
                         float(normal[2].toDouble())};
            auto layer = aiRenderProjection(s, 0, 64, camera(), rgba(QColor(200, 80, 30)), {}, {},
                                            {"surface"}, settings["overlay"].toBool(), 0);
            auto expected = test["rgba"].toArray();
            auto pixel = layer.materials["surface"].pixelColor(32, 32);
            for (int c = 0; c < 4; ++c) {
                int actual[] = {pixel.red(), pixel.green(), pixel.blue(), pixel.alpha()};
                // RGB in discarded/zero-alpha texels is immaterial; alpha eligibility must agree.
                if (c == 3 || expected[3].toInt() > 0)
                    QVERIFY2(std::abs(actual[c] - expected[c].toInt()) <= 1,
                             qPrintable(name + " channel " + QString::number(c) + " got " +
                                        QString::number(actual[c])));
            }
            ++checked;
        }
        QVERIFY(checked >= 7);
    }
    void browserCompositionGoldens() {
        QFile file("tests/projection_reference_golden.json");
        QVERIFY(file.open(QIODevice::ReadOnly));
        int checked = 0;
        for (const auto &v : QJsonDocument::fromJson(file.readAll()).object()["cases"].toArray()) {
            const auto test = v.toObject();
            if (test["kind"] != "composition")
                continue;
            QList<QJsonObject> records;
            const auto layers = test["layers"].toArray();
            const auto strengths = test["strengths"].toArray(), modes = test["modes"].toArray();
            for (int i = 0; i < layers.size(); ++i) {
                auto p = layers[i].toArray();
                records << contribution(
                    rgba(QColor::fromRgbF(p[0].toDouble(), p[1].toDouble(), p[2].toDouble(), p[3].toDouble()),
                         1),
                    modes[i].toInt() == 1 ? "overlay" : "blend", float(strengths[i].toDouble()));
            }
            auto result = aiComposeParityLayers(base(), records);
            auto expected = test["rgba"].toArray();
            const auto pixel = result["surface"].color.pixelColor(0, 0);
            if (!expected.empty() && result["surface"].coverage.constScanLine(0)[0]) {
                QVERIFY2(std::abs(pixel.red() - expected[0].toInt()) <= 1,
                         qPrintable(test["name"].toString()));
                QVERIFY(std::abs(pixel.green() - expected[1].toInt()) <= 1);
                QVERIFY(std::abs(pixel.blue() - expected[2].toInt()) <= 1);
            }
            ++checked;
        }
        QVERIFY(checked >= 5);
    }
    void blendOrderStrengthAndMaximumCoverage() {
        auto a = contribution(rgba(QColor(255, 0, 0, 128), 1)),
             b = contribution(rgba(QColor(0, 0, 255, 255), 1));
        auto first = aiComposeParityLayers(base(), {a, b}), reverse = aiComposeParityLayers(base(), {b, a});
        QCOMPARE(first["surface"].color, reverse["surface"].color);
        auto pixel = first["surface"].color.pixelColor(0, 0);
        QVERIFY(std::abs(pixel.red() - 85) <= 1);
        QVERIFY(std::abs(pixel.blue() - 170) <= 1);
        QCOMPARE(first["surface"].projectionWeight[0], 1.f);
        auto strong = contribution(rgba(QColor(255, 0, 0), 1), "blend", 2);
        auto c = aiComposeParityLayers(base(), {strong});
        QCOMPARE(c["surface"].projectionWeight[0], 2.f);
        auto round = aiDeserializeAtlases(aiSerializeAtlases(c));
        QCOMPARE(round["surface"].projectionWeight, c["surface"].projectionWeight);
    }
    void overlayOrderingVisibilityAndEmptyStack() {
        auto red = contribution(rgba(Qt::red, 1)),
             blue = contribution(rgba(QColor(0, 0, 255, 128), 1), "overlay");
        auto c = aiComposeParityLayers(base(), {red, blue});
        QCOMPARE(c["surface"].color.pixelColor(0, 0), QColor(127, 0, 128));
        blue["visible"] = false;
        auto hidden = aiComposeParityLayers(base(), {red, blue});
        QCOMPARE(hidden["surface"].color.pixelColor(0, 0), QColor(Qt::red));
        auto empty = aiComposeParityLayers(base(), {});
        QCOMPARE(empty["surface"].coverage, base()["surface"].coverage);
        auto b = base();
        b["surface"].color.fill(Qt::green);
        b["surface"].coverage.fill(255);
        QCOMPARE(aiComposeParityLayers(b, {})["surface"].color, b["surface"].color);
    }
    void alphaMasksAndNoHoleFill() {
        auto s = plane();
        auto source = rgba(QColor(200, 80, 30, 128));
        auto layer = aiRenderProjection(s, 0, 64, camera(), source, {}, {}, {"surface"});
        auto p = layer.materials["surface"].pixelColor(32, 32);
        QVERIFY(std::abs(p.alpha() - 128) <= 1);
        QVERIFY(std::abs(p.red() - 200) <= 1);
        auto mask = rgba(Qt::white);
        for (int y = 24; y < 40; ++y)
            for (int x = 24; x < 40; ++x)
                mask.setPixelColor(x, y, Qt::black);
        layer = aiRenderProjection(s, 0, 64, camera(), rgba(Qt::red), mask, {}, {"surface"});
        QCOMPARE(layer.materials["surface"].pixelColor(32, 32).alpha(), 0);
        auto uvMask = rgba(Qt::black);
        layer = aiRenderProjection(s, 0, 64, camera(), rgba(Qt::red), {}, uvMask, {"surface"});
        QCOMPARE(layer.materials["surface"].pixelColor(32, 32).alpha(), 0);
    }
    void hiddenSurfaceAndMaterialIsolation() {
        auto s = plane();
        s.materials << "hidden";
        auto back = s.tris;
        for (auto &t : back) {
            t.material = 1;
            for (auto &p : t.p)
                p.z = -.01f;
            s.tris.push_back(t);
        }
        auto result = aiRenderProjection(s, 0, 64, camera(), rgba(Qt::red), {}, {}, {"surface", "hidden"});
        QVERIFY(result.materials["surface"].pixelColor(32, 32).alpha() > 0);
        QCOMPARE(result.materials["hidden"].pixelColor(32, 32).alpha(), 0);
        result = aiRenderProjection(s, 0, 64, camera(), rgba(Qt::red), {}, {}, {"hidden"});
        QVERIFY(!result.materials.contains("surface"));
    }
    void sourceResolutionPerspectiveAndCancellation() {
        auto s = plane();
        auto c = camera(false);
        auto result =
            aiRenderProjection(s, 0, 64, c, rgba(Qt::red, 128), rgba(Qt::white, 64), {}, {"surface"});
        QVERIFY(result.materials["surface"].pixelColor(32, 32).alpha() > 0);
        QVERIFY(result.materials["surface"].pixelColor(32, 32).alpha() < 255);
        std::atomic_bool cancel = true;
        QVERIFY_EXCEPTION_THROWN(
            aiRenderProjection(s, 0, 64, c, rgba(Qt::red), {}, {}, {"surface"}, false, 2, &cancel),
            std::runtime_error);
    }
    void interpolatedNormalsTransparentEdgesAndErosion() {
        auto s = plane();
        for (auto &t : s.tris)
            for (int k = 0; k < 3; ++k)
                t.n[k] = {t.p[k].x < 0 ? .8660254f : -.8660254f, 0, .5f};
        auto layer = aiRenderProjection(s, 0, 64, camera(), rgba(Qt::red), {}, {}, {"surface"}, false, 0);
        QVERIFY(std::abs(layer.materials["surface"].pixelColor(32, 32).alpha() - 64) <= 1);
        s = plane();
        auto image = rgba(QColor(200, 0, 0));
        for (int y = 0; y < 64; ++y)
            for (int x = 32; x < 64; ++x)
                image.setPixelColor(x, y, QColor(0, 0, 255, 0));
        layer = aiRenderProjection(s, 0, 64, camera(), image, {}, {}, {"surface"}, false, 0);
        int partial = 0;
        for (int x = 0; x < 64; ++x) {
            auto pixel = layer.materials["surface"].pixelColor(x, 32);
            if (pixel.alpha() > 0 && pixel.alpha() < 255) {
                ++partial;
                QVERIFY(std::abs(pixel.red() - 200) <= 1);
                QCOMPARE(pixel.blue(), 0);
            }
        }
        QVERIFY(partial > 0);
        layer = aiRenderProjection(s, 0, 64, camera(), rgba(Qt::red), {}, {}, {"surface"}, false, 0);
        QCOMPARE(layer.materials["surface"].pixelColor(0, 32).alpha(), 0);
        QCOMPARE(layer.materials["surface"].pixelColor(32, 32).alpha(), 255);
    }
    void depthDiscontinuityMarginIsApplied() {
        auto s = plane();
        for (auto &t : s.tris)
            for (int k = 0; k < 3; ++k) {
                t.p[k].z = .6f * t.p[k].x;
                t.n[k] = unit(V3{-.6f, 0, 1});
            }
        const auto plain =
            aiRenderProjection(s, 0, 64, camera(), rgba(Qt::red), {}, {}, {"surface"}, false, 0);
        const auto margin =
            aiRenderProjection(s, 0, 64, camera(), rgba(Qt::red), {}, {}, {"surface"}, false, 2);
        QVERIFY(plain.materials["surface"].pixelColor(32, 32).alpha() > 0);
        QCOMPARE(margin.materials["surface"].pixelColor(32, 32).alpha(), 0);
    }
    void paddingCrossesUvSeamsOnlyOutsideIslands() {
        auto s = plane();
        s.tris[0].uv[0] = {V2{.1f, .1f}, V2{.4f, .1f}, V2{.4f, .4f}};
        s.tris[1].uv[0] = {V2{.6f, .6f}, V2{.9f, .9f}, V2{.6f, .9f}};
        const auto layer =
            aiRenderProjection(s, 0, 64, camera(), rgba(Qt::red), {}, {}, {"surface"}, false, 0);
        QVERIFY(layer.materials["surface"].pixelColor(14, 43).alpha() > 0);
        QCOMPARE(layer.materials["surface"].pixelColor(12, 41).alpha(), 0);
    }
    void supportedAtlasResolutions() {
        for (int resolution : {512, 1024, 4096}) {
            const auto layer =
                aiRenderProjection(plane(), 0, resolution, camera(), rgba(Qt::red), {}, {}, {"surface"});
            QCOMPARE(layer.materials["surface"].size(), QSize(resolution, resolution));
            QCOMPARE(layer.materials["surface"].pixelColor(resolution / 2, resolution / 2).alpha(), 255);
        }
    }
    void independentDataRoundTripAndValidation() {
        auto record = contribution(rgba(QColor(40, 150, 200, 50), 8));
        QCOMPARE(aiDecodeProjectionLayer(record["uvLayer"].toObject()).materials["surface"],
                 rgba(QColor(40, 150, 200, 50), 8));
        auto invalid = record["uvLayer"].toObject();
        invalid["resolution"] = 4097;
        QVERIFY_EXCEPTION_THROWN(aiDecodeProjectionLayer(invalid), std::runtime_error);
        auto bad = record;
        bad["blending"] = QJsonObject{{"strength", -1}};
        QVERIFY_EXCEPTION_THROWN(aiComposeParityLayers(base(8), {bad}), std::runtime_error);
        QTemporaryDir dir;
        auto output = dir.filePath("project.fornaioai");
        auto composed = aiComposeParityLayers(base(8), {record});
        aiSaveProject(output, {{"projectionBackend", "modddif-v1"}}, composed, {record});
        AiAtlases loaded;
        QList<QJsonObject> records;
        auto settings = aiLoadProject(output, loaded, records);
        QCOMPARE(settings["projectionBackend"].toString(), QString("modddif-v1"));
        QCOMPARE(records.size(), 1);
        QCOMPARE(aiComposeLayers(base(8), records)["surface"].color, composed["surface"].color);
    }
};

int main(int argc, char **argv) {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSamples(0);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);
    ProjectionTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "projection_tests.moc"
