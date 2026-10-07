#pragma once
#include "shared.h"
#include <QImage>
#include <QStringList>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>
struct Triangle {
    V3 p[3], n[3];
    std::vector<std::array<V2, 3>> uv;
    int island = 0;
    QString sourceMesh;
    int sourcePolygon = -1;
    int material = -1, object = -1;
};
struct Scene {
    std::vector<Triangle> tris;
    QStringList uvNames;
    QStringList materials, objects;
    float scale = 1;
    V3 center;
    QString source;
    // Optional opaque casting geometry. It has no output texels or UV requirements.
    std::shared_ptr<const Scene> occluder;
};
struct Settings {
    int resolution = 2048, rays = 256, ss = 2, padding = 8, bits = 16, maskUV = 0, aoUV = 0, channel = 0;
    bool invert = false, flip = false, repeat = false, averageOverlaps = true;
    float distance = 0, bias = 0;
};
struct Mask {
    int w = 0, h = 0;
    std::vector<float> values;
    QImage image;
    bool alpha = false;
    MaskView view(const Settings &s) const { return {values.data(), w, h, (int)s.repeat, (int)s.flip}; }
};
struct Validation {
    struct Overlap {
        int first, second;
        double area;
    };
    QStringList errors;
    QStringList warnings;
    std::vector<int> faces;
    std::vector<Overlap> overlaps;
    bool overlapScanLimited = false;
    bool ok() const { return errors.empty(); }
};
struct Prepared {
    std::vector<GpuTri> tris;
    std::vector<int> offsets, indices;
    int grid = 64;
    float scale;
};
struct Result {
    int resolution = 0;
    std::vector<float> pixels;
    std::vector<int> owners;
    double seconds = 0;
    size_t gpuBytes = 0;
    QString device;
};
using Progress = std::function<void(float, const QString &)>;
Scene loadScene(const QString &path);
QStringList validateOccluder(const Scene &scene);
Mask loadMask(const QString &path, int channel = -1, bool invert = false);
Mask maskFromImage(const QImage &image, int channel, bool invert);
Validation validate(const Scene &, const Settings &);
Prepared prepare(const Scene &, const Settings &);
void checkSettings(const Settings &);
Result bakeGpu(const Scene &, const Mask &, const Settings &, std::atomic_bool &, Progress = {});
Result bakeReference(const Scene &, const Mask &, const Settings &);
float referenceTransmission(const Prepared &, MaskView, V3 origin, V3 direction, float bias, float distance,
                            int skip);
void padResult(Result &, int padding, std::atomic_bool *cancel = nullptr);
QImage resultImage(const Result &);
void savePng(const Result &, const QString &, int bits);
enum class PngSpace { Linear, Srgb, Untagged };
// values: w*h*channels samples already quantized to the 0..(2^bits-1) range.
void writePng(const QString &path, int w, int h, int channels, int bits, const uint16_t *values, PngSpace space,
              bool adaptiveFilter = true);
Scene testCards(int count = 3);
