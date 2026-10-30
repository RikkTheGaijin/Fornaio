#pragma once
#include "ai_texture.h"

// A projection is an independent, encoded-RGB RGBA8 UV image. Alpha is camera
// quality (Blend) or source opacity (Overlay), before layer strength is applied.
struct AiLayerContribution {
    QMap<QString, QImage> materials;
};

// Geometric UV occupancy excludes projection padding. Chart IDs identify
// edge-connected faces within one material/object; images and IDs are top-left.
struct AiUvDomain {
    QImage occupancy;
    QVector<int> charts;
};
using AiUvDomains = QMap<QString, AiUvDomain>;
AiUvDomains aiUvDomains(const Scene &, int uv, int resolution, const QSet<QString> &materials,
                        std::atomic_bool *cancel = nullptr);

// Initialization/destruction belong to the GUI thread. Rendering is synchronous
// and may be requested from any thread; GL work belongs to its dedicated thread.
// Source and camera masks use top-left image origins and normalized camera
// coordinates, so their resolutions may differ. Masks are white where allowed.
void aiInitializeProjectionRenderer();
void aiShutdownProjectionRenderer();
AiLayerContribution aiRenderProjection(const Scene &, int uv, int resolution, const AiCamera &,
                                       const QImage &source, const QImage &cameraMask, const QImage &uvMask,
                                       const QSet<QString> &materials, bool overlay = false,
                                       int depthMargin = 2, std::atomic_bool *cancel = nullptr);
