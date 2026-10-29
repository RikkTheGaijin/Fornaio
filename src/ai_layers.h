#pragma once
#include "ai_projection.h"

// Version 2 layers are independent RGBA8 projections. Strength and visibility are
// composition operations; the image is never a stroke against the previous atlas.
struct AiLayerBlendSettings {
    bool overlay = false;
    float strength = 1;
    int depthMargin = 2;
};
AiLayerBlendSettings aiLayerBlendSettings(const QJsonObject &record);
QJsonObject aiEncodeProjectionLayer(const AiLayerContribution &, int resolution);
AiLayerContribution aiDecodeProjectionLayer(const QJsonObject &layer);
QJsonObject aiBakeProjectionLayer(const Scene &, int uv, int resolution, QJsonObject record,
                                  std::atomic_bool *cancel = nullptr);
AiAtlases aiComposeParityLayers(const AiAtlases &base, const QList<QJsonObject> &records,
                                std::atomic_bool *cancel = nullptr);
QList<QJsonObject> aiRebuildProjectionLayers(const Scene &, int uv, int resolution,
                                             const QList<QJsonObject> &records,
                                             std::atomic_bool *cancel = nullptr);
