#include "ai_layers.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QImageReader>
#include <QMutex>
#include <QMutexLocker>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
void check(std::atomic_bool *cancel) {
    if (cancel && *cancel)
        throw std::runtime_error("Cancelled");
}
void fail(const char *message) {
    throw std::runtime_error(message);
}
QString encode(const QImage &im) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!im.save(&buffer, "PNG"))
        fail("Cannot encode projection layer");
    return QString::fromLatin1(bytes.toBase64());
}
QImage decode(const QJsonValue &value, int limit = 16384) {
    QByteArray bytes = QByteArray::fromBase64(value.toString().toLatin1());
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer, "PNG");
    const QSize size = reader.size();
    if (size.isEmpty() || size.width() > limit || size.height() > limit)
        fail("Invalid projection image dimensions");
    QImage image = reader.read();
    if (image.isNull())
        fail("Invalid projection image");
    return image.convertToFormat(QImage::Format_RGBA8888);
}
QImage cachedProjectionImage(const QJsonValue &value) {
    static QMutex mutex;
    static QCache<QByteArray, QImage> images(256 * 1024); // KiB, shared across short-lived panel jobs.
    const QByteArray key = QCryptographicHash::hash(value.toString().toLatin1(), QCryptographicHash::Sha256);
    {
        const QMutexLocker lock(&mutex);
        if (const auto image = images.object(key))
            return *image;
    }
    const QImage image = decode(value, 4096);
    const QMutexLocker lock(&mutex);
    images.insert(key, new QImage(image), int((image.sizeInBytes() + 1023) / 1024));
    return image;
}
int byte(float v) {
    return std::clamp(qRound(v * 255), 0, 255);
}
} // namespace

AiLayerBlendSettings aiLayerBlendSettings(const QJsonObject &record) {
    const auto json = record["blending"].toObject();
    const auto mode = json["mode"].toString("blend");
    if (mode != "blend" && mode != "overlay")
        fail("Invalid parity layer blend mode");
    AiLayerBlendSettings result;
    result.overlay = mode == "overlay";
    result.strength = float(json["strength"].toDouble(1));
    result.depthMargin = json["depthMargin"].toInt(2);
    if (!std::isfinite(result.strength) || result.strength < 0 || result.strength > 2 ||
        result.depthMargin < 0 || result.depthMargin > 32)
        fail("Invalid parity layer settings");
    return result;
}

QJsonObject aiEncodeProjectionLayer(const AiLayerContribution &layer, int resolution) {
    QJsonArray materials;
    for (auto it = layer.materials.begin(); it != layer.materials.end(); ++it) {
        if (it->size() != QSize(resolution, resolution))
            fail("Projection layer does not match the atlas");
        materials.append(QJsonObject{{"material", it.key()}, {"rgba", encode(*it)}});
    }
    return {{"version", 2}, {"resolution", resolution}, {"materials", materials}};
}

AiLayerContribution aiDecodeProjectionLayer(const QJsonObject &layer) {
    if (layer["version"].toInt() != 2)
        fail("Unsupported independent projection layer");
    int resolution = layer["resolution"].toInt();
    if (resolution < 1 || resolution > 4096)
        fail("Invalid projection layer resolution");
    AiLayerContribution result;
    for (const auto &value : layer["materials"].toArray()) {
        const auto material = value.toObject();
        const QString name = material["material"].toString();
        if (result.materials.contains(name))
            fail("Duplicate projection material");
        QImage image = decode(material["rgba"], 4096);
        if (image.size() != QSize(resolution, resolution))
            fail("Projection layer does not match the atlas");
        result.materials.insert(name, image);
    }
    return result;
}

