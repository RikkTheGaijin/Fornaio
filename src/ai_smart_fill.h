#pragma once
#include "ai_layers.h"

// A pending repair is self-contained JSON: native source atlases, immutable eligibility,
// editable masks, UV chart ownership, one combined atlas request, and its generated image.
// Every write is clipped to its original, unpainted geometric texel at native resolution.
// Version 1 cropped candidates remain readable/applicable but must be replanned to generate.
QJsonObject aiPlanSmartFill(const Scene &, int uv, const AiAtlases &source, const AiUvDomains &,
                            const QSet<QString> &materials, const QByteArray &fingerprint,
                            std::atomic_bool *cancel = nullptr);
QJsonObject aiSmartFillSetMasks(QJsonObject plan, const QMap<QString, QImage> &masks,
                                std::atomic_bool *cancel = nullptr);
int aiSmartFillRemaining(const QJsonObject &plan);
QJsonObject aiSmartFillRequest(const QJsonObject &plan, int patchIndex, const QString &workDir,
                               const QImage &appearance, std::atomic_bool *cancel = nullptr);
QJsonObject aiSmartFillAcceptPatch(QJsonObject plan, int patchIndex, const QImage &generated,
                                   std::atomic_bool *cancel = nullptr);
// Reprocess a retained version-2 generated board locally, including raw-only pending candidates.
// Uses accepted-only hole boundaries and harmonic correction; it never invokes a provider.
QJsonObject aiSmartFillReprocess(QJsonObject plan, std::atomic_bool *cancel = nullptr);
QJsonObject aiSmartFillLayer(const QJsonObject &plan);
AiAtlases aiSmartFillPreview(const AiAtlases &source, const QJsonObject &plan,
                             std::atomic_bool *cancel = nullptr);
QByteArray aiSmartFillFingerprint(const Scene &, int uv, const AiAtlases &source,
                                  const QList<QJsonObject> &records, bool parity,
                                  std::atomic_bool *cancel = nullptr);
