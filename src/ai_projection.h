#pragma once
#include "ai_texture.h"

// A projection is an independent, encoded-RGB RGBA8 UV image. Alpha is camera
// quality (Blend) or source opacity (Overlay), before layer strength is applied.
struct AiLayerContribution {
    QMap<QString, QImage> materials;
};

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