QJsonObject aiBakeProjectionLayer(const Scene &scene, int uv, int resolution, QJsonObject record,
                                  std::atomic_bool *cancel) {
    check(cancel);
    const auto settings = aiLayerBlendSettings(record);
    auto source = record["sourceImage"];
    if (!source.isString())
        source = record["candidate"];
    const QImage image = decode(source);
    if (image.width() != image.height())
        fail("Use a square source image with the captured camera's framing");
    auto maskValue = record["userMask"];
    if (!maskValue.isString())
        maskValue = record["mask"];
    const QImage mask = maskValue.isString() ? decode(maskValue) : QImage();
    const QImage uvMask = record["uvMask"].isString() ? decode(record["uvMask"]) : QImage();
    const auto camera = aiCameraFromJson(record["camera"].toObject());
    QSet<QString> materials;
    for (const auto &material : record["materials"].toArray())
        materials.insert(material.toString());
    if (materials.empty())
        fail("Projection layer has no selected materials");
    const auto projected = aiRenderProjection(scene, uv, resolution, camera, image, mask, uvMask, materials,
                                              settings.overlay, settings.depthMargin, cancel);
    check(cancel);
    record["uvLayer"] = aiEncodeProjectionLayer(projected, resolution);
    if (record["layerId"].toString().isEmpty())
        record["layerId"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record["sourceImage"] = source;
    if (maskValue.isString())
        record["userMask"] = maskValue;
    record["projectionBackend"] = "modddif-v1";
    return record;
}

AiAtlases aiComposeParityLayers(const AiAtlases &base, const QList<QJsonObject> &records,
                                std::atomic_bool *cancel) {
    check(cancel);
    if (records.empty())
        return base;
    AiAtlases result = base;
    for (auto it = result.begin(); it != result.end(); ++it) {
        check(cancel);
        auto &atlas = it.value();
        const int n = atlas.color.width();
        if (n < 1 || atlas.color.size() != QSize(n, n) || atlas.coverage.size() != QSize(n, n) || n > 4096)
            fail("Invalid base atlas");
        // Four unquantized premultiplied accumulation channels and independent
        // max coverage. Work one material at a time to bound peak memory at 4K.
        std::vector<std::array<float, 4>> sum(size_t(n) * n);
        QVector<float> maximum(n * n, 0);
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
                if (atlas.coverage.constScanLine(y)[x]) {
                    const auto p = atlas.color.pixel(x, y);
                    sum[size_t(y) * n + x] = {qRed(p) / 255.f, qGreen(p) / 255.f, qBlue(p) / 255.f, 1};
                    maximum[y * n + x] = 1;
                }
        for (const auto &record : records) {
            check(cancel);
            if (!record["visible"].toBool(true))
                continue;
            const auto settings = aiLayerBlendSettings(record);
            const auto layer = record["uvLayer"].toObject();
            if (layer["version"].toInt() != 2 || layer["resolution"].toInt() != n)
                fail("Projection layer does not match the atlas");
            QImage image;
            for (const auto &value : layer["materials"].toArray()) {
                const auto block = value.toObject();
                if (block["material"].toString() == it.key()) {
                    if (!image.isNull())
                        fail("Duplicate projection material");
                    image = cachedProjectionImage(block["rgba"]);
                }
            }
            if (image.isNull())
                continue;
            if (image.size() != QSize(n, n))
                fail("Projection layer does not match the atlas");
            const float strength = settings.overlay ? std::min(1.f, settings.strength) : settings.strength;
            for (int y = 0; y < n; ++y) {
                if (!(y & 31))
                    check(cancel);
                const uchar *pixels = image.constScanLine(y);
                for (int x = 0; x < n; ++x) {
                    const uchar *pixel = pixels + x * 4;
                    const float alpha = pixel[3] / 255.f * strength;
                    if (!alpha)
                        continue;
                    auto &value = sum[size_t(y) * n + x];
                    const float retain = settings.overlay ? std::max(0.f, 1 - alpha) : 1;
                    for (int channel = 0; channel < 3; ++channel)
                        value[channel] = value[channel] * retain + pixel[channel] / 255.f * alpha;
                    value[3] = value[3] * retain + alpha;
                    maximum[y * n + x] = std::max(maximum[y * n + x], alpha);
                }
            }
        }
        atlas.color = QImage(n, n, QImage::Format_RGB32);
        atlas.coverage = QImage(n, n, QImage::Format_Grayscale8);
        atlas.confidence = QImage(n, n, QImage::Format_Grayscale16);
        atlas.projectionWeight = maximum;
        for (int y = 0; y < n; ++y) {
            check(cancel);
            auto *quality = reinterpret_cast<quint16 *>(atlas.confidence.scanLine(y));
            for (int x = 0; x < n; ++x) {
                const auto &value = sum[size_t(y) * n + x];
                const bool hasColor = value[3] > .001f;
                atlas.color.setPixel(x, y,
                                     hasColor ? qRgb(byte(value[0] / value[3]), byte(value[1] / value[3]),
                                                     byte(value[2] / value[3]))
                                              : qRgb(128, 128, 128));
                atlas.coverage.scanLine(y)[x] = hasColor ? 255 : 0;
                quality[x] = quint16(std::clamp(qRound(maximum[y * n + x] * 65535), 0, 65535));
            }
        }
    }
    return result;
}

QList<QJsonObject> aiRebuildProjectionLayers(const Scene &scene, int uv, int resolution,
                                             const QList<QJsonObject> &records, std::atomic_bool *cancel) {
    QList<QJsonObject> rebuilt;
    QSet<QString> identities;
    for (auto record : records) {
        check(cancel);
        auto blending = record["blending"].toObject();
        // Legacy overlay weights included ownership and angle falloff. A rebuild
        // intentionally adopts the reference's default independent Blend layer.
        if (record["projectionBackend"] != "modddif-v1") {
            blending = {{"mode", "blend"}, {"strength", 1}, {"depthMargin", 2}};
            record["rebuildSourceProcessed"] = true;
        }
        record["blending"] = blending;
        if (identities.contains(record["layerId"].toString()))
            record.remove("layerId");
        auto baked = aiBakeProjectionLayer(scene, uv, resolution, record, cancel);
        identities.insert(baked["layerId"].toString());
        rebuilt.append(baked);
    }
    check(cancel);
    return rebuilt;
}
