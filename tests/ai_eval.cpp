// Maintainer tool: drive AI capture/feedback/projection from scripts to evaluate generators end to end.
// State (atlases plus the last capture camera) lives in a JSON file between steps; no GUI or model is needed.
//
//   fornaio_ai_eval capture <mesh> <state.json> <yaw> <pitch> <outdir>   writes guides for the next view
//   fornaio_ai_eval project <mesh> <state.json> <outdir> <candidate.png> [mask.png [retained.png]]
//   fornaio_ai_eval render  <mesh> <state.json> <yaw> <pitch> <out.png>  atlas seen from a camera (no
//   lighting) fornaio_ai_eval ownproject <mesh> <state.json> <outdir> <generated.png>  the panel's GPT Image
//   projection
#include "ai_cloud.h"
#include "ai_layers.h"
#include "ai_smart_fill.h"
#include "ai_texture.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QSurfaceFormat>
#include <QTimer>
#include <cmath>
#include <cstdio>

namespace {
constexpr int atlasSize = 2048, viewSize = 1024;
constexpr float continuity = .8f;

QString blendMode() { // FORNAIO_EVAL_BLEND: overlay (panel default, repaint) | blend | fill
    const QString mode = qEnvironmentVariable("FORNAIO_EVAL_BLEND", "overlay");
    if (mode != "overlay" && mode != "blend" && mode != "fill")
        throw std::runtime_error("FORNAIO_EVAL_BLEND must be overlay, blend or fill");
    return mode;
}
AiProjectionOptions panelDefaults() {
    AiProjectionOptions o; // Matches AiPanel's defaults: camera projection (repaint).
    o.overlay = blendMode() == "overlay";
    o.strength = 1;
    o.feather = .06f;
    o.anglePower = 2;
    o.minCosine = std::cos(60 * 3.14159265359f / 180);
    o.fillGaps = true;
    o.coverAll = true;
    o.colorMatch = false;
    return o;
}
AiCamera camera(const Scene &s, float yaw, float pitch) {
    OrbitCamera o;
    o.yaw = yaw;
    o.pitch = pitch;
    o.fieldOfView = 20;
    o.orthographic = true;
    return aiCamera(s, aiFrameCamera(s, o), viewSize);
}
AiAtlases loadState(const Scene &s, const QString &path, QJsonObject *root = nullptr) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return aiEmptyAtlases(s, atlasSize);
    const auto json = QJsonDocument::fromJson(f.readAll()).object();
    if (root)
        *root = json;
    return aiDeserializeAtlases(json["atlases"].toArray());
}
void saveState(const QString &path, QJsonObject root, const AiAtlases &atlases) {
    root["atlases"] = aiSerializeAtlases(atlases);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(root).toJson(QJsonDocument::Compact)) < 0)
        throw std::runtime_error("Cannot write state");
}
void save(const QImage &image, const QString &path) {
    if (!image.save(path))
        throw std::runtime_error(("Cannot write " + path).toStdString());
}
QString imageData(const QImage &image) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!image.save(&buffer, "PNG"))
        throw std::runtime_error("Cannot encode evaluation image");
    return QString::fromLatin1(bytes.toBase64());
}
void smartFillProbe(const QString &project, const QString &out, bool liveOne, bool prepareOnly = false,
                    const QString &replayImage = {}) {
    AiAtlases before;
    QList<QJsonObject> records;
    auto settings = aiLoadProject(project, before, records);
    const QString meshPath =
        QDir(QFileInfo(project).absolutePath()).absoluteFilePath(settings["mesh"].toString());
    QFile meshFile(meshPath);
    if (!meshFile.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot read Smart fill test mesh");
    QCryptographicHash meshHash(QCryptographicHash::Sha256);
    meshHash.addData(&meshFile);
    if (QString::fromLatin1(meshHash.result().toHex()) != settings["meshSha256"].toString())
        throw std::runtime_error("Smart fill test mesh fingerprint changed");
    const Scene scene = loadScene(meshPath);
    const int uv = settings["uv"].toInt(), resolution = settings["resolution"].toInt(2048);
    const bool parity = settings["projectionBackend"] == "modddif-v1";
    const auto base = settings["layerBase"].isArray() ? aiDeserializeAtlases(settings["layerBase"].toArray())
                                                      : aiEmptyAtlases(scene, resolution);
    QSet<QString> materials;
    for (auto it = before.begin(); it != before.end(); ++it)
        materials.insert(it.key());
    QDir().mkpath(out);
    QElapsedTimer timer;
    timer.start();
    const auto domains = aiUvDomains(scene, uv, resolution, materials);
    auto plan = aiPlanSmartFill(scene, uv, before, domains, materials,
                                aiSmartFillFingerprint(scene, uv, before, records, parity));
    const int requestCount = aiSmartFillRemaining(plan);
    const int livePatch = qEnvironmentVariableIntValue("FORNAIO_SMART_FILL_PATCH");
    if ((liveOne || prepareOnly) && (livePatch < 0 || livePatch >= requestCount))
        throw std::runtime_error("Invalid live Smart fill sample patch");
    AiCamera inspection = settings["camera"].isObject() ? aiCameraFromJson(settings["camera"].toObject())
                                                        : camera(scene, 20, 15);
    const QImage appearance = aiCapture(scene, uv, before, inspection).partial;
    if (prepareOnly) {
        const auto request = aiSmartFillRequest(plan, livePatch, out, appearance);
        QFile requestFile(out + "/request.json");
        if (!requestFile.open(QIODevice::WriteOnly))
            throw std::runtime_error("Cannot save prepared Smart fill request");
        requestFile.write(QJsonDocument(request).toJson());
        const auto images = request["images"].toArray();
        save(QImage(images[0].toString()), out + "/texture-board.png");
        save(QImage(images[1].toString()), out + "/repair-map.png");
        save(QImage(images[2].toString()), out + "/appearance.png");
        save(QImage(request["mask"].toString()), out + "/api-mask.png");
        std::printf("Prepared request %d of %d; no generation performed.\n", livePatch + 1, requestCount);
        return;
    }
    int completed = 0;
    qint64 acceptanceMilliseconds = 0;
    QImage lastGenerated;
    for (int i = 0; i < plan["patches"].toArray().size(); ++i) {
        if (liveOne && i != livePatch)
            continue;
        const QString patchDirectory = out + QString("/patch-%1").arg(i);
        QDir().mkpath(patchDirectory);
        const auto request = aiSmartFillRequest(plan, i, patchDirectory, appearance);
        QImage generated;
        if (liveOne) {
            AiCloudImage cloud;
            if (cloud.backend() == AiCloudImage::Backend::None)
                throw std::runtime_error("No GPT Image 2 backend is available");
            QEventLoop loop;
            QString error;
            QObject::connect(&cloud, &AiCloudImage::event, &loop, [&](const QJsonObject &event) {
                if (event["event"] == "error") {
                    error = event["message"].toString();
                    loop.quit();
                }
                if (event["event"] == "result") {
                    generated = QImage(event["output"].toString());
                    loop.quit();
                }
            });
            QTimer deadline;
            deadline.setSingleShot(true);
            QObject::connect(&deadline, &QTimer::timeout, &loop, [&] {
                error = "Smart fill live sample timed out";
                cloud.cancel();
                loop.quit();
            });
            deadline.start(600000);
            cloud.generate(request);
            loop.exec();
            if (!error.isEmpty())
                throw std::runtime_error(error.toStdString());
            if (generated.isNull())
                throw std::runtime_error("Smart fill live sample returned no image");
        } else if (!replayImage.isEmpty()) {
            generated = QImage(replayImage);
            if (generated.isNull())
                throw std::runtime_error("Cannot load cached Smart fill result");
        } else {
            // Deliberately repaint every returned pixel. Native acceptance must
            // discard those changes everywhere except the frozen missing texels.
            generated = QImage(1024, 1024, QImage::Format_RGB32);
            generated.fill(QColor(45, 220, 165));
            save(generated, patchDirectory + "/synthetic-output.png");
        }
        QElapsedTimer acceptance;
        lastGenerated = generated;
        acceptance.start();
        plan = aiSmartFillAcceptPatch(plan, i, generated);
        acceptanceMilliseconds += acceptance.elapsed();
        ++completed;
        if (liveOne)
            break; // Bounded live quality probe: exactly one request.
    }
    const auto repair = aiSmartFillLayer(plan);
    auto afterRecords = records;
    int repairIndex = 0;
    while (repairIndex < afterRecords.size() && aiIsRepairLayer(afterRecords[repairIndex]))
        ++repairIndex;
    afterRecords.insert(repairIndex, repair);
    const auto after = aiComposeLayerStack(base, afterRecords, parity);
    qint64 changedKnown = 0, changedOutside = 0, missingBefore = 0, missingAfter = 0, painted = 0;
    double rawSeamError = 0, matchedSeamError = 0;
    qint64 seamSamples = 0;
    int m = 0;
    for (auto it = before.begin(); it != before.end(); ++it) {
        const auto &fresh = after[it.key()];
        const auto &domain = domains[it.key()].occupancy;
        const QString prefix = out + QString("/atlas-%1").arg(m++);
        QImage raw;
        const auto panel = plan["layout"].toObject()[it.key()].toArray();
        const int boardSize = plan["boardSize"].toInt();
        if (panel.size() == 4 && boardSize && !lastGenerated.isNull()) {
            const int left = qRound(double(panel[0].toInt()) * lastGenerated.width() / boardSize);
            const int top = qRound(double(panel[1].toInt()) * lastGenerated.height() / boardSize);
            const int right =
                qRound(double(panel[0].toInt() + panel[2].toInt()) * lastGenerated.width() / boardSize);
            const int bottom =
                qRound(double(panel[1].toInt() + panel[3].toInt()) * lastGenerated.height() / boardSize);
            raw = lastGenerated.copy(QRect(left, top, right - left, bottom - top))
                      .scaled(it->color.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            save(raw, prefix + "-raw-candidate.png");
        }
        save(it->color, prefix + "-before.png");
        save(fresh.color, prefix + "-after.png");
        save(domain, prefix + "-domain.png");
        save(QImage::fromData(
                 QByteArray::fromBase64(plan["eligibleMasks"].toObject()[it.key()].toString().toLatin1())),
             prefix + "-gaps.png");
        save(fresh.repair, prefix + "-repairs.png");
        for (int y = 0; y < resolution; ++y)
            for (int x = 0; x < resolution; ++x) {
                const bool known = it->coverage.constScanLine(y)[x] != 0,
                           inside = domain.constScanLine(y)[x] != 0;
                const bool different = it->color.pixel(x, y) != fresh.color.pixel(x, y);
                changedKnown += known && different;
                changedOutside += !inside && different;
                missingBefore += inside && !known;
                missingAfter += inside && !fresh.coverage.constScanLine(y)[x];
                painted += inside && !known && fresh.coverage.constScanLine(y)[x];
                if (!raw.isNull() && inside && !known && fresh.coverage.constScanLine(y)[x]) {
                    const auto &charts = domains[it.key()].charts;
                    for (auto delta : {QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1)}) {
                        const int nx = x + delta.x(), ny = y + delta.y();
                        if (nx < 0 || ny < 0 || nx >= resolution || ny >= resolution ||
                            !it->coverage.constScanLine(ny)[nx] ||
                            charts[ny * resolution + nx] != charts[y * resolution + x])
                            continue;
                        const QRgb reference = it->color.pixel(nx, ny);
                        const QRgb candidate = raw.pixel(x, y), repaired = fresh.color.pixel(x, y);
                        rawSeamError += std::abs(qRed(candidate) - qRed(reference)) +
                                        std::abs(qGreen(candidate) - qGreen(reference)) +
                                        std::abs(qBlue(candidate) - qBlue(reference));
                        matchedSeamError += std::abs(qRed(repaired) - qRed(reference)) +
                                            std::abs(qGreen(repaired) - qGreen(reference)) +
                                            std::abs(qBlue(repaired) - qBlue(reference));
                        seamSamples += 3;
                    }
                }
            }
    }
    int view = 0;
    for (auto angles :
         {QPointF(17.5, 15.5), QPointF(90, 0), QPointF(-90, 0), QPointF(15, -35), QPointF(20, 65)}) {
        const auto c = camera(scene, float(angles.x()), float(angles.y()));
        save(aiCapture(scene, uv, before, c).partial, out + QString("/view-%1-before.png").arg(view));
        save(aiCapture(scene, uv, after, c).partial, out + QString("/view-%1-after.png").arg(view++));
    }
    settings["mesh"] = QDir(out).relativeFilePath(meshPath);
    settings["pendingSmartFill"] = plan;
    aiSaveProject(out + "/smart-fill-copy.fornaioai", settings, after, afterRecords);
    QFile planFile(out + "/plan.json");
    if (!planFile.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot save repair plan");
    planFile.write(QJsonDocument(plan).toJson(QJsonDocument::Compact));
    QJsonObject report{{"requestCount", requestCount},
                       {"completed", completed},
                       {"live", liveOne},
                       {"cachedGeneration", !replayImage.isEmpty()},
                       {"acceptanceMilliseconds", acceptanceMilliseconds},
                       {"rawBoundaryMeanError", seamSamples ? rawSeamError / seamSamples : 0},
                       {"matchedBoundaryMeanError", seamSamples ? matchedSeamError / seamSamples : 0},
                       {"boundaryChannelSamples", seamSamples},
                       {"changedKnown", changedKnown},
                       {"changedOutside", changedOutside},
                       {"missingBefore", missingBefore},
                       {"missingAfter", missingAfter},
                       {"painted", painted},
                       {"milliseconds", timer.elapsed()}};
    QFile reportFile(out + "/report.json");
    if (!reportFile.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot save Smart fill probe report");
    reportFile.write(QJsonDocument(report).toJson());
    std::printf("%s\n", QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
    if (changedKnown || changedOutside)
        throw std::runtime_error("Smart fill modified protected pixels");
}
void replay(const QString &project, const QString &out) {
    AiAtlases before;
    QList<QJsonObject> records;
    auto settings = aiLoadProject(project, before, records);
    const auto meshPath =
        QDir(QFileInfo(project).absolutePath()).absoluteFilePath(settings["mesh"].toString());
    QFile mesh(meshPath);
    if (!mesh.open(QIODevice::ReadOnly))
        throw std::runtime_error("Cannot open project mesh");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(&mesh);
    if (QString::fromLatin1(hash.result().toHex()) != settings["meshSha256"].toString())
        throw std::runtime_error("Project mesh fingerprint differs");
    const auto scene = loadScene(meshPath);
    const int uv = settings["uv"].toInt(), resolution = settings["resolution"].toInt(2048);
    const auto base = settings["layerBase"].isArray() ? aiDeserializeAtlases(settings["layerBase"].toArray())
                                                      : aiEmptyAtlases(scene, resolution);
    QDir().mkpath(out);
    QElapsedTimer timer;
    timer.start();
    auto rebuilt = aiRebuildProjectionLayers(scene, uv, resolution, records);
    const auto after = aiComposeParityLayers(base, rebuilt);
    QJsonArray views;
    int viewIndex = 0;
    QList<AiCamera> cameras;
    if (settings["camera"].isObject())
        cameras << aiCameraFromJson(settings["camera"].toObject());
    for (auto direction :
         {QPointF(25, 15), QPointF(90, 15), QPointF(-90, 15), QPointF(180, 15), QPointF(25, 75)})
        cameras << camera(scene, float(direction.x()), float(direction.y()));
    for (const auto &c : cameras) {
        const auto old = aiCapture(scene, uv, before, c), fresh = aiCapture(scene, uv, after, c);
        save(old.partial, out + QString("/view-%1-before.png").arg(viewIndex));
        save(fresh.partial, out + QString("/view-%1-after.png").arg(viewIndex));
        save(fresh.known, out + QString("/view-%1-coverage.png").arg(viewIndex));
        qint64 missing = 0;
        for (int y = 0; y < c.size; ++y)
            for (int x = 0; x < c.size; ++x)
                missing += qGray(fresh.silhouette.pixel(x, y)) > 127 && qGray(fresh.known.pixel(x, y)) < 128;
        views.append(QJsonObject{
            {"index", viewIndex++}, {"yaw", c.orbit.yaw}, {"pitch", c.orbit.pitch}, {"missing", missing}});
    }
    int material = 0;
    QJsonArray metrics;
    for (auto it = after.begin(); it != after.end(); ++it) {
        const auto prefix = out + QString("/atlas-%1").arg(material++);
        save(before[it.key()].color, prefix + "-before.png");
        save(it->color, prefix + "-after.png");
        save(it->coverage, prefix + "-coverage.png");
        qint64 covered = 0, weak = 0;
        for (int i = 0; i < it->projectionWeight.size(); ++i) {
            covered += it->projectionWeight[i] > .001f;
            weak += it->projectionWeight[i] > .001f && it->projectionWeight[i] < .05f;
        }
        metrics.append(QJsonObject{{"material", it.key()}, {"covered", covered}, {"weakBelow05", weak}});
    }
    for (int i = 0; i < rebuilt.size(); ++i)
        if (!rebuilt[i]["visible"].toBool(true)) {
            auto record = rebuilt[i];
            record["visible"] = true;
            const auto isolated = aiComposeParityLayers(base, {record});
            save(aiCapture(scene, uv, isolated, aiCameraFromJson(record["camera"].toObject())).partial,
                 out + QString("/hidden-%1.png").arg(i));
        }
    settings["projectionBackend"] = "modddif-v1";
    settings["mesh"] = QDir(out).relativeFilePath(meshPath);
    settings["layerBase"] = aiSerializeAtlases(base);
    aiSaveProject(out + "/rebuilt.fornaioai", settings, after, rebuilt);
    QJsonObject report{{"layers", records.size()},
                       {"milliseconds", timer.elapsed()},
                       {"materials", metrics},
                       {"views", views}};
    QFile reportFile(out + "/report.json");
    if (!reportFile.open(QIODevice::WriteOnly))
        throw std::runtime_error("Cannot write replay report");
    reportFile.write(QJsonDocument(report).toJson());
    std::printf("%s\n", QJsonDocument(report).toJson(QJsonDocument::Compact).constData());
}
} // namespace

int main(int argc, char **argv) {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSamples(0);
    QSurfaceFormat::setDefaultFormat(format);
    QGuiApplication app(argc, argv);
    const auto args = app.arguments();
    try {
        aiInitializeProjectionRenderer();
        const auto shutdown = qScopeGuard([] { aiShutdownProjectionRenderer(); });
        if (args.size() < 4)
            throw std::runtime_error("usage: capture|project|render <mesh> <state.json> ...");
        const QString command = args[1];
        if (command == "smartfill-replay" && args.size() == 5) {
            smartFillProbe(args[2], args[4], false, false, args[3]);
            return 0;
        }
        if ((command == "smartfill-probe" || command == "smartfill-live-one" ||
             command == "smartfill-prepare") &&
            args.size() == 4) {
            smartFillProbe(args[2], args[3], command == "smartfill-live-one", command == "smartfill-prepare");
            return 0;
        }
        if (command == "replay" && args.size() == 4) {
            replay(args[2], args[3]);
            return 0;
        }
        const Scene scene = loadScene(args[2]);
        const QString statePath = args[3];
        QJsonObject root;
        AiAtlases atlases = loadState(scene, statePath, &root);
        QSet<QString> materials;
        for (auto it = atlases.begin(); it != atlases.end(); ++it)
            materials.insert(it.key());
        if (command == "capture" && args.size() == 7) {
            const QString dir = args[6];
            QDir().mkpath(dir);
            const auto c = aiCapture(scene, 0, atlases, camera(scene, args[4].toFloat(), args[5].toFloat()));
            const auto feedback = aiProjectionFeedback(scene, c, continuity, panelDefaults());
            save(c.confidence, dir + "/confidence.png"); // 16-bit accepted-paint quality
            for (auto pair :
                 {qMakePair(QString("normal"), c.normal), qMakePair(QString("depth"), c.depth),
                  qMakePair(QString("silhouette"), c.silhouette), qMakePair(QString("accepted"), c.partial),
                  qMakePair(QString("known"), c.known), qMakePair(QString("feedback"), feedback.image),
                  qMakePair(QString("feedbackMask"), feedback.mask)})
                save(pair.second, dir + "/" + pair.first + ".png");
            root["camera"] = aiCameraJson(c.camera);
            saveState(statePath, root, atlases);
            std::printf("{\"feedbackActive\": %s, \"newCoverage\": %.4f}\n",
                        feedback.active ? "true" : "false", aiNewCoverage(scene, 0, atlases, c, materials));
        } else if (command == "project" && args.size() >= 6 && args.size() <= 8) {
            QDir().mkpath(args[4]);
            const QImage source(args[5]);
            QJsonObject record{
                {"camera", root["camera"]},
                {"sourceImage", imageData(source)},
                {"materials", QJsonArray::fromStringList(materials.values())},
                {"visible", true},
                {"blending", QJsonObject{{"mode", qEnvironmentVariable("FORNAIO_EVAL_BLEND", "blend")},
                                         {"strength", 1},
                                         {"depthMargin", 2}}}};
            if (args.size() > 6)
                record["userMask"] = imageData(QImage(args[6]));
            const auto baked = aiBakeProjectionLayer(scene, 0, atlasSize, record);
            auto records = root["projections"].toArray();
            records.append(baked);
            root["projections"] = records;
            QList<QJsonObject> list;
            for (const auto &value : records)
                list << value.toObject();
            const auto composition = aiComposeParityLayers(aiEmptyAtlases(scene, atlasSize), list);
            saveState(statePath, root, composition);
            for (auto it = composition.begin(); it != composition.end(); ++it)
                save(it->color, args[4] + "/atlas-" + it.key() + ".png");
        } else if (command == "legacyproject" && args.size() >= 6 && args.size() <= 8) {
            // Optional: [mask.png] limits projection (white = use), [retained.png] marks restored accepted
            // paint.
            const QImage candidate = QImage(args[5]).convertToFormat(QImage::Format_RGB32);
            const auto c = aiCapture(scene, 0, atlases, aiCameraFromJson(root["camera"].toObject()));
            const QImage mask =
                args.size() > 6 ? QImage(args[6]).convertToFormat(QImage::Format_RGB32) : c.silhouette;
            auto options = panelDefaults();
            if (args.size() > 7)
                options.retainedMask = QImage(args[7]).convertToFormat(QImage::Format_Grayscale8);
            const auto result = aiProject(scene, 0, atlases, c, candidate, mask, materials,
                                          blendMode() == "fill", 8, nullptr, options);
            saveState(statePath, root, result.atlases);
            for (auto it = result.atlases.begin(); it != result.atlases.end(); ++it)
                save(it->color, args[4] + "/atlas-" + it.key() + ".png");
            std::printf("{\"changed\": %lld}\n", static_cast<long long>(result.changed));
        } else if (command == "ownproject" && args.size() == 6) {
            // The panel's GPT Image path: lock, tone match, crossfade band, paint mask, mask-weighted
            // projection.
            const auto c = aiCapture(scene, 0, atlases, aiCameraFromJson(root["camera"].toObject()));
            const auto lock = aiLockedGuide(c);
            const QImage generated =
                QImage(args[5])
                    .convertToFormat(QImage::Format_RGB32)
                    .scaled(c.camera.size, c.camera.size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            const QImage aligned = qEnvironmentVariable("FORNAIO_EVAL_ALIGN", "1") == "0"
                                       ? generated
                                       : aiAlignToGeometry(c, generated);
            save(aligned, args[4] + "/own-aligned.png");
            const QImage candidate = aiComposeLocked(c, lock, aiMatchLockedTone(c, lock, aligned));
            const QImage mask = aiGeneratedPaintMask(c, lock, candidate);
            auto options = panelDefaults();
            options.overlay = true;
            options.lockQuality = 0;
            const auto result =
                aiProject(scene, 0, atlases, c, candidate, mask, materials, false, 8, nullptr, options);
            saveState(statePath, root, result.atlases);
            save(lock.guide, args[4] + "/own-guide.png");
            save(candidate, args[4] + "/own-candidate.png");
            save(mask, args[4] + "/own-mask.png");
            std::printf("{\"changed\": %lld}\n", static_cast<long long>(result.changed));
        } else if (command == "render" && args.size() == 7) {
            const auto c = aiCapture(scene, 0, atlases, camera(scene, args[4].toFloat(), args[5].toFloat()));
            QImage out = c.partial.copy();
            for (int y = 0; y < out.height(); ++y) // Untextured surface shows as magenta.
                for (int x = 0; x < out.width(); ++x)
                    if (qGray(c.silhouette.pixel(x, y)) > 127 && qGray(c.known.pixel(x, y)) < 128)
                        out.setPixel(x, y, qRgb(255, 0, 255));
            save(out, args[6]);
        } else
            throw std::runtime_error("bad arguments");
    } catch (const std::exception &e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
    return 0;
}
