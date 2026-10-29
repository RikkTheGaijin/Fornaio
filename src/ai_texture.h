#pragma once
#include "transfer_viewport.h"
#include <QCache>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QVector>

// Images use top-left origins; mesh UVs use bottom-left. All cameras are immutable snapshots.
struct AiCamera {
    QMatrix4x4 view, projection;
    OrbitCamera orbit;
    int size = 1024;
};
struct AiAtlas {
    QImage color;      // sRGB, padding included
    QImage coverage;   // grayscale; padding never changes coverage
    QImage confidence; // linear 16-bit projection quality, separate from coverage and padding
    QVector<float> projectionWeight; // parity max weight, may exceed 1; confidence is its clamped guide preview
};
using AiAtlases = QMap<QString, AiAtlas>;
struct AiCapture {
    AiCamera camera;
    QImage neutral, normal, depth, silhouette, partial, known, confidence;
    std::vector<int> faces;
    std::vector<float> z;
    std::vector<float> linearDepth;
    std::vector<V2> texcoords; // visible surface UV at each camera pixel, independent of painted colors
};
struct AiProjection {
    AiAtlases atlases;
    qint64 changed = 0;
    QJsonObject layer; // Lossless frozen UV paint operations, independent of inference.
};
struct AiFeedback {
    QImage image, mask; // confidence-filtered color guide and reuse strength in camera space
    bool active = false;
};
struct AiProjectionOptions {
    bool overlay = false;
    bool colorMatch = true;
    float strength = 1;
    float anglePower = 2;
    float feather = .06f;   // fraction of the camera image width
    float minCosine = .15f; // Legacy limit; AI panel defaults to cos(60 degrees).
    bool fillGaps = false;  // Allow visible enclosed holes up to 80 degrees, only into empty texels.
    bool coverAll = false;  // Paint every visible front face into empty texels, at low confidence.
    QImage retainedMask;    // pixels inherited from accepted paint; never changes projection opacity
    bool recordLayer = false;
    float lockQuality = 0;  // > 0: accepted paint at least this good is never repainted (ownership)
};
// Legacy ownership multiview: paint seen well by an earlier view is locked. A new view's guide shows
// the locked paint and bare clay elsewhere; the generated image is composited back over the locked pixels.
constexpr float aiDefaultLockQuality = .1f;
struct AiLock {
    QImage guide;  // RGB32: locked paint, mid-gray camera-lit clay elsewhere, white background
    QImage locked; // Grayscale8: 255 where accepted paint is locked
    QImage keep;   // Grayscale8: share of accepted paint kept; ramps 0 to 255 over a band inside the lock edge
    bool any = false;
};
AiLock aiLockedGuide(const AiCapture &, float lockQuality = aiDefaultLockQuality);
// Geometry-guided registration: warps a generated view so its painted edges (eyelids, nostrils, lip line, the
// outline) sit on the mesh's creases and silhouette. A smooth field of local shifts up to about 2% of the image;
// areas without clear geometric structure stay in place. meanShift receives the mean shift on the mesh in pixels.
QImage aiAlignToGeometry(const AiCapture &, const QImage &generated, double *meanShift = nullptr);
// Corrects this view's lighting/colour shift: the generated image also redraws locked pixels, and its gain against
// the accepted paint there (linear light, per channel, smoothed) is applied to the whole image.
QImage aiMatchLockedTone(const AiCapture &, const AiLock &, const QImage &generated);
// Locked pixels restored from the accepted paint; over the lock's outer band, new paint crossfades into it.
QImage aiComposeLocked(const AiCapture &, const AiLock &, const QImage &candidate);
// Projection strength mask (white = full): new areas, the lock band ramping to zero in its core, without white
// background or large bare-clay regions the model left unpainted (colourless, still at the clay brightness).
QImage aiGeneratedPaintMask(const AiCapture &, const AiLock &, const QImage &candidate);
QString aiMaterial(const Scene &, const Triangle &);
QStringList validateAiMesh(const Scene &, int uv);
AiCamera aiCamera(const Scene &, const OrbitCamera &, int size = 1024);
// Fit the view-space bounding box to 95% of the square view, preserving direction and lens.
// visibleOnly (orthographic) frames only the part of the box inside the current view.
OrbitCamera aiFrameCamera(const Scene &, OrbitCamera, bool visibleOnly = false);
// Multiview set: front, the 45-degree ring around it, back and top, in generation order.
QList<OrbitCamera> aiAutomaticCameras(const Scene &, const OrbitCamera &front);
AiAtlases aiEmptyAtlases(const Scene &, int resolution);
AiCapture aiCapture(const Scene &, int uv, const AiAtlases &, const AiCamera &,
                    std::atomic_bool *cancel = nullptr);
AiFeedback aiFeedback(const AiCapture &, float strength);
AiFeedback aiProjectionFeedback(const Scene &, const AiCapture &, float strength, const AiProjectionOptions &,
                                std::atomic_bool *cancel = nullptr);
double aiNewCoverage(const Scene &, int uv, const AiAtlases &, const AiCapture &, const QSet<QString> &);
double aiViewContribution(const Scene &, int uv, const AiCapture &, const QSet<QString> &,
                          const AiProjectionOptions &, std::atomic_bool *cancel = nullptr);
AiProjection aiProject(const Scene &, int uv, const AiAtlases &, const AiCapture &, const QImage &candidate,
                       const QImage &mask, const QSet<QString> &materials, bool fillOnly, int padding = 8,
                       std::atomic_bool *cancel = nullptr, const AiProjectionOptions &options = {});
QJsonObject aiCameraJson(const AiCamera &);
AiCamera aiCameraFromJson(const QJsonObject &);
QJsonArray aiSerializeAtlases(const AiAtlases &);
AiAtlases aiDeserializeAtlases(const QJsonArray &);
AiAtlases aiComposeLayers(const AiAtlases &base, const QList<QJsonObject> &,
                          std::atomic_bool *cancel = nullptr,
                          QCache<QByteArray, QByteArray> *decoded = nullptr);
QList<QJsonObject> aiPrepareLayers(const Scene &, int uv, const AiAtlases &base, const QList<QJsonObject> &,
                                   std::atomic_bool *cancel = nullptr);
void aiSaveProject(const QString &path, const QJsonObject &settings, const AiAtlases &,
                   const QList<QJsonObject> &projections);
QJsonObject aiLoadProject(const QString &path, AiAtlases &, QList<QJsonObject> &projections);
