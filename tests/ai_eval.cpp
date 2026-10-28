// Maintainer tool: drive AI capture/feedback/projection from scripts to evaluate generators end to end.
// State (atlases plus the last capture camera) lives in a JSON file between steps; no GUI or model is needed.
//
//   fornaio_ai_eval capture <mesh> <state.json> <yaw> <pitch> <outdir>   writes guides for the next view
//   fornaio_ai_eval project <mesh> <state.json> <outdir> <candidate.png> [mask.png [retained.png]]
//   fornaio_ai_eval render  <mesh> <state.json> <yaw> <pitch> <out.png>  atlas seen from a camera (no lighting)
//   fornaio_ai_eval ownproject <mesh> <state.json> <outdir> <generated.png>  the panel's GPT Image projection
#include "ai_texture.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
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
} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    try {
        if (args.size() < 4)
            throw std::runtime_error("usage: capture|project|render <mesh> <state.json> ...");
        const QString command = args[1];
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
            for (auto pair : {qMakePair(QString("normal"), c.normal), qMakePair(QString("depth"), c.depth),
                              qMakePair(QString("silhouette"), c.silhouette),
                              qMakePair(QString("accepted"), c.partial), qMakePair(QString("known"), c.known),
                              qMakePair(QString("feedback"), feedback.image),
                              qMakePair(QString("feedbackMask"), feedback.mask)})
                save(pair.second, dir + "/" + pair.first + ".png");
            root["camera"] = aiCameraJson(c.camera);
            saveState(statePath, root, atlases);
            std::printf("{\"feedbackActive\": %s, \"newCoverage\": %.4f}\n", feedback.active ? "true" : "false",
                        aiNewCoverage(scene, 0, atlases, c, materials));
        } else if (command == "project" && args.size() >= 6 && args.size() <= 8) {
            // Optional: [mask.png] limits projection (white = use), [retained.png] marks restored accepted paint.
            const QImage candidate = QImage(args[5]).convertToFormat(QImage::Format_RGB32);
            const auto c = aiCapture(scene, 0, atlases, aiCameraFromJson(root["camera"].toObject()));
            const QImage mask = args.size() > 6 ? QImage(args[6]).convertToFormat(QImage::Format_RGB32) : c.silhouette;
            auto options = panelDefaults();
            if (args.size() > 7)
                options.retainedMask = QImage(args[7]).convertToFormat(QImage::Format_Grayscale8);
            const auto result =
                aiProject(scene, 0, atlases, c, candidate, mask, materials, blendMode() == "fill", 8, nullptr, options);
            saveState(statePath, root, result.atlases);
            for (auto it = result.atlases.begin(); it != result.atlases.end(); ++it)
                save(it->color, args[4] + "/atlas-" + it.key() + ".png");
            std::printf("{\"changed\": %lld}\n", static_cast<long long>(result.changed));
        } else if (command == "ownproject" && args.size() == 6) {
            // The panel's GPT Image path: lock, tone match, crossfade band, paint mask, mask-weighted projection.
            const auto c = aiCapture(scene, 0, atlases, aiCameraFromJson(root["camera"].toObject()));
            const auto lock = aiLockedGuide(c);
            const QImage generated = QImage(args[5]).convertToFormat(QImage::Format_RGB32).scaled(
                c.camera.size, c.camera.size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            const QImage aligned = qEnvironmentVariable("FORNAIO_EVAL_ALIGN", "1") == "0"
                                       ? generated
                                       : aiAlignToGeometry(c, generated);
            save(aligned, args[4] + "/own-aligned.png");
            const QImage candidate = aiComposeLocked(c, lock, aiMatchLockedTone(c, lock, aligned));
            const QImage mask = aiGeneratedPaintMask(c, lock, candidate);
            auto options = panelDefaults();
            options.overlay = true;
            options.lockQuality = 0;
            const auto result = aiProject(scene, 0, atlases, c, candidate, mask, materials, false, 8, nullptr, options);
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
