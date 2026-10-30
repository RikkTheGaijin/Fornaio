#include "ai_panel.h"
#include "ai_layers.h"
#include "ai_smart_fill.h"
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QGroupBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <iostream>

static Scene plane(float z = 0, int material = 0, float offset = 0) {
    Scene s;
    s.uvNames = {"UV1"};
    s.materials = {"front", "back"};
    s.center = {};
    s.scale = 2;
    V3 p[] = {
        {-.8f + offset, -.8f, z}, {.8f + offset, -.8f, z}, {.8f + offset, .8f, z}, {-.8f + offset, .8f, z}};
    V2 uv[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
    for (auto indices : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}}) {
        Triangle t;
        t.material = material;
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
static AiCapture capture(const Scene &s, const AiAtlases &a, int size = 128) {
    OrbitCamera o;
    o.yaw = o.pitch = 0;
    o.distance = 3;
    return aiCapture(s, 0, a, aiCamera(s, o, size));
}
static qint64 covered(const QImage &im) {
    qint64 n = 0;
    for (int y = 0; y < im.height(); ++y)
        for (int x = 0; x < im.width(); ++x)
            n += im.constScanLine(y)[x] != 0;
    return n;
}
static QImage solid(int size, QRgb color) {
    QImage im(size, size, QImage::Format_RGB32);
    im.fill(color);
    return im;
}
static void paintAtlas(AiAtlas &a, QRgb color, float quality = 1) {
    a.color.fill(color);
    a.coverage.fill(255);
    for (int y = 0; y < a.color.height(); ++y)
        for (int x = 0; x < a.color.width(); ++x)
            reinterpret_cast<quint16 *>(a.confidence.scanLine(y))[x] = quint16(qRound(quality * 65535));
}
static float atlasQuality(const AiAtlas &a, int x, int y) {
    return reinterpret_cast<const quint16 *>(a.confidence.constScanLine(y))[x] / 65535.f;
}

static Scene qualityScene(bool organic, bool multiple) {
    Scene s;
    s.uvNames = {"UV1"};
    s.materials = {"body", "trim"};
    s.scale = multiple ? 4.3f : 2.8f;
    auto add = [&](std::array<V3, 3> points, std::array<V3, 3> normals, std::array<V2, 3> uv, int material) {
        if (dot(cross(points[1] - points[0], points[2] - points[0]), normals[0]) < 0) {
            std::swap(points[1], points[2]);
            std::swap(normals[1], normals[2]);
            std::swap(uv[1], uv[2]);
        }
        Triangle t;
        t.material = material;
        for (int k = 0; k < 3; ++k) {
            t.p[k] = points[k];
            t.n[k] = normals[k];
        }
        t.uv.push_back(uv);
        s.tris.push_back(t);
    };
    if (organic) {
        const int rows = 12, cols = 20;
        for (int j = 0; j < rows; ++j)
            for (int i = 0; i < cols; ++i) {
                V3 p[4];
                V2 uv[4];
                int di[] = {0, 1, 1, 0}, dj[] = {0, 0, 1, 1};
                for (int k = 0; k < 4; ++k) {
                    float u = float(i + di[k]) / cols, v = float(j + dj[k]) / rows, phi = v * 3.14159265f,
                          theta = u * 6.2831853f;
                    p[k] = {.8f * std::sin(phi) * std::cos(theta), .8f * std::cos(phi),
                            .8f * std::sin(phi) * std::sin(theta)};
                    uv[k] = {u, 1 - v};
                }
                if (j > 0)
                    add({p[0], p[1], p[2]}, {unit(p[0]), unit(p[1]), unit(p[2])}, {uv[0], uv[1], uv[2]}, 0);
                if (j + 1 < rows)
                    add({p[0], p[2], p[3]}, {unit(p[0]), unit(p[2]), unit(p[3])}, {uv[0], uv[2], uv[3]}, 0);
            }
    } else
        for (int object = 0; object < (multiple ? 2 : 1); ++object)
            for (int face = 0; face < 6; ++face) {
                QMatrix4x4 rotate;
                if (face < 4)
                    rotate.rotate(face * 90.f, 0, 1, 0);
                else
                    rotate.rotate(face == 4 ? 90.f : -90.f, 1, 0, 0);
                V3 p[4], n;
                V2 uv[4];
                QVector3D nq = rotate.mapVector(QVector3D(0, 0, 1));
                n = {nq.x(), nq.y(), nq.z()};
                V3 base[] = {{-.8f, -.8f, .8f}, {.8f, -.8f, .8f}, {.8f, .8f, .8f}, {-.8f, .8f, .8f}};
                V2 tex[] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
                for (int k = 0; k < 4; ++k) {
                    auto q = rotate.map(QVector3D(base[k].x, base[k].y, base[k].z));
                    p[k] = {q.x() + (multiple ? (object ? 1.f : -1.f) : 0), q.y(), q.z()};
                    uv[k] = {(face % 3 + .04f + tex[k].x * .92f) / 3,
                             (face / 3 + .04f + tex[k].y * .92f) / 2};
                }
                add({p[0], p[1], p[2]}, {n, n, n}, {uv[0], uv[1], uv[2]}, object);
                add({p[0], p[2], p[3]}, {n, n, n}, {uv[0], uv[2], uv[3]}, object);
            }
    return s;
}

static QJsonObject blendQuality(const QString &root = {}) {
    auto s = qualityScene(true, false);
    auto truth = aiEmptyAtlases(s, 256);
    paintAtlas(truth["body"], qRgb(160, 65, 30));
    for (int y = 0; y < 256; ++y)
        for (int x = 0; x < 256; ++x) {
            int d = ((x / 12 + y / 12) & 1) ? 30 : -30;
            truth["body"].color.setPixel(x, y, qRgb(160 + d, 65 + d / 3, 30 + d / 5));
        }
    OrbitCamera orbit;
    orbit.yaw = 0;
    orbit.pitch = 10;
    orbit.distance = 3;
    auto empty = aiEmptyAtlases(s, 256);
    auto camera = aiCamera(s, orbit, 512);
    auto tint = [](AiCapture source, int amount) {
        for (int y = 0; y < source.camera.size; ++y)
            for (int x = 0; x < source.camera.size; ++x)
                if (source.faces[y * source.camera.size + x] >= 0) {
                    QRgb p = source.partial.pixel(x, y);
                    source.partial.setPixel(x, y, qRgb(qRed(p) + amount, qGreen(p) + amount / 3, qBlue(p)));
                }
        return source.partial;
    };
    auto firstImage = tint(aiCapture(s, 0, truth, camera), 20);
    auto firstCapture = aiCapture(s, 0, empty, camera);
    auto first =
        aiProject(s, 0, empty, firstCapture, firstImage, firstCapture.silhouette, {"body"}, false, 0);
    orbit.yaw = 75;
    camera = aiCamera(s, orbit, 512);
    auto secondImage = tint(aiCapture(s, 0, truth, camera), -20);
    auto secondCapture = aiCapture(s, 0, first.atlases, camera);
    auto filled = aiProject(s, 0, first.atlases, secondCapture, secondImage, secondCapture.silhouette,
                            {"body"}, true, 0);
    auto blended = aiProject(s, 0, first.atlases, secondCapture, secondImage, secondCapture.silhouette,
                             {"body"}, false, 0);
    double fillJump = 0, blendJump = 0, dark = 0, light = 0;
    int samples = 0, darkCount = 0, lightCount = 0;
    const auto oldCoverage = first.atlases["body"].coverage;
    const auto coverage = blended.atlases["body"].coverage;
    const auto reference = truth["body"].color;
    const auto after = blended.atlases["body"].color;
    for (int y = 2; y < 254; ++y)
        for (int x = 2; x < 254; ++x) {
            if (!coverage.constScanLine(y)[x])
                continue;
            QRgb expected = reference.pixel(x, y);
            if (expected != reference.pixel(x - 1, y) || expected != reference.pixel(x + 1, y) ||
                expected != reference.pixel(x, y - 1) || expected != reference.pixel(x, y + 1))
                continue;
            if (qRed(expected) < 160) {
                dark += qRed(after.pixel(x, y));
                ++darkCount;
            } else {
                light += qRed(after.pixel(x, y));
                ++lightCount;
            }
            for (const QPoint &d : {QPoint(1, 0), QPoint(0, 1)}) {
                int xx = x + d.x(), yy = y + d.y();
                if (!coverage.constScanLine(yy)[xx] ||
                    bool(oldCoverage.constScanLine(y)[x]) == bool(oldCoverage.constScanLine(yy)[xx]) ||
                    expected != reference.pixel(xx, yy))
                    continue;
                fillJump += std::abs(qRed(filled.atlases["body"].color.pixel(x, y)) -
                                     qRed(filled.atlases["body"].color.pixel(xx, yy)));
                blendJump += std::abs(qRed(after.pixel(x, y)) - qRed(after.pixel(xx, yy)));
                ++samples;
            }
        }
    QJsonObject metrics{{"boundarySamples", samples},
                        {"fillBoundaryJump", fillJump / std::max(1, samples)},
                        {"blendBoundaryJump", blendJump / std::max(1, samples)},
                        {"checkerContrast", light / std::max(1, lightCount) - dark / std::max(1, darkCount)}};
    if (!root.isEmpty()) {
        QDir().mkpath(root);
        filled.atlases["body"].color.save(root + "/fill-uv.png");
        after.save(root + "/blend-uv.png");
        coverage.save(root + "/coverage.png");
        orbit.yaw = 37.5f;
        auto display = aiCamera(s, orbit, 512);
        QImage comparison(1024, 570, QImage::Format_RGB32);
        comparison.fill(QColor("#111923"));
        QPainter painter(&comparison);
        painter.setPen(Qt::white);
        auto font = painter.font();
        font.setPixelSize(19);
        painter.setFont(font);
        painter.drawImage(0, 38, aiCapture(s, 0, filled.atlases, display).partial);
        painter.drawImage(512, 38, aiCapture(s, 0, blended.atlases, display).partial);
        painter.drawText(QRect(0, 3, 512, 32), Qt::AlignCenter, "Fill only");
        painter.drawText(QRect(512, 3, 512, 32), Qt::AlignCenter, "Camera-quality blend + color match");
        painter.end();
        comparison.save(root + "/comparison.png");
        QFile file(root + "/metrics.json");
        if (file.open(QIODevice::WriteOnly))
            file.write(QJsonDocument(metrics).toJson());
    }
    return metrics;
}
static void renderQuality(const QString &root) {
    QDir().mkpath(root);
    for (const QString &name : {QString("box"), QString("organic"), QString("materials")}) {
        Scene s = qualityScene(name == "organic", name == "materials");
        auto errors = validateAiMesh(s, 0);
        if (!errors.empty())
            throw std::runtime_error(errors.join('\n').toStdString());
        AiAtlases a = aiEmptyAtlases(s, 512);
        OrbitCamera o;
        o.distance = s.scale * 1.6f;
        auto c = aiCapture(s, 0, a, aiCamera(s, o));
        QString dir = QFileInfo(root + "/" + name).absoluteFilePath();
        QDir().mkpath(dir);
        c.normal.save(dir + "/normal.png");
        c.depth.save(dir + "/depth.png");
        c.neutral.save(dir + "/neutral.png");
        c.partial.save(dir + "/partial.png");
        c.silhouette.save(dir + "/silhouette.png");
        QJsonObject request{
            {"command", "generate"},
            {"id", name},
            {"normal", dir + "/normal.png"},
            {"depth", dir + "/depth.png"},
            {"neutral", dir + "/neutral.png"},
            {"partial", dir + "/partial.png"},
            {"silhouette", dir + "/silhouette.png"},
            {"output", dir + "/candidate.png"},
            {"size", 1024},
            {"seed", 42},
            {"prompt", name == "organic"
                           ? "Orange fruit with a matte pebbled orange peel and subtle green mottling. "
                             "Preserve the round object."
                           : "Worn red painted metal, silver wear on edges, small dark scratches, matte "
                             "surface. Preserve every flat face and the exact shape."}};
        QFile f(dir + "/request.json");
        f.open(QIODevice::WriteOnly);
        f.write(QJsonDocument(request).toJson());
    }
}

static void projectQuality(const QString &root, bool second) {
    for (const QString &name : {QString("box"), QString("organic"), QString("materials")}) {
        Scene s = qualityScene(name == "organic", name == "materials");
        auto atlases = aiEmptyAtlases(s, 512);
        OrbitCamera o;
        o.distance = s.scale * 1.6f;
        QString dir = QFileInfo(root + "/" + name).absoluteFilePath();
        auto first = aiCapture(s, 0, atlases, aiCamera(s, o));
        QImage image(dir + "/candidate.png");
        if (image.isNull())
            continue;
        QSet<QString> selected;
        for (const auto &key : atlases.keys())
            selected.insert(key);
        auto projected = aiProject(s, 0, atlases, first, image, first.silhouette, selected, true);
        atlases = projected.atlases;
        o.yaw += 90;
        auto next = aiCapture(s, 0, atlases, aiCamera(s, o));
        if (second) {
            QImage candidate(dir + "/candidate-second.png");
            if (!candidate.isNull())
                atlases = aiProject(s, 0, atlases, next, candidate, next.silhouette, selected, true).atlases;
        }
        for (auto it = atlases.begin(); it != atlases.end(); ++it) {
            it->color.save(dir + "/atlas-" + it.key() + ".png");
            it->coverage.save(dir + "/coverage-" + it.key() + ".png");
        }
        next.normal.save(dir + "/normal-second.png");
        next.depth.save(dir + "/depth-second.png");
        next.neutral.save(dir + "/neutral-second.png");
        next.partial.save(dir + "/partial-second.png");
        next.silhouette.save(dir + "/silhouette-second.png");
        QFile f(dir + "/request.json");
        f.open(QIODevice::ReadOnly);
        auto request = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        request["id"] = name + "-second";
        request["normal"] = dir + "/normal-second.png";
        request["depth"] = dir + "/depth-second.png";
        request["neutral"] = dir + "/neutral-second.png";
        request["partial"] = dir + "/partial-second.png";
        request["silhouette"] = dir + "/silhouette-second.png";
        request["anchor"] = dir + "/candidate.png";
        request["output"] = dir + "/candidate-second.png";
        f.setFileName(dir + "/request-second.json");
        f.open(QIODevice::WriteOnly);
        f.write(QJsonDocument(request).toJson());
    }
}

static void continuityFixture(const QString &root) {
    const QString dir = QFileInfo(root).absoluteFilePath();
    QDir().mkpath(dir);
    auto scene = qualityScene(false, false);
    auto atlases = aiEmptyAtlases(scene, 512);
    OrbitCamera camera;
    camera.distance = scene.scale * 1.6f;
    auto first = aiCapture(scene, 0, atlases, aiCamera(scene, camera));
    QImage candidate(".ai-qa/fresh-blue-ceramic.png");
    if (candidate.isNull())
        throw std::runtime_error("Run the existing blue ceramic fixture before the continuity probe");
    AiProjectionOptions options;
    options.overlay = true;
    options.colorMatch = false;
    atlases =
        aiProject(scene, 0, atlases, first, candidate, first.silhouette, {"body"}, false, 8, nullptr, options)
            .atlases;
    camera.yaw += 40;
    const auto next = aiCapture(scene, 0, atlases, aiCamera(scene, camera));
    const auto feedback = aiProjectionFeedback(scene, next, .8f, options);
    QJsonObject request{{"command", "generate"},
                        {"id", "continuity-box"},
                        {"size", 1024},
                        {"seed", 98761},
                        {"prompt", "Cobalt blue ceramic with white floral decoration. Continue the same "
                                   "floral painting, keeping the existing flowers in their exact positions."},
                        {"conditioning", "confidence"},
                        {"continuityStrength", .8},
                        {"protectDetails", true},
                        {"repaint", true},
                        {"output", dir + "/candidate.png"}};
    for (auto pair :
         {qMakePair(QString("neutral"), next.neutral), qMakePair(QString("normal"), next.normal),
          qMakePair(QString("depth"), next.depth), qMakePair(QString("silhouette"), next.silhouette),
          qMakePair(QString("feedback"), feedback.image), qMakePair(QString("feedbackMask"), feedback.mask),
          qMakePair(QString("accepted"), next.partial)}) {
        const QString path = dir + "/" + pair.first + ".png";
        if (!pair.second.save(path))
            throw std::runtime_error("Cannot write continuity fixture");
        request[pair.first] = path;
    }
    next.confidence.save(dir + "/source-confidence.png");
    QFile out(dir + "/request.json");
    if (!out.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot write continuity request");
    out.write(QJsonDocument(request).toJson());
}

// Maintainer probe: read a user's project without changing it, regenerate the
// saved inspection camera, and project only into a separate QA directory.
static void projectContinuityFixture(const QString &path, const QString &root) {
    AiAtlases atlases;
    QList<QJsonObject> records;
    const auto settings = aiLoadProject(path, atlases, records);
    const auto mesh = QDir(QFileInfo(path).absolutePath()).absoluteFilePath(settings["mesh"].toString());
    const auto scene = loadScene(mesh);
    const int uv = settings["uv"].toInt();
    auto camera = aiCameraFromJson(settings["camera"].toObject());
    if (!settings["camera"].toObject().contains("fieldOfView")) {
        camera.orbit.distance *=
            std::tan(camera.orbit.fieldOfView * 3.14159265359f / 360) / std::tan(20.f * 3.14159265359f / 360);
        camera.orbit.fieldOfView = 20;
        camera = aiCamera(scene, camera.orbit, camera.size);
    }
    const auto c = aiCapture(scene, uv, atlases, camera);
    AiProjectionOptions options;
    options.overlay = settings["blendMode"].toString() == "overlay";
    options.colorMatch = settings["colorMatch"].toBool();
    options.anglePower = settings["angleFalloff"].toDouble(2);
    options.strength = settings["projectionStrength"].toDouble(100) / 100;
    options.feather = settings["projectionFeather"].toDouble(6) / 100;
    options.minCosine = std::cos(settings["projectionAngle"].toDouble(60) * 3.14159265359 / 180);
    const float strength = settings["continuityStrength"].toDouble(80) / 100;
    const auto feedback = aiProjectionFeedback(scene, c, strength, options);
    const QString dir = QFileInfo(root).absoluteFilePath();
    QDir().mkpath(dir);
    QJsonObject request{{"command", "generate"},
                        {"id", "project-continuity"},
                        {"size", camera.size},
                        {"seed", 907099168},
                        {"prompt", settings["prompt"]},
                        {"conditioning", "confidence"},
                        {"continuityStrength", strength},
                        {"continuityVersion", 2},
                        {"protectDetails", true},
                        {"repaint", true},
                        {"output", dir + "/candidate.png"}};
    for (auto pair :
         {qMakePair(QString("neutral"), c.neutral), qMakePair(QString("normal"), c.normal),
          qMakePair(QString("depth"), c.depth), qMakePair(QString("silhouette"), c.silhouette),
          qMakePair(QString("feedback"), feedback.image), qMakePair(QString("feedbackMask"), feedback.mask),
          qMakePair(QString("accepted"), c.partial)}) {
        const auto imagePath = dir + "/" + pair.first + ".png";
        if (!pair.second.save(imagePath))
            throw std::runtime_error("Cannot write project probe image");
        request[pair.first] = imagePath;
    }
    c.confidence.save(dir + "/confidence.png");
    aiFeedback(c, strength).mask.save(dir + "/previous-reuse-mask.png");
    for (const auto &record : records) {
        if (!record["visible"].toBool(true))
            continue;
        const auto bytes = QByteArray::fromBase64(record["candidate"].toString().toLatin1());
        if (bytes.isEmpty())
            continue;
        QFile image(dir + "/appearance-anchor.png");
        if (!image.open(QIODevice::WriteOnly))
            throw std::runtime_error("Cannot write appearance anchor");
        image.write(bytes);
        image.close();
        request["appearanceAnchor"] = image.fileName();
        break;
    }
    QFile out(dir + "/request.json");
    if (!out.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot write project probe request");
    out.write(QJsonDocument(request).toJson());
    out.close();
    QImage candidate(dir + "/candidate.png");
    if (!candidate.isNull()) {
        options.retainedMask = QImage(dir + "/candidate-retained.png");
        QSet<QString> materials;
        for (const auto &m : settings["materials"].toArray())
            materials.insert(m.toString());
        const auto result =
            aiProject(scene, uv, atlases, c, candidate, c.silhouette, materials, false, 8, nullptr, options);
        const auto after = aiCapture(scene, uv, result.atlases, camera);
        after.partial.save(dir + "/projected.png");
        after.known.save(dir + "/projected-known.png");
        for (const auto &record : records) {
            auto view = aiCameraFromJson(record["camera"].toObject());
            aiCapture(scene, uv, atlases, view).partial.save(dir + "/original-before.png");
            aiCapture(scene, uv, result.atlases, view).partial.save(dir + "/original-after.png");
            break;
        }
    }
}

static void freshHeadPairFixture(const QString &path, const QString &root) {
    AiAtlases loaded;
    QList<QJsonObject> records;
    auto settings = aiLoadProject(path, loaded, records);
    const auto mesh = QDir(QFileInfo(path).absolutePath()).absoluteFilePath(settings["mesh"].toString());
    const auto scene = loadScene(mesh);
    const int uv = settings["uv"].toInt();
    const QString dir = QFileInfo(root).absoluteFilePath();
    QDir().mkpath(dir);
    OrbitCamera first;
    first.fieldOfView = 20;
    first = aiFrameCamera(scene, first);
    const auto base = aiEmptyAtlases(scene, 2048);
    const auto capture = aiCapture(scene, uv, base, aiCamera(scene, first));
    QJsonObject request{{"command", "generate"},
                        {"id", "sharp-first"},
                        {"size", 1024},
                        {"seed", 907099167},
                        {"conditioning", "fresh"},
                        {"repaint", true},
                        {"prompt", settings["prompt"]},
                        {"output", dir + "/first-candidate.png"}};
    for (auto pair :
         {qMakePair(QString("normal"), capture.normal), qMakePair(QString("depth"), capture.depth),
          qMakePair(QString("neutral"), capture.neutral),
          qMakePair(QString("silhouette"), capture.silhouette)}) {
        const QString image = dir + "/first-" + pair.first + ".png";
        if (!pair.second.save(image))
            throw std::runtime_error("Cannot save fresh fixture");
        request[pair.first] = image;
    }
    QFile out(dir + "/first-request.json");
    if (!out.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot save fresh request");
    out.write(QJsonDocument(request).toJson());
    out.close();
    QImage image(dir + "/first-candidate.png");
    if (image.isNull())
        return;
    AiProjectionOptions options;
    options.overlay = true;
    options.colorMatch = false;
    options.minCosine = .5f;
    QSet<QString> materials;
    for (const auto &material : base.keys())
        materials.insert(material);
    const auto projected =
        aiProject(scene, uv, base, capture, image, capture.silhouette, materials, false, 8, nullptr, options);
    aiCapture(scene, uv, projected.atlases, capture.camera).partial.save(dir + "/first-projected.png");
    aiCapture(scene, uv, projected.atlases, capture.camera).known.save(dir + "/first-known.png");
    QFile source(dir + "/first-candidate.png");
    source.open(QIODevice::ReadOnly);
    records = {{{"camera", aiCameraJson(capture.camera)},
                {"candidate", QString::fromLatin1(source.readAll().toBase64())},
                {"visible", true},
                {"id", "first"}}};
    first.yaw -= 35;
    first = aiFrameCamera(scene, first);
    settings["camera"] = aiCameraJson(aiCamera(scene, first));
    settings["mesh"] = mesh;
    settings["projectionAngle"] = 60;
    settings["materials"] = QJsonArray::fromStringList(materials.values());
    const auto fixture = dir + "/first-only.fornaioai";
    aiSaveProject(fixture, settings, projected.atlases, records);
    projectContinuityFixture(fixture, dir + "/second");
}

static void headGapProbe(const QString &mesh, const QString &root) {
    const auto scene = loadScene(mesh);
    const auto base = aiEmptyAtlases(scene, 2048);
    const QString dir = QFileInfo(root).absoluteFilePath();
    QDir().mkpath(dir);
    QSet<QString> materials;
    for (const auto &material : base.keys())
        materials.insert(material);
    QJsonArray metrics;
    for (const float yaw : {155.f, 180.f, 205.f}) {
        OrbitCamera orbit;
        orbit.yaw = yaw;
        orbit.pitch = 0;
        orbit.fieldOfView = 20;
        const auto camera = aiCamera(scene, aiFrameCamera(scene, orbit));
        const auto c = aiCapture(scene, 0, base, camera);
        AiProjectionOptions options;
        options.overlay = true;
        options.colorMatch = false;
        options.minCosine = .5f;
        const auto result = aiProject(scene, 0, base, c, solid(1024, qRgb(160, 120, 65)), c.silhouette,
                                      materials, false, 8, nullptr, options);
        const auto after = aiCapture(scene, 0, result.atlases, camera);
        after.partial.save(dir + QString("/%1-projected.png").arg(yaw));
        after.known.save(dir + QString("/%1-known.png").arg(yaw));
        QImage diagnostic = after.partial.copy();
        const auto inverse = camera.projection.inverted();
        int safe = 0, missing = 0, shadingRejects = 0;
        for (int y = 0; y < 1024; ++y)
            for (int x = 0; x < 1024; ++x) {
                const int face = c.faces[y * 1024 + x];
                if (face < 0)
                    continue;
                const auto &t = scene.tris[face];
                V3 g = unit(cross(t.p[1] - t.p[0], t.p[2] - t.p[0]));
                if (dot(g, t.n[0] + t.n[1] + t.n[2]) < 0)
                    g = g * -1;
                const auto normal = camera.view.mapVector(QVector3D(g.x, g.y, g.z)).normalized();
                const auto ray =
                    inverse.map(QVector3D(2 * (x + .5f) / 1024 - 1, 1 - 2 * (y + .5f) / 1024, 1));
                if (QVector3D::dotProduct(normal, -ray.normalized()) < .6f)
                    continue;
                ++safe;
                const auto rgb = c.normal.pixel(x, y);
                QVector3D shading(qRed(rgb) / 127.5f - 1, qGreen(rgb) / 127.5f - 1, qBlue(rgb) / 127.5f - 1);
                shadingRejects += QVector3D::dotProduct(shading.normalized(), -ray.normalized()) <= .5f;
                if (qGray(after.known.pixel(x, y)) < 128) {
                    ++missing;
                    diagnostic.setPixel(x, y, qRgb(255, 20, 30));
                }
            }
        diagnostic.save(dir + QString("/%1-safe-missing.png").arg(yaw));
        metrics.append(QJsonObject{{"yaw", yaw},
                                   {"geometrySafePixels", safe},
                                   {"missingGeometrySafePixels", missing},
                                   {"shadingRejectedGeometrySafePixels", shadingRejects}});
    }
    QFile out(dir + "/metrics.json");
    if (!out.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot write gap probe");
    out.write(QJsonDocument(metrics).toJson());
}

static void savedProjectGapProbe(const QString &path, const QString &root) {
    AiAtlases atlases;
    QList<QJsonObject> records;
    const auto settings = aiLoadProject(path, atlases, records);
    const auto scene =
        loadScene(QDir(QFileInfo(path).absolutePath()).absoluteFilePath(settings["mesh"].toString()));
    const auto pending = settings["pendingCandidate"].toObject();
    if (pending.isEmpty())
        throw std::runtime_error("Gap probe requires a saved candidate");
    auto decode = [](const QJsonValue &value) {
        return QImage::fromData(QByteArray::fromBase64(value.toString().toLatin1()), "PNG");
    };
    const int uv = settings["uv"].toInt();
    auto capture = aiCapture(scene, uv, atlases, aiCameraFromJson(pending["camera"].toObject()));
    const auto confidence = decode(pending["feedbackConfidenceImage"]);
    if (!confidence.isNull())
        capture.confidence = confidence.convertToFormat(QImage::Format_Grayscale16);
    AiProjectionOptions options;
    options.overlay = settings["blendMode"].toString() == "overlay";
    options.colorMatch = settings["colorMatch"].toBool();
    options.anglePower = settings["angleFalloff"].toDouble(2);
    options.strength = settings["projectionStrength"].toDouble(100) / 100;
    options.feather = settings["projectionFeather"].toDouble(6) / 100;
    options.minCosine = std::cos(settings["projectionAngle"].toDouble(60) * 3.14159265359 / 180);
    options.retainedMask = decode(pending["retentionMask"]);
    QSet<QString> materials;
    for (const auto &material : settings["materials"].toArray())
        materials.insert(material.toString());
    const auto strict = aiProject(scene, uv, atlases, capture, decode(pending["candidate"]),
                                  decode(pending["mask"]), materials, false, 8, nullptr, options);
    options.fillGaps = true;
    const auto filled = aiProject(scene, uv, atlases, capture, decode(pending["candidate"]),
                                  decode(pending["mask"]), materials, false, 8, nullptr, options);
    const auto camera = aiCameraFromJson(settings["camera"].toObject());
    const auto before = aiCapture(scene, uv, atlases, camera);
    const auto normal = aiCapture(scene, uv, strict.atlases, camera);
    const auto after = aiCapture(scene, uv, filled.atlases, camera);
    auto hatch = [](const AiCapture &c) {
        auto image = c.partial.copy();
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                if (c.faces[y * image.width() + x] < 0)
                    image.setPixel(x, y, qRgb(15, 21, 28));
                else if (qGray(c.known.pixel(x, y)) < 128)
                    image.setPixel(x, y, ((x + y) % 12) < 6 ? qRgb(240, 20, 30) : qRgb(15, 21, 28));
            }
        return image;
    };
    const QString dir = QFileInfo(root).absoluteFilePath();
    QDir().mkpath(dir);
    hatch(before).save(dir + "/before.png");
    hatch(normal).save(dir + "/strict.png");
    hatch(after).save(dir + "/after.png");
    qint64 extra = 0, total = 0;
    for (auto it = filled.atlases.cbegin(); it != filled.atlases.cend(); ++it) {
        extra += covered(it->coverage) - covered(strict.atlases[it.key()].coverage);
        total += covered(it->coverage) - covered(atlases[it.key()].coverage);
    }
    auto holes = [](const AiCapture &c) {
        qint64 count = 0;
        for (int y = 0; y < c.camera.size; ++y)
            for (int x = 0; x < c.camera.size; ++x)
                count += c.faces[y * c.camera.size + x] >= 0 && qGray(c.known.pixel(x, y)) < 128;
        return count;
    };
    QJsonObject metrics{{"newCoveredTexels", double(total)},
                        {"extraGapTexels", double(extra)},
                        {"beforeVisibleHoles", double(holes(before))},
                        {"afterVisibleHoles", double(holes(after))}};
    QFile out(dir + "/metrics.json");
    if (!out.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot save gap probe metrics");
    out.write(QJsonDocument(metrics).toJson());
}

class AiTests : public QObject {
    Q_OBJECT
  private slots:
    void smartFillWorkflowPreservesPaintAndPendingCamera() {
        qputenv("FORNAIO_GPT_BACKEND", "fake");
        const auto restore = qScopeGuard([] { qunsetenv("FORNAIO_GPT_BACKEND"); });
        AiPanel p;
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.resolution_->setCurrentIndex(p.resolution_->findData(1024));
        const auto material = p.atlases_.firstKey();
        paintAtlas(p.atlases_[material], qRgb(190, 80, 50));
        for (int y = 500; y < 508; ++y)
            for (int x = 500; x < 508; ++x) {
                p.atlases_[material].coverage.scanLine(y)[x] = 0;
                reinterpret_cast<quint16 *>(p.atlases_[material].confidence.scanLine(y))[x] = 0;
            }
        p.layerBase_ = p.atlases_;
        const auto original = p.atlases_;
        p.capture_ = std::make_shared<AiCapture>(capture(*p.scene_, p.atlases_, 128));
        p.candidate_ = solid(128, qRgb(70, 20, 180));
        p.candidateSettings_ = {{"id", "keep-camera-candidate"},
                                {"camera", aiCameraJson(p.capture_->camera)}};
        const auto cameraCandidate = p.candidate_;
        const auto cameraSettings = p.candidateSettings_;
        p.startSmartFill();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 20000);
        QVERIFY2(!p.pendingSmartFill_.empty(), qPrintable(p.status_->text()));
        QVERIFY(!p.smartFillStale());
        QCOMPARE(aiSmartFillRemaining(p.pendingSmartFill_), 1);
        QVERIFY(p.smartFillDialog_);
        QCOMPARE(p.candidate_, cameraCandidate);
        QCOMPARE(p.candidateSettings_, cameraSettings);
        p.smartFillDialog_->grab().save(".ai-qa/smart-fill-dialog.png");
        p.smartFillGenerate();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        QCOMPARE(aiSmartFillRemaining(p.pendingSmartFill_), 0);
        QCOMPARE(p.atlases_.first().color, original.first().color);
        p.smartFillApply();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        QVERIFY(p.pendingSmartFill_.empty());
        QCOMPARE(p.projections_.size(), 1);
        QVERIFY(aiIsRepairLayer(p.projections_[0]));
        QVERIFY(p.atlases_[material].coverage.constScanLine(504)[504]);
        QVERIFY(p.atlases_[material].repair.constScanLine(504)[504]);
        for (int y = 0; y < 1024; ++y)
            for (int x = 0; x < 1024; ++x)
                if (original[material].coverage.constScanLine(y)[x])
                    QCOMPARE(p.atlases_[material].color.pixel(x, y), original[material].color.pixel(x, y));
        QCOMPARE(p.candidate_, cameraCandidate);
        QCOMPARE(p.candidateSettings_, cameraSettings);
        const auto filled = p.atlases_;
        p.undoButton_->click();
        QCOMPARE(p.atlases_.first().coverage, original.first().coverage);
        QVERIFY(p.projections_.empty());
        p.redoButton_->click();
        QCOMPARE(p.atlases_.first().color, filled.first().color);
        p.setLayerVisible(0, false);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        QCOMPARE(p.atlases_.first().coverage, original.first().coverage);
        p.deleteLayer(0);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        QVERIFY(p.projections_.empty());
    }
    void smartFillPendingStaleAndSaveRoundTrip() {
        qputenv("FORNAIO_GPT_BACKEND", "fake");
        const auto restore = qScopeGuard([] { qunsetenv("FORNAIO_GPT_BACKEND"); });
        QTemporaryDir dir;
        AiPanel p;
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.resolution_->setCurrentIndex(p.resolution_->findData(1024));
        const auto material = p.atlases_.firstKey();
        paintAtlas(p.atlases_[material], qRgb(160, 70, 30));
        p.atlases_[material].coverage.scanLine(512)[512] = 0;
        p.layerBase_ = p.atlases_;
        p.startSmartFill();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 20000);
        QVERIFY(!p.pendingSmartFill_.empty());
        p.smartFillGenerate();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        const auto pending = p.pendingSmartFill_;
        const auto accepted = p.atlases_;
        p.atlases_[material].color.setPixel(0, 0, qRgb(0, 0, 255));
        QVERIFY(p.smartFillStale());
        p.smartFillApply();
        QVERIFY(p.projections_.empty());
        QVERIFY(!p.pendingSmartFill_.empty());
        p.atlases_ = accepted;
        QVERIFY(!p.smartFillStale());
        if (p.smartFillDialog_)
            p.smartFillDialog_->close();
        QCoreApplication::processEvents();
        const auto path = dir.filePath("pending-smart.fornaioai");
        auto choose = [path] {
            auto dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->selectFile(path);
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        };
        QTimer::singleShot(0, &p, choose);
        p.saveProject();
        QVERIFY(QFileInfo::exists(path));
        p.pendingSmartFill_ = {};
        QTimer::singleShot(0, &p, choose);
        p.openProject();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        QCOMPARE(p.pendingSmartFill_["patches"], pending["patches"]);
        QVERIFY(!p.smartFillStale());
        p.smartFillApply();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        QCOMPARE(p.projections_.size(), 1);
        QVERIFY(aiIsRepairLayer(p.projections_[0]));
    }
    void smartFillUsesOneGenerationForAllGapsAndCanRetryAfterCancellation() {
        qputenv("FORNAIO_GPT_BACKEND", "fake");
        const auto restore = qScopeGuard([] { qunsetenv("FORNAIO_GPT_BACKEND"); });
        AiPanel p;
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.resolution_->setCurrentIndex(p.resolution_->findData(1024));
        const QString material = p.atlases_.firstKey();
        paintAtlas(p.atlases_[material], qRgb(190, 80, 50));
        for (const int start : {50, 800, 950})
            for (int y = 500; y < 508; ++y)
                for (int x = start; x < start + 8; ++x) {
                    p.atlases_[material].coverage.scanLine(y)[x] = 0;
                    reinterpret_cast<quint16 *>(p.atlases_[material].confidence.scanLine(y))[x] = 0;
                }
        p.layerBase_ = p.atlases_;
        const auto before = p.atlases_;
        p.startSmartFill();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 20000);
        QCOMPARE(p.pendingSmartFill_["version"].toInt(), 2);
        QCOMPARE(aiSmartFillRemaining(p.pendingSmartFill_), 1);
        const auto planned = p.pendingSmartFill_;
        QSignalSpy providerEvents(&p.cloud_, &AiCloudImage::event);
        // Cancellation keeps the one combined request available for an explicit retry.
        p.smartFillGenerate();
        p.cancelJob();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        QCOMPARE(aiSmartFillRemaining(p.pendingSmartFill_), 1);
        QCOMPARE(p.pendingSmartFill_["patches"].toArray().size(), 1);
        QCOMPARE(p.pendingSmartFill_["masks"], planned["masks"]);
        QCOMPARE(p.atlases_[material].color, before[material].color);
        QCOMPARE(p.atlases_[material].coverage, before[material].coverage);
        QVERIFY(!p.smartFillApply_->isEnabled());
        QCOMPARE(p.smartFillGenerate_->text(), QString("Retry generation"));
        p.smartFillGenerate();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        QTest::qWait(150); // No automatic follow-up request may start after the single result.
        int results = 0;
        for (const auto &arguments : providerEvents)
            results += arguments[0].toJsonObject()["event"] == "result";
        QCOMPARE(results, 1);
        QCOMPARE(aiSmartFillRemaining(p.pendingSmartFill_), 0);
        QCOMPARE(p.pendingSmartFill_["patches"].toArray().size(), 1);
        QCOMPARE(p.atlases_[material].color, before[material].color);
        const auto preview = aiSmartFillPreview(before, p.pendingSmartFill_);
        for (const int x : {54, 804, 954})
            QVERIFY(preview[material].coverage.constScanLine(504)[x]);
        p.smartFillApply();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        QVERIFY(p.pendingSmartFill_.empty());
        QCOMPARE(p.projections_.size(), 1);
        for (const int x : {54, 804, 954})
            QVERIFY(p.atlases_[material].coverage.constScanLine(504)[x]);
        for (int y = 0; y < 1024; ++y)
            for (int x = 0; x < 1024; ++x)
                if (before[material].coverage.constScanLine(y)[x])
                    QCOMPARE(p.atlases_[material].color.pixel(x, y), before[material].color.pixel(x, y));
        p.undoButton_->click();
        QCOMPARE(p.atlases_[material].coverage, before[material].coverage);
    }
    void smartFillProcessingAndApplyRemainResponsiveAndCancelAtomically() {
        qputenv("FORNAIO_GPT_BACKEND", "fake");
        const auto restore = qScopeGuard([] { qunsetenv("FORNAIO_GPT_BACKEND"); });
        AiPanel p;
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.resolution_->setCurrentIndex(p.resolution_->findData(2048));
        const QString material = p.atlases_.firstKey();
        paintAtlas(p.atlases_[material], qRgb(190, 80, 50));
        for (int y = 256; y < 1792; ++y)
            for (int x = 256; x < 1792; ++x) {
                p.atlases_[material].coverage.scanLine(y)[x] = 0;
                reinterpret_cast<quint16 *>(p.atlases_[material].confidence.scanLine(y))[x] = 0;
            }
        p.layerBase_ = p.atlases_;
        const auto original = p.atlases_;
        p.startSmartFill();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        int providerResults = 0, cancelledHeartbeats = 0;
        const auto cancelProcessing =
            connect(&p.cloud_, &AiCloudImage::event, &p,
                    [&p, &providerResults, &cancelledHeartbeats](const QJsonObject &event) {
                        if (event.value("event") != "result")
                            return;
                        ++providerResults;
                        QCOMPARE(p.smartFillPhase_, AiPanel::SmartFillPhase::Processing);
                        QTimer::singleShot(0, &p, [&p, &cancelledHeartbeats] {
                            ++cancelledHeartbeats;
                            QVERIFY(p.busy_);
                            QCOMPARE(p.progress_->maximum(), 0);
                            p.cancelJob();
                        });
                    });
        p.smartFillGenerate();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        disconnect(cancelProcessing);
        QCOMPARE(providerResults, 1);
        QCOMPARE(cancelledHeartbeats, 1);
        QCOMPARE(p.atlases_[material].color, original[material].color);
        QCOMPARE(p.atlases_[material].coverage, original[material].coverage);
        QVERIFY(p.projections_.empty());
        QVERIFY(!p.pendingSmartFill_.value("patches")
                     .toArray()
                     .first()
                     .toObject()
                     .value("generated")
                     .toString()
                     .isEmpty());
        QCOMPARE(aiSmartFillRemaining(p.pendingSmartFill_), 1);
        QCOMPARE(p.smartFillGenerate_->text(), QString("Retry processing"));
        QCOMPARE(p.progress_->maximum(), 1000);
        int processingBeats = 0, applyingBeats = 0;
        QTimer heartbeat;
        heartbeat.setInterval(1);
        connect(&heartbeat, &QTimer::timeout, &p, [&] {
            processingBeats += p.smartFillPhase_ == AiPanel::SmartFillPhase::Processing;
            applyingBeats += p.smartFillPhase_ == AiPanel::SmartFillPhase::Applying;
        });
        heartbeat.start();
        QSignalSpy repeatedProviderEvents(&p.cloud_, &AiCloudImage::event);
        p.smartFillGenerate(); // Reuses the checkpointed image; no provider request.
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 60000);
        QVERIFY(processingBeats > 0);
        QCOMPARE(repeatedProviderEvents.size(), 0);
        QCOMPARE(aiSmartFillRemaining(p.pendingSmartFill_), 0);
        QCOMPARE(p.pendingSmartFill_.value("matchingVersion").toInt(), 2);
        p.smartFillApply();
        QVERIFY(p.busy_);
        QTimer::singleShot(0, &p, [&p] { p.cancelJob(); });
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        QVERIFY(p.projections_.empty());
        QVERIFY(!p.pendingSmartFill_.empty());
        QCOMPARE(p.atlases_[material].color, original[material].color);
        QCOMPARE(p.atlases_[material].coverage, original[material].coverage);
        p.smartFillApply();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        heartbeat.stop();
        QVERIFY(applyingBeats > 0);
        QVERIFY(p.pendingSmartFill_.empty());
        QCOMPARE(p.projections_.size(), 1);
        QVERIFY(p.atlases_[material].coverage.constScanLine(1024)[1024]);
        for (int y = 0; y < 2048; ++y)
            for (int x = 0; x < 2048; ++x)
                if (original[material].coverage.constScanLine(y)[x])
                    QCOMPARE(p.atlases_[material].color.pixel(x, y), original[material].color.pixel(x, y));
        QCOMPARE(p.progress_->maximum(), 1000);
    }
    void smartFillCachedSceneProcessingProbe() {
        const QString scenePath = qEnvironmentVariable("FORNAIO_SMART_FILL_SCENE");
        const QString imagePath = qEnvironmentVariable("FORNAIO_SMART_FILL_IMAGE");
        if (scenePath.isEmpty() || imagePath.isEmpty())
            QSKIP("Set FORNAIO_SMART_FILL_SCENE and FORNAIO_SMART_FILL_IMAGE for the cached UI probe.");
        const QByteArray previousBackend = qgetenv("FORNAIO_GPT_BACKEND");
        qputenv("FORNAIO_GPT_BACKEND", "none");
        const auto restore = qScopeGuard([previousBackend] {
            if (previousBackend.isNull())
                qunsetenv("FORNAIO_GPT_BACKEND");
            else
                qputenv("FORNAIO_GPT_BACKEND", previousBackend);
        });
        auto fingerprint = [](const QString &path) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly))
                return QByteArray();
            QCryptographicHash hash(QCryptographicHash::Sha256);
            hash.addData(&file);
            return hash.result();
        };
        const auto originalProjectHash = fingerprint(scenePath);
        const auto originalImageHash = fingerprint(imagePath);
        QVERIFY(!originalProjectHash.isEmpty());
        QVERIFY(!originalImageHash.isEmpty());
        AiPanel p;
        p.resize(1420, 900);
        p.show();
        QVERIFY(QTest::qWaitForWindowExposed(&p));
        auto choose = [&p](const QString &path) {
            const QString absolutePath = QFileInfo(path).absoluteFilePath();
            QTimer::singleShot(0, &p, [absolutePath] {
                auto dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                dialog->setOption(QFileDialog::DontConfirmOverwrite);
                dialog->selectFile(absolutePath);
                QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
            });
        };
        choose(scenePath);
        p.openProject();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 60000);
        QTRY_VERIFY_WITH_TIMEOUT(!p.view_->texturesDirty_, 30000);
        QVERIFY2(p.scene_ && !p.atlases_.empty(), qPrintable(p.status_->text()));
        QVERIFY(p.meshErrors_.empty());
        const auto originalMeshHash = fingerprint(p.meshPath_);
        QVERIFY(!originalMeshHash.isEmpty());
        const auto before = p.atlases_;
        const auto originalLayers = p.projections_;
        const auto originalCandidate = p.candidate_;
        const auto originalCandidateSettings = p.candidateSettings_;
        const auto originalCapture = p.capture_;
        const int originalUndoCount = p.undo_.size();
        QSignalSpy providerEvents(&p.cloud_, &AiCloudImage::event);
        QElapsedTimer phaseTimer, heartbeatClock;
        qint64 processingHeartbeats = 0, applyingHeartbeats = 0;
        qint64 maximumProcessingGap = 0, maximumApplyingGap = 0, previousBeat = 0;
        auto previousPhase = AiPanel::SmartFillPhase::Idle;
        QTimer heartbeat;
        heartbeat.setInterval(10);
        heartbeatClock.start();
        connect(&heartbeat, &QTimer::timeout, &p, [&] {
            const auto now = heartbeatClock.elapsed();
            if (previousPhase == AiPanel::SmartFillPhase::Processing)
                maximumProcessingGap = std::max(maximumProcessingGap, now - previousBeat);
            if (previousPhase == AiPanel::SmartFillPhase::Applying)
                maximumApplyingGap = std::max(maximumApplyingGap, now - previousBeat);
            processingHeartbeats += p.smartFillPhase_ == AiPanel::SmartFillPhase::Processing;
            applyingHeartbeats += p.smartFillPhase_ == AiPanel::SmartFillPhase::Applying;
            previousPhase = p.smartFillPhase_;
            previousBeat = now;
        });
        heartbeat.start();
        phaseTimer.start();
        p.prepareSmartFill();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 60000);
        const qint64 preparationMs = phaseTimer.elapsed();
        QVERIFY2(!p.pendingSmartFill_.empty() && !p.smartFillStale(), qPrintable(p.status_->text()));
        QCOMPARE(aiSmartFillRemaining(p.pendingSmartFill_), 1);
        const auto targetMasks = p.smartFillPreview_->masks;
        QFile cachedImage(imagePath);
        QVERIFY(cachedImage.open(QIODevice::ReadOnly));
        const auto raw = cachedImage.readAll();
        QVERIFY(!QImage::fromData(raw).isNull());
        auto patches = p.pendingSmartFill_.value("patches").toArray();
        auto patch = patches.first().toObject();
        patch["generated"] = QString::fromLatin1(raw.toBase64());
        patch["done"] = false;
        patches[0] = patch;
        p.pendingSmartFill_["patches"] = patches;
        phaseTimer.restart();
        p.smartFillReprocess();
        QVERIFY(p.busy_);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 120000);
        const qint64 processingMs = phaseTimer.elapsed();
        QTest::qWait(20);
        QVERIFY2(aiSmartFillRemaining(p.pendingSmartFill_) == 0, qPrintable(p.status_->text()));
        QVERIFY(processingHeartbeats > 0);
        QCOMPARE(p.atlases_.first().color, before.first().color);
        QCOMPARE(p.candidate_, originalCandidate);
        QCOMPARE(p.candidateSettings_, originalCandidateSettings);
        QCOMPARE(p.capture_.get(), originalCapture.get());
        phaseTimer.restart();
        p.smartFillApply();
        QVERIFY(p.busy_);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 120000);
        QTRY_VERIFY_WITH_TIMEOUT(!p.view_->texturesDirty_, 30000);
        const qint64 applyMs = phaseTimer.elapsed();
        QTest::qWait(20);
        heartbeat.stop();
        QVERIFY2(p.pendingSmartFill_.empty(), qPrintable(p.status_->text()));
        QVERIFY(applyingHeartbeats > 0);
        QCOMPARE(providerEvents.size(), 0);
        QCOMPARE(p.undo_.size(), originalUndoCount + 1);
        QCOMPARE(p.candidate_, originalCandidate);
        QCOMPARE(p.candidateSettings_, originalCandidateSettings);
        QCOMPARE(p.capture_.get(), originalCapture.get());
        QSet<QString> originalIds;
        for (const auto &layer : originalLayers)
            originalIds.insert(layer.value("layerId").toString());
        QList<QJsonObject> preservedLayers;
        for (const auto &layer : p.projections_)
            if (originalIds.contains(layer.value("layerId").toString()))
                preservedLayers.append(layer);
        QCOMPARE(preservedLayers, originalLayers);
        QCOMPARE(p.projections_.size(), originalLayers.size() + 1);
        qint64 knownChanges = 0, outsideMaskChanges = 0, targetedTexels = 0, repairedTexels = 0;
        for (auto it = before.cbegin(); it != before.cend(); ++it) {
            const auto &after = p.atlases_[it.key()];
            const QImage mask = targetMasks.value(it.key());
            for (int y = 0; y < it->color.height(); ++y)
                for (int x = 0; x < it->color.width(); ++x) {
                    if (it->coverage.constScanLine(y)[x])
                        knownChanges += it->color.pixel(x, y) != after.color.pixel(x, y);
                    if (!mask.isNull() && qGray(mask.pixel(x, y))) {
                        ++targetedTexels;
                        repairedTexels += after.coverage.constScanLine(y)[x] != 0;
                    } else
                        outsideMaskChanges +=
                            it->color.pixel(x, y) != after.color.pixel(x, y) ||
                            it->coverage.constScanLine(y)[x] != after.coverage.constScanLine(y)[x];
                }
        }
        QCOMPARE(knownChanges, 0);
        QCOMPARE(outsideMaskChanges, 0);
        QVERIFY(targetedTexels > 0);
        QCOMPARE(repairedTexels, targetedTexels);
        const QString output = qEnvironmentVariable("FORNAIO_SMART_FILL_OUTPUT");
        if (!output.isEmpty()) {
            const auto absoluteOutput = QFileInfo(output).absoluteFilePath();
            QVERIFY(absoluteOutput.compare(QFileInfo(scenePath).absoluteFilePath(), Qt::CaseInsensitive));
            QVERIFY(absoluteOutput.compare(QFileInfo(p.meshPath_).absoluteFilePath(), Qt::CaseInsensitive));
            QVERIFY(absoluteOutput.compare(QFileInfo(imagePath).absoluteFilePath(), Qt::CaseInsensitive));
            QVERIFY(QDir().mkpath(QFileInfo(output).absolutePath()));
            if (p.smartFillDialog_)
                p.smartFillDialog_->close();
            QCoreApplication::processEvents();
            choose(output);
            p.saveProject();
            QVERIFY2(QFileInfo::exists(output), qPrintable(p.status_->text()));
            AiAtlases savedAtlases;
            QList<QJsonObject> savedLayers;
            const auto settings = aiLoadProject(output, savedAtlases, savedLayers);
            QVERIFY(!settings.contains("pendingSmartFill"));
            QCOMPARE(savedLayers, p.projections_);
            if (!originalCandidate.isNull()) {
                const auto saved = settings.value("pendingCandidate").toObject();
                QCOMPARE(saved.value("camera"), originalCandidateSettings.value("camera"));
                const auto savedImage =
                    QImage::fromData(QByteArray::fromBase64(saved.value("candidate").toString().toLatin1()));
                QCOMPARE(savedImage.convertToFormat(QImage::Format_RGB32),
                         originalCandidate.convertToFormat(QImage::Format_RGB32));
            }
        }
        QCOMPARE(fingerprint(scenePath), originalProjectHash);
        QCOMPARE(fingerprint(imagePath), originalImageHash);
        QCOMPARE(fingerprint(p.meshPath_), originalMeshHash);
        const QJsonObject metrics{{"preparationMs", preparationMs},
                                  {"processingMs", processingMs},
                                  {"applyMs", applyMs},
                                  {"processingHeartbeats", processingHeartbeats},
                                  {"applyingHeartbeats", applyingHeartbeats},
                                  {"maximumProcessingHeartbeatGapMs", maximumProcessingGap},
                                  {"maximumApplyHeartbeatGapMs", maximumApplyingGap},
                                  {"targetedTexels", targetedTexels},
                                  {"repairedTexels", repairedTexels},
                                  {"knownPaintChanges", knownChanges},
                                  {"outsideRepairMaskChanges", outsideMaskChanges},
                                  {"providerEvents", providerEvents.size()},
                                  {"originalLayers", originalLayers.size()},
                                  {"finalLayers", p.projections_.size()},
                                  {"projectSha256", QString::fromLatin1(originalProjectHash.toHex())},
                                  {"output", output}};
        qInfo().noquote() << "SMART_FILL_CACHED_UI_PROBE"
                          << QJsonDocument(metrics).toJson(QJsonDocument::Compact);
    }
    void smartFillLegacyPreviewRequiresExplicitOneRequestReplan() {
        qputenv("FORNAIO_GPT_BACKEND", "fake");
        const auto restore = qScopeGuard([] { qunsetenv("FORNAIO_GPT_BACKEND"); });
        AiPanel p;
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.resolution_->setCurrentIndex(p.resolution_->findData(1024));
        const QString material = p.atlases_.firstKey();
        paintAtlas(p.atlases_[material], qRgb(190, 80, 50));
        p.atlases_[material].coverage.scanLine(500)[500] = 0;
        p.layerBase_ = p.atlases_;
        p.startSmartFill();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 20000);
        // Simulate a loaded old multi-crop plan. Its source/masks and any
        // completed repair data must survive a rejected Generate action.
        p.pendingSmartFill_["version"] = 1;
        p.pendingSmartFill_["patches"] =
            QJsonArray{QJsonObject{{"done", true}}, QJsonObject{{"done", false}}};
        QImage repaired(1024, 1024, QImage::Format_RGBA8888);
        repaired.fill(Qt::transparent);
        repaired.setPixelColor(500, 500, QColor(180, 70, 40));
        QByteArray bytes;
        QBuffer buffer(&bytes);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(repaired.save(&buffer, "PNG"));
        p.pendingSmartFill_["repairs"] = QJsonObject{{material, QString::fromLatin1(bytes.toBase64())}};
        const QJsonValue masks = p.pendingSmartFill_["masks"];
        const QJsonValue patches = p.pendingSmartFill_["patches"];
        const QJsonValue repairs = p.pendingSmartFill_["repairs"];
        QSignalSpy providerEvents(&p.cloud_, &AiCloudImage::event);
        p.refreshSmartFill();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        QVERIFY(!p.smartFillGenerate_->isEnabled());
        QVERIFY(p.smartFillApply_->isEnabled());
        p.smartFillGenerate();
        QVERIFY(!p.busy_);
        QCOMPARE(providerEvents.size(), 0);
        QCOMPARE(p.pendingSmartFill_["masks"], masks);
        QCOMPARE(p.pendingSmartFill_["patches"], patches);
        QCOMPARE(p.pendingSmartFill_["repairs"], repairs);
        QVERIFY(p.status_->text().contains("one-request preview"));
        QCOMPARE(p.smartFillReplan_->text(), QString("New one-request preview"));
        p.smartFillApply();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        QVERIFY(p.pendingSmartFill_.empty());
        QCOMPARE(p.atlases_[material].color.pixel(500, 500), qRgb(180, 70, 40));
    }
    void smartFillLegacyMissingCacheAppliesWithoutRebuild() {
        qputenv("FORNAIO_GPT_BACKEND", "fake");
        const auto restore = qScopeGuard([] { qunsetenv("FORNAIO_GPT_BACKEND"); });
        AiPanel p;
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.resolution_->setCurrentIndex(p.resolution_->findData(1024));
        p.parity_ = false;
        const auto base = p.atlases_;
        const auto c = capture(*p.scene_, base, 128);
        QImage mask = c.silhouette.convertToFormat(QImage::Format_RGB32);
        QPainter painter(&mask);
        painter.fillRect(QRect(60, 60, 8, 8), Qt::black);
        painter.end();
        const auto image = solid(128, qRgb(190, 80, 50));
        AiProjectionOptions options;
        options.overlay = true;
        options.feather = 0;
        options.colorMatch = false;
        options.recordLayer = true;
        const auto projected =
            aiProject(*p.scene_, 0, base, c, image, mask, p.selectedMaterials(), false, 8, nullptr, options);
        auto encode = [](const QImage &im) {
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            im.save(&buffer, "PNG");
            return QString::fromLatin1(bytes.toBase64());
        };
        p.projections_ = {
            QJsonObject{{"layerId", "unfrozen-camera"},
                        {"visible", true},
                        {"camera", aiCameraJson(c.camera)},
                        {"candidate", encode(image)},
                        {"mask", encode(mask)},
                        {"materials", QJsonArray::fromStringList(p.selectedMaterials().values())},
                        {"blending", QJsonObject{{"mode", "overlay"},
                                                 {"strength", 1},
                                                 {"feather", 0},
                                                 {"anglePower", 2},
                                                 {"minCosine", .15},
                                                 {"colorMatch", false}}}}};
        p.atlases_ = projected.atlases;
        p.refreshLayers();
        p.refresh();
        const auto before = p.atlases_;
        QVERIFY(covered(before.first().coverage) > 0);
        QVERIFY(!p.projections_[0].contains("uvLayer"));
        p.startSmartFill();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 20000);
        QVERIFY2(!p.pendingSmartFill_.empty(), qPrintable(p.status_->text()));
        QVERIFY(aiSmartFillRemaining(p.pendingSmartFill_) > 0);
        QVERIFY(!p.projections_[0].contains("uvLayer"));
        p.smartFillGenerate();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        QCOMPARE(aiSmartFillRemaining(p.pendingSmartFill_), 0);
        p.smartFillApply();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        QVERIFY2(p.pendingSmartFill_.empty(), qPrintable(p.status_->text()));
        QVERIFY(!p.parity_);
        QCOMPARE(p.projections_.size(), 2);
        QVERIFY(aiIsRepairLayer(p.projections_[0]));
        QCOMPARE(p.projections_[1]["layerId"].toString(), QString("unfrozen-camera"));
        QCOMPARE(p.projections_[1]["uvLayer"].toObject()["version"].toInt(), 1);
        QVERIFY(covered(p.atlases_.first().coverage) > covered(before.first().coverage));
        for (int y = 0; y < 1024; ++y)
            for (int x = 0; x < 1024; ++x)
                if (before.first().coverage.constScanLine(y)[x])
                    QCOMPARE(p.atlases_.first().color.pixel(x, y), before.first().color.pixel(x, y));
        p.undoButton_->click();
        QCOMPARE(p.projections_.size(), 1);
        QVERIFY(!p.projections_[0].contains("uvLayer"));
        QCOMPARE(p.atlases_.first().color, before.first().color);
        QCOMPARE(p.atlases_.first().coverage, before.first().coverage);
    }
    void smartFillKeepsPendingRegenerationTargetAcrossInsertionAndReopen() {
        qputenv("FORNAIO_GPT_BACKEND", "fake");
        const auto restore = qScopeGuard([] { qunsetenv("FORNAIO_GPT_BACKEND"); });
        QTemporaryDir dir;
        AiPanel p;
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.resolution_->setCurrentIndex(p.resolution_->findData(1024));
        const auto material = p.atlases_.firstKey();
        QImage rgba(1024, 1024, QImage::Format_RGBA8888);
        rgba.fill(QColor(190, 80, 50));
        for (int y = 500; y < 508; ++y)
            for (int x = 500; x < 508; ++x)
                rgba.scanLine(y)[x * 4 + 3] = 0;
        AiLayerContribution contribution;
        contribution.materials[material] = rgba;
        p.projections_ = {QJsonObject{{"kind", "projection"},
                                      {"layerId", "camera-to-replace"},
                                      {"visible", true},
                                      {"materials", QJsonArray{material}},
                                      {"blending", QJsonObject{{"mode", "blend"}, {"strength", 1}}},
                                      {"uvLayer", aiEncodeProjectionLayer(contribution, 1024)}}};
        p.atlases_ = aiComposeLayerStack(p.layerBase_, p.projections_, true);
        p.refreshLayers();
        p.capture_ = std::make_shared<AiCapture>(capture(*p.scene_, p.atlases_, 128));
        p.candidateSettings_ = {{"id", "replacement"}, {"camera", aiCameraJson(p.capture_->camera)}};
        const auto output = dir.filePath("replacement.png");
        QVERIFY(solid(128, qRgb(20, 40, 220)).save(output));
        p.setBusy(true);
        p.workerEvent({{"event", "result"}, {"id", "replacement"}, {"output", output}, {"alignment", 1.0}});
        p.regenerateIndex_ = 0; // Compatibility with candidates that predate the stable target metadata.
        const auto pendingCamera = p.candidate_;
        p.startSmartFill();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 20000);
        QVERIFY2(!p.pendingSmartFill_.empty(), qPrintable(p.status_->text()));
        p.smartFillGenerate();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        p.smartFillApply();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 30000);
        QVERIFY2(p.pendingSmartFill_.empty(), qPrintable(p.status_->text()));
        QCOMPARE(p.projections_.size(), 2);
        QCOMPARE(p.regenerateIndex_, 1);
        QCOMPARE(p.regenerateLayerId_, QString("camera-to-replace"));
        QCOMPARE(p.candidate_, pendingCamera);
        QCOMPARE(p.candidateSettings_["regenerateLayerId"].toString(), QString("camera-to-replace"));
        const QJsonObject repair = p.projections_[0];
        p.undoButton_->click();
        QCOMPARE(p.regenerateIndex_, 0);
        QCOMPARE(p.candidate_, pendingCamera);
        p.redoButton_->click();
        QCOMPARE(p.regenerateIndex_, 1);
        if (p.smartFillDialog_)
            p.smartFillDialog_->close();
        QCoreApplication::processEvents();
        const auto path = dir.filePath("pending-regeneration.fornaioai");
        auto choose = [path] {
            auto dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->selectFile(path);
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        };
        QTimer::singleShot(0, &p, choose);
        p.saveProject();
        QVERIFY(QFileInfo::exists(path));
        QTimer::singleShot(0, &p, choose);
        p.openProject();
        QCOMPARE(p.regenerateIndex_, 1);
        QCOMPARE(p.regenerateLayerId_, QString("camera-to-replace"));
        QCOMPARE(p.candidate_, pendingCamera);
        p.projectCandidate(false);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        QCOMPARE(p.projections_.size(), 2);
        QCOMPARE(p.projections_[0], repair);
        QCOMPARE(p.projections_[1]["layerId"].toString(), QString("camera-to-replace"));
        QCOMPARE(p.regenerateIndex_, -1);
        QVERIFY(p.regenerateLayerId_.isEmpty());
        QVERIFY(qBlue(p.atlases_[material].color.pixel(512, 512)) > 200);
    }
    void parityLayerDeleteReorderIsolationAndHistory() {
        QTemporaryDir storage;
        AiPanel p;
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.resolution_->setCurrentIndex(p.resolution_->findData(1024));
        QVERIFY(p.parity_);
        const QString material = p.atlases_.firstKey();
        const auto base = p.atlases_;
        auto record = [&](QColor color, const QString &id) {
            QImage image(1024,1024,QImage::Format_RGBA8888);image.fill(color);
            AiLayerContribution contribution;contribution.materials[material]=image;
            return QJsonObject{{"layerId",id},{"visible",true},
                               {"blending",QJsonObject{{"mode","blend"},{"strength",1}}},
                               {"uvLayer",aiEncodeProjectionLayer(contribution,1024)}};
        };
        p.projections_={record(Qt::red,"red"),record(Qt::blue,"blue")};
        p.atlases_=aiComposeParityLayers(base,p.projections_);
        const auto both=p.atlases_;
        p.refreshLayers();p.refresh();
        QVERIFY(p.findChild<QPushButton *>("aiDeleteLayer")->isEnabled());
        p.updatePreview();p.resize(1420,900);p.show();QVERIFY(QTest::qWaitForWindowExposed(&p));
        QVERIFY(p.grab().save(".ai-qa/parity-layer-controls.png"));
        p.moveLayer(0,1);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_,10000);
        QCOMPARE(p.projections_[1]["layerId"].toString(),QString("red"));
        QCOMPARE(p.atlases_[material].color,both[material].color);
        p.applyLayerEdit(p.projections_,-1,1,"Isolate test","red");
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_,10000);
        QCOMPARE(p.atlases_[material].color.pixel(512,512),qRgb(255,0,0));
        p.deleteLayer(1);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_,10000);
        QCOMPARE(p.projections_.size(),1);QVERIFY(p.isolatedLayer_.isEmpty());
        QCOMPARE(p.atlases_[material].color.pixel(512,512),qRgb(0,0,255));
        p.undoButton_->click();QCOMPARE(p.projections_.size(),2);QCOMPARE(p.isolatedLayer_,QString("red"));
        p.redoButton_->click();QCOMPARE(p.projections_.size(),1);QVERIFY(p.isolatedLayer_.isEmpty());
        p.deleteLayer(0);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_,10000);
        QVERIFY(p.projections_.empty());QCOMPARE(p.selectedLayer(),-1);
        QCOMPARE(p.atlases_[material].color,base[material].color);
        QCOMPARE(p.atlases_[material].coverage,base[material].coverage);
        QCOMPARE(p.atlases_[material].confidence,base[material].confidence);
        p.undoButton_->click();QCOMPARE(p.projections_.size(),1);
        p.setExternalBusy(true);p.deleteLayer(0);QCOMPARE(p.projections_.size(),1);
        QVERIFY(!p.findChild<QPushButton *>("aiDeleteLayer")->isEnabled());p.setExternalBusy(false);
        p.setLayerVisible(0,false);QTRY_VERIFY_WITH_TIMEOUT(!p.busy_,10000);
        p.deleteLayer(0);QTRY_VERIFY_WITH_TIMEOUT(!p.busy_,10000);QVERIFY(p.projections_.empty());
        const auto path=storage.filePath("deleted.fornaioai");
        aiSaveProject(path,{{"projectionBackend","modddif-v1"}},p.atlases_,p.projections_);
        AiAtlases loaded;QList<QJsonObject> saved;aiLoadProject(path,loaded,saved);QVERIFY(saved.empty());
        QVERIFY(QFileInfo::exists("samples/single_card.obj"));
    }
    void legacyRebuildIsExplicitAtomicAndUndoable() {
        AiPanel p;QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.resolution_->setCurrentIndex(p.resolution_->findData(1024));p.parity_=false;
        const auto base=p.atlases_;const auto c=capture(*p.scene_,base,128);
        AiProjectionOptions options;options.overlay=true;options.recordLayer=true;
        auto projected=aiProject(*p.scene_,0,base,c,solid(128,qRgb(200,40,20)),c.silhouette,p.selectedMaterials(),false,8,nullptr,options);
        auto encode=[](const QImage &image){QByteArray bytes;QBuffer buffer(&bytes);buffer.open(QIODevice::WriteOnly);image.save(&buffer,"PNG");return QString::fromLatin1(bytes.toBase64());};
        QJsonObject record{{"layerId","legacy"},{"camera",aiCameraJson(c.camera)},
                           {"candidate",encode(solid(128,qRgb(200,40,20)))},{"mask",encode(c.silhouette)},
                           {"materials",QJsonArray::fromStringList(p.selectedMaterials().values())},
                           {"blending",QJsonObject{{"mode","overlay"}}},{"uvLayer",projected.layer}};
        p.projections_={record};p.atlases_=projected.atlases;p.refreshLayers();p.refresh();
        const auto before=p.atlases_;const int undoCount=p.undo_.size();
        p.projectionError_="test context unavailable";p.rebuildProjectionLayers();
        QVERIFY(!p.busy_);QVERIFY(!p.parity_);QCOMPARE(p.atlases_.first().color,before.first().color);
        p.projectionError_.clear();
        p.rebuildProjectionLayers();QTRY_VERIFY_WITH_TIMEOUT(!p.busy_,10000);
        QVERIFY2(p.parity_,qPrintable(p.status_->text()));
        QCOMPARE(p.projections_[0]["uvLayer"].toObject()["version"].toInt(),2);
        QCOMPARE(p.undo_.size(),undoCount+1);
        p.undoButton_->click();QVERIFY(!p.parity_);QCOMPARE(p.atlases_.first().color,before.first().color);
        QCOMPARE(p.projections_[0]["uvLayer"].toObject()["version"].toInt(),1);
        p.redoButton_->click();QVERIFY(p.parity_);p.undoButton_->click();
        p.projections_[0]["candidate"]="invalid";const auto failed=p.projections_;const auto size=p.undo_.size();
        p.rebuildProjectionLayers();QTRY_VERIFY_WITH_TIMEOUT(!p.busy_,10000);
        QVERIFY(!p.parity_);QCOMPARE(p.undo_.size(),size);QCOMPARE(p.projections_,failed);
        QCOMPARE(p.atlases_.first().color,before.first().color);
        p.projections_[0]=record;p.rebuildProjectionLayers();p.cancelJob();QTRY_VERIFY_WITH_TIMEOUT(!p.busy_,10000);
        QVERIFY(!p.parity_);QCOMPARE(p.atlases_.first().color,before.first().color);
    }
    void parityPreservesOriginalCloudSourceAndLayerEdits() {
        QTemporaryDir storage;AiPanel p;QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.resolution_->setCurrentIndex(p.resolution_->findData(1024));
        p.capture_=std::make_shared<AiCapture>(capture(*p.scene_,p.atlases_,128));
        p.lock_=std::make_shared<AiLock>(aiLockedGuide(*p.capture_));
        p.candidateSettings_={{"id","original"},{"model","gpt-image-2"},{"camera",aiCameraJson(p.capture_->camera)}};
        QImage original(256,256,QImage::Format_RGBA8888);original.fill(QColor(200,60,20,128));
        const auto path=storage.filePath("original.png");QVERIFY(original.save(path));
        const auto originalBytes=[&]{QFile file(path);file.open(QIODevice::ReadOnly);return file.readAll();}();
        p.setBusy(true);p.workerEvent({{"event","result"},{"id","original"},{"output",path},{"alignment",1.0}});
        auto source=QImage::fromData(QByteArray::fromBase64(p.candidateSettings_["sourceImage"].toString().toLatin1()),"PNG");
        QCOMPARE(source.size(),original.size());QCOMPARE(source.pixelColor(100,100),original.pixelColor(100,100));
        QCOMPARE(p.candidate_.pixelColor(64,64).alpha(),128);
        p.project_->click();QTRY_VERIFY_WITH_TIMEOUT(!p.busy_,10000);
        QCOMPARE(p.projections_.size(),1);QCOMPARE(p.projections_[0]["uvLayer"].toObject()["version"].toInt(),2);
        const auto baked=p.projections_[0]["uvLayer"];
        auto changed=p.projections_;auto blend=changed[0]["blending"].toObject();blend["strength"]=.5;changed[0]["blending"]=blend;
        p.applyLayerEdit(changed,-1,0,"Strength",{});QTRY_VERIFY_WITH_TIMEOUT(!p.busy_,10000);
        QCOMPARE(p.projections_[0]["uvLayer"],baked);
        changed=p.projections_;blend=changed[0]["blending"].toObject();blend["mode"]="overlay";changed[0]["blending"]=blend;
        p.applyLayerEdit(changed,0,0,"Mode",{});QTRY_VERIFY_WITH_TIMEOUT(!p.busy_,10000);
        QVERIFY2(p.projections_[0]["uvLayer"]!=baked,qPrintable(p.status_->text()));
        p.deleteLayer(0);QTRY_VERIFY_WITH_TIMEOUT(!p.busy_,10000);
        QFile unchanged(path);QVERIFY(unchanged.open(QIODevice::ReadOnly));QCOMPARE(unchanged.readAll(),originalBytes);
    }
    void deletingLegacyLayerFreezesOriginalStackFirst() {
        AiPanel p;QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.resolution_->setCurrentIndex(p.resolution_->findData(1024));p.parity_=false;
        const auto base=p.atlases_;const auto c=capture(*p.scene_,base,128);
        auto encode=[](const QImage &image){QByteArray data;QBuffer buffer(&data);buffer.open(QIODevice::WriteOnly);image.save(&buffer,"PNG");return QString::fromLatin1(data.toBase64());};
        auto record=[&](const QString &id,QRgb color){return QJsonObject{
            {"layerId",id},{"camera",aiCameraJson(c.camera)},{"candidate",encode(solid(128,color))},
            {"mask",encode(c.silhouette)},{"materials",QJsonArray::fromStringList(p.selectedMaterials().values())},
            {"blending",QJsonObject{{"mode","overlay"},{"strength",.5},{"feather",.06}}}};};
        const QList<QJsonObject> records={record("old-red",qRgb(255,0,0)),record("old-blue",qRgb(0,0,255))};
        const auto frozen=aiPrepareLayers(*p.scene_,0,base,records);
        const auto expected=aiComposeLayers(base,{frozen[1]});
        p.projections_=records;p.atlases_=aiComposeLayers(base,frozen);p.refreshLayers();p.refresh();
        p.deleteLayer(0);QTRY_VERIFY_WITH_TIMEOUT(!p.busy_,10000);
        QCOMPARE(p.projections_.size(),1);QCOMPARE(p.projections_[0]["uvLayer"],frozen[1]["uvLayer"]);
        QCOMPARE(p.atlases_.first().color,expected.first().color);
        QCOMPARE(p.atlases_.first().confidence,expected.first().confidence);
    }
    void frozenProjectionLayersComposeExactly() {
        const auto s = plane();
        const auto base = aiEmptyAtlases(s, 128);
        auto c = capture(s, base);
        AiProjectionOptions options;
        options.overlay = true;
        options.colorMatch = false;
        options.recordLayer = true;
        auto mask = solid(128, qRgb(255, 255, 255));
        const auto first = aiProject(s, 0, base, c, solid(128, qRgb(210, 20, 10)), mask, {"front"}, false, 8,
                                     nullptr, options);
        options.strength = .65f;
        options.colorMatch = true;
        c = capture(s, first.atlases);
        auto second = aiProject(s, 0, first.atlases, c, solid(128, qRgb(10, 30, 230)), mask, {"front"}, false,
                                8, nullptr, options);
        QList<QJsonObject> records{{{"uvLayer", first.layer}}, {{"uvLayer", second.layer}}};
        auto all = aiComposeLayers(base, records);
        QCOMPARE(all["front"].color, second.atlases["front"].color);
        QCOMPARE(all["front"].coverage, second.atlases["front"].coverage);
        QCOMPARE(all["front"].confidence, second.atlases["front"].confidence);
        records[1]["visible"] = false;
        auto lower = aiComposeLayers(base, records);
        QCOMPARE(lower["front"].color, first.atlases["front"].color);
        QCOMPARE(lower["front"].confidence, first.atlases["front"].confidence);
        records[0]["visible"] = false;
        auto empty = aiComposeLayers(base, records);
        QCOMPARE(empty["front"].color, base["front"].color);
        QCOMPARE(covered(empty["front"].coverage), 0);
        records[1]["visible"] = true;
        auto upper = aiComposeLayers(base, records);
        QVERIFY(qBlue(upper["front"].color.pixel(64, 64)) > 200);
        QVERIFY(qRed(upper["front"].color.pixel(64, 64)) < 100);
        records[0]["visible"] = true;
        QCOMPARE(aiComposeLayers(base, records)["front"].color, second.atlases["front"].color);
        // Freeze both fill-boundary color correction and quality blending as well.
        auto partial = base;
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 64; ++x) {
                partial["front"].color.setPixel(x, y, qRgb(170, 30, 15));
                partial["front"].coverage.scanLine(y)[x] = 255;
                reinterpret_cast<quint16 *>(partial["front"].confidence.scanLine(y))[x] = 60000;
            }
        for (bool fill : {false, true}) {
            options.overlay = false;
            const auto projected =
                aiProject(s, 0, partial, capture(s, partial), solid(128, qRgb(10, 30, 230)), mask, {"front"},
                          fill, 8, nullptr, options);
            const auto composed = aiComposeLayers(partial, {{{"uvLayer", projected.layer}}});
            QCOMPARE(composed["front"].color, projected.atlases["front"].color);
            QCOMPARE(composed["front"].confidence, projected.atlases["front"].confidence);
        }
        std::atomic_bool cancel{true};
        QVERIFY_EXCEPTION_THROWN(aiComposeLayers(base, records, &cancel), std::runtime_error);
        auto broken = records;
        auto layer = broken[0]["uvLayer"].toObject();
        auto blocks = layer["materials"].toArray();
        auto block = blocks[0].toObject();
        block["strokes"] = "invalid";
        blocks[0] = block;
        layer["materials"] = blocks;
        broken[0]["uvLayer"] = layer;
        QVERIFY_EXCEPTION_THROWN(aiComposeLayers(base, broken), std::runtime_error);
    }
    void retainedPaintIsOwnedByItsOriginalLayer() {
        const auto s = plane();
        const auto base = aiEmptyAtlases(s, 128);
        AiProjectionOptions options;
        options.overlay = true;
        options.colorMatch = false;
        options.recordLayer = true;
        auto c = capture(s, base);
        const auto mask = solid(128, qRgb(255, 255, 255));
        const auto first = aiProject(s, 0, base, c, solid(128, qRgb(220, 20, 10)), mask, {"front"}, false, 8,
                                     nullptr, options);
        c = capture(s, first.atlases);
        options.retainedMask = mask;
        const auto kept = aiProject(s, 0, first.atlases, c, solid(128, qRgb(0, 0, 255)), mask, {"front"},
                                    false, 8, nullptr, options);
        QCOMPARE(kept.changed, 0);
        const auto isolated = aiComposeLayers(base, {{{"uvLayer", kept.layer}}});
        QCOMPARE(covered(isolated["front"].coverage), 0);
        QCOMPARE(isolated["front"].color, base["front"].color);
    }
    void legacyProjectionLayersMigrateAndSave() {
        const auto s = plane();
        const auto base = aiEmptyAtlases(s, 128);
        const auto c = capture(s, base);
        auto encode = [](const QImage &image) {
            QByteArray bytes;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            return QString::fromLatin1(bytes.toBase64());
        };
        QJsonObject record{{"camera", aiCameraJson(c.camera)},
                           {"candidate", encode(solid(128, qRgb(20, 50, 180)))},
                           {"mask", encode(solid(128, qRgb(255, 255, 255)))},
                           {"materials", QJsonArray{"front"}},
                           {"blending", QJsonObject{{"mode", "overlay"}, {"colorMatch", false}}}};
        auto records = aiPrepareLayers(s, 0, base, {record});
        QVERIFY(records[0]["uvLayer"].toObject()["version"].toInt() == 1);
        const auto painted = aiComposeLayers(base, records);
        QVERIFY(covered(painted["front"].coverage) > 0);
        records[0]["visible"] = false;
        QTemporaryDir dir;
        const auto path = dir.filePath("layers.fornaioai");
        aiSaveProject(path, {{"layerBase", aiSerializeAtlases(base)}}, base, records);
        AiAtlases loaded;
        QList<QJsonObject> restored;
        const auto settings = aiLoadProject(path, loaded, restored);
        const auto restoredBase = aiDeserializeAtlases(settings["layerBase"].toArray());
        QCOMPARE(restored[0]["visible"].toBool(true), false);
        QCOMPARE(aiComposeLayers(restoredBase, restored)["front"].color, base["front"].color);
        restored[0]["visible"] = true;
        QCOMPARE(aiComposeLayers(restoredBase, restored)["front"].color, painted["front"].color);
    }
    void materialValidation() {
        Scene s = plane();
        QVERIFY(validateAiMesh(s, 0).empty());
        Scene other = plane(0, 1, 2);
        s.tris.insert(s.tris.end(), other.tris.begin(), other.tris.end());
        QVERIFY(validateAiMesh(s, 0).empty());
        s.tris.back().material = 0;
        QVERIFY(!validateAiMesh(s, 0).empty());
        QVERIFY(!validateAiMesh(plane(), 1).empty());
    }
    void cameraAndProjection() {
        Scene s = plane();
        auto a = aiEmptyAtlases(s, 64);
        auto c = capture(s, a);
        QVERIFY(c.faces[64 * 128 + 64] >= 0);
        QCOMPARE(qRed(c.normal.pixel(64, 64)), 128);
        QCOMPARE(qBlue(c.normal.pixel(64, 64)), 255);
        QImage image(128, 128, QImage::Format_RGB32);
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x)
                image.setPixel(x, y, qRgb(x * 2, 0, y * 2));
        auto p = aiProject(s, 0, a, c, image, solid(128, qRgb(255, 255, 255)), {"front"}, false, 0);
        QVERIFY(p.changed > 3000);
        auto color = p.atlases["front"].color;
        QVERIFY(qRed(color.pixel(16, 32)) < qRed(color.pixel(48, 32)));
        QVERIFY(qBlue(color.pixel(32, 16)) < qBlue(color.pixel(32, 48)));
        QCOMPARE(covered(a["front"].coverage), 0); // Input snapshots remain immutable.
        auto full = capture(s, p.atlases);
        QVERIFY(aiNewCoverage(s, 0, p.atlases, full, {"front"}) < .02);
        auto roundtrip = aiCameraFromJson(aiCameraJson(c.camera));
        QCOMPARE(roundtrip.view, c.camera.view);
        QCOMPARE(roundtrip.projection, c.camera.projection);
    }
    void narrowLensPreservesFramingAndSavedCamera() {
        auto scene = std::make_shared<Scene>(plane());
        TransferViewport view;
        view.setMesh(scene, 0);
        OrbitCamera orbit;
        orbit.yaw = orbit.pitch = 0;
        orbit.distance = 5;
        orbit.pan = {.2f, -.1f, 0};
        view.setCamera(orbit);
        const auto wide = aiCamera(*scene, view.camera(), 128);
        view.setFieldOfView(20);
        const auto narrow = aiCamera(*scene, view.camera(), 128);
        const auto wideMvp = wide.projection * wide.view;
        const auto narrowMvp = narrow.projection * narrow.view;
        for (const auto &point : {QVector3D(), QVector3D(.7f, .6f, 0), QVector3D(-.6f, -.7f, 0)}) {
            const auto a = wideMvp.map(point), b = narrowMvp.map(point);
            QVERIFY(std::abs(a.x() - b.x()) < 1e-5);
            QVERIFY(std::abs(a.y() - b.y()) < 1e-5);
        }
        auto ratio = [](const QMatrix4x4 &mvp) {
            return mvp.map(QVector3D(.5f, .1f, .8f)).x() / mvp.map(QVector3D(.5f, .1f, -.8f)).x();
        };
        QVERIFY(ratio(narrowMvp) < ratio(wideMvp));
        const auto saved = aiCameraJson(narrow);
        QCOMPARE(aiCameraFromJson(saved).orbit.fieldOfView, 20.f);
        auto legacy = aiCameraJson(wide);
        legacy.remove("fieldOfView");
        QVERIFY(std::abs(aiCameraFromJson(legacy).orbit.fieldOfView - 40) < .001f);
        for (const auto &camera : aiAutomaticCameras(*scene, view.camera()))
            QCOMPARE(camera.fieldOfView, 20.f);
        const auto fitted = aiFrameCamera(*scene, view.camera());
        const auto fitMvp = aiCamera(*scene, fitted).projection * aiCamera(*scene, fitted).view;
        float extent = 0;
        for (const auto &t : scene->tris)
            for (const auto &p : t.p) {
                const auto q = fitMvp.map(QVector3D(p.x, p.y, p.z));
                extent = std::max({extent, std::abs(q.x()), std::abs(q.y())});
                QVERIFY(std::abs(q.x()) <= .95001f && std::abs(q.y()) <= .95001f);
            }
        QVERIFY(std::abs(extent - .95f) < 1e-5);
        view.fit();
        QCOMPARE(view.camera().fieldOfView, 20.f);
        view.setFieldOfView(40);
        QVERIFY(std::abs(view.camera().distance - scene->scale * 1.2f) < 1e-5);
    }
    void linkedViewportsPropagateCameraAndLensWithoutFeedbackLoop() {
        TransferViewport source, target;
        connect(&source, &TransferViewport::cameraChanged, &target, &TransferViewport::setCamera);
        connect(&target, &TransferViewport::cameraChanged, &source, &TransferViewport::setCamera);
        QSignalSpy fromSource(&source, &TransferViewport::cameraChanged);
        QSignalSpy fromTarget(&target, &TransferViewport::cameraChanged);
        auto camera = source.camera();
        camera.yaw = 55;
        camera.pan = {.2f, .3f, 0};
        source.setCamera(camera);
        QCOMPARE(fromSource.size(), 1);
        QCOMPARE(fromTarget.size(), 1);
        QCOMPARE(target.camera().yaw, 55.f);
        source.setCamera(camera);
        QCOMPARE(fromSource.size(), 1);
        source.setFieldOfView(20);
        QCOMPARE(fromSource.size(), 2);
        QCOMPARE(fromTarget.size(), 2);
        QCOMPARE(target.camera().fieldOfView, 20.f);
        QCOMPARE(target.camera().distance, source.camera().distance);
    }
    void grazingSurfacesStayUnpaintedInsteadOfStretching() {
        for (const float degrees : {45.f, 75.f}) {
            auto scene = plane();
            QMatrix4x4 rotate;
            rotate.rotate(degrees, 0, 1, 0);
            for (auto &t : scene.tris)
                for (int k = 0; k < 3; ++k) {
                    const auto p = rotate.map(QVector3D(t.p[k].x, t.p[k].y, t.p[k].z));
                    const auto n = rotate.mapVector(QVector3D(0, 0, 1));
                    t.p[k] = {p.x(), p.y(), p.z()};
                    t.n[k] = {n.x(), n.y(), n.z()};
                }
            auto base = aiEmptyAtlases(scene, 128);
            const auto c = capture(scene, base, 256);
            AiProjectionOptions options;
            options.overlay = true;
            options.feather = 0;
            const auto image = solid(256, qRgb(40, 90, 160));
            const auto permissive =
                aiProject(scene, 0, base, c, image, c.silhouette, {"front"}, false, 0, nullptr, options);
            QVERIFY(permissive.changed > 0);
            options.minCosine = .5f;
            options.fillGaps = true;
            const auto sharp =
                aiProject(scene, 0, base, c, image, c.silhouette, {"front"}, false, 8, nullptr, options);
            if (degrees > 60) {
                QCOMPARE(sharp.changed, qint64(0));
                QCOMPARE(covered(sharp.atlases["front"].coverage), qint64(0));
                QCOMPARE(sharp.atlases["front"].color, base["front"].color);
                QCOMPARE(aiViewContribution(scene, 0, c, {"front"}, options), 0.0);
            } else
                QVERIFY(sharp.changed > 128 * 128 * .9);
        }
    }
    void orthographicFramingFillsVisibleBounds() {
        auto scene = std::make_shared<Scene>(plane(0, 0, .5f)); // Off-centre: framing must pan.
        OrbitCamera orbit;
        orbit.yaw = orbit.pitch = 0;
        orbit.fieldOfView = 20;
        orbit.orthographic = true;
        const auto framed = aiFrameCamera(*scene, orbit);
        const auto c = aiCamera(*scene, framed, 128);
        float x0 = INFINITY, x1 = -INFINITY;
        for (const auto &t : scene->tris)
            for (const auto &p : t.p) {
                const auto q = (c.projection * c.view).map(QVector3D(p.x, p.y, p.z));
                x0 = std::min(x0, q.x());
                x1 = std::max(x1, q.x());
            }
        QVERIFY(std::abs(x0 + .95f) < 1e-4f && std::abs(x1 - .95f) < 1e-4f);
        QCOMPARE(aiCameraFromJson(aiCameraJson(c)).orbit.orthographic, true);
        // A close-up keeps framing only the part of the mesh already inside the view.
        auto closeUp = framed;
        closeUp.distance *= .25f;
        const auto refined = aiFrameCamera(*scene, closeUp, true);
        QVERIFY(refined.distance <= closeUp.distance * 1.0001f / .95f);
        // Parallel rays see the whole front face, so an orthographic capture covers it completely.
        auto base = aiEmptyAtlases(*scene, 64);
        const auto capture = aiCapture(*scene, 0, base, c);
        const auto result = aiProject(*scene, 0, base, capture, solid(128, qRgb(200, 30, 30)), capture.silhouette,
                                      {"front"}, false, 0);
        QCOMPARE(covered(result.atlases["front"].coverage), qint64(64 * 64));
    }
    void coverAllPaintsGrazingEmptySurfacesAtLowConfidence() {
        auto scene = plane();
        QMatrix4x4 rotate;
        rotate.rotate(75, 0, 1, 0);
        for (auto &t : scene.tris)
            for (int k = 0; k < 3; ++k) {
                const auto p = rotate.map(QVector3D(t.p[k].x, t.p[k].y, t.p[k].z));
                const auto n = rotate.mapVector(QVector3D(0, 0, 1));
                t.p[k] = {p.x(), p.y(), p.z()};
                t.n[k] = {n.x(), n.y(), n.z()};
            }
        auto base = aiEmptyAtlases(scene, 128);
        const auto c = capture(scene, base, 256);
        AiProjectionOptions options;
        options.overlay = true;
        options.minCosine = .5f;
        options.coverAll = true;
        const auto image = solid(256, qRgb(40, 90, 160));
        const auto painted = aiProject(scene, 0, base, c, image, c.silhouette, {"front"}, false, 0, nullptr, options);
        QVERIFY(covered(painted.atlases["front"].coverage) > 128 * 128 * .9);
        QVERIFY(aiViewContribution(scene, 0, c, {"front"}, options) > .9);
        // Already painted texels keep the angle limit: a second grazing view changes nothing.
        const auto again =
            aiProject(scene, 0, painted.atlases, c, solid(256, qRgb(250, 250, 0)), c.silhouette, {"front"}, false,
                      0, nullptr, options);
        QCOMPARE(again.changed, qint64(0));
    }
    void enclosedGapFillPreservesPaintAndLayerHistory() {
        auto scene = plane();
        QMatrix4x4 rotate;
        rotate.rotate(75, 0, 1, 0);
        for (auto &t : scene.tris)
            for (int k = 0; k < 3; ++k) {
                const auto p = rotate.map(QVector3D(t.p[k].x, t.p[k].y, t.p[k].z));
                const auto n = rotate.mapVector(QVector3D(0, 0, 1));
                t.p[k] = {p.x(), p.y(), p.z()};
                t.n[k] = {n.x(), n.y(), n.z()};
            }
        auto base = aiEmptyAtlases(scene, 128);
        paintAtlas(base["front"], qRgb(20, 50, 180), .95f);
        // Freeze the camera before clearing coverage: projection must use live UV coverage.
        const auto stale = capture(scene, base, 512);
        for (int y = 48; y < 80; ++y)
            for (int x = 48; x < 80; ++x) {
                base["front"].coverage.scanLine(y)[x] = 0;
                reinterpret_cast<quint16 *>(base["front"].confidence.scanLine(y))[x] = 0;
            }
        AiProjectionOptions options;
        options.overlay = true;
        options.colorMatch = false;
        options.feather = 0;
        options.minCosine = .5f;
        options.recordLayer = true;
        const auto image = solid(512, qRgb(190, 80, 30));
        const auto strict =
            aiProject(scene, 0, base, stale, image, stale.silhouette, {"front"}, false, 0, nullptr, options);
        QCOMPARE(strict.changed, qint64(0));
        options.fillGaps = true;
        const auto filled =
            aiProject(scene, 0, base, stale, image, stale.silhouette, {"front"}, false, 0, nullptr, options);
        QCOMPARE(covered(filled.atlases["front"].coverage), qint64(128 * 128));
        QCOMPARE(filled.changed, qint64(32 * 32));
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x) {
                const auto &a = filled.atlases["front"];
                if (x >= 48 && x < 80 && y >= 48 && y < 80) {
                    QCOMPARE(a.color.pixel(x, y), image.pixel(0, 0));
                    QVERIFY(atlasQuality(a, x, y) > 0 && atlasQuality(a, x, y) <= .07501f);
                } else {
                    QCOMPARE(a.color.pixel(x, y), base["front"].color.pixel(x, y));
                    QCOMPARE(atlasQuality(a, x, y), atlasQuality(base["front"], x, y));
                }
            }
        QList<QJsonObject> records{{{"uvLayer", filled.layer}}};
        const auto composed = aiComposeLayers(base, records);
        QCOMPARE(composed["front"].color, filled.atlases["front"].color);
        QCOMPARE(composed["front"].coverage, filled.atlases["front"].coverage);
        QCOMPARE(composed["front"].confidence, filled.atlases["front"].confidence);
        records[0]["visible"] = false;
        QCOMPARE(aiComposeLayers(base, records)["front"].coverage, base["front"].coverage);
        // An old candidate's retained snapshot cannot promote provisional fill
        // to the strong confidence of paint that has since been removed.
        options.retainedMask = stale.silhouette;
        const auto inherited =
            aiProject(scene, 0, base, stale, image, stale.silhouette, {"front"}, false, 0, nullptr, options);
        QVERIFY(atlasQuality(inherited.atlases["front"], 64, 64) <= .07501f);
        options.retainedMask = {};
        const auto live = capture(scene, base, 512);
        QVERIFY(aiViewContribution(scene, 0, live, {"front"}, options) > 0);
        options.fillGaps = false;
        QCOMPARE(aiViewContribution(scene, 0, live, {"front"}, options), 0.0);
        options.fillGaps = true;
        // A channel to the silhouette must not expand the fresh outer projection.
        auto open = base;
        for (int y = 56; y < 72; ++y)
            for (int x = 0; x < 64; ++x)
                open["front"].coverage.scanLine(y)[x] = 0;
        QCOMPARE(
            aiProject(scene, 0, open, stale, image, stale.silhouette, {"front"}, false, 0, nullptr, options)
                .changed,
            qint64(0));
        const auto excluded = solid(512, qRgb(0, 0, 0));
        QCOMPARE(
            aiProject(scene, 0, base, stale, image, excluded, {"front"}, false, 0, nullptr, options).changed,
            qint64(0));
        auto splitMask = stale.silhouette.copy();
        for (int y = 252; y < 260; ++y)
            for (int x = 0; x < 512; ++x)
                splitMask.setPixel(x, y, qRgb(0, 0, 0));
        QCOMPARE(
            aiProject(scene, 0, base, stale, image, splitMask, {"front"}, false, 0, nullptr, options).changed,
            qint64(0));
        QCOMPARE(
            aiProject(scene, 0, base, stale, image, stale.silhouette, {"back"}, false, 0, nullptr, options)
                .atlases["front"]
                .coverage,
            base["front"].coverage);
        // A front occluder cannot supply pixels to the empty rear UVs.
        auto blocked = scene;
        const auto occluder = plane(1, 1);
        blocked.tris.insert(blocked.tris.end(), occluder.tris.begin(), occluder.tris.end());
        const auto hidden = capture(blocked, base, 512);
        const auto occluded = aiProject(blocked, 0, base, hidden, image, hidden.silhouette, {"front"}, false,
                                        0, nullptr, options);
        QCOMPARE(occluded.atlases["front"].coverage, base["front"].coverage);
        QCOMPARE(occluded.atlases["back"].coverage, base["back"].coverage);
    }
    void projectionAngleCutoffHasSmoothOverlapFalloff() {
        auto scene = plane();
        QMatrix4x4 rotate;
        rotate.rotate(60, 0, 1, 0);
        for (auto &t : scene.tris)
            for (int k = 0; k < 3; ++k) {
                const auto p = rotate.map(QVector3D(t.p[k].x, t.p[k].y, t.p[k].z));
                const auto n = rotate.mapVector(QVector3D(0, 0, 1));
                t.p[k] = {p.x(), p.y(), p.z()};
                t.n[k] = {n.x(), n.y(), n.z()};
            }
        auto base = aiEmptyAtlases(scene, 256);
        paintAtlas(base["front"], qRgb(0, 0, 255));
        const auto c = capture(scene, base, 512);
        AiProjectionOptions options;
        options.overlay = true;
        options.colorMatch = false;
        options.feather = 0;
        options.minCosine = .5f;
        const auto result = aiProject(scene, 0, base, c, solid(512, qRgb(255, 0, 0)), c.silhouette, {"front"},
                                      false, 0, nullptr, options);
        int maximumStep = 0, maximumRed = 0, unchanged = 0;
        const auto &image = result.atlases["front"].color;
        for (int x = 1; x < 256; ++x) {
            const int red = qRed(image.pixel(x, 128));
            maximumRed = std::max(maximumRed, red);
            unchanged += red == 0;
            maximumStep = std::max(maximumStep, std::abs(red - qRed(image.pixel(x - 1, 128))));
        }
        QVERIFY(maximumRed > 100);
        QVERIFY(unchanged > 20);
        QVERIFY2(maximumStep < 20, qPrintable(QString::number(maximumStep)));
        QCOMPARE(result.atlases["front"].coverage, base["front"].coverage);
    }
    void maskAndOcclusion() {
        Scene s = plane(.1f);
        auto back = plane(0, 1);
        s.tris.insert(s.tris.end(), back.tris.begin(), back.tris.end());
        auto a = aiEmptyAtlases(s, 64);
        auto c = capture(s, a);
        QImage mask = solid(128, qRgb(255, 255, 255));
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 64; ++x)
                mask.setPixel(x, y, qRgb(0, 0, 0));
        auto p = aiProject(s, 0, a, c, solid(128, qRgb(255, 0, 0)), mask, {"front", "back"}, false, 0);
        QVERIFY(p.changed > 1000);
        QCOMPARE(covered(p.atlases["back"].coverage), 0);
        QCOMPARE(p.atlases["front"].coverage.constScanLine(32)[16], uchar(0));
        QCOMPARE(p.atlases["front"].coverage.constScanLine(32)[48], uchar(255));
        auto excluded = aiProject(s, 0, a, c, solid(128, qRgb(255, 0, 0)), solid(128, qRgb(255, 255, 255)),
                                  {"back"}, false, 0);
        QCOMPARE(excluded.changed, 0);
    }
    void paddingAndPreservation() {
        Scene s = plane();
        auto a = aiEmptyAtlases(s, 64);
        auto c = capture(s, a);
        QImage mask = solid(128, qRgb(0, 0, 0));
        for (int y = 48; y < 80; ++y)
            for (int x = 48; x < 80; ++x)
                mask.setPixel(x, y, qRgb(255, 255, 255));
        auto p = aiProject(s, 0, a, c, solid(128, qRgb(255, 0, 0)), mask, {"front"}, false, 0);
        auto padded = aiProject(s, 0, a, c, solid(128, qRgb(255, 0, 0)), mask, {"front"}, false, 8);
        QCOMPARE(covered(p.atlases["front"].coverage), covered(padded.atlases["front"].coverage));
        QCOMPARE(p.atlases["front"].confidence, padded.atlases["front"].confidence);
        QVERIFY(p.atlases["front"].color != padded.atlases["front"].color);
        auto c2 = capture(s, padded.atlases);
        auto filled = aiProject(s, 0, padded.atlases, c2, solid(128, qRgb(0, 0, 255)),
                                solid(128, qRgb(255, 255, 255)), {"front"}, true, 0);
        QCOMPARE(filled.atlases["front"].color.pixel(32, 32), qRgb(255, 0, 0));
        std::atomic_bool cancel{true};
        QVERIFY_EXCEPTION_THROWN(aiProject(s, 0, a, c, mask, mask, {"front"}, false, 8, &cancel),
                                 std::runtime_error);
        QVERIFY_EXCEPTION_THROWN(aiProject(s, 0, a, c, solid(64, qRgb(0, 0, 0)), mask, {"front"}, false),
                                 std::runtime_error);
    }
    void slopedProjectionHasNoTriangleCracks() {
        Scene s = plane();
        QMatrix4x4 rotation;
        rotation.rotate(48, 1, 0, 0);
        rotation.rotate(20, 0, 1, 0);
        for (auto &t : s.tris)
            for (int k = 0; k < 3; ++k) {
                auto p = rotation.map(QVector3D(t.p[k].x, t.p[k].y, t.p[k].z));
                auto n = rotation.mapVector(QVector3D(0, 0, 1));
                t.p[k] = {p.x(), p.y(), p.z()};
                t.n[k] = {n.x(), n.y(), n.z()};
            }
        auto atlases = aiEmptyAtlases(s, 512);
        auto c = capture(s, atlases, 64);
        auto candidate = solid(64, qRgb(255, 255, 255));
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                if (c.faces[y * 64 + x] >= 0)
                    candidate.setPixel(x, y, qRgb(180, 30, 20));
        auto result = aiProject(s, 0, atlases, c, candidate, c.silhouette, {"front"}, false, 0);
        int holes = 0, whiteFringe = 0;
        for (int y = 20; y < 492; ++y)
            for (int x = 20; x < 492; ++x) {
                holes += !result.atlases["front"].coverage.constScanLine(y)[x];
                whiteFringe += qGreen(result.atlases["front"].color.pixel(x, y)) > 35;
            }
        QCOMPARE(holes, 0);
        QCOMPARE(whiteFringe, 0);
    }
    void seamCorrectionPreservesAcceptedPaint() {
        Scene s = plane();
        auto a = aiEmptyAtlases(s, 128);
        a["front"].color.fill(Qt::red);
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 64; ++x)
                a["front"].coverage.scanLine(y)[x] = 255;
        auto c = capture(s, a, 256);
        auto result = aiProject(s, 0, a, c, solid(256, qRgb(0, 0, 255)), c.silhouette, {"front"}, true, 0);
        const auto color = result.atlases["front"].color;
        QCOMPARE(color.pixel(63, 64), qRgb(255, 0, 0));
        QVERIFY(qRed(color.pixel(64, 64)) > 200);
        QVERIFY(qBlue(color.pixel(64, 64)) < 120);
        QCOMPARE(color.pixel(75, 64), qRgb(0, 0, 255));
        QCOMPARE(covered(result.atlases["front"].coverage), qint64(128 * 128));
    }
    void seamCorrectionDoesNotCrossUvIslands() {
        Scene s = plane();
        Scene other = plane(0, 0, 1.7f);
        for (auto &t : s.tris)
            for (auto &uv : t.uv[0])
                uv.x *= .49f;
        for (auto &t : other.tris)
            for (auto &uv : t.uv[0])
                uv.x = .51f + uv.x * .49f;
        s.tris.insert(s.tris.end(), other.tris.begin(), other.tris.end());
        s.center.x = .85f;
        s.scale = 4;
        auto a = aiEmptyAtlases(s, 128);
        a["front"].color.fill(Qt::red);
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 63; ++x)
                a["front"].coverage.scanLine(y)[x] = 255;
        OrbitCamera orbit;
        orbit.yaw = orbit.pitch = 0;
        orbit.distance = 6;
        auto c = aiCapture(s, 0, a, aiCamera(s, orbit, 256));
        auto result = aiProject(s, 0, a, c, solid(256, qRgb(0, 0, 255)), c.silhouette, {"front"}, true, 0);
        QCOMPARE(result.atlases["front"].color.pixel(66, 64), qRgb(0, 0, 255));
        QCOMPARE(result.atlases["front"].color.pixel(60, 64), qRgb(255, 0, 0));
    }
    void projectionAt4096() {
        const auto s = plane();
        auto a = aiEmptyAtlases(s, 4096);
        auto c = capture(s, a, 512);
        AiProjectionOptions options;
        options.colorMatch = false;
        QElapsedTimer timer;
        timer.start();
        auto first = aiProject(s, 0, a, c, solid(512, qRgb(255, 0, 0)), c.silhouette, {"front"}, false, 8,
                               nullptr, options);
        const auto initialMs = timer.restart();
        auto second = aiProject(s, 0, first.atlases, c, solid(512, qRgb(0, 0, 255)), c.silhouette, {"front"},
                                false, 8, nullptr, options);
        qInfo("4K projection: first %lld ms, overlap %lld ms", initialMs, timer.elapsed());
        auto color = second.atlases["front"].color.pixel(2048, 2048);
        QVERIFY(std::abs(qRed(color) - 188) <= 1 && std::abs(qBlue(color) - 188) <= 1);
        QVERIFY(covered(second.atlases["front"].coverage) > qint64(4096) * 4096 * .98);
        QCOMPARE(first.atlases["front"].color.pixel(2048, 2048), qRgb(255, 0, 0));
    }
    void multiViewBlendReducesPatchBoundaries() {
        const auto metrics = blendQuality();
        QVERIFY(metrics["boundarySamples"].toInt() > 40);
        QVERIFY2(metrics["blendBoundaryJump"].toDouble() < metrics["fillBoundaryJump"].toDouble() * .5,
                 QJsonDocument(metrics).toJson().constData());
        QVERIFY(metrics["checkerContrast"].toDouble() > 35);
    }
    void cameraQualityBlendsInLinearLight() {
        const auto s = plane();
        auto a = aiEmptyAtlases(s, 128);
        const auto c = capture(s, a, 256);
        AiProjectionOptions options;
        options.colorMatch = false;
        options.feather = 0;
        auto red = aiProject(s, 0, a, c, solid(256, qRgb(255, 0, 0)), c.silhouette, {"front"}, false, 0,
                             nullptr, options);
        auto blue = aiProject(s, 0, red.atlases, c, solid(256, qRgb(0, 0, 255)), c.silhouette, {"front"},
                              false, 0, nullptr, options);
        auto p = blue.atlases["front"].color.pixel(64, 64);
        QVERIFY(std::abs(qRed(p) - 188) <= 1);
        QVERIFY(std::abs(qBlue(p) - 188) <= 1);
        QVERIFY(atlasQuality(blue.atlases["front"], 64, 64) > .98f);
        QCOMPARE(red.atlases["front"].color.pixel(64, 64), qRgb(255, 0, 0));
        QCOMPARE(covered(blue.atlases["front"].coverage), covered(red.atlases["front"].coverage));
    }
    void continuityUsesReliablePaintAndReleasesFalloff() {
        const auto s = plane();
        auto c = capture(s, aiEmptyAtlases(s, 128), 128);
        c.partial = solid(128, qRgb(255, 0, 0));
        c.neutral = solid(128, qRgb(150, 150, 150));
        c.known = c.silhouette = solid(128, qRgb(255, 255, 255));
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x) {
                float q = x < 32 ? .1f : x < 64 ? .5f : .95f;
                reinterpret_cast<quint16 *>(c.confidence.scanLine(y))[x] = quint16(qRound(q * 65535));
                if (x >= 96)
                    c.known.setPixel(x, y, qRgb(0, 0, 0));
            }
        const auto guide = aiFeedback(c, .8f);
        QVERIFY(guide.active);
        QCOMPARE(guide.mask.constScanLine(64)[16], uchar(0));
        QCOMPARE(guide.image.pixel(16, 64), c.neutral.pixel(16, 64));
        QVERIFY(guide.mask.constScanLine(64)[48] > 10 && guide.mask.constScanLine(64)[48] < 100);
        QCOMPARE(guide.image.pixel(48, 64), c.partial.pixel(48, 64)); // Do not desaturate context.
        QCOMPARE(guide.mask.constScanLine(64)[80], uchar(255));
        QCOMPARE(guide.image.pixel(80, 64), c.partial.pixel(80, 64));
        QCOMPARE(guide.mask.constScanLine(64)[112], uchar(0));
        QCOMPARE(guide.image.pixel(112, 64), c.neutral.pixel(112, 64));
        auto off = aiFeedback(c, 0);
        QVERIFY(!off.active);
        QCOMPARE(off.image, c.neutral);
        QCOMPARE(covered(off.mask), 0);
        QVERIFY(aiFeedback(c, 1).mask.constScanLine(64)[48] > aiFeedback(c, .25f).mask.constScanLine(64)[48]);
        QVERIFY_EXCEPTION_THROWN(aiFeedback(c, 1.1f), std::runtime_error);
    }
    void continuityAnchorsExistingPaintAtProjectionFalloff() {
        const auto s = plane();
        auto atlases = aiEmptyAtlases(s, 128);
        paintAtlas(atlases["front"], qRgb(210, 40, 15), .4f);
        auto c = capture(s, atlases);
        AiProjectionOptions options;
        options.overlay = true;
        const auto frontal = aiProjectionFeedback(s, c, .8f, options);
        QVERIFY(frontal.mask.constScanLine(64)[64] > 190);
        QVERIFY(frontal.mask.constScanLine(64)[64] < 255);
        // A previously usable surface now seen at a grazing angle.
        c.normal.fill(qRgb(250, 128, 150));
        const auto old = aiFeedback(c, .8f);
        const auto anchored = aiProjectionFeedback(s, c, .8f, options);
        QVERIFY(old.mask.constScanLine(64)[64] < 224);
        QCOMPARE(anchored.mask.constScanLine(64)[64], uchar(255));
        QCOMPARE(anchored.image.pixel(64, 64), c.partial.pixel(64, 64));
        reinterpret_cast<quint16 *>(c.confidence.scanLine(64))[64] = 6000;
        const auto context = aiProjectionFeedback(s, c, .8f, options);
        QCOMPARE(context.mask.constScanLine(64)[64], uchar(0));
        QCOMPARE(context.image.pixel(64, 64), c.partial.pixel(64, 64));
        reinterpret_cast<quint16 *>(c.confidence.scanLine(64))[64] = 1000;
        QCOMPARE(aiProjectionFeedback(s, c, .8f, options).image.pixel(64, 64), c.neutral.pixel(64, 64));
        c.known.setPixel(65, 64, qRgb(0, 0, 0));
        const auto unknown = aiProjectionFeedback(s, c, .8f, options);
        QCOMPARE(unknown.mask.constScanLine(64)[65], uchar(0));
        QCOMPARE(unknown.image.pixel(65, 64), c.neutral.pixel(65, 64));
        QVERIFY(!aiProjectionFeedback(s, c, 0, options).active);
    }
    void retainedDetailsKeepTheirOriginalConfidence() {
        const auto s = plane();
        auto a = aiEmptyAtlases(s, 128);
        paintAtlas(a["front"], qRgb(255, 0, 0), .95f);
        OrbitCamera orbit;
        orbit.yaw = 60;
        orbit.pitch = 0;
        orbit.distance = 3;
        const auto c = aiCapture(s, 0, a, aiCamera(s, orbit, 256));
        AiProjectionOptions options;
        options.overlay = true;
        options.colorMatch = false;
        options.feather = 0;
        auto plain = aiProject(s, 0, a, c, solid(256, qRgb(0, 0, 255)), c.silhouette, {"front"}, false, 0,
                               nullptr, options);
        options.retainedMask = solid(256, qRgb(255, 255, 255));
        auto inherited = aiProject(s, 0, a, c, solid(256, qRgb(0, 0, 255)), c.silhouette, {"front"}, false, 0,
                                   nullptr, options);
        QCOMPARE(inherited.atlases["front"].color,
                 a["front"].color); // No resampling of locked atlas details.
        QVERIFY(atlasQuality(plain.atlases["front"], 64, 64) < .9f);
        QVERIFY(atlasQuality(inherited.atlases["front"], 64, 64) > .94f);
        for (int i = 0; i < 3; ++i)
            inherited = aiProject(s, 0, inherited.atlases, c, solid(256, qRgb(255, 0, 0)), c.silhouette,
                                  {"front"}, false, 0, nullptr, options);
        QVERIFY(atlasQuality(inherited.atlases["front"], 64, 64) > .94f);
    }
    void cameraProjectionRepaintsFullyCoveredAtlas() {
        const auto s = plane();
        auto a = aiEmptyAtlases(s, 128);
        paintAtlas(a["front"], qRgb(255, 0, 0));
        auto c = capture(s, a, 256);
        AiProjectionOptions options;
        options.overlay = true;
        options.colorMatch = false;
        auto blue = aiProject(s, 0, a, c, solid(256, qRgb(0, 0, 255)), c.silhouette, {"front"}, false, 0,
                              nullptr, options);
        QVERIFY(blue.changed > 1000);
        auto color = blue.atlases["front"].color.pixel(64, 64);
        QVERIFY(qBlue(color) > 250 && qRed(color) < 10);
        QCOMPARE(blue.atlases["front"].coverage, a["front"].coverage);
        auto green = aiProject(s, 0, blue.atlases, c, solid(256, qRgb(0, 255, 0)), c.silhouette, {"front"},
                               false, 0, nullptr, options);
        color = green.atlases["front"].color.pixel(64, 64);
        QVERIFY(qGreen(color) > 250 && qBlue(color) < 10);
        OrbitCamera orbit;
        orbit.yaw = 60;
        orbit.pitch = 0;
        orbit.distance = 3;
        auto oblique = aiCapture(s, 0, a, aiCamera(s, orbit, 256));
        auto weaker = aiProject(s, 0, a, oblique, solid(256, qRgb(0, 0, 255)), oblique.silhouette, {"front"},
                                false, 0, nullptr, options);
        QVERIFY(qBlue(weaker.atlases["front"].color.pixel(64, 64)) > 80);
        QCOMPARE(weaker.atlases["front"].coverage, a["front"].coverage);
    }
    void cameraModeDoesNotSkipCoveredViews() {
        const auto s = plane();
        auto a = aiEmptyAtlases(s, 128);
        paintAtlas(a["front"], qRgb(255, 0, 0));
        const auto c = capture(s, a, 256);
        AiProjectionOptions options;
        QVERIFY(aiViewContribution(s, 0, c, {"front"}, options) < .01);
        options.overlay = true;
        QVERIFY(aiViewContribution(s, 0, c, {"front"}, options) > .5);
        QCOMPARE(aiViewContribution(s, 0, c, {"back"}, options), 0.0);
        options.strength = 0;
        QCOMPARE(aiViewContribution(s, 0, c, {"front"}, options), 0.0);
    }
    void frontalViewReplacesGrazingView() {
        const auto s = plane();
        auto a = aiEmptyAtlases(s, 128);
        AiProjectionOptions options;
        options.colorMatch = false;
        options.feather = 0;
        const auto front = capture(s, a, 256);
        OrbitCamera orbit;
        orbit.yaw = 60;
        orbit.pitch = 0;
        orbit.distance = 3;
        const auto grazing = aiCapture(s, 0, a, aiCamera(s, orbit, 256));
        auto weak = aiProject(s, 0, a, grazing, solid(256, qRgb(0, 0, 255)), grazing.silhouette, {"front"},
                              false, 0, nullptr, options);
        QVERIFY(atlasQuality(weak.atlases["front"], 64, 64) < .3f);
        auto strong = aiProject(s, 0, weak.atlases, front, solid(256, qRgb(255, 0, 0)), front.silhouette,
                                {"front"}, false, 0, nullptr, options);
        QCOMPARE(strong.atlases["front"].color.pixel(64, 64), qRgb(255, 0, 0));
        for (int i = 0; i < 3; ++i)
            strong = aiProject(s, 0, strong.atlases, grazing, solid(256, qRgb(0, 0, 255)), grazing.silhouette,
                               {"front"}, false, 0, nullptr, options);
        QCOMPARE(strong.atlases["front"].color.pixel(64, 64), qRgb(255, 0, 0));
    }
    void paintedMaskHasCameraSpaceFalloff() {
        const auto s = plane();
        auto a = aiEmptyAtlases(s, 256);
        paintAtlas(a["front"], qRgb(0, 0, 255));
        auto c = capture(s, a, 512);
        auto mask = solid(512, qRgb(0, 0, 0));
        for (int y = 0; y < 512; ++y)
            for (int x = 256; x < 512; ++x)
                mask.setPixel(x, y, qRgb(255, 255, 255));
        AiProjectionOptions options;
        options.overlay = true;
        options.colorMatch = false;
        options.feather = .1f;
        auto soft =
            aiProject(s, 0, a, c, solid(512, qRgb(255, 0, 0)), mask, {"front"}, false, 0, nullptr, options)
                .atlases["front"];
        QCOMPARE(soft.color.pixel(127, 128), qRgb(0, 0, 255));
        QVERIFY(qRed(soft.color.pixel(129, 128)) < 60);
        QVERIFY(qRed(soft.color.pixel(180, 128)) > 220);
        int largestStep = 0;
        for (int x = 128; x < 180; ++x)
            largestStep = std::max(
                largestStep, std::abs(qRed(soft.color.pixel(x, 128)) - qRed(soft.color.pixel(x - 1, 128))));
        QVERIFY2(largestStep < 20, qPrintable(QString::number(largestStep)));
        QCOMPARE(soft.coverage, a["front"].coverage);
        options.feather = 0;
        auto hard =
            aiProject(s, 0, a, c, solid(512, qRgb(255, 0, 0)), mask, {"front"}, false, 0, nullptr, options)
                .atlases["front"];
        QVERIFY(qRed(hard.color.pixel(129, 128)) > 240);
    }
    void coverageBoundaryBlendsIntoAcceptedPaint() {
        const auto s = plane();
        auto a = aiEmptyAtlases(s, 256);
        paintAtlas(a["front"], qRgb(255, 0, 0));
        for (int y = 0; y < 256; ++y)
            for (int x = 128; x < 256; ++x) {
                a["front"].coverage.scanLine(y)[x] = 0;
                reinterpret_cast<quint16 *>(a["front"].confidence.scanLine(y))[x] = 0;
            }
        auto c = capture(s, a, 512);
        AiProjectionOptions options;
        options.colorMatch = false;
        options.feather = .1f;
        auto result = aiProject(s, 0, a, c, solid(512, qRgb(0, 0, 255)), c.silhouette, {"front"}, false, 0,
                                nullptr, options)
                          .atlases["front"];
        QVERIFY(qRed(result.color.pixel(127, 128)) < 20);
        QCOMPARE(result.color.pixel(128, 128), qRgb(0, 0, 255));
        QVERIFY(qRed(result.color.pixel(80, 128)) > 150);
        QCOMPARE(covered(result.coverage), qint64(256 * 256));
        int largestStep = 0;
        for (int x = 103; x <= 128; ++x)
            largestStep = std::max(largestStep, std::abs(qRed(result.color.pixel(x, 128)) -
                                                         qRed(result.color.pixel(x - 1, 128))));
        QVERIFY2(largestStep < 20, qPrintable(QString::number(largestStep)));
    }
    void repaintFeathersAcrossPreviousCoverageBoundary() {
        auto s = plane();
        // A grazing camera makes the coverage-dependent opacity jump unmistakable.
        for (auto &t : s.tris)
            for (auto &n : t.n)
                n = unit(V3{.9f, 0, .4f});
        auto a = aiEmptyAtlases(s, 256);
        paintAtlas(a["front"], qRgb(255, 0, 0));
        for (int y = 0; y < 256; ++y)
            for (int x = 128; x < 256; ++x)
                a["front"].coverage.scanLine(y)[x] = 0;
        const auto c = capture(s, a, 512);
        AiProjectionOptions options;
        options.overlay = true;
        options.colorMatch = false;
        options.feather = .1f;
        for (int fixture = 0; fixture < 3; ++fixture) {
            options.strength = fixture == 1 ? .25f : 1.f;
            options.feather = fixture == 2 ? .25f : .1f;
            QImage mask = c.silhouette;
            if (fixture == 2)
                for (int y = 0; y < 512; ++y)
                    for (int x = 0; x < 185; ++x)
                        mask.setPixel(x, y, qRgb(0, 0, 0));
            const auto result = aiProject(s, 0, a, c, solid(512, qRgb(0, 0, 255)), mask, {"front"}, false, 0,
                                          nullptr, options)
                                    .atlases["front"];
            int largestStep = 0;
            for (int x = 98; x <= 130; ++x)
                largestStep = std::max(largestStep, std::abs(qRed(result.color.pixel(x, 128)) -
                                                             qRed(result.color.pixel(x - 1, 128))));
            qInfo("Repaint fixture %d: largest boundary step %d levels", fixture, largestStep);
            QVERIFY2(largestStep < 20, qPrintable(QString::number(largestStep)));
            auto full = a;
            full["front"].coverage.fill(255);
            const auto baseline = aiProject(s, 0, full, c, solid(512, qRgb(0, 0, 255)), mask, {"front"},
                                            false, 0, nullptr, options)
                                      .atlases["front"];
            // Away from the join, ordinary camera opacity is unchanged.
            QCOMPARE(result.color.pixel(8, 128), baseline.color.pixel(8, 128));
            if (fixture == 2)
                QCOMPARE(result.color.pixel(32, 128), a["front"].color.pixel(32, 128));
            QCOMPARE(result.color.pixel(128, 128), qRgb(0, 0, 255));
            QCOMPARE(covered(result.coverage), qint64(256 * 256));
            if (fixture == 0) {
                QDir().mkpath(".ai-qa");
                result.color.save(".ai-qa/repaint-boundary.png");
            }
        }
    }
    void repaintTransitionCrossesGeometricUvSeam() {
        auto s = plane();
        auto right = plane();
        for (auto &t : s.tris)
            for (int k = 0; k < 3; ++k) {
                t.p[k].x = t.p[k].x * .5f - .4f;
                t.n[k] = unit(V3{.9f, 0, .4f});
                t.uv[0][k].x *= .45f;
            }
        for (auto &t : right.tris)
            for (int k = 0; k < 3; ++k) {
                t.p[k].x = t.p[k].x * .5f + .4f;
                t.n[k] = unit(V3{.9f, 0, .4f});
                t.uv[0][k].x = .55f + t.uv[0][k].x * .45f;
            }
        s.tris.insert(s.tris.end(), right.tris.begin(), right.tris.end());
        auto a = aiEmptyAtlases(s, 128);
        a["front"].color.fill(qRgb(255, 0, 0));
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 58; ++x)
                a["front"].coverage.scanLine(y)[x] = 255;
        const auto c = capture(s, a, 512);
        AiProjectionOptions options;
        options.overlay = true;
        options.colorMatch = false;
        options.feather = .1f;
        const auto result = aiProject(s, 0, a, c, solid(512, qRgb(0, 0, 255)), c.silhouette, {"front"}, false,
                                      0, nullptr, options)
                                .atlases["front"];
        QVERIFY(qRed(result.color.pixel(57, 64)) < 20);
        QVERIFY(qBlue(result.color.pixel(57, 64)) > 250);
        QCOMPARE(result.color.pixel(70, 64), qRgb(0, 0, 255));
        QVERIFY(qRed(result.color.pixel(4, 64)) > 230);
        QCOMPARE(result.coverage.constScanLine(64)[64], uchar(0)); // Atlas gutter stays unpainted.
        QCOMPARE(result.color.pixel(64, 64), a["front"].color.pixel(64, 64));
        // Reusing a capture must read current coverage, not stale capture.known.
        auto full = result;
        full.coverage.fill(255);
        auto repeated = aiProject(s, 0, {{"front", full}}, c, solid(512, qRgb(255, 0, 0)), c.silhouette,
                                  {"front"}, false, 0, nullptr, options)
                            .atlases["front"];
        QVERIFY(qBlue(repeated.color.pixel(57, 64)) > 230); // Normal camera falloff, no obsolete join boost.
    }
    void denseSubpixelTrianglesProjectWithoutHoles() {
        Scene s = plane();
        s.tris.clear();
        const int cells = 96;
        QMatrix4x4 rotation;
        rotation.rotate(37, 1, 0, 0);
        rotation.rotate(20, 0, 1, 0);
        const auto n = rotation.mapVector(QVector3D(0, 0, 1));
        for (int y = 0; y < cells; ++y)
            for (int x = 0; x < cells; ++x) {
                V2 tex[] = {{float(x) / cells, float(y) / cells},
                            {float(x + 1) / cells, float(y) / cells},
                            {float(x + 1) / cells, float(y + 1) / cells},
                            {float(x) / cells, float(y + 1) / cells}};
                for (auto indices : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}}) {
                    Triangle t;
                    t.material = 0;
                    std::array<V2, 3> uv;
                    for (int k = 0; k < 3; ++k) {
                        uv[k] = tex[indices[k]];
                        const auto p =
                            rotation.map(QVector3D((uv[k].x - .5f) * 1.6f, (uv[k].y - .5f) * 1.6f, 0));
                        t.p[k] = {p.x(), p.y(), p.z()};
                        t.n[k] = {n.x(), n.y(), n.z()};
                    }
                    t.uv.push_back(uv);
                    s.tris.push_back(t);
                }
            }
        auto a = aiEmptyAtlases(s, 256);
        const auto c = capture(s, a, 64); // Many triangles are smaller than a capture pixel.
        auto candidate = solid(64, qRgb(255, 255, 255));
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x)
                if (c.faces[y * 64 + x] >= 0)
                    candidate.setPixel(x, y, qRgb(180, 30, 20));
        AiProjectionOptions options;
        options.overlay = true;
        options.colorMatch = false;
        const auto result =
            aiProject(s, 0, a, c, candidate, c.silhouette, {"front"}, false, 0, nullptr, options)
                .atlases["front"];
        int holes = 0, badColor = 0;
        for (int y = 20; y < 236; ++y)
            for (int x = 20; x < 236; ++x) {
                holes += result.coverage.constScanLine(y)[x] == 0;
                badColor += result.color.pixel(x, y) != qRgb(180, 30, 20);
            }
        QVERIFY2(holes == 0, qPrintable(QString::number(holes)));
        QCOMPARE(badColor, 0);
    }
    void subpixelVisibilityKeepsThinFoldOccluded() {
        Scene s = plane();
        auto front = plane(.0001f);
        for (auto &t : s.tris)
            for (auto &uv : t.uv[0])
                uv.x *= .45f;
        for (auto &t : front.tris)
            for (auto &uv : t.uv[0])
                uv.x = .55f + uv.x * .45f;
        s.tris.insert(s.tris.end(), front.tris.begin(), front.tris.end());
        // Connect both sheets geometrically. Component identity alone must not
        // treat the front sheet as a harmless part of the back sheet's pixel.
        V3 p[] = {{-.8f, -.8f, 0}, {.8f, -.8f, 0}, {.8f, -.8f, .0001f}, {-.8f, -.8f, .0001f}};
        for (auto indices : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}}) {
            Triangle t;
            t.material = 0;
            for (int k = 0; k < 3; ++k) {
                t.p[k] = p[indices[k]];
                t.n[k] = {0, -1, 0};
            }
            t.uv.push_back({V2{}, V2{}, V2{}});
            s.tris.push_back(t);
        }
        auto a = aiEmptyAtlases(s, 128);
        const auto c = capture(s, a, 64);
        AiProjectionOptions options;
        options.overlay = true;
        options.colorMatch = false;
        auto result = aiProject(s, 0, a, c, solid(64, qRgb(180, 30, 20)), c.silhouette, {"front"}, false, 0,
                                nullptr, options)
                          .atlases["front"];
        QCOMPARE(result.coverage.constScanLine(64)[32], uchar(0));
        QCOMPARE(result.coverage.constScanLine(64)[96], uchar(255));
        QCOMPARE(result.color.pixel(32, 64), a["front"].color.pixel(32, 64));
    }
    void matchingAndStrengthControls() {
        const auto s = plane();
        auto a = aiEmptyAtlases(s, 64);
        paintAtlas(a["front"], qRgb(128, 128, 128));
        auto c = capture(s, a, 256);
        AiProjectionOptions options;
        options.feather = 0;
        options.colorMatch = false;
        const auto image = solid(256, qRgb(160, 160, 160));
        auto unmatched = aiProject(s, 0, a, c, image, c.silhouette, {"front"}, false, 0, nullptr, options);
        options.colorMatch = true;
        auto matched = aiProject(s, 0, a, c, image, c.silhouette, {"front"}, false, 0, nullptr, options);
        QVERIFY(qRed(matched.atlases["front"].color.pixel(32, 32)) + 5 <
                qRed(unmatched.atlases["front"].color.pixel(32, 32)));
        options.strength = 0;
        auto off = aiProject(s, 0, a, c, image, c.silhouette, {"front"}, false, 0, nullptr, options);
        QCOMPARE(off.changed, 0);
        QCOMPARE(off.atlases["front"].color, a["front"].color);
        options.colorMatch = false;
        options.strength = .25f;
        auto gentle = aiProject(s, 0, a, c, image, c.silhouette, {"front"}, false, 0, nullptr, options);
        QVERIFY(qRed(gentle.atlases["front"].color.pixel(32, 32)) > 128);
        QVERIFY(qRed(gentle.atlases["front"].color.pixel(32, 32)) <
                qRed(unmatched.atlases["front"].color.pixel(32, 32)));
        options.strength = 3;
        QVERIFY_EXCEPTION_THROWN(
            aiProject(s, 0, a, c, image, c.silhouette, {"front"}, false, 0, nullptr, options),
            std::runtime_error);
    }
    void overlapFeatherDoesNotCrossUvIslands() {
        Scene s = plane();
        auto other = plane(0, 0, 1.7f);
        for (auto &t : s.tris) {
            for (auto &normal : t.n)
                normal = unit(V3{.9f, 0, .4f});
            for (auto &uv : t.uv[0])
                uv.x *= .5f;
        }
        for (auto &t : other.tris)
            for (auto &uv : t.uv[0])
                uv.x = .5f + uv.x * .5f;
        s.tris.insert(s.tris.end(), other.tris.begin(), other.tris.end());
        s.center.x = .85f;
        s.scale = 4;
        auto a = aiEmptyAtlases(s, 128);
        paintAtlas(a["front"], qRgb(255, 0, 0));
        for (int y = 0; y < 128; ++y)
            for (int x = 64; x < 128; ++x)
                a["front"].coverage.scanLine(y)[x] = 0;
        OrbitCamera orbit;
        orbit.yaw = orbit.pitch = 0;
        orbit.distance = 6;
        auto c = aiCapture(s, 0, a, aiCamera(s, orbit, 512));
        AiProjectionOptions options;
        options.colorMatch = false;
        options.feather = .25f;
        auto result = aiProject(s, 0, a, c, solid(512, qRgb(0, 0, 255)), c.silhouette, {"front"}, false, 0,
                                nullptr, options);
        QCOMPARE(result.atlases["front"].color.pixel(63, 64), qRgb(255, 0, 0));
        QCOMPARE(result.atlases["front"].color.pixel(64, 64), qRgb(0, 0, 255));
    }
    void occlusionEdgeFeathersAndProtectsHiddenSurface() {
        Scene s = plane();
        auto occluder = plane(.4f, 1, .8f);
        s.tris.insert(s.tris.end(), occluder.tris.begin(), occluder.tris.end());
        auto a = aiEmptyAtlases(s, 128);
        paintAtlas(a["front"], qRgb(255, 0, 0));
        paintAtlas(a["back"], qRgb(0, 255, 0));
        auto c = capture(s, a, 512);
        AiProjectionOptions options;
        options.overlay = true;
        options.colorMatch = false;
        options.feather = .1f;
        auto result = aiProject(s, 0, a, c, solid(512, qRgb(0, 0, 255)), c.silhouette, {"front"}, false, 0,
                                nullptr, options);
        const auto color = result.atlases["front"].color;
        QCOMPARE(color.pixel(65, 64), qRgb(255, 0, 0));
        QVERIFY(qBlue(color.pixel(62, 64)) < 60);
        QVERIFY(qBlue(color.pixel(20, 64)) > 200);
        QCOMPARE(result.atlases["back"].color, a["back"].color);
        QCOMPARE(result.atlases["back"].confidence, a["back"].confidence);
    }
    void projectionStrengthAndAngleReach() {
        auto s = plane();
        auto a = aiEmptyAtlases(s, 64);
        paintAtlas(a["front"], qRgb(128, 128, 128));
        for (auto &t : s.tris)
            for (auto &normal : t.n)
                normal = unit(V3{.65f, 0, .76f});
        auto c = capture(s, a, 256);
        AiProjectionOptions options;
        options.colorMatch = false;
        options.feather = 0;
        auto image = solid(256, qRgb(200, 200, 200));
        auto normal = aiProject(s, 0, a, c, image, c.silhouette, {"front"}, false, 0, nullptr, options);
        options.strength = 2;
        auto extended = aiProject(s, 0, a, c, image, c.silhouette, {"front"}, false, 0, nullptr, options);
        QCOMPARE(normal.atlases["front"].color.pixel(32, 32), qRgb(128, 128, 128));
        QVERIFY(qRed(extended.atlases["front"].color.pixel(32, 32)) > 150);
        options.anglePower = 8;
        auto focused = aiProject(s, 0, a, c, image, c.silhouette, {"front"}, false, 0, nullptr, options);
        QCOMPARE(focused.atlases["front"].color.pixel(32, 32), qRgb(128, 128, 128));
    }
    void automaticViewsConsiderQualityAndPaddingDoesNot() {
        auto s = plane();
        auto a = aiEmptyAtlases(s, 64);
        AiProjectionOptions options;
        options.colorMatch = false;
        auto c = capture(s, a, 256);
        auto strong = aiProject(s, 0, a, c, solid(256, qRgb(180, 80, 20)), c.silhouette, {"front"}, false, 8,
                                nullptr, options);
        auto current = capture(s, strong.atlases, 256);
        auto repeated = aiViewContribution(s, 0, current, {"front"}, options);
        QVERIFY2(repeated < .01, qPrintable(QString::number(repeated)));
        for (auto &t : s.tris)
            for (auto &normal : t.n)
                normal = unit(V3{.9f, 0, .4f});
        auto weakCapture = capture(s, a, 256);
        auto weak = aiProject(s, 0, a, weakCapture, solid(256, qRgb(180, 80, 20)), weakCapture.silhouette,
                              {"front"}, false, 8, nullptr, options);
        s = plane();
        auto improved = capture(s, weak.atlases, 256);
        QVERIFY(aiNewCoverage(s, 0, weak.atlases, improved, {"front"}) < .01);
        QVERIFY(aiViewContribution(s, 0, improved, {"front"}, options) > .5);
        QVERIFY(atlasQuality(strong.atlases["front"], 32, 32) > .98f);
        QVERIFY(std::abs(atlasQuality(strong.atlases["front"], 31, 32) -
                         atlasQuality(strong.atlases["front"], 32, 32)) < .01f);
        std::atomic_bool cancel{true};
        QVERIFY_EXCEPTION_THROWN(aiViewContribution(s, 0, improved, {"front"}, options, &cancel),
                                 std::runtime_error);
    }
    void projectRoundTrip() {
        QTemporaryDir dir;
        Scene s = plane();
        auto a = aiEmptyAtlases(s, 64);
        a["front"].color.fill(Qt::red);
        a["front"].coverage.fill(255);
        reinterpret_cast<quint16 *>(a["front"].confidence.scanLine(20))[20] = 12345;
        QJsonObject settings{{"prompt", "worn red metal"}};
        QList<QJsonObject> records{{{"seed", 7}, {"camera", aiCameraJson(capture(s, a).camera)}}};
        QString path = dir.filePath("test.fornaioai");
        aiSaveProject(path, settings, a, records);
        AiAtlases restored;
        QList<QJsonObject> history;
        QCOMPARE(aiLoadProject(path, restored, history), settings);
        QCOMPARE(restored["front"].color, a["front"].color);
        QCOMPARE(restored["front"].coverage, a["front"].coverage);
        QCOMPARE(restored["front"].confidence, a["front"].confidence);
        QCOMPARE(history, records);
        // Version-1 projects without confidence still load and accept new blends.
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        auto legacy = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        auto images = legacy["atlases"].toArray();
        for (int i = 0; i < images.size(); ++i) {
            auto image = images[i].toObject();
            image.remove("confidence");
            images[i] = image;
        }
        legacy["atlases"] = images;
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(QJsonDocument(legacy).toJson());
        file.close();
        aiLoadProject(path, restored, history);
        QVERIFY(restored["front"].confidence.isNull());
        auto c = capture(s, restored);
        auto blended =
            aiProject(s, 0, restored, c, solid(128, qRgb(120, 20, 20)), c.silhouette, {"front"}, false, 0);
        QVERIFY(!blended.atlases["front"].confidence.isNull());
    }
    void manifestSafety() {
        auto m = AiManifest::load(aiAssetsPath("manifest.json"));
        QVERIFY(m.downloadBytes > qint64(10000000000));
        QVERIFY(m.json["files"].toArray().size() > 10);
        for (const auto &entry : m.json["files"].toArray()) // Marigold was removed from the engine.
            QVERIFY(!entry.toObject()["path"].toString().startsWith("albedo/"));
        QVERIFY(!m.json.contains("albedo"));
        QJsonObject unsafe = m.json;
        auto files = unsafe["files"].toArray();
        auto f = files[0].toObject();
        f["path"] = "../outside";
        files[0] = f;
        unsafe["files"] = files;
        QVERIFY_EXCEPTION_THROWN(AiManifest::parse(QJsonDocument(unsafe).toJson()), std::runtime_error);
        f["path"] = "runtime/test.zip";
        f["url"] = "http://example.com/test";
        files[0] = f;
        unsafe["files"] = files;
        QVERIFY_EXCEPTION_THROWN(AiManifest::parse(QJsonDocument(unsafe).toJson()), std::runtime_error);
    }
    void engineWithFormerMarigoldExtensionStaysInstalled() {
        QTemporaryDir storage;
        AiInstaller installer;
        auto manifest = AiManifest::load(aiAssetsPath("manifest.json"));
        installer.setRoot(storage.path());
        installer.setManifest(manifest);
        const QString engine = storage.filePath("engines/previous");
        auto put = [](const QString &path, const QByteArray &data) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        QVERIFY(put(storage.filePath("active.json"), "{\"id\":\"previous\"}"));
        QVERIFY(put(engine + "/runtime/python.exe", "fixture"));
        QVERIFY(put(engine + "/ready.json", "{}"));
        QVERIFY(installer.installed());
        auto former = manifest.json;
        former["albedo"] = QJsonObject{{"model", "prs-eth/marigold-iid-lighting-v1-1"}};
        QVERIFY(put(engine + "/manifest.json", QJsonDocument(former).toJson()));
        QVERIFY(put(engine + "/albedo/model_index.json", "{}")); // Leftover files are simply unused.
        QVERIFY(installer.installed());
    }
    void downloadResumeAndChecksum() {
        QTemporaryDir dir;
        QTcpServer server;
        QVERIFY(server.listen(QHostAddress::LocalHost));
        QByteArray payload("verified native download fixture");
        bool resumed = false, corruptResponse = false;
        connect(&server, &QTcpServer::newConnection, this, [&] {
            while (server.hasPendingConnections()) {
                auto socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, socket, [&, socket] {
                    QByteArray request = socket->readAll();
                    if (!request.contains("\r\n\r\n"))
                        return;
                    int start = request.toLower().contains("range: bytes=8-") ? 8 : 0;
                    resumed |= start == 8;
                    QByteArray body = payload.mid(start);
                    if (corruptResponse)
                        body.fill('x');
                    QByteArray headers = start ? "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes 8-" +
                                                     QByteArray::number(payload.size() - 1) + "/" +
                                                     QByteArray::number(payload.size()) + "\r\n"
                                               : QByteArray("HTTP/1.1 200 OK\r\n");
                    socket->write(headers + "Content-Length: " + QByteArray::number(body.size()) +
                                  "\r\nConnection: close\r\n\r\n" + body);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        QString hash =
            QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
        QJsonObject file{{"path", "model/test.bin"},
                         {"url", QString("http://127.0.0.1:%1/test").arg(server.serverPort())},
                         {"bytes", payload.size()},
                         {"sha256", hash}};
        QJsonObject json{{"format", "FornaioAI"},
                         {"version", 1},
                         {"id", "fixture"},
                         {"python", "runtime/python.exe"},
                         {"files", QJsonArray{file}}};
        AiInstaller installer;
        installer.setRoot(dir.path());
        installer.setManifest(AiManifest::parse(QJsonDocument(json).toJson(), true));
        installer.setGpuAvailable(false);
        QDir().mkpath(dir.filePath("objects"));
        QFile part(dir.filePath("objects/" + hash + ".part"));
        QVERIFY(part.open(QIODevice::WriteOnly));
        part.write(payload.left(8));
        part.close();
        installer.install();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(dir.filePath("objects/" + hash)), 10000);
        QVERIFY(resumed);
        QFile object(dir.filePath("objects/" + hash));
        QVERIFY(object.open(QIODevice::ReadOnly));
        QCOMPARE(object.readAll(), payload);
        object.close();
        installer.cancel();
        QVERIFY(!installer.installed());
        // Repair automatically replaces corrupt cached data without a second user click.
        QFile corrupt(dir.filePath("objects/" + hash));
        QVERIFY(corrupt.open(QIODevice::WriteOnly | QIODevice::Truncate));
        corrupt.write(QByteArray(payload.size(), 'x'));
        corrupt.close();
        QSignalSpy finished(&installer, &AiInstaller::finished);
        installer.install();
        auto contents = [&] {
            QFile f(corrupt.fileName());
            if (!f.open(QIODevice::ReadOnly))
                return QByteArray{};
            return f.readAll();
        };
        QTRY_COMPARE_WITH_TIMEOUT(contents(), payload, 10000);
        installer.cancel();
        finished.clear();
        // Repeated checksum failures are rejected and never activate an installation.
        QFile::remove(corrupt.fileName());
        corruptResponse = true;
        installer.install();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        QVERIFY(!finished[0][0].toBool());
        QVERIFY(!QFileInfo::exists(corrupt.fileName()));
    }
    void offlineCopyAndPreviousInstallation() {
        QTemporaryDir source, destination;
        QByteArray payload(8 * 1024 * 1024, 'v');
        QString hash =
            QString::fromLatin1(QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex());
        QDir().mkpath(source.filePath("objects"));
        QFile cached(source.filePath("objects/" + hash));
        QVERIFY(cached.open(QIODevice::WriteOnly));
        cached.write(payload);
        cached.close();
        QJsonObject file{{"path", "model/test.bin"},
                         {"url", "https://example.com/unused"},
                         {"bytes", payload.size()},
                         {"sha256", hash}};
        QJsonObject json{{"format", "FornaioAI"},
                         {"version", 1},
                         {"id", "offline-fixture"},
                         {"python", "runtime/python.exe"},
                         {"files", QJsonArray{file}}};
        QDir().mkpath(destination.filePath("engines/previous/runtime"));
        for (const QString &name :
             {QString("engines/previous/runtime/python.exe"), QString("engines/previous/ready.json"),
              QString("saved-project.fornaioai")}) {
            QFile f(destination.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("keep");
        }
        const QByteArray active = "{\"id\":\"previous\"}";
        QFile pointer(destination.filePath("active.json"));
        QVERIFY(pointer.open(QIODevice::WriteOnly));
        pointer.write(active);
        pointer.close();
        AiInstaller installer;
        installer.setRoot(destination.path());
        installer.setManifest(AiManifest::parse(QJsonDocument(json).toJson()));
        installer.setGpuAvailable(false);
        QSignalSpy checking(&installer, &AiInstaller::checkingEngine);
        installer.install(source.path());
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(destination.filePath("objects/" + hash)), 10000);
        QVERIFY(installer.busy());
        QVERIFY(checking.empty());
        QVERIFY(pointer.open(QIODevice::ReadOnly));
        QCOMPARE(pointer.readAll(), active);
        pointer.close();
        installer.cancel();
        QVERIFY(installer.installed());
        installer.remove();
        QVERIFY(!installer.installed());
        QVERIFY(QFileInfo::exists(destination.filePath("saved-project.fornaioai")));
    }
    void fakeWorkerProtocol() {
        QString python = QStandardPaths::findExecutable("python");
        if (python.isEmpty())
            QSKIP("Maintainer Python is needed only for the fake-worker test");
        QTemporaryDir dir;
        QString neutral = dir.filePath("neutral.png");
        QVERIFY(solid(128, qRgb(220, 40, 10)).save(neutral));
        AiWorker worker;
        QSignalSpy events(&worker, &AiWorker::event), failures(&worker, &AiWorker::failed);
        worker.start(python, {}, "standard", true);
        QTRY_VERIFY_WITH_TIMEOUT(events.size() >= 1, 10000);
        QVERIFY(failures.empty());
        QCOMPARE(events[0][0].toJsonObject()["event"].toString(), QString("ready"));
        worker.send({{"command", "generate"},
                     {"id", "fixture"},
                     {"neutral", neutral},
                     {"output", dir.filePath("result.png")}});
        QTRY_VERIFY_WITH_TIMEOUT(events.size() >= 2, 10000);
        QVERIFY(QFileInfo::exists(dir.filePath("result.png")));
        QCOMPARE(QImage(neutral), QImage(dir.filePath("result.png")));
        worker.send({{"command", "generate"},
                     {"id", "second"},
                     {"neutral", neutral},
                     {"output", dir.filePath("second.png")}});
        QTRY_VERIFY_WITH_TIMEOUT(events.size() >= 3, 10000);
        QCOMPARE(events.last()[0].toJsonObject()["id"].toString(), QString("second"));
        QVERIFY(worker.active());
        worker.stop();
        QVERIFY(failures.empty());
        AiPanel panel;
        QSignalSpy panelEvents(&panel.engine_, &AiWorker::event);
        panel.engine_.start(python, {}, "standard", true);
        QTRY_VERIFY_WITH_TIMEOUT(!panelEvents.empty(), 10000);
        panel.setBusy(true);
        panel.setBusy(false);
        QVERIFY(panel.engine_.active());
        panel.setExternalBusy(true);
        QVERIFY(!panel.engine_.active());
    }
    void freshRepaintInputsAndSeeds() {
        const QString python = QStandardPaths::findExecutable("python");
        if (python.isEmpty())
            QSKIP("Maintainer Python is needed only for the fake-worker test");
        QTemporaryDir storage;
        AiPanel p;
        p.model_->setCurrentIndex(p.model_->findData("fibo")); // the local worker protocol
        p.installer_.setRoot(storage.path());
        p.continuity_->setValue(0);
        QCOMPARE(p.generationResolution_->currentData().toInt(), 1024);
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        auto material = p.atlases_.firstKey();
        paintAtlas(p.atlases_[material], qRgb(255, 0, 0));
        auto c = std::make_shared<AiCapture>(capture(*p.scene_, p.atlases_));
        QVERIFY(c->neutral != c->partial);
        p.work_ = std::make_unique<QTemporaryDir>();
        p.anchor_ = "old-generated-patch.png";
        auto encoded = [](const QImage &image) {
            QByteArray data;
            QBuffer buffer(&data);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            return QString::fromLatin1(data.toBase64());
        };
        const auto acceptedReference = solid(128, qRgb(20, 40, 160));
        p.projections_ = {
            {{"id", "hidden"}, {"visible", false}, {"candidate", encoded(solid(128, qRgb(255, 0, 255)))}},
            {{"id", "excluded"},
             {"visible", true},
             {"materials", QJsonArray{"other material"}},
             {"candidate", encoded(solid(128, qRgb(0, 255, 0)))}},
            {{"id", "accepted"},
             {"visible", true},
             {"materials", QJsonArray{material}},
             {"candidate", encoded(acceptedReference)}}};
        p.enginePath_ = p.installer_.activePath();
        p.engineProfile_ = p.profile_->currentData().toString();
        QSignalSpy events(&p.engine_, &AiWorker::event);
        p.engine_.start(python, {}, "standard", true);
        QTRY_VERIFY_WITH_TIMEOUT(!events.empty(), 10000);
        const auto reference = storage.filePath("reference photo.png");
        QVERIFY(solid(64, qRgb(30, 80, 170)).save(reference));
        p.references_->addItem(reference);
        p.prompt_->clear();
        int previousSeed = -1;
        for (int pass = 0; pass < 2; ++pass) {
            p.setBusy(true);
            p.receiveCapture(c, 1);
            auto request = p.candidateSettings_;
            QCOMPARE(request["references"].toArray(), QJsonArray{reference});
            QCOMPARE(request["prompt"].toString(), p.prompt_->toPlainText());
            QVERIFY(!request.contains("appearanceAnchor")); // An earlier view copied its face onto later ones.
            QCOMPARE(QImage(request["silhouette"].toString()), c->silhouette);
            QCOMPARE(QImage(request["normal"].toString()), c->normal);
            QVERIFY(request["viewTurn"].isDouble()); // View-aware captions name the camera relative to the front.
            const int actualSeed = request["seed"].toInt();
            QVERIFY(actualSeed != previousSeed);
            previousSeed = actualSeed;
            QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
            QVERIFY(p.engine_.active()); // Both requests reuse the running worker.
            p.prompt_->setPlainText("Use the reference material with smaller markings");
        }
        p.randomSeed_->setChecked(false);
        p.seed_->setValue(1234);
        QCOMPARE(p.generationSeed(), 1234);
        QCOMPARE(p.generationSeed(), 1234);
        p.cameraIndex_ = 2;
        QCOMPARE(p.generationSeed(), 1236);
        // Hybrid feedback is independent of the projection compositing mode.
        p.cameraIndex_ = 0;
        p.blendMode_->setCurrentIndex(p.blendMode_->findData("fill"));
        p.continuity_->setValue(80);
        p.setBusy(true);
        p.receiveCapture(c, 1);
        QCOMPARE(p.candidateSettings_["conditioning"].toString(), QString("confidence"));
        QCOMPARE(QImage(p.candidateSettings_["accepted"].toString()), c->partial);
        QVERIFY(!p.candidateSettings_["feedbackConfidenceImage"].toString().isEmpty());
        // Whole earlier views are never sent: Fibo copied their layout (a face) onto later cameras.
        QVERIFY(!p.candidateSettings_.contains("appearanceAnchor"));
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        // Draft changes the captured camera and worker image, never the atlas resolution.
        const QSize atlasSize = p.atlases_.first().color.size();
        p.generationResolution_->setCurrentIndex(p.generationResolution_->findData(512));
        p.cameras_ = {p.view_->camera()};
        p.setBusy(true);
        p.nextCamera();
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        QCOMPARE(p.capture_->camera.size, 512);
        QCOMPARE(p.candidateSettings_["size"].toInt(), 512);
        QCOMPARE(p.candidate_.size(), QSize(512, 512));
        QCOMPARE(p.atlases_.first().color.size(), atlasSize);
    }
    void uncoveredHatchingIsPreviewOnly() {
        auto scene = std::make_shared<Scene>(plane());
        auto atlases = aiEmptyAtlases(*scene, 64);
        auto &a = atlases["front"];
        paintAtlas(a, qRgb(30, 180, 70));
        for (int y = 0; y < 64; ++y)
            for (int x = 32; x < 64; ++x)
                a.coverage.scanLine(y)[x] = 0;
        PreviewMaterial m;
        m.base = a.color;
        m.maps["ALB"] = a.color;
        m.maps["COVERAGE"] = a.coverage;
        m.coverage = a.coverage;
        TransferViewport view;
        QVERIFY(!view.uncoveredOverlay_); // Other modules opt out by default.
        view.resize(512, 512);
        view.setMesh(scene, 0);
        view.setCamera(capture(*scene, atlases).camera.orbit);
        view.setMaterials({{"front", m}}, false);
        view.setDisplay("ALB");
        view.show();
        QVERIFY(QTest::qWaitForWindowExposed(&view));
        auto countRed = [](const QImage &im) {
            int result = 0;
            for (int y = 0; y < im.height(); ++y)
                for (int x = 0; x < im.width(); ++x) {
                    auto p = im.pixel(x, y);
                    result += qRed(p) > 200 && qGreen(p) < 70 && qBlue(p) < 80;
                }
            return result;
        };
        QCOMPARE(countRed(view.grabFramebuffer()), 0);
        view.setUncoveredOverlay(true);
        const auto hatched = view.grabFramebuffer();
        QVERIFY(view.shader_->isLinked());
        QVERIFY(countRed(hatched) > 10000);
        const QPoint center(hatched.width() / 2, hatched.height() / 2);
        const auto coveredColor = hatched.pixel(center.x() - hatched.width() / 8, center.y());
        QVERIFY(qGreen(coveredColor) > 150 && qRed(coveredColor) < 50);
        view.setDisplay({}); // Visible with lit shading too.
        QVERIFY(countRed(view.grabFramebuffer()) > 10000);
        view.setDisplay("COVERAGE"); // The explicit coverage view remains a data preview.
        QCOMPARE(countRed(view.grabFramebuffer()), 0);
        view.setDisplay("ALB");
        m.coverage.fill(255);
        view.setMaterials({{"front", m}}, false);
        QCOMPARE(countRed(view.grabFramebuffer()), 0);
        QCOMPARE(a.color.pixel(48, 32), qRgb(30, 180, 70)); // Padded/export color is untouched.
        QDir().mkpath(".ai-qa");
        hatched.save(".ai-qa/uncovered-hatching.png");
    }
    void backFacesAreNotCaptured() {
        // The inside of an open collar seen from behind must not reach the guide as a paintable surface.
        const auto scene = plane();
        const auto atlases = aiEmptyAtlases(scene, 64);
        auto whitePixels = [](const QImage &im) {
            qint64 n = 0;
            for (int y = 0; y < im.height(); ++y)
                for (int x = 0; x < im.width(); ++x)
                    n += qGray(im.pixel(x, y)) > 127;
            return n;
        };
        QVERIFY(whitePixels(capture(scene, atlases).silhouette) > 1000);
        OrbitCamera behind;
        behind.yaw = 180;
        behind.distance = 3;
        const auto back = aiCapture(scene, 0, atlases, aiCamera(scene, behind, 128));
        QCOMPARE(whitePixels(back.silhouette), 0);
        QVERIFY(std::all_of(back.faces.begin(), back.faces.end(), [](int f) { return f < 0; }));
    }
    void lockedPaintIsNeverRepainted() {
        const auto scene = plane();
        auto atlases = aiEmptyAtlases(scene, 64);
        auto &a = atlases["front"];
        paintAtlas(a, qRgb(255, 0, 0));
        for (int y = 0; y < 64; ++y)
            for (int x = 0; x < 64; ++x) {
                if (x >= 32) // Right half: unpainted.
                    a.coverage.scanLine(y)[x] = 0;
                else if (y >= 32) // Bottom-left: placeholder paint from a grazing view.
                    reinterpret_cast<quint16 *>(a.confidence.scanLine(y))[x] = quint16(.05 * 65535);
            }
        const auto c = capture(scene, atlases);
        AiProjectionOptions options;
        options.lockQuality = aiDefaultLockQuality;
        options.colorMatch = false;
        const auto result = aiProject(scene, 0, atlases, c, solid(128, qRgb(0, 0, 255)), c.silhouette, {"front"},
                                      false, 0, nullptr, options);
        const auto &out = result.atlases["front"];
        QCOMPARE(out.color.pixel(16, 16), qRgb(255, 0, 0)); // Locked: seen well by an earlier view.
        QVERIFY(qBlue(out.color.pixel(48, 16)) > 250);       // Uncovered: painted by this view.
        QVERIFY(qBlue(out.color.pixel(16, 48)) > 128);       // Grazing placeholder: replaced.
    }
    void lockedGuideComposeAndMask() {
        const auto scene = plane();
        auto atlases = aiEmptyAtlases(scene, 64);
        auto &a = atlases["front"];
        paintAtlas(a, qRgb(255, 0, 0));
        for (int y = 0; y < 64; ++y)
            for (int x = 32; x < 64; ++x)
                a.coverage.scanLine(y)[x] = 0;
        const auto c = capture(scene, atlases);
        const auto lock = aiLockedGuide(c);
        QVERIFY(lock.any);
        const QPoint left(40, 64), right(88, 64);
        QCOMPARE(lock.guide.pixel(left), qRgb(255, 0, 0));
        const QRgb clay = lock.guide.pixel(right);
        QVERIFY(qRed(clay) == qGreen(clay) && qGreen(clay) == qBlue(clay) && qRed(clay) > 60 && qRed(clay) < 160);
        QCOMPARE(lock.guide.pixel(1, 1), qRgb(255, 255, 255)); // Background stays white.
        // The model's version of locked pixels is replaced by the accepted paint.
        const auto composed = aiComposeLocked(c, lock, solid(128, qRgb(0, 200, 0)));
        QCOMPARE(composed.pixel(left), qRgb(255, 0, 0));
        QCOMPARE(composed.pixel(right), qRgb(0, 200, 0));
        // Paint is projected; locked pixels and large bare clay the model left untouched are not.
        auto mask = aiGeneratedPaintMask(c, lock, composed);
        QCOMPARE(mask.pixel(right), qRgb(255, 255, 255));
        QCOMPARE(mask.pixel(left), qRgb(0, 0, 0));
        mask = aiGeneratedPaintMask(c, lock, aiComposeLocked(c, lock, lock.guide));
        QCOMPARE(mask.pixel(right), qRgb(0, 0, 0));
        // Scattered clay-like pixels inside real grey paint (steel, graphite) do not punch holes.
        QImage speckled = composed;
        for (int y = right.y() - 1; y <= right.y() + 1; ++y)
            for (int x = right.x() - 1; x <= right.x() + 1; ++x)
                speckled.setPixel(x, y, lock.guide.pixel(x, y));
        QCOMPARE(aiGeneratedPaintMask(c, lock, speckled).pixel(right), qRgb(255, 255, 255));
    }
    void cloudViewsMatchToneAndCrossfadeIntoLockedPaint() {
        const auto scene = plane();
        auto atlases = aiEmptyAtlases(scene, 64);
        auto &a = atlases["front"];
        paintAtlas(a, qRgb(255, 0, 0));
        for (int y = 0; y < 64; ++y)
            for (int x = 32; x < 64; ++x)
                a.coverage.scanLine(y)[x] = 0;
        const auto c = capture(scene, atlases);
        const auto lock = aiLockedGuide(c);
        // The model drew this view darker: its redraw of the locked paint is (200, 0, 0) instead of (255, 0, 0).
        QImage generated(128, 128, QImage::Format_RGB32);
        for (int y = 0; y < 128; ++y)
            for (int x = 0; x < 128; ++x)
                generated.setPixel(x, y, lock.locked.constScanLine(y)[x] ? qRgb(200, 0, 0) : qRgb(100, 100, 200));
        const auto matched = aiMatchLockedTone(c, lock, generated);
        const QRgb fresh = matched.pixel(88, 64);
        QVERIFY2(qRed(fresh) > 120, qPrintable(QString::number(qRed(fresh)))); // Brightened like the redraw.
        QVERIFY(std::abs(qBlue(fresh) - 200) <= 2);                              // No shift measured in blue.
        QCOMPARE(aiMatchLockedTone(c, aiLockedGuide(capture(scene, aiEmptyAtlases(scene, 64))), generated),
                 generated); // Nothing locked: nothing to measure.
        // Over the lock's outer band, new paint crossfades into the accepted paint with a ramped strength.
        const auto composed = aiComposeLocked(c, lock, matched);
        const auto mask = aiGeneratedPaintMask(c, lock, composed);
        int edge = -1;
        for (int x = 70; x > 20 && edge < 0; --x)
            if (lock.locked.constScanLine(64)[x] && lock.keep.constScanLine(64)[x] < 128)
                edge = x;
        QVERIFY(edge > 0);
        QVERIFY(qGray(mask.pixel(edge, 64)) > 0 && qGray(mask.pixel(edge, 64)) < 255);
        // The band's new paint is the model's redraw of the locked area; after the tone match it agrees with the
        // accepted red, so the crossfade shows no step.
        QVERIFY2(qRed(composed.pixel(edge, 64)) >= 245, qPrintable(QString::number(qRed(composed.pixel(edge, 64)))));
        QVERIFY(qRed(generated.pixel(edge, 64)) == 200);
        QCOMPARE(composed.pixel(40, 64), qRgb(255, 0, 0));
        QCOMPARE(qGray(mask.pixel(40, 64)), 0);
    }
    void generatedFeaturesAreAlignedToGeometry() {
        // A sculpted surface (bumps and creases) and a "generation" whose painted features are offset by 4 px.
        AiCapture c;
        c.camera.size = 256;
        c.normal = QImage(256, 256, QImage::Format_RGB32);
        c.silhouette = QImage(256, 256, QImage::Format_RGB32);
        c.silhouette.fill(Qt::white);
        auto height = [](float x, float y) -> float {
            float h = 0;
            for (auto [cx, cy] : {std::pair{70.f, 80.f}, {180.f, 90.f}, {128.f, 170.f}, {60.f, 200.f}})
                h += 18 * std::exp(-((x - cx) * (x - cx) + (y - cy) * (y - cy)) / 300);
            return h - 8 * std::exp(-std::pow((y - 128) / 4, 2)); // A crease, like a lip line.
        };
        for (int y = 0; y < 256; ++y)
            for (int x = 0; x < 256; ++x) {
                QVector3D n(-(height(x + 1, y) - height(x - 1, y)) / 2, (height(x, y + 1) - height(x, y - 1)) / 2, 1);
                n.normalize();
                c.normal.setPixel(x, y, qRgb(qRound((n.x() * .5f + .5f) * 255), qRound((n.y() * .5f + .5f) * 255),
                                             qRound((n.z() * .5f + .5f) * 255)));
            }
        auto painted = [&](int dy) { // Paint whose features follow the sculpted shape, drawn dy pixels off.
            QImage im(256, 256, QImage::Format_RGB32);
            for (int y = 0; y < 256; ++y)
                for (int x = 0; x < 256; ++x) {
                    const int sy = std::clamp(y + dy, 0, 255);
                    const int g = qBlue(c.normal.pixel(x, sy)); // Facing ratio: dark on slopes and in the crease.
                    im.setPixel(x, y, qRgb(g, g * 3 / 4, g / 2));
                }
            return im;
        };
        const QImage truth = painted(0), offset = painted(4); // Within the 5 px reach at 256 (20 px at 1024).
        auto error = [&](const QImage &im) {
            double e = 0;
            for (int y = 16; y < 240; ++y)
                for (int x = 16; x < 240; ++x)
                    e += std::abs(qRed(im.pixel(x, y)) - qRed(truth.pixel(x, y)));
            return e;
        };
        double shift = 0;
        const QImage aligned = aiAlignToGeometry(c, offset, &shift);
        QVERIFY2(error(aligned) < .5 * error(offset),
                 qPrintable(QString("%1 vs %2, shift %3").arg(error(aligned)).arg(error(offset)).arg(shift)));
        QVERIFY(shift > .5 && shift < 6);
        // Already aligned paint stays put.
        double still = 0;
        aiAlignToGeometry(c, truth, &still);
        QVERIFY2(still < 1.5, qPrintable(QString::number(still)));
    }
    void cloudModelGeneratesWithLockedGuide() {
        qputenv("FORNAIO_GPT_BACKEND", "fake");
        auto restore = qScopeGuard([] { qunsetenv("FORNAIO_GPT_BACKEND"); });
        QTemporaryDir storage;
        AiPanel p;
        p.installer_.setRoot(storage.path()); // No local engine: GPT Image 2 needs no download.
        p.model_->setCurrentIndex(p.model_->findData("gpt"));
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.prompt_->setPlainText("weathered bronze");
        p.refresh();
        QVERIFY(p.generate_->isEnabled());
        auto c = std::make_shared<AiCapture>(capture(*p.scene_, p.atlases_));
        p.work_ = std::make_unique<QTemporaryDir>();
        p.setBusy(true);
        p.receiveCapture(c, 1);
        const auto request = p.candidateSettings_;
        QCOMPARE(request["model"].toString(), QString("gpt-image-2"));
        QVERIFY(request["prompt"].toString().contains("weathered bronze"));
        QVERIFY(request["prompt"].toString().contains("unlit albedo"));
        QVERIFY(QFileInfo::exists(request["images"].toArray().first().toString()));
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        QCOMPARE(p.candidate_.size(), QSize(c->camera.size, c->camera.size));
        // The test backend returns the guide itself: untouched clay is never projected as paint.
        QVERIFY(p.lock_ && p.capture_);
        QCOMPARE(covered(aiGeneratedPaintMask(*p.capture_, *p.lock_, p.candidate_)
                             .convertToFormat(QImage::Format_Grayscale8)),
                 0);
        QVERIFY(aiGptPrompt(false, true, {}, "side view").contains("Paint ONLY the flat gray clay areas"));
        p.model_->setCurrentIndex(p.model_->findData("fibo"));
        p.refresh();
        QVERIFY(!p.generate_->isEnabled()); // Fibo is an optional download.
    }
    void realCloudGenerationSmoke() {
        // One real GPT Image 2 call through the panel (Codex or the API). Opt-in: it uses the plan's image limits.
        if (qEnvironmentVariableIsEmpty("FORNAIO_REAL_GPT"))
            QSKIP("Set FORNAIO_REAL_GPT=1 to run one real GPT Image 2 generation");
        AiPanel p;
        p.model_->setCurrentIndex(p.model_->findData("gpt"));
        if (p.cloud_.backend() == AiCloudImage::Backend::None)
            QSKIP("No Codex or OPENAI_API_KEY available");
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.prompt_->setPlainText("red and white checkerboard tiles");
        auto c = std::make_shared<AiCapture>(capture(*p.scene_, p.atlases_, 512));
        p.work_ = std::make_unique<QTemporaryDir>();
        p.setBusy(true);
        p.receiveCapture(c, 1);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 400000);
        QVERIFY2(!p.candidate_.isNull(), qPrintable(p.status_->text()));
        QCOMPARE(p.candidate_.size(), QSize(512, 512));
        QVERIFY(covered(aiGeneratedPaintMask(*p.capture_, *p.lock_, p.candidate_)
                            .convertToFormat(QImage::Format_Grayscale8)) > 10000);
    }
    void panelWithoutEngine() {
        QTemporaryDir storage;
        AiPanel p;
        p.model_->setCurrentIndex(p.model_->findData("fibo"));
        // New projects use independent projection layers; legacy generation remains tested below.
        QVERIFY(p.parity_);
        QCOMPARE(p.blendMode_->currentData().toString(), QString("blend"));
        QVERIFY(!p.colorMatch_->isChecked());
        p.installer_.setRoot(storage.path());
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        QVERIFY(!p.generate_->isEnabled());
        p.parity_ = false; // Preserve coverage of the legacy retained-detail worker protocol.
        for (auto group : p.findChildren<QGroupBox *>())
            if (group->title() == "Local AI engine")
                group->setChecked(true);
        QVERIFY(p.install_->isEnabled());
        QCOMPARE(p.materials_->count(), 1);
        p.prompt_->setPlainText("red paint");
        QVERIFY(!p.generate_->isEnabled());
        p.setExternalBusy(true);
        QVERIFY(!p.install_->isEnabled());
        p.setExternalBusy(false);
        p.resize(1320, 900);
        p.show();
        QVERIFY(QTest::qWaitForWindowExposed(&p));
        auto c = std::make_shared<AiCapture>(capture(*p.scene_, p.atlases_));
        c->confidence.fill(0xcccc);
        p.capture_ = c;
        p.candidateSettings_ = {{"id", "ui-candidate"}, {"camera", aiCameraJson(c->camera)}};
        QByteArray confidenceBytes;
        QBuffer confidenceBuffer(&confidenceBytes);
        confidenceBuffer.open(QIODevice::WriteOnly);
        QVERIFY(c->confidence.save(&confidenceBuffer, "PNG"));
        p.candidateSettings_["feedbackConfidenceImage"] = QString::fromLatin1(confidenceBytes.toBase64());
        QString candidate = storage.filePath("candidate.png");
        QString retained = storage.filePath("retained.png");
        QVERIFY(solid(128, qRgb(180, 30, 20)).save(candidate));
        QVERIFY(solid(128, qRgb(255, 255, 255)).save(retained));
        p.setBusy(true);
        p.workerEvent({{"event", "result"},
                       {"id", "ui-candidate"},
                       {"output", candidate},
                       {"detailReference", candidate},
                       {"preservationMask", retained},
                       {"seconds", 1},
                       {"alignment", 1.0}});
        QCOMPARE(p.candidate_, QImage(candidate));
        QCOMPARE(p.previews_->currentIndex(), 0); // Generation leaves 3D visible.
        auto side = p.findChild<QWidget *>("aiSideCandidate");
        QVERIFY(side && side->isVisible());
        QVERIFY(p.rect().contains(p.project_->mapTo(&p, p.project_->rect().center())));
        QVERIFY(p.project_->isVisible()); // Acceptance stays in the fixed footer, outside the scroll panel.
        QVERIFY(side->mapTo(&p, QPoint()).y() < p.prompt_->mapTo(&p, QPoint()).y());
        QVERIFY(p.prompt_->mapTo(&p, QPoint()).y() < p.references_->mapTo(&p, QPoint()).y());
        QCOMPARE(p.generate_->text(), QString("Generate image"));
        p.generationMode_->setCurrentIndex(1);
        QCOMPARE(p.generate_->text(), QString("Generate"));
        QVERIFY(p.multiviewBox_->isVisible() && !p.candidateFrame_->isVisible()); // Views replace the candidate.
        QCOMPARE(p.viewGrid_->count(), p.multiviewCameras_.size());
        QCOMPARE(p.multiviewCameras_.size(), 9); // Front, 45-degree ring, back and top.
        p.randomSeed_->setChecked(false);
        p.continuity_->setValue(37);
        p.generationResolution_->setCurrentIndex(p.generationResolution_->findData(512));
        p.protectDetails_->setChecked(false);
        p.seed_->setValue(412);
        p.showUncovered_->setChecked(false);
        QTest::mouseClick(side, Qt::LeftButton, Qt::NoModifier, side->rect().center());
        QString project = storage.filePath("mask.fornaioai");
        QTimer::singleShot(0, &p, [project] {
            auto dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->selectFile(project);
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        p.saveProject();
        AiAtlases restored;
        QList<QJsonObject> records;
        auto settings = aiLoadProject(project, restored, records);
        QImage mask = QImage::fromData(
            QByteArray::fromBase64(settings["pendingCandidate"].toObject()["mask"].toString().toLatin1()),
            "PNG");
        QVERIFY(!mask.isNull());
        QCOMPARE(qGray(mask.pixel(64, 64)), 0); // Side canvas edits share the full-size canvas mask.
        QTimer::singleShot(0, &p, [project] {
            auto dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->selectFile(project);
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        p.openProject();
        QCOMPARE(p.generationMode_->currentIndex(), 1);
        QVERIFY(!p.randomSeed_->isChecked());
        QCOMPARE(p.seed_->value(), 412);
        QCOMPARE(p.continuity_->value(), 37);
        QCOMPARE(p.generationResolution_->currentData().toInt(), 512);
        QVERIFY(!p.protectDetails_->isChecked());
        QCOMPARE(qGray(p.candidateRetention_.pixel(64, 64)), 255);
        QCOMPARE(QImage::fromData(QByteArray::fromBase64(
                                      p.candidateSettings_["detailReferenceImage"].toString().toLatin1()),
                                  "PNG"),
                 QImage(candidate));
        QCOMPARE(reinterpret_cast<const quint16 *>(p.capture_->confidence.constScanLine(64))[64],
                 quint16(0xcccc));
        QVERIFY(!p.showUncovered_->isChecked());
        p.generationMode_->setCurrentIndex(0);
        p.showUncovered_->setChecked(true);
        p.continuity_->setValue(80);
        p.grab().save("artifacts/ai-panel.png");
    }
    void projectRestoresNonDefaultUv() {
        QTemporaryDir storage;
        AiPanel p;
        p.installer_.setRoot(storage.path());
        QVERIFY(p.loadMesh("samples/stacked_cards.fbx"));
        QVERIFY(!p.meshErrors_.empty());
        p.uv_->setCurrentIndex(1);
        p.parity_ = false; // Exercise old project settings and migration explicitly.
        QVERIFY(p.meshErrors_.empty());
        auto material = p.atlases_.firstKey();
        p.atlases_[material].color.fill(Qt::red);
        p.atlases_[material].coverage.fill(255);
        p.projections_.append(QJsonObject{{"seed", 42}});
        p.blendMode_->setCurrentIndex(p.blendMode_->findData("blend"));
        p.projectionStrength_->setValue(85);
        p.projectionFeather_->setValue(12);
        p.angleFalloff_->setValue(4);
        p.projectionAngle_->setValue(55);
        QVERIFY(p.fillGaps_->isChecked());
        p.fillGaps_->setChecked(false);
        p.view_->setFieldOfView(15);
        p.colorMatch_->setChecked(false);
        QString path = storage.filePath("saved.fornaioai");
        auto choose = [path] {
            auto d = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            QVERIFY(d);
            d->selectFile(path);
            QMetaObject::invokeMethod(d, "accept", Qt::QueuedConnection);
        };
        QTimer::singleShot(0, &p, choose);
        p.saveProject();
        QVERIFY(QFileInfo::exists(path));
        p.uv_->setCurrentIndex(0);
        p.blendMode_->setCurrentIndex(2);
        p.projectionStrength_->setValue(100);
        p.projectionAngle_->setValue(80);
        p.fillGaps_->setChecked(true);
        p.view_->setFieldOfView(40);
        QVERIFY(!p.meshErrors_.empty());
        QTimer::singleShot(0, &p, choose);
        p.openProject();
        QCOMPARE(p.uv_->currentIndex(), 1);
        QVERIFY(p.meshErrors_.empty());
        QCOMPARE(p.atlases_[material].color.pixel(100, 100), qRgb(255, 0, 0));
        QCOMPARE(p.projections_.size(), 1);
        QCOMPARE(p.blendMode_->currentData().toString(), QString("blend"));
        QCOMPARE(p.projectionStrength_->value(), 85);
        QCOMPARE(p.projectionFeather_->value(), 12);
        QCOMPARE(p.angleFalloff_->value(), 4);
        QCOMPARE(p.projectionAngle_->value(), 55);
        QVERIFY(!p.fillGaps_->isChecked());
        QCOMPARE(p.cameraFov_->value(), 15);
        QCOMPARE(p.view_->camera().fieldOfView, 15.f);
        QVERIFY(!p.colorMatch_->isChecked());
        // Migrate 1.2.3's protected default without changing saved paint.
        AiAtlases legacyAtlases;
        QList<QJsonObject> legacyRecords;
        auto legacy = aiLoadProject(path, legacyAtlases, legacyRecords);
        legacy.remove("projectionBackend");
        legacy.remove("projectionBehaviorVersion");
        legacy.remove("projectionAngle");
        legacy.remove("projectionFillGaps");
        auto legacyCamera = legacy["camera"].toObject();
        legacyCamera.remove("fieldOfView");
        legacy["camera"] = legacyCamera;
        legacy["blendMode"] = "blend";
        legacy["colorMatch"] = true;
        aiSaveProject(path, legacy, legacyAtlases, legacyRecords);
        QTimer::singleShot(0, &p, choose);
        p.openProject();
        QCOMPARE(p.blendMode_->currentData().toString(), QString("overlay"));
        QCOMPARE(p.projectionAngle_->value(), 60);
        QVERIFY(p.fillGaps_->isChecked());
        QCOMPARE(p.cameraFov_->value(), 20);
        QVERIFY(!p.colorMatch_->isChecked());
        QCOMPARE(p.atlases_[material].color.pixel(100, 100), qRgb(255, 0, 0));
    }
    void panelReprojectsRepeatedlyOverFullCoverage() {
        QTemporaryDir storage;
        AiPanel p;
        p.blendMode_->setCurrentIndex(p.blendMode_->findData("overlay")); // repaint semantics
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.resolution_->setCurrentIndex(p.resolution_->findData(1024));
        p.parity_ = false;
        p.blendMode_->setCurrentIndex(p.blendMode_->findData("overlay"));
        const auto material = p.atlases_.firstKey();
        paintAtlas(p.atlases_[material], qRgb(255, 0, 0));
        const auto coverage = p.atlases_[material].coverage;
        p.capture_ = std::make_shared<AiCapture>(capture(*p.scene_, p.atlases_, 128));
        p.candidateSettings_ = {{"id", "repaint-fixture"}, {"camera", aiCameraJson(p.capture_->camera)}};
        const auto output = storage.filePath("blue.png");
        QVERIFY(solid(128, qRgb(0, 0, 255)).save(output));
        p.setBusy(true);
        p.workerEvent(
            {{"event", "result"}, {"id", "repaint-fixture"}, {"output", output}, {"alignment", 1.0}});
        for (int pass = 0; pass < 2; ++pass) {
            QVERIFY(p.project_->isEnabled());
            p.project_->click();
            QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
            QCOMPARE(p.projections_.size(), pass + 1);
            QCOMPARE(p.atlases_[material].coverage, coverage);
            QRgb color = p.atlases_[material].color.pixel(512, 512);
            QVERIFY(qBlue(color) > 250 && qRed(color) < 10);
        }
    }
    void regeneratedLayerIsReplacedInPlace() {
        QTemporaryDir storage;
        AiPanel p;
        p.blendMode_->setCurrentIndex(p.blendMode_->findData("overlay")); // repaint semantics
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.resolution_->setCurrentIndex(p.resolution_->findData(1024));
        p.parity_ = false;
        p.blendMode_->setCurrentIndex(p.blendMode_->findData("overlay"));
        const auto material = p.atlases_.firstKey();
        auto project = [&](QRgb color, const QString &id) {
            p.capture_ = std::make_shared<AiCapture>(capture(*p.scene_, p.atlases_, 128));
            p.candidateSettings_ = {{"id", id}, {"camera", aiCameraJson(p.capture_->camera)}};
            const auto output = storage.filePath(id + ".png");
            QVERIFY(solid(128, color).save(output));
            p.setBusy(true);
            p.workerEvent({{"event", "result"}, {"id", id}, {"output", output}, {"alignment", 1.0}});
            p.project_->click();
            QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        };
        project(qRgb(255, 0, 0), "first");
        project(qRgb(0, 255, 0), "second");
        QCOMPARE(p.projections_.size(), 2);
        // Copies: `auto` would hold QJsonValueRefs into projections_, which the regeneration replaces.
        const QJsonValue upper = p.projections_[1]["candidate"];
        const QJsonValue replaced = p.projections_[0]["candidate"];
        p.regenerateIndex_ = 0; // As regenerateLayer() leaves it once the worker returns a candidate.
        project(qRgb(0, 0, 255), "regenerated");
        QCOMPARE(p.projections_.size(), 2); // Replaced, not appended.
        QVERIFY(p.projections_[0]["candidate"] != replaced);
        QCOMPARE(p.projections_[1]["candidate"], upper); // Layers above are replayed unchanged.
        QRgb color = p.atlases_[material].color.pixel(512, 512);
        QVERIFY(qGreen(color) > 250 && qBlue(color) < 10); // The upper layer still covers the replaced one.
        QCOMPARE(p.regenerateIndex_, -1);
        p.setLayerVisible(1, false);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        color = p.atlases_[material].color.pixel(512, 512);
        QVERIFY(qBlue(color) > 250 && qRed(color) < 10); // The regenerated paint is underneath.
        p.undoButton_->click();
        p.undoButton_->click();
        QCOMPARE(p.projections_[0]["candidate"], replaced);
    }
    void automaticProjectionBlendsAndHistoryRestoresConfidence() {
        QTemporaryDir storage;
        AiPanel p;
        p.blendMode_->setCurrentIndex(p.blendMode_->findData("overlay")); // repaint semantics
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.parity_ = false;
        p.blendMode_->setCurrentIndex(p.blendMode_->findData("overlay"));
        auto initial = p.atlases_;
        p.capture_ = std::make_shared<AiCapture>(aiCapture(*p.scene_, p.uv_->currentIndex(), p.atlases_,
                                                           aiCamera(*p.scene_, p.view_->camera(), 128)));
        const auto output = storage.filePath("candidate.png");
        QVERIFY(solid(128, qRgb(170, 60, 20)).save(output));
        p.candidateSettings_ = {{"id", "automatic-fixture"}, {"camera", aiCameraJson(p.capture_->camera)}};
        // Simulate the final automatic view without an inference dependency.
        p.automatic_ = true;
        p.setBusy(true);
        p.workerEvent(
            {{"event", "result"}, {"id", "automatic-fixture"}, {"output", output}, {"alignment", 1.0}});
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        QCOMPARE(p.projections_.size(), 1);
        QVERIFY(p.anchor_.isEmpty()); // Repaint multiview never feeds a generated view back in.
        QVERIFY(!p.projections_.last()["fillOnly"].toBool());
        QCOMPARE(p.projections_.last()["blending"].toObject()["mode"].toString(), QString("overlay"));
        QVERIFY(p.projections_.last()["blending"].toObject()["fillGaps"].toBool());
        const auto material = p.atlases_.firstKey();
        auto accepted = p.atlases_[material].confidence;
        QVERIFY(accepted != initial[material].confidence);
        p.undoButton_->click();
        QCOMPARE(p.atlases_[material].confidence, initial[material].confidence);
        p.redoButton_->click();
        QCOMPARE(p.atlases_[material].confidence, accepted);
    }
    void projectionLayerPanelVisibilityHistoryAndSave() {
        QTemporaryDir storage;
        AiPanel p;
        QVERIFY(p.loadMesh("samples/single_card.obj"));
        p.resolution_->setCurrentIndex(p.resolution_->findData(1024));
        p.parity_ = false;
        p.blendMode_->setCurrentIndex(p.blendMode_->findData("blend"));
        const auto material = p.atlases_.firstKey();
        AiAtlases first;
        for (int i = 0; i < 2; ++i) {
            p.capture_ = std::make_shared<AiCapture>(capture(*p.scene_, p.atlases_, 128));
            const auto id = QString::number(i);
            const auto output = storage.filePath(id + ".png");
            QVERIFY(solid(128, i == 0 ? qRgb(210, 30, 20) : qRgb(20, 40, 220)).save(output));
            p.candidateSettings_ = {{"id", id}, {"camera", aiCameraJson(p.capture_->camera)}};
            p.setBusy(true);
            p.workerEvent({{"event", "result"}, {"id", id}, {"output", output}, {"alignment", 1.0}});
            p.project_->click();
            QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
            if (i == 0)
                first = p.atlases_;
        }
        const auto both = p.atlases_;
        QCOMPARE(p.layers_->count(), 2);
        QCOMPARE(p.layers_->item(0)->data(Qt::UserRole).toInt(), 1);
        QVERIFY(p.projections_[0]["uvLayer"].isObject());
        p.resize(1420, 900);
        p.show();
        QVERIFY(QTest::qWaitForWindowExposed(&p));
        QTRY_VERIFY(!p.view_->texturesDirty_);
        const unsigned originalTexture = p.view_->gpu_[material].base;
        QCOMPARE(p.view_->gpu_[material].maps["ALB"], originalTexture);
        QCOMPARE(p.view_->gpu_[material].maps["COVERAGE"], p.view_->gpu_[material].coverage);
        auto row = p.layers_->visualItemRect(p.layers_->item(0));
        QTest::mouseClick(p.layers_->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(row.left() + 13, row.center().y()));
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        QVERIFY(!p.projections_[1]["visible"].toBool(true));
        QCOMPARE(p.atlases_[material].color, first[material].color);
        QCOMPARE(p.atlases_[material].confidence, first[material].confidence);
        QCOMPARE(p.projections_.size(), 2); // Hiding is not deleting.
        const auto hiddenCacheKey = p.atlases_[material].color.cacheKey();
        QElapsedTimer visibilityTimer;
        visibilityTimer.start();
        p.layers_->item(0)->setCheckState(Qt::Checked);
        QVERIFY(!p.busy_); // A cached visibility state is synchronous, not another composition job.
        QCOMPARE(p.atlases_[material].color, both[material].color);
        QTRY_VERIFY(!p.view_->texturesDirty_);
        QCOMPARE(p.view_->gpu_[material].base, originalTexture);
        qInfo() << "Cached layer show, including viewport paint:" << visibilityTimer.elapsed() << "ms";
        visibilityTimer.restart();
        p.layers_->item(0)->setCheckState(Qt::Unchecked);
        QVERIFY(!p.busy_);
        QCOMPARE(p.atlases_[material].color.cacheKey(), hiddenCacheKey);
        qInfo() << "Cached layer hide:" << visibilityTimer.elapsed() << "ms";
        p.undoButton_->click();
        QCOMPARE(p.atlases_[material].color, both[material].color);
        QCOMPARE(p.layers_->item(0)->checkState(), Qt::Checked);
        p.redoButton_->click();
        QCOMPARE(p.atlases_[material].color, first[material].color);
        const auto project = storage.filePath("layer-visibility.fornaioai");
        auto choose = [project] {
            auto dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->selectFile(project);
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        };
        QTimer::singleShot(0, &p, choose);
        p.saveProject();
        QTimer::singleShot(0, &p, choose);
        p.openProject();
        QCOMPARE(p.layers_->count(), 2);
        QCOMPARE(p.layers_->item(0)->checkState(), Qt::Unchecked);
        QCOMPARE(p.atlases_[material].color, first[material].color);
        p.layers_->item(0)->setCheckState(Qt::Checked);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        QCOMPARE(p.atlases_[material].color, both[material].color);
        QCOMPARE(p.atlases_[material].coverage, both[material].coverage);
        // Existing 1.2.8 projects contain camera/image records but no UV layer cache.
        AiAtlases savedAtlases;
        QList<QJsonObject> legacyRecords;
        auto legacySettings = aiLoadProject(project, savedAtlases, legacyRecords);
        legacySettings.remove("projectionBackend");
        legacySettings.remove("layerBase");
        for (auto &record : legacyRecords) {
            record.remove("uvLayer");
            record.remove("visible");
        }
        const auto legacyProject = storage.filePath("legacy-layers.fornaioai");
        aiSaveProject(legacyProject, legacySettings, both, legacyRecords);
        QTimer::singleShot(0, &p, [legacyProject] {
            auto dialog = qobject_cast<QFileDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->selectFile(legacyProject);
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        });
        p.openProject();
        QCOMPARE(p.atlases_[material].color, both[material].color);
        p.layers_->item(0)->setCheckState(Qt::Unchecked);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        QCOMPARE(p.atlases_[material].color, first[material].color);
        QVERIFY(p.projections_[0]["uvLayer"].isObject());
        p.layers_->item(0)->setCheckState(Qt::Checked);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        QCOMPARE(p.atlases_[material].color, both[material].color);
        p.layers_->item(1)->setCheckState(Qt::Unchecked);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        p.layers_->item(0)->setCheckState(Qt::Unchecked);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        QCOMPARE(covered(p.atlases_[material].coverage), 0);
        p.layers_->item(1)->setCheckState(Qt::Checked);
        QTRY_VERIFY_WITH_TIMEOUT(!p.busy_, 10000);
        QCOMPARE(p.atlases_[material].color, first[material].color);
        auto layerPanel = p.findChild<QWidget *>("aiLayersPanel");
        QVERIFY(layerPanel && layerPanel->isVisible());
        QVERIFY(layerPanel->mapTo(&p, QPoint()).x() > p.previews_->mapTo(&p, QPoint()).x());
        p.grab().save("artifacts/ai-layers.png");
    }
};

int main(int argc, char **argv) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    const auto projectionCleanup = qScopeGuard([] { aiShutdownProjectionRenderer(); });
    app.setOrganizationName("Fornaio-Tests");
    app.setApplicationName("AI-regressions");
    if (app.arguments().contains("--head-gap-probe")) {
        const int index = app.arguments().indexOf("--head-gap-probe");
        headGapProbe(app.arguments().value(index + 1), app.arguments().value(index + 2));
        return 0;
    }
    if (app.arguments().contains("--saved-gap-probe")) {
        const int index = app.arguments().indexOf("--saved-gap-probe");
        savedProjectGapProbe(app.arguments().value(index + 1), app.arguments().value(index + 2));
        return 0;
    }
    if (app.arguments().contains("--blend-quality")) {
        blendQuality(app.arguments().value(app.arguments().indexOf("--blend-quality") + 1));
        return 0;
    }
    if (app.arguments().contains("--render-quality")) {
        renderQuality(app.arguments().value(app.arguments().indexOf("--render-quality") + 1));
        return 0;
    }
    if (app.arguments().contains("--continuity-fixture")) {
        continuityFixture(app.arguments().value(app.arguments().indexOf("--continuity-fixture") + 1));
        return 0;
    }
    if (app.arguments().contains("--project-continuity-fixture")) {
        const int index = app.arguments().indexOf("--project-continuity-fixture");
        projectContinuityFixture(app.arguments().value(index + 1), app.arguments().value(index + 2));
        return 0;
    }
    if (app.arguments().contains("--fresh-head-pair")) {
        const int index = app.arguments().indexOf("--fresh-head-pair");
        freshHeadPairFixture(app.arguments().value(index + 1), app.arguments().value(index + 2));
        return 0;
    }
    if (app.arguments().contains("--project-quality")) {
        projectQuality(app.arguments().value(app.arguments().indexOf("--project-quality") + 1),
                       app.arguments().contains("--second"));
        return 0;
    }
    if (app.arguments().contains("--install-live")) {
        AiInstaller installer;
        auto m = AiManifest::load(aiAssetsPath("manifest.json"));
        installer.setManifest(m);
        QString root = app.arguments().value(app.arguments().indexOf("--install-live") + 1);
        installer.setRoot(root);
        QDir().mkpath(root);
        QFile log(root + "/validation.log");
        log.open(QIODevice::WriteOnly | QIODevice::Truncate);
        QObject::connect(&installer, &AiInstaller::progress, [&](qint64 n, qint64 t, const QString &s) {
            static qint64 last = -1;
            if (n / 100000000 != last || s.startsWith("Checking") || s.startsWith("Verifying")) {
                last = n / 100000000;
                QByteArray line =
                    QByteArray::number(n) + "/" + QByteArray::number(t) + " " + s.toUtf8() + "\n";
                log.write(line);
                log.flush();
            }
        });
        QObject::connect(&installer, &AiInstaller::finished, &app, [&](bool ok, const QString &message) {
            log.write(message.toUtf8() + "\n");
            log.flush();
            app.exit(ok ? 0 : 1);
        });
        QTimer::singleShot(0, &installer, [&] { installer.install(); });
        return app.exec();
    }
    QTemporaryDir prefs;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, prefs.path());
    AiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ai_tests.moc"
