#include "ai_texture.h"
#include <QBuffer>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QVector3D>
#include <QVector4D>
#include <QtEndian>
#include <algorithm>
#include <deque>
#include <map>
#include <numeric>
#include <stdexcept>

namespace {
void fail(const QString &s) {
    throw std::runtime_error(s.toStdString());
}
void checkCancel(std::atomic_bool *c) {
    if (c && *c)
        fail("Cancelled");
}
QVector3D qv(V3 v) {
    return {v.x, v.y, v.z};
}
constexpr float coverAllCosine = .02f; // About 89 degrees: any front face that is still visible.
bool orthographic(const QMatrix4x4 &projection) {
    return projection(3, 2) == 0 && projection(3, 3) == 1;
}
// View-space unit vector from the surface seen at a capture pixel toward the camera.
// Orthographic captures see every pixel along the same direction.
QVector3D towardCamera(const AiCamera &c, const QMatrix4x4 &inverseProjection, int x, int y) {
    if (orthographic(c.projection))
        return {0, 0, 1};
    const int n = c.size;
    return -inverseProjection.map(QVector3D(2 * (x + .5f) / n - 1, 1 - 2 * (y + .5f) / n, 1)).normalized();
}
float linear(float v) {
    return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f);
}
int srgb(float v) {
    v = std::clamp(v, 0.f, 1.f);
    return qRound(255 * (v <= .0031308f ? v * 12.92f : 1.055f * std::pow(v, 1 / 2.4f) - .055f));
}
V3 decode(QRgb pixel) {
    static const auto values = [] {
        std::array<float, 256> table{};
        for (int i = 0; i < 256; ++i)
            table[i] = linear(i / 255.f);
        return table;
    }();
    return {values[qRed(pixel)], values[qGreen(pixel)], values[qBlue(pixel)]};
}
V3 sample(const QImage &im, float x, float y) {
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    float fx = x - ix, fy = y - iy;
    if (fx == 0 && fy == 0) {
        QRgb p = im.pixel(std::clamp(ix, 0, im.width() - 1), std::clamp(iy, 0, im.height() - 1));
        return decode(p);
    }
    V3 sum{};
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx) {
            QRgb p =
                im.pixel(std::clamp(ix + dx, 0, im.width() - 1), std::clamp(iy + dy, 0, im.height() - 1));
            float w = (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
            sum = sum + decode(p) * w;
        }
    return sum;
}
QByteArray png(const QImage &im) {
    QByteArray data;
    QBuffer b(&data);
    b.open(QIODevice::WriteOnly);
    if (!im.save(&b, "PNG"))
        fail("Cannot encode project image");
    return data.toBase64();
}
QImage unpng(const QJsonValue &v) {
    QImage im = QImage::fromData(QByteArray::fromBase64(v.toString().toLatin1()), "PNG");
    if (im.isNull())
        fail("Invalid project image");
    return im;
}
struct ClipVertex {
    QVector4D clip;
    QVector3D bary;
};
float plane(const QVector4D &v, int p) {
    return p == 0   ? v.w() + v.x()
           : p == 1 ? v.w() - v.x()
           : p == 2 ? v.w() + v.y()
           : p == 3 ? v.w() - v.y()
           : p == 4 ? v.w() + v.z()
                    : v.w() - v.z();
}
float smooth(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
float featherCurve(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return t * t * t * (t * (6 * t - 15) + 10);
}
float confidence(const AiAtlas &a, int x, int y) {
    if (!a.coverage.constScanLine(y)[x])
        return 0;
    if (a.confidence.size() == a.color.size() && a.confidence.format() == QImage::Format_Grayscale16) {
        auto value = reinterpret_cast<const quint16 *>(a.confidence.constScanLine(y))[x];
        if (value)
            return value / 65535.f;
    }
    return .5f; // Projects saved before camera quality was recorded.
}
float sampleConfidence(const AiAtlas &a, float x, float y) {
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    float fx = x - ix, fy = y - iy, result = 0;
    for (int dy = 0; dy < 2; ++dy)
        for (int dx = 0; dx < 2; ++dx)
            result += confidence(a, std::clamp(ix + dx, 0, a.color.width() - 1),
                                 std::clamp(iy + dy, 0, a.color.height() - 1)) *
                      (dx ? fx : 1 - fx) * (dy ? fy : 1 - fy);
    return result;
}
void setConfidence(QImage &image, int x, int y, float value) {
    reinterpret_cast<quint16 *>(image.scanLine(y))[x] = quint16(std::clamp(qRound(value * 65535), 1, 65535));
}
float angleQuality(float cosine, const AiProjectionOptions &options) {
    const float value = std::clamp((cosine - .15f) / .85f, 0.f, 1.f);
    float quality = options.anglePower == 2 ? value * value : std::pow(value, options.anglePower);
    if (options.minCosine > .15001f) {
        // Ease to zero over the last ten degrees before the quality cutoff.
        // A hard rejection alone would create another visible overlap border.
        const float full = options.minCosine * .984807753f +
                           std::sqrt(1 - options.minCosine * options.minCosine) * .173648178f;
        quality *= smooth((cosine - options.minCosine) / std::max(1e-6f, full - options.minCosine));
    }
    return quality;
}
float qualityBlend(float incoming, float previous) {
    // Prefer the better camera. A smooth transition between comparable cameras
    // avoids both hard winners and pollution by repeated grazing-angle views.
    return smooth(.5f + 2 * (incoming - previous) / std::max(1e-6f, incoming + previous));
}
// Evaluate each shared edge with the same double-precision expression in either
// winding. The float barycentric sum can reject both triangles at a shared edge.
bool projectionBary(const V2 *t, V2 p, float &u, float &v) {
    auto edge = [](V2 a, V2 b, V2 c) {
        return (double(b.x) - a.x) * (double(c.y) - a.y) - (double(b.y) - a.y) * (double(c.x) - a.x);
    };
    double det = edge(t[0], t[1], t[2]);
    if (std::abs(det) < 1e-30)
        return false;
    const bool ccw = det > 0;
    double weights[3];
    for (int i = 0; i < 3; ++i) {
        V2 a = t[(i + 1) % 3], b = t[(i + 2) % 3];
        weights[i] = edge(a, b, p) * (ccw ? 1 : -1);
        if (weights[i] < 0 || (weights[i] == 0 && !(ccw ? ownedEdge(a, b) : ownedEdge(b, a))))
            return false;
    }
    u = float(weights[1] / std::abs(det));
    v = float(weights[2] / std::abs(det));
    return true;
}

// A capture pixel may span many triangles. Resolve ambiguous visibility at the
// actual atlas sample, rather than requiring its triangle to own that pixel.
// Segment tests still reject thin, disconnected, or folded occluders exactly.
class ProjectionOcclusion {
    struct D3 {
        double x, y, z;
        D3(V3 v) : x(v.x), y(v.y), z(v.z) {}
        D3(double a, double b, double c) : x(a), y(b), z(c) {}
        D3 operator-(D3 b) const { return {x - b.x, y - b.y, z - b.z}; }
        D3 cross(D3 b) const { return {y * b.z - z * b.y, z * b.x - x * b.z, x * b.y - y * b.x}; }
        double dot(D3 b) const { return x * b.x + y * b.y + z * b.z; }
        double axis(int a) const { return a == 0 ? x : a == 1 ? y : z; }
    };
    struct Node {
        V3 low{}, high{};
        int first = 0, count = 0, left = -1, right = -1;
    };
    const Scene &scene_;
    D3 eye_;
    bool parallel_ = false; // Orthographic: eye_ is the direction toward the camera.
    std::vector<int> order_;
    std::vector<Node> nodes_;
    static float axis(V3 v, int a) { return a == 0 ? v.x : a == 1 ? v.y : v.z; }
    int build(int first, int count, std::atomic_bool *cancel) {
        checkCancel(cancel);
        Node node;
        node.first = first;
        node.count = count;
        node.low = {INFINITY, INFINITY, INFINITY};
        node.high = {-INFINITY, -INFINITY, -INFINITY};
        for (int i = first; i < first + count; ++i)
            for (auto p : scene_.tris[order_[i]].p) {
                node.low = {std::min(node.low.x, p.x), std::min(node.low.y, p.y), std::min(node.low.z, p.z)};
                node.high = {std::max(node.high.x, p.x), std::max(node.high.y, p.y),
                             std::max(node.high.z, p.z)};
            }
        const int index = int(nodes_.size());
        nodes_.push_back(node);
        if (count > 8) {
            V3 extent = node.high - node.low;
            const int a = extent.x > extent.y ? (extent.x > extent.z ? 0 : 2) : (extent.y > extent.z ? 1 : 2);
            int middle = first + count / 2;
            auto center = [&](int i) {
                const auto &t = scene_.tris[i];
                return double(axis(t.p[0], a)) + axis(t.p[1], a) + axis(t.p[2], a);
            };
            std::nth_element(order_.begin() + first, order_.begin() + middle, order_.begin() + first + count,
                             [&](int i, int j) { return center(i) < center(j); });
            const int left = build(first, middle - first, cancel);
            const int right = build(middle, first + count - middle, cancel);
            nodes_[index].left = left;
            nodes_[index].right = right;
            nodes_[index].count = 0;
        }
        return index;
    }
    bool blocked(int index, D3 origin3, D3 direction, const std::array<double, 3> &inverse, double limit,
                 int target) const {
        const auto &node = nodes_[index];
        double near = 0, far = limit;
        for (int a = 0; a < 3; ++a) {
            double low = axis(node.low, a), high = axis(node.high, a), origin = origin3.axis(a);
            if (direction.axis(a) == 0) {
                if (origin < low || origin > high)
                    return false;
            } else {
                double t0 = (low - origin) * inverse[a], t1 = (high - origin) * inverse[a];
                if (t0 > t1)
                    std::swap(t0, t1);
                near = std::max(near, t0);
                far = std::min(far, t1);
                if (near > far)
                    return false;
            }
        }
        if (!node.count)
            return blocked(node.left, origin3, direction, inverse, limit, target) ||
                   blocked(node.right, origin3, direction, inverse, limit, target);
        for (int i = node.first; i < node.first + node.count; ++i) {
            int face = order_[i];
            if (face == target)
                continue;
            const auto &t = scene_.tris[face];
            D3 a(t.p[0]), e1 = D3(t.p[1]) - a, e2 = D3(t.p[2]) - a;
            D3 h = direction.cross(e2);
            double det = e1.dot(h);
            if (std::abs(det) < 1e-30)
                continue;
            double inv = 1 / det;
            D3 delta = origin3 - a;
            double u = delta.dot(h) * inv;
            if (u < -1e-10 || u > 1 + 1e-10)
                continue;
            D3 q = delta.cross(e1);
            double v = direction.dot(q) * inv;
            if (v < -1e-10 || u + v > 1 + 1e-10)
                continue;
            double distance = e2.dot(q) * inv;
            if (distance > 0 && distance < limit)
                return true;
        }
        return false;
    }

  public:
    ProjectionOcclusion(const Scene &s, QVector3D eye, bool parallel, std::atomic_bool *cancel)
        : scene_(s), eye_(eye.x(), eye.y(), eye.z()), parallel_(parallel), order_(s.tris.size()) {
        std::iota(order_.begin(), order_.end(), 0);
        if (!order_.empty())
            build(0, int(order_.size()), cancel);
    }
    bool blocked(V3 point, int target) const {
        if (nodes_.empty())
            return false;
        // A parallel ray starts outside the whole scene (scale is its diagonal), toward the camera.
        const double reach = 2.0 * std::max(1e-6f, scene_.scale);
        const D3 origin = parallel_ ? D3(point.x + eye_.x * reach, point.y + eye_.y * reach,
                                         point.z + eye_.z * reach)
                                    : eye_;
        D3 direction = D3(point) - origin;
        double distance = std::sqrt(direction.dot(direction));
        double limit = 1 - std::max(1e-7, double(scene_.scale) * 2e-7 / std::max(1e-20, distance));
        std::array<double, 3> inverse;
        for (int a = 0; a < 3; ++a)
            inverse[a] = direction.axis(a) == 0 ? 0 : 1 / direction.axis(a);
        return blocked(0, origin, direction, inverse, limit, target);
    }
};
std::vector<QSet<int>> projectionNeighbours(const Scene &s, std::atomic_bool *cancel) {
    std::vector<QSet<int>> result(s.tris.size());
    std::map<std::array<float, 6>, std::vector<int>> edges;
    for (int ti = 0; ti < int(s.tris.size()); ++ti) {
        if (!(ti & 255))
            checkCancel(cancel);
        for (int k = 0; k < 3; ++k) {
            const auto &p = s.tris[ti].p[k], &q = s.tris[ti].p[(k + 1) % 3];
            std::array<float, 3> a{p.x, p.y, p.z}, b{q.x, q.y, q.z};
            if (b < a)
                std::swap(a, b);
            auto &found = edges[{a[0], a[1], a[2], b[0], b[1], b[2]}];
            for (int other : found) {
                result[ti].insert(other);
                result[other].insert(ti);
            }
            found.push_back(ti);
        }
    }
    return result;
}
std::vector<float> projectionFeather(const Scene &s, const AiCapture &c, const QImage &mask,
                                     const std::vector<QSet<int>> &neighbours, float fraction,
                                     std::atomic_bool *cancel) {
    const int n = c.camera.size;
    const float radius = fraction * n;
    std::vector<float> distance(size_t(n) * n, radius + 1);
    if (radius <= 0)
        return std::vector<float>(distance.size(), 1);
    auto valid = [&](int x, int y) {
        return x >= 0 && y >= 0 && x < n && y < n && c.faces[y * n + x] >= 0 && qGray(mask.pixel(x, y)) > 0;
    };
    for (int y = 0; y < n; ++y) {
        checkCancel(cancel);
        for (int x = 0; x < n; ++x) {
            int i = y * n + x, face = c.faces[i];
            if (!valid(x, y)) {
                distance[i] = 0;
                continue;
            }
            for (const QPoint &delta : {QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1)}) {
                int xx = x + delta.x(), yy = y + delta.y();
                if (!valid(xx, yy)) {
                    distance[i] = 1;
                    break;
                }
                int other = c.faces[yy * n + xx];
                bool depthJump = true;
                if (c.linearDepth.size() == c.faces.size()) {
                    float depth = std::min(c.linearDepth[i], c.linearDepth[yy * n + xx]);
                    float footprint = 6 * depth / (n * c.camera.projection(1, 1));
                    depthJump = std::abs(c.linearDepth[i] - c.linearDepth[yy * n + xx]) >
                                std::max(depth * .002f, footprint);
                }
                if (s.tris[face].material != s.tris[other].material ||
                    (face != other && !neighbours[face].contains(other) && depthJump)) {
                    distance[i] = 1; // Occlusion edge; shared triangle edges are not seams.
                    break;
                }
            }
        }
    }
    // Two-pass chamfer distance, measured in camera pixels rather than UV texels.
    for (int pass = 0; pass < 2; ++pass) {
        int step = pass ? -1 : 1;
        for (int y = pass ? n - 1 : 0; y >= 0 && y < n; y += step) {
            checkCancel(cancel);
            for (int x = pass ? n - 1 : 0; x >= 0 && x < n; x += step) {
                float &d = distance[y * n + x];
                auto relax = [&](int xx, int yy, float cost) {
                    if (xx >= 0 && yy >= 0 && xx < n && yy < n)
                        d = std::min(d, distance[yy * n + xx] + cost);
                };
                relax(x - step, y, 1);
                relax(x, y - step, 1);
                relax(x - 1, y - step, 1.414214f);
                relax(x + 1, y - step, 1.414214f);
            }
        }
    }
    for (float &d : distance)
        d = featherCurve(d / radius);
    return distance;
}
// Transition from new coverage into old paint in camera space. UV seams must not
// become blend boundaries; disconnected or occluded surfaces remain barriers.
std::vector<float> coverageTransition(const Scene &s, const AiCapture &c, const AiAtlases &before,
                                      const QImage &mask, const QSet<QString> &materials,
                                      const std::vector<int> &surfaces, float fraction,
                                      std::atomic_bool *cancel) {
    const int n = c.camera.size;
    const float radius = fraction * n;
    std::vector<float> result(c.faces.size(), 0);
    if (radius <= 0)
        return result;
    std::vector<int> distance(c.faces.size(), -1);
    std::deque<int> queue;
    bool hasKnown = false;
    for (int y = 0; y < n; ++y) {
        checkCancel(cancel);
        for (int x = 0; x < n; ++x) {
            int i = y * n + x, face = c.faces[i];
            if (face < 0 || !materials.contains(aiMaterial(s, s.tris[face])) || !qGray(mask.pixel(x, y)))
                continue;
            const auto atlas = before.constFind(aiMaterial(s, s.tris[face]));
            if (atlas == before.cend())
                continue;
            const auto uv = c.texcoords[i];
            int tx = std::clamp(int(uv.x * atlas->coverage.width()), 0, atlas->coverage.width() - 1);
            int ty = std::clamp(int((1 - uv.y) * atlas->coverage.height()), 0, atlas->coverage.height() - 1);
            if (atlas->coverage.constScanLine(ty)[tx]) {
                hasKnown = true;
                distance[i] = int(std::ceil(radius)) + 1;
            } else {
                distance[i] = 0;
                queue.push_back(i);
            }
        }
    }
    if (!hasKnown || queue.empty())
        return result;
    while (!queue.empty()) {
        int i = queue.front();
        queue.pop_front();
        if (!(i & 8191))
            checkCancel(cancel);
        if (distance[i] >= radius)
            continue;
        for (int d : {-n, -1, 1, n}) {
            int j = i + d;
            if (j < 0 || j >= int(distance.size()) || (d == -1 && i % n == 0) || (d == 1 && i % n == n - 1) ||
                distance[j] < 0 || distance[j] <= distance[i] + 1 ||
                surfaces[c.faces[i]] != surfaces[c.faces[j]])
                continue;
            if (c.linearDepth.size() == c.faces.size()) {
                const float depth = std::min(c.linearDepth[i], c.linearDepth[j]);
                const float footprint = 6 * depth / (n * c.camera.projection(1, 1));
                if (std::abs(c.linearDepth[i] - c.linearDepth[j]) > std::max(depth * .002f, footprint))
                    continue;
            }
            distance[j] = distance[i] + 1;
            queue.push_back(j);
        }
    }
    for (size_t i = 0; i < result.size(); ++i)
        if (distance[i] >= 0 && distance[i] < radius)
            result[i] = 1 - featherCurve(distance[i] / radius);
    return result;
}
// Only extend coverage inside existing paint. Fresh silhouettes and broad newly
// exposed regions retain the normal sharpness cutoff. Work in camera space so
// UV-island boundaries cannot split an otherwise enclosed surface hole.
std::vector<uchar> enclosedProjectionGaps(const Scene &s, const AiCapture &c, const AiAtlases &before,
                                          const QImage &mask, const QSet<QString> &materials,
                                          const std::vector<int> &surfaces,
                                          const std::vector<QSet<int>> &neighbours,
                                          std::atomic_bool *cancel) {
    const int n = c.camera.size;
    std::vector<uchar> state(c.faces.size(), 3), result(c.faces.size(), 0);
    bool anyKnown = false, anyEmpty = false;
    for (int y = 0; y < n; ++y) {
        checkCancel(cancel);
        for (int x = 0; x < n; ++x) {
            const int i = y * n + x, face = c.faces[i];
            if (face < 0 || !qGray(mask.pixel(x, y)))
                continue;
            const auto material = aiMaterial(s, s.tris[face]);
            if (!materials.contains(material))
                continue;
            bool known = qGray(c.known.pixel(x, y)) >= 128;
            if (!before.isEmpty()) {
                const auto atlas = before.constFind(material);
                if (atlas == before.cend())
                    continue;
                const auto uv = c.texcoords[i];
                const int tx =
                    std::clamp(int(uv.x * atlas->coverage.width()), 0, atlas->coverage.width() - 1);
                const int ty =
                    std::clamp(int((1 - uv.y) * atlas->coverage.height()), 0, atlas->coverage.height() - 1);
                known = atlas->coverage.constScanLine(ty)[tx] != 0;
            }
            state[i] = known ? 2 : 0;
            anyKnown |= known;
            anyEmpty |= !known;
        }
    }
    if (!anyKnown || !anyEmpty)
        return result;
    std::vector<int> component;
    for (int seed = 0; seed < int(state.size()); ++seed) {
        if (state[seed] != 0)
            continue;
        component.clear();
        component.push_back(seed);
        state[seed] = 1;
        bool open = false, supported = false;
        for (size_t cursor = 0; cursor < component.size(); ++cursor) {
            if (!(cursor & 8191))
                checkCancel(cancel);
            const int i = component[cursor], x = i % n, y = i / n;
            for (const QPoint &d : {QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1)}) {
                const int xx = x + d.x(), yy = y + d.y();
                if (xx < 0 || yy < 0 || xx >= n || yy >= n) {
                    open = true;
                    continue;
                }
                const int j = yy * n + xx;
                if (state[j] == 3) {
                    // Background, unselected material, and an excluded image
                    // mask are open boundaries, never paint support.
                    open = true;
                    continue;
                }
                if (surfaces[c.faces[i]] != surfaces[c.faces[j]])
                    continue;
                // Match projection feathering: shared triangle edges are not
                // occlusion seams, even on a steeply sloped surface.
                if (c.faces[i] != c.faces[j] && !neighbours[c.faces[i]].contains(c.faces[j]) &&
                    c.linearDepth.size() == c.faces.size()) {
                    const float depth = std::min(c.linearDepth[i], c.linearDepth[j]);
                    const float footprint = 6 * depth / (n * c.camera.projection(1, 1));
                    if (std::abs(c.linearDepth[i] - c.linearDepth[j]) > std::max(depth * .002f, footprint))
                        continue; // Occlusion boundary: do not cross onto the other surface.
                }
                if (state[j] == 2)
                    supported = true;
                else if (state[j] == 0) {
                    state[j] = 1;
                    component.push_back(j);
                }
            }
        }
        if (!open && supported)
            for (const int i : component)
                result[i] = 255;
    }
    // UV samples can fall inside a gap while the nearest camera pixel's center
    // lands just outside it. Include one known-pixel footprint around the gap;
    // projection still admits only empty atlas texels. Never grow into another
    // empty/open component, excluded mask, or across a visibility boundary.
    auto expanded = result;
    for (int y = 0; y < n; ++y) {
        checkCancel(cancel);
        for (int x = 0; x < n; ++x) {
            const int i = y * n + x;
            if (state[i] != 2)
                continue;
            for (int dy = -1; dy <= 1 && !expanded[i]; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= n || yy >= n)
                        continue;
                    const int j = yy * n + xx;
                    if (!result[j] || surfaces[c.faces[i]] != surfaces[c.faces[j]])
                        continue;
                    if (c.faces[i] != c.faces[j] && !neighbours[c.faces[i]].contains(c.faces[j]) &&
                        c.linearDepth.size() == c.faces.size()) {
                        const float depth = std::min(c.linearDepth[i], c.linearDepth[j]);
                        const float footprint = 6 * depth / (n * c.camera.projection(1, 1));
                        if (std::abs(c.linearDepth[i] - c.linearDepth[j]) >
                            std::max(depth * .002f, footprint))
                            continue;
                    }
                    expanded[i] = 255;
                    break;
                }
        }
    }
    return expanded;
}
void pad(AiAtlas &a, int radius, std::atomic_bool *cancel) {
    const int n = a.color.width(), h = a.color.height();
    std::vector<int> owner(size_t(n) * h, -1), dist(owner.size(), 0);
    std::deque<int> queue;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < n; ++x)
            if (a.coverage.constScanLine(y)[x]) {
                int i = y * n + x;
                owner[i] = i;
                queue.push_back(i);
            }
    while (!queue.empty()) {
        int i = queue.front();
        queue.pop_front();
        if ((i & 8191) == 0)
            checkCancel(cancel);
        if (dist[i] >= radius)
            continue;
        int x = i % n, y = i / n;
        for (int d : {-n, -1, 1, n}) {
            int j = i + d;
            if (j < 0 || j >= int(owner.size()) || (d == -1 && !x) || (d == 1 && x == n - 1))
                continue;
            if (owner[j] >= 0)
                continue;
            owner[j] = owner[i];
            dist[j] = dist[i] + 1;
            queue.push_back(j);
        }
    }
    QImage result = a.color.copy();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < n; ++x) {
            int j = owner[y * n + x];
            if (j >= 0 && !a.coverage.constScanLine(y)[x])
                result.setPixel(x, y, a.color.pixel(j % n, j / n));
        }
    a.color = result;
}
} // namespace

QString aiMaterial(const Scene &s, const Triangle &t) {
    return t.material >= 0 && t.material < s.materials.size() ? s.materials[t.material] : QString("Default");
}
QStringList validateAiMesh(const Scene &s, int uv) {
    QStringList errors;
    QMap<QString, Scene> groups;
    for (const Triangle &t : s.tris)
        groups[aiMaterial(s, t)].tris.push_back(t);
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        it->uvNames = s.uvNames;
        Settings settings;
        settings.maskUV = settings.aoUV = uv;
        settings.averageOverlaps = false;
        const Validation v = validate(*it, settings);
        for (QString e : v.errors)
            errors << it.key() + ": " + e.replace("AO UV", "Texture UV");
    }
    if (s.tris.empty())
        errors << "The mesh contains no triangles.";
    return errors;
}
AiCamera aiCamera(const Scene &s, const OrbitCamera &o, int size) {
    AiCamera c;
    c.orbit = o;
    c.size = size;
    if (o.orthographic) {
        const float half = o.distance * std::tan(o.fieldOfView * 3.14159265359f / 360);
        c.projection.ortho(-half, half, -half, half, o.distance - s.scale * 2, o.distance + s.scale * 4);
    } else
        c.projection.perspective(o.fieldOfView, 1.f, std::max(s.scale * .0001f, o.distance - s.scale * 2),
                                 o.distance + s.scale * 4);
    c.view.translate(o.pan.x, o.pan.y, -o.distance);
    c.view.rotate(o.pitch, 1, 0, 0);
    c.view.rotate(o.yaw, 0, 1, 0);
    c.view.translate(-s.center.x, -s.center.y, -s.center.z);
    return c;
}
OrbitCamera aiFrameCamera(const Scene &s, OrbitCamera camera, bool visibleOnly) {
    QMatrix4x4 rotate;
    rotate.rotate(camera.pitch, 1, 0, 0);
    rotate.rotate(camera.yaw, 0, 1, 0);
    constexpr float fill = .95f;
    const float lens = std::tan(camera.fieldOfView * 3.14159265359f / 360);
    // Tight view-space bounding box, centred with pan so the image spends its pixels on the mesh.
    float x0 = INFINITY, x1 = -INFINITY, y0 = INFINITY, y1 = -INFINITY;
    for (const auto &triangle : s.tris)
        for (const auto &p : triangle.p) {
            const auto q = rotate.map(qv(p - s.center));
            x0 = std::min(x0, q.x());
            x1 = std::max(x1, q.x());
            y0 = std::min(y0, q.y());
            y1 = std::max(y1, q.y());
        }
    if (!(x0 <= x1 && y0 <= y1))
        return camera;
    if (visibleOnly && camera.orthographic) {
        // Keep a deliberate close-up: frame only the part of the box inside the current square window.
        const float half = camera.distance * lens;
        const float wx0 = std::max(x0, -half - camera.pan.x), wx1 = std::min(x1, half - camera.pan.x);
        const float wy0 = std::max(y0, -half - camera.pan.y), wy1 = std::min(y1, half - camera.pan.y);
        if (wx0 < wx1 && wy0 < wy1) {
            x0 = wx0, x1 = wx1, y0 = wy0, y1 = wy1;
        }
    }
    const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    camera.pan = {-cx, -cy, 0};
    if (camera.orthographic) {
        const float half = std::max({x1 - x0, y1 - y0, s.scale * 1e-4f}) / (2 * fill);
        camera.distance = half / lens;
        return camera;
    }
    camera.distance = s.scale * .01f;
    for (const auto &triangle : s.tris)
        for (const auto &p : triangle.p) {
            const auto q = rotate.map(qv(p - s.center));
            camera.distance = std::max(camera.distance, q.z() + std::max(std::abs(q.x() - cx), std::abs(q.y() - cy)) /
                                                                    (fill * lens));
        }
    return camera;
}
QList<OrbitCamera> aiAutomaticCameras(const Scene &s, const OrbitCamera &front) {
    // A ring around the front at 45 degree steps, then the top. Generation order grows outward from the
    // front, so every later view continues from neighbouring accepted paint.
    QList<OrbitCamera> cameras;
    for (const auto &[yaw, pitch] : std::initializer_list<std::pair<float, float>>{
             {0, 0}, {45, 0}, {-45, 0}, {90, 0}, {-90, 0}, {135, 0}, {-135, 0}, {180, 0}, {0, 70}}) {
        OrbitCamera c;
        c.yaw = front.yaw + yaw;
        c.pitch = pitch ? pitch : front.pitch;
        c.fieldOfView = front.fieldOfView;
        c.orthographic = front.orthographic;
        cameras << aiFrameCamera(s, c);
    }
    return cameras;
}
AiAtlases aiEmptyAtlases(const Scene &s, int size) {
    if (size < 1 || size > 4096)
        fail("Invalid AI atlas resolution");
    AiAtlases result;
    for (const auto &t : s.tris)
        if (!result.contains(aiMaterial(s, t))) {
            AiAtlas a;
            a.color = QImage(size, size, QImage::Format_RGB32);
            a.color.fill(QColor(128, 128, 128));
            a.coverage = QImage(size, size, QImage::Format_Grayscale8);
            a.coverage.fill(0);
            a.confidence = QImage(size, size, QImage::Format_Grayscale16);
            a.confidence.fill(0);
            result[aiMaterial(s, t)] = a;
        }
    return result;
}
AiCapture aiCapture(const Scene &s, int uv, const AiAtlases &atlases, const AiCamera &c,
                    std::atomic_bool *cancel) {
    if (c.size < 1 || c.size > 2048)
        fail("Invalid generation view size");
    AiCapture out;
    out.camera = c;
    for (QImage *im : {&out.neutral, &out.normal, &out.depth, &out.silhouette, &out.partial, &out.known}) {
        *im = QImage(c.size, c.size, QImage::Format_RGB32);
        im->fill(Qt::white);
    }
    out.silhouette.fill(Qt::black);
    out.known.fill(Qt::black);
    out.depth.fill(Qt::black);
    out.faces.assign(size_t(c.size) * c.size, -1);
    out.z.assign(out.faces.size(), INFINITY);
    out.linearDepth.assign(out.faces.size(), INFINITY);
    out.texcoords.resize(out.faces.size());
    out.confidence = QImage(c.size, c.size, QImage::Format_Grayscale16);
    out.confidence.fill(0);
    QMatrix4x4 mvp = c.projection * c.view;
    float nearDepth = INFINITY, farDepth = -INFINITY;
    for (const auto &t : s.tris)
        for (const auto &p : t.p) {
            float depth = -(c.view * QVector4D(qv(p), 1)).z();
            nearDepth = std::min(nearDepth, depth);
            farDepth = std::max(farDepth, depth);
        }
    for (int ti = 0; ti < int(s.tris.size()); ++ti) {
        if (!(ti & 255))
            checkCancel(cancel);
        const Triangle &t = s.tris[ti];
        if (uv < 0 || uv >= int(t.uv.size()))
            fail("Missing selected UV set");
        // Back faces (the inside of an open collar or neck seen from behind) are culled: a guide showing
        // them makes the model paint outer clothing onto inner surfaces. Vertex normals, not winding, decide.
        {
            const QVector3D normal = c.view.mapVector(qv(t.n[0]) + qv(t.n[1]) + qv(t.n[2]));
            const QVector3D centroid = (c.view.map(qv(t.p[0])) + c.view.map(qv(t.p[1])) + c.view.map(qv(t.p[2]))) / 3;
            if ((orthographic(c.projection) ? normal.z() : QVector3D::dotProduct(normal, -centroid)) <= 0)
                continue;
        }
        QList<ClipVertex> poly;
        for (int k = 0; k < 3; ++k) {
            QVector3D b;
            b[k] = 1;
            poly << ClipVertex{mvp * QVector4D(qv(t.p[k]), 1), b};
        }
        for (int p = 0; p < 6 && !poly.empty(); ++p) {
            QList<ClipVertex> clipped;
            ClipVertex a = poly.back();
            float da = plane(a.clip, p);
            for (const auto &b : poly) {
                float db = plane(b.clip, p);
                if ((da >= 0) != (db >= 0)) {
                    float f = da / (da - db);
                    clipped << ClipVertex{a.clip + (b.clip - a.clip) * f, a.bary + (b.bary - a.bary) * f};
                }
                if (db >= 0)
                    clipped << b;
                a = b;
                da = db;
            }
            poly = clipped;
        }
        const AiAtlas atlas = atlases.value(aiMaterial(s, t));
        for (int fan = 1; fan + 1 < poly.size(); ++fan) {
            ClipVertex cv[] = {poly[0], poly[fan], poly[fan + 1]};
            V2 screen[3];
            for (int k = 0; k < 3; ++k)
                screen[k] = {(cv[k].clip.x() / cv[k].clip.w() * .5f + .5f) * c.size,
                             (.5f - cv[k].clip.y() / cv[k].clip.w() * .5f) * c.size};
            int x0 =
                std::clamp(int(std::floor(std::min({screen[0].x, screen[1].x, screen[2].x}))), 0, c.size - 1);
            int x1 =
                std::clamp(int(std::ceil(std::max({screen[0].x, screen[1].x, screen[2].x}))), 0, c.size - 1);
            int y0 =
                std::clamp(int(std::floor(std::min({screen[0].y, screen[1].y, screen[2].y}))), 0, c.size - 1);
            int y1 =
                std::clamp(int(std::ceil(std::max({screen[0].y, screen[1].y, screen[2].y}))), 0, c.size - 1);
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    float u, v;
                    if (!projectionBary(screen, {x + .5f, y + .5f}, u, v))
                        continue;
                    float b[] = {1 - u - v, u, v};
                    float z = 0;
                    QVector3D bc;
                    float den = 0;
                    for (int k = 0; k < 3; ++k) {
                        z += b[k] * cv[k].clip.z() / cv[k].clip.w();
                        float w = b[k] / cv[k].clip.w();
                        bc += cv[k].bary * w;
                        den += w;
                    }
                    int i = y * c.size + x;
                    if (z >= out.z[i])
                        continue;
                    bc /= den;
                    out.z[i] = z;
                    out.faces[i] = ti;
                    V3 nn = unit(blend(t.n, bc.y(), bc.z()));
                    QVector3D n = c.view.mapVector(qv(nn)).normalized();
                    out.normal.setPixel(x, y,
                                        qRgb(qRound((n.x() * .5f + .5f) * 255),
                                             qRound((n.y() * .5f + .5f) * 255),
                                             qRound((n.z() * .5f + .5f) * 255)));
                    out.neutral.setPixel(x, y, qRgb(150, 150, 150));
                    out.silhouette.setPixel(x, y, qRgb(255, 255, 255));
                    float linearDepth = -(c.view * QVector4D(qv(blend(t.p, bc.y(), bc.z())), 1)).z();
                    out.linearDepth[i] = linearDepth;
                    int d = std::clamp(
                        qRound((1 - (linearDepth - nearDepth) / std::max(1e-6f, farDepth - nearDepth)) * 255),
                        0, 255);
                    out.depth.setPixel(x, y, qRgb(d, d, d));
                    V2 tex = blend(t.uv[uv].data(), bc.y(), bc.z());
                    out.texcoords[i] = tex;
                    if (!atlas.color.isNull()) {
                        int tx = std::clamp(int(tex.x * atlas.color.width()), 0, atlas.color.width() - 1);
                        int ty =
                            std::clamp(int((1 - tex.y) * atlas.color.height()), 0, atlas.color.height() - 1);
                        bool known = atlas.coverage.constScanLine(ty)[tx] != 0;
                        out.partial.setPixel(x, y, known ? atlas.color.pixel(tx, ty) : qRgb(150, 150, 150));
                        out.known.setPixel(x, y, known ? qRgb(255, 255, 255) : qRgb(0, 0, 0));
                        if (known)
                            setConfidence(out.confidence, x, y,
                                          sampleConfidence(atlas, tex.x * atlas.color.width() - .5f,
                                                           (1 - tex.y) * atlas.color.height() - .5f));
                    }
                }
        }
    }
    return out;
}
AiFeedback aiFeedback(const AiCapture &c, float strength) {
    if (!std::isfinite(strength) || strength < 0 || strength > 1)
        fail("Invalid detail continuity strength");
    if (c.partial.size() != c.neutral.size() || c.known.size() != c.neutral.size() ||
        c.silhouette.size() != c.neutral.size() || c.confidence.size() != c.neutral.size() ||
        c.confidence.format() != QImage::Format_Grayscale16)
        fail("Invalid continuity capture");
    AiFeedback out;
    out.image = c.neutral.convertToFormat(QImage::Format_RGB32);
    out.mask = QImage(c.neutral.size(), QImage::Format_Grayscale8);
    out.mask.fill(0);
    if (!strength)
        return out;
    // Strength selects how much reliable surface qualifies, rather than capping
    // every old detail at partial opacity. Strong cores always retain full color.
    const float low = .25f + .55f * (1 - strength), high = .8f + .2f * (1 - strength);
    for (int y = 0; y < out.image.height(); ++y)
        for (int x = 0; x < out.image.width(); ++x) {
            if (qGray(c.known.pixel(x, y)) < 128 || qGray(c.silhouette.pixel(x, y)) < 128)
                continue;
            float quality = reinterpret_cast<const quint16 *>(c.confidence.constScanLine(y))[x] / 65535.f;
            float weight = smooth((quality - low) / (high - low));
            out.mask.scanLine(y)[x] = uchar(qRound(weight * 255));
            if (!out.mask.constScanLine(y)[x])
                continue;
            // Confidence controls reuse, not paint color. Fading toward neutral
            // teaches the editor a washed-out, different material at the join.
            out.image.setPixel(x, y, c.partial.pixel(x, y));
            out.active = true;
        }
    return out;
}
AiFeedback aiProjectionFeedback(const Scene &s, const AiCapture &c, float strength,
                                const AiProjectionOptions &options, std::atomic_bool *cancel) {
    auto result = aiFeedback(c, strength);
    if (!strength)
        return result;
    const auto neighbours = projectionNeighbours(s, cancel);
    const auto feather = projectionFeather(s, c, c.silhouette, neighbours, options.feather, cancel);
    const auto inverse = c.camera.projection.inverted();
    const int n = c.camera.size;
    const float low = .08f + .2f * (1 - strength), high = .28f + .4f * (1 - strength);
    const float contextFloor = .04f + .2f * (1 - strength);
    for (int y = 0; y < n; ++y) {
        checkCancel(cancel);
        for (int x = 0; x < n; ++x) {
            const int i = y * n + x;
            if (c.faces[i] < 0 || qGray(c.known.pixel(x, y)) < 128)
                continue;
            const QRgb rgb = c.normal.pixel(x, y);
            const QVector3D normal(qRed(rgb) / 127.5f - 1, qGreen(rgb) / 127.5f - 1, qBlue(rgb) / 127.5f - 1);
            const float incoming =
                angleQuality(QVector3D::dotProduct(normal.normalized(), towardCamera(c.camera, inverse, x, y)), options) *
                feather[i];
            const float old = reinterpret_cast<const quint16 *>(c.confidence.constScanLine(y))[x] / 65535.f;
            // Retain ordinary sharp overlap (roughly the previous camera's
            // central 40–45 degrees), not just its near-perpendicular core.
            // Weak grazing paint still remains free to regenerate.
            const float reliableLow = .10f + .3f * (1 - strength);
            const float reliableHigh = .40f + .5f * (1 - strength);
            const int reliable = qRound(smooth((old - reliableLow) / (reliableHigh - reliableLow)) * 255);
            result.mask.scanLine(y)[x] = uchar(std::max(int(result.mask.constScanLine(y)[x]), reliable));
            // Keep neighboring patterns visible as spatial context even when
            // they are free to change. Removing every unlocked pixel leaves
            // isolated paint cutouts and gives the editor no edge to continue.
            // Very poor paint still becomes neutral; reuse remains independent.
            if (old >= contextFloor)
                result.image.setPixel(x, y, c.partial.pixel(x, y));
            const float anchor = smooth((old - low) / (high - low)) * (1 - smooth((incoming - .35f) / .4f));
            const int reuse = std::max(int(result.mask.constScanLine(y)[x]), qRound(anchor * 255));
            if (reuse) {
                result.mask.scanLine(y)[x] = uchar(reuse);
                result.image.setPixel(x, y, c.partial.pixel(x, y));
                result.active = true;
            }
        }
    }
    return result;
}
double aiNewCoverage(const Scene &s, int, const AiAtlases &, const AiCapture &c,
                     const QSet<QString> &materials) {
    qint64 all = 0, fresh = 0;
    for (int y = 0; y < c.camera.size; ++y)
        for (int x = 0; x < c.camera.size; ++x) {
            int f = c.faces[y * c.camera.size + x];
            if (f < 0 || !materials.contains(aiMaterial(s, s.tris[f])))
                continue;
            ++all;
            if (qRed(c.known.pixel(x, y)) < 128)
                ++fresh;
        }
    return all ? double(fresh) / all : 0;
}
double aiViewContribution(const Scene &s, int, const AiCapture &c, const QSet<QString> &materials,
                          const AiProjectionOptions &options, std::atomic_bool *cancel) {
    const auto neighbours = projectionNeighbours(s, cancel);
    const auto feather = projectionFeather(s, c, c.silhouette, neighbours, options.feather, cancel);
    std::vector<uchar> gaps;
    if (options.fillGaps) {
        std::vector<int> surfaces(s.tris.size(), -1), pending;
        for (int seed = 0; seed < int(s.tris.size()); ++seed) {
            if (surfaces[seed] >= 0)
                continue;
            pending = {seed};
            surfaces[seed] = seed;
            for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
                checkCancel(cancel);
                const int face = pending[cursor];
                for (int other : neighbours[face])
                    if (surfaces[other] < 0 && s.tris[other].material == s.tris[face].material) {
                        surfaces[other] = seed;
                        pending.push_back(other);
                    }
            }
        }
        gaps = enclosedProjectionGaps(s, c, {}, c.silhouette, materials, surfaces, neighbours, cancel);
    }
    const auto inverse = c.camera.projection.inverted();
    qint64 visible = 0, useful = 0;
    const int n = c.camera.size;
    for (int y = 0; y < n; ++y) {
        checkCancel(cancel);
        for (int x = 0; x < n; ++x) {
            int i = y * n + x, face = c.faces[i];
            if (face < 0 || !materials.contains(aiMaterial(s, s.tris[face])))
                continue;
            ++visible;
            QRgb rgb = c.normal.pixel(x, y);
            QVector3D normal(qRed(rgb) / 127.5f - 1, qGreen(rgb) / 127.5f - 1, qBlue(rgb) / 127.5f - 1);
            const float cosine = QVector3D::dotProduct(normal.normalized(), towardCamera(c.camera, inverse, x, y));
            if (cosine <= options.minCosine) {
                if (qGray(c.known.pixel(x, y)) < 128 &&
                    ((cosine > .173648178f && !gaps.empty() && gaps[i]) ||
                     (options.coverAll && cosine > coverAllCosine)))
                    ++useful;
                continue;
            }
            if (!options.overlay && qRed(c.known.pixel(x, y)) < 128) {
                ++useful;
                continue;
            }
            float quality = std::min(
                1.f, angleQuality(cosine, options) *
                         feather[i] * options.strength);
            float old = c.confidence.isNull()
                            ? .5f
                            : reinterpret_cast<const quint16 *>(c.confidence.constScanLine(y))[x] / 65535.f;
            if (options.overlay ? quality > .01f : quality > old + .1f)
                ++useful;
        }
    }
    return visible ? double(useful) / visible : 0;
}
AiProjection aiProject(const Scene &s, int uv, const AiAtlases &before, const AiCapture &c,
                       const QImage &input, const QImage &mask, const QSet<QString> &materials, bool fillOnly,
                       int padding, std::atomic_bool *cancel, const AiProjectionOptions &options) {
    if (!std::isfinite(options.strength) || options.strength < 0 || options.strength > 2 ||
        !std::isfinite(options.anglePower) || options.anglePower < 1 || options.anglePower > 8 ||
        !std::isfinite(options.feather) || options.feather < 0 || options.feather > .25f ||
        !std::isfinite(options.minCosine) || options.minCosine < .15f || options.minCosine >= 1)
        fail("Invalid projection blending settings");
    if (input.size() != QSize(c.camera.size, c.camera.size) || mask.size() != input.size())
        fail("Candidate/mask size does not match the captured camera");
    if (!options.retainedMask.isNull() &&
        (options.retainedMask.size() != input.size() || c.confidence.size() != input.size() ||
         c.confidence.format() != QImage::Format_Grayscale16))
        fail("Retained detail mask does not match the capture");
    if (c.faces.size() != size_t(c.camera.size) * c.camera.size || c.z.size() != c.faces.size() ||
        c.texcoords.size() != c.faces.size())
        fail("Invalid capture visibility data");
    QImage candidate = input.convertToFormat(QImage::Format_RGB32);
    AiProjection result;
    QJsonArray recordedMaterials;
    result.atlases = before;
    if (!options.strength)
        return result;
    QMatrix4x4 mvp = c.camera.projection * c.camera.view;
    const bool parallel = orthographic(c.camera.projection);
    // World-space eye, or for orthographic cameras the unit direction toward the camera.
    const QVector3D eye = parallel ? c.camera.view.inverted().mapVector(QVector3D(0, 0, 1)).normalized()
                                   : c.camera.view.inverted().map(QVector3D());
    ProjectionOcclusion occlusion(s, eye, parallel, cancel);
    // Shared geometric edges may cover the same pixel footprint. Treat those as
    // neighbours, but never accept an unrelated nearly-coplanar occluding surface.
    std::vector<QSet<int>> neighbours(s.tris.size());
    std::vector<int> islands(s.tris.size());
    std::iota(islands.begin(), islands.end(), 0);
    std::vector<int> surfaces = islands;
    auto surfaceRoot = [&](int i) {
        while (surfaces[i] != i) {
            surfaces[i] = surfaces[surfaces[i]];
            i = surfaces[i];
        }
        return i;
    };
    auto root = [&](int i) {
        while (islands[i] != i) {
            islands[i] = islands[islands[i]];
            i = islands[i];
        }
        return i;
    };
    std::map<std::array<float, 6>, std::vector<int>> edges;
    std::map<std::array<float, 10>, int> uvEdges;
    for (int ti = 0; ti < int(s.tris.size()); ++ti) {
        if (!(ti & 255))
            checkCancel(cancel);
        const auto &t = s.tris[ti];
        if (uv < 0 || uv >= int(t.uv.size()))
            fail("Missing selected UV set");
        for (int k = 0; k < 3; ++k) {
            int j = (k + 1) % 3;
            std::array<float, 3> a{t.p[k].x, t.p[k].y, t.p[k].z}, b{t.p[j].x, t.p[j].y, t.p[j].z};
            if (b < a) {
                std::swap(a, b);
            }
            std::array<float, 6> key{a[0], a[1], a[2], b[0], b[1], b[2]};
            for (int other : edges[key]) {
                neighbours[ti].insert(other);
                neighbours[other].insert(ti);
                if (s.tris[other].material == t.material)
                    surfaces[surfaceRoot(ti)] = surfaceRoot(other);
            }
            edges[key].push_back(ti);
            std::array<float, 5> ua{t.p[k].x, t.p[k].y, t.p[k].z, t.uv[uv][k].x, t.uv[uv][k].y},
                ub{t.p[j].x, t.p[j].y, t.p[j].z, t.uv[uv][j].x, t.uv[uv][j].y};
            if (ub < ua)
                std::swap(ua, ub);
            std::array<float, 10> uk{ua[0], ua[1], ua[2], ua[3], ua[4], ub[0], ub[1], ub[2], ub[3], ub[4]};
            auto found = uvEdges.find(uk);
            if (found != uvEdges.end() && s.tris[found->second].material == t.material)
                islands[root(ti)] = root(found->second);
            else
                uvEdges[uk] = ti;
        }
    }
    for (int ti = 0; ti < int(s.tris.size()); ++ti)
        surfaces[ti] = surfaceRoot(ti);
    struct Layer {
        QImage color, opacity, quality, transition, contentQuality, retained;
        std::vector<int> island;
    };
    QMap<QString, Layer> layers;
    for (auto it = result.atlases.begin(); it != result.atlases.end(); ++it)
        if (materials.contains(it.key())) {
            it->color.detach();
            it->coverage.detach();
            if (it->confidence.size() != it->color.size() ||
                it->confidence.format() != QImage::Format_Grayscale16) {
                it->confidence = QImage(it->color.size(), QImage::Format_Grayscale16);
                it->confidence.fill(0);
                for (int y = 0; y < it->color.height(); ++y)
                    for (int x = 0; x < it->color.width(); ++x)
                        if (it->coverage.constScanLine(y)[x])
                            setConfidence(it->confidence, x, y, .5f);
            }
            it->confidence.detach();
            Layer layer;
            layer.color = it->color.copy();
            layer.opacity = QImage(it->color.size(), QImage::Format_Grayscale8);
            layer.opacity.fill(0);
            layer.quality = QImage(it->color.size(), QImage::Format_Grayscale16);
            layer.quality.fill(0);
            if (!options.retainedMask.isNull()) {
                layer.contentQuality = QImage(it->color.size(), QImage::Format_Grayscale16);
                layer.contentQuality.fill(0);
                layer.retained = QImage(it->color.size(), QImage::Format_Grayscale8);
                layer.retained.fill(0);
            }
            if (fillOnly)
                layer.island.assign(size_t(it->color.width()) * it->color.height(), -1);
            else {
                layer.transition = QImage(it->color.size(), QImage::Format_Grayscale16);
                layer.transition.fill(0);
            }
            layers[it.key()] = std::move(layer);
        }
    const auto feather = projectionFeather(s, c, mask, neighbours, options.feather, cancel);
    const auto transition =
        fillOnly ? std::vector<float>()
                 : coverageTransition(s, c, before, mask, materials, surfaces, options.feather, cancel);
    const auto gaps =
        options.fillGaps ? enclosedProjectionGaps(s, c, before, mask, materials, surfaces, neighbours, cancel)
                         : std::vector<uchar>();
    auto gapOptions = options;
    gapOptions.minCosine = .15f; // Fallback paint stays low-confidence for a better later view.
    for (int ti = 0; ti < int(s.tris.size()); ++ti) {
        checkCancel(cancel);
        const Triangle &t = s.tris[ti];
        QString material = aiMaterial(s, t);
        if (!materials.contains(material))
            continue;
        if (uv < 0 || uv >= int(t.uv.size()) || !result.atlases.contains(material))
            fail("Invalid projection UV/material");
        AiAtlas &a = result.atlases[material];
        const AiAtlas &sourceAtlas = before[material];
        Layer &layer = layers[material];
        int n = a.color.width();
        QVector3D screen[3];
        for (int k = 0; k < 3; ++k) {
            auto h = mvp * QVector4D(qv(t.p[k]), 1);
            screen[k] = {(h.x() / h.w() * .5f + .5f) * c.camera.size,
                         (.5f - h.y() / h.w() * .5f) * c.camera.size, h.z() / h.w()};
        }
        float det = (screen[1].x() - screen[0].x()) * (screen[2].y() - screen[0].y()) -
                    (screen[2].x() - screen[0].x()) * (screen[1].y() - screen[0].y());
        float dzdx = 0, dzdy = 0;
        if (std::isfinite(det) && std::abs(det) > 1e-8f) {
            dzdx = ((screen[1].z() - screen[0].z()) * (screen[2].y() - screen[0].y()) -
                    (screen[2].z() - screen[0].z()) * (screen[1].y() - screen[0].y())) /
                   det;
            dzdy = ((screen[1].x() - screen[0].x()) * (screen[2].z() - screen[0].z()) -
                    (screen[2].x() - screen[0].x()) * (screen[1].z() - screen[0].z())) /
                   det;
        }
        const auto &tex = t.uv[uv];
        int x0 = std::clamp(int(std::floor(std::min({tex[0].x, tex[1].x, tex[2].x}) * n)), 0, n - 1);
        int x1 = std::clamp(int(std::ceil(std::max({tex[0].x, tex[1].x, tex[2].x}) * n)), 0, n - 1);
        int y0 = std::clamp(int(std::floor((1 - std::max({tex[0].y, tex[1].y, tex[2].y})) * n)), 0, n - 1);
        int y1 = std::clamp(int(std::ceil((1 - std::min({tex[0].y, tex[1].y, tex[2].y})) * n)), 0, n - 1);
        for (int y = y0; y <= y1; ++y) {
            if (!(y & 63))
                checkCancel(cancel);
            for (int x = x0; x <= x1; ++x) {
                float u, v;
                if (!projectionBary(tex.data(), {(x + .5f) / n, 1 - (y + .5f) / n}, u, v))
                    continue;
                if (fillOnly)
                    layer.island[y * n + x] = root(ti);
                V3 p = blend(t.p, u, v), normal = unit(blend(t.n, u, v));
                QVector4D h = mvp * QVector4D(qv(p), 1);
                if (h.w() <= 0)
                    continue;
                float sx = (h.x() / h.w() * .5f + .5f) * c.camera.size,
                      sy = (.5f - h.y() / h.w() * .5f) * c.camera.size;
                int px = int(std::floor(sx)), py = int(std::floor(sy));
                if (px < 0 || py < 0 || px >= c.camera.size || py >= c.camera.size)
                    continue;
                int i = py * c.camera.size + px;
                int face = c.faces[i];
                bool adjacent =
                    face >= 0 && neighbours[ti].contains(face) && s.tris[face].material == t.material;
                float expected = h.z() / h.w() + dzdx * (px + .5f - sx) + dzdy * (py + .5f - sy);
                // Compare depth at the same pixel centre, rather than at two
                // different positions on a sloped triangle (which made cracks).
                if ((face != ti && !adjacent) || !std::isfinite(expected) ||
                    std::abs(c.z[i] - expected) > (face == ti ? 2e-5f : 2e-4f)) {
                    if (occlusion.blocked(p, ti))
                        continue;
                }
                float angle = QVector3D::dotProduct(qv(normal), parallel ? eye : (eye - qv(p)).normalized());
                const bool gapFill = angle <= options.minCosine;
                if (gapFill) {
                    // Beyond the angle limit, only empty texels accept low-confidence paint: enclosed holes,
                    // or with coverAll any front face, which a later camera-facing view will replace.
                    const bool enclosed = angle > .173648178f && !gaps.empty() && gaps[i];
                    if (sourceAtlas.coverage.constScanLine(y)[x] ||
                        !(enclosed || (options.coverAll && angle > coverAllCosine)))
                        continue;
                }
                float strength = qGray(mask.pixel(px, py)) / 255.f;
                if (strength <= 0)
                    continue;
                // Bilinear filtering must not pull white background or another
                // surface into silhouette / triangle-boundary texels.
                float fx = sx - .5f, fy = sy - .5f, total = 0, edge = 0, coverageBlend = 0, retained = 0,
                      retainedQuality = 0;
                V3 color{};
                int ix = int(std::floor(fx)), iy = int(std::floor(fy));
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        int xx = ix + dx, yy = iy + dy;
                        if (xx < 0 || yy < 0 || xx >= c.camera.size || yy >= c.camera.size)
                            continue;
                        int f = c.faces[yy * c.camera.size + xx];
                        if (f != ti) {
                            float expectedTap =
                                h.z() / h.w() + dzdx * (xx + .5f - sx) + dzdy * (yy + .5f - sy);
                            if (f < 0 || surfaces[f] != surfaces[ti] || s.tris[f].material != t.material ||
                                std::abs(c.z[yy * c.camera.size + xx] - expectedTap) > 2e-5f)
                                continue;
                        }
                        float weight = (dx ? fx - ix : 1 - (fx - ix)) * (dy ? fy - iy : 1 - (fy - iy));
                        color = color + sample(candidate, float(xx), float(yy)) * weight;
                        edge += feather[yy * c.camera.size + xx] * weight;
                        if (!fillOnly)
                            coverageBlend += transition[yy * c.camera.size + xx] * weight;
                        if (!options.retainedMask.isNull()) {
                            const float keep = qGray(options.retainedMask.pixel(xx, yy)) / 255.f;
                            retained += keep * weight;
                            retainedQuality +=
                                keep * weight *
                                (reinterpret_cast<const quint16 *>(c.confidence.constScanLine(yy))[xx] /
                                 65535.f);
                        }
                        total += weight;
                    }
                if (total <= 1e-6f)
                    continue;
                color = color * (1 / total);
                edge /= total;
                if (!fillOnly)
                    reinterpret_cast<quint16 *>(layer.transition.scanLine(y))[x] =
                        quint16(std::clamp(qRound(coverageBlend / total * 65535), 0, 65535));
                layer.color.setPixel(x, y, qRgb(srgb(color.x), srgb(color.y), srgb(color.z)));
                float quality = std::min(gapFill ? .075f : 1.f,
                                         angleQuality(angle, gapFill ? gapOptions : options) * edge *
                                             strength * (gapFill ? .5f : std::max(1.f, options.strength)));
                setConfidence(layer.quality, x, y, quality);
                if (!layer.contentQuality.isNull()) {
                    setConfidence(layer.contentQuality, x, y,
                                  gapFill ? quality
                                          : quality * (1 - retained / total) + retainedQuality / total);
                    layer.retained.scanLine(y)[x] = uchar(std::clamp(qRound(retained / total * 255), 0, 255));
                }
                float influence = strength * (fillOnly ? 1 : edge);
                layer.opacity.scanLine(y)[x] = uchar(std::max(1, qRound(influence * 255)));
            }
        }
    }
    for (auto it = result.atlases.begin(); it != result.atlases.end(); ++it)
        if (materials.contains(it.key())) {
            checkCancel(cancel);
            auto &a = it.value();
            QByteArray strokes;
            QDataStream strokeStream(&strokes, QIODevice::WriteOnly);
            strokeStream.setByteOrder(QDataStream::LittleEndian);
            strokeStream.setFloatingPointPrecision(QDataStream::SinglePrecision);
            const auto &old = before[it.key()];
            const auto &layer = layers[it.key()];
            const int n = a.color.width();
            const int radius = std::max(4, n / 64);
            const size_t count = size_t(n) * n;
            std::vector<int> owner(fillOnly ? count : 0, -1), distance(fillOnly ? count : 0, -1);
            std::deque<int> queue;
            V3 correction{};
            std::array<double, 3> correctionSum{};
            double correctionWeight = 0;
            for (int y = 0; y < n; ++y) {
                checkCancel(cancel);
                for (int x = 0; x < n; ++x)
                    if (layer.opacity.constScanLine(y)[x]) {
                        int i = y * n + x;
                        bool known = old.coverage.constScanLine(y)[x] != 0;
                        if (fillOnly && known) {
                            owner[i] = i;
                            queue.push_back(i);
                            distance[i] = 0;
                        }
                        if (!fillOnly && options.colorMatch && known) {
                            float quality =
                                reinterpret_cast<const quint16 *>(layer.quality.constScanLine(y))[x] /
                                65535.f;
                            float weight = quality * confidence(old, x, y);
                            V3 delta = sample(old.color, float(x), float(y)) -
                                       sample(layer.color, float(x), float(y));
                            // Robust, bounded exposure/color correction from corresponding
                            // overlap texels. Large detail changes cannot recolor a material.
                            correctionSum[0] += std::clamp(delta.x, -.15f, .15f) * weight;
                            correctionSum[1] += std::clamp(delta.y, -.15f, .15f) * weight;
                            correctionSum[2] += std::clamp(delta.z, -.15f, .15f) * weight;
                            correctionWeight += weight;
                        }
                    }
            }
            if (correctionWeight > 1e-6) {
                correction = {float(std::clamp(correctionSum[0] / correctionWeight, -.08, .08)),
                              float(std::clamp(correctionSum[1] / correctionWeight, -.08, .08)),
                              float(std::clamp(correctionSum[2] / correctionWeight, -.08, .08))};
            }
            while (!queue.empty()) {
                int i = queue.front();
                queue.pop_front();
                if (!(i & 8191))
                    checkCancel(cancel);
                if (distance[i] >= radius)
                    continue;
                for (int d : {-n, -1, 1, n}) {
                    int j = i + d;
                    if (j < 0 || j >= int(distance.size()) || (d == -1 && i % n == 0) ||
                        (d == 1 && i % n == n - 1) || distance[j] >= 0 || layer.island[j] < 0 ||
                        layer.island[j] != layer.island[i] || !layer.opacity.constScanLine(j / n)[j % n])
                        continue;
                    if (fillOnly)
                        owner[j] = owner[i];
                    distance[j] = distance[i] + 1;
                    queue.push_back(j);
                }
            }
            for (int y = 0; y < n; ++y) {
                checkCancel(cancel);
                for (int x = 0; x < n; ++x) {
                    float influence = layer.opacity.constScanLine(y)[x] / 255.f;
                    bool known = old.coverage.constScanLine(y)[x] != 0;
                    if (!influence || (fillOnly && known))
                        continue;
                    float incoming =
                        reinterpret_cast<const quint16 *>(layer.quality.constScanLine(y))[x] / 65535.f;
                    float previousQuality = confidence(old, x, y);
                    // Ownership: paint from a view that saw this texel well enough is never repainted, so
                    // independently generated views cannot blend different versions of a detail into ghosts.
                    if (known && options.lockQuality > 0 && previousQuality >= options.lockQuality)
                        continue;
                    V3 color = sample(layer.color, float(x), float(y)) + correction;
                    int i = y * n + x;
                    if (fillOnly && !known && owner[i] >= 0) {
                        int j = owner[i];
                        // Continue the accepted boundary colour while preserving
                        // incoming high-frequency detail. Smoothly fade the linear-
                        // light correction over 32 texels at the default atlas size.
                        V3 delta = sample(old.color, float(j % n), float(j / n)) -
                                   sample(layer.color, float(j % n), float(j / n));
                        float t = std::clamp(1.f - float(distance[i]) / radius, 0.f, 1.f);
                        color = color + delta * (t * t * (3 - 2 * t));
                    }
                    float w = 1;
                    const V3 sourceColor = color;
                    if (known) {
                        w = options.overlay ? incoming : qualityBlend(incoming, previousQuality);
                        w *= std::min(1.f, options.strength);
                        // New coverage has no previous color to blend against. Match it
                        // at the join, then ease back to camera opacity inside old paint.
                        // This field follows visible geometry across UV seams.
                        float boundary =
                            reinterpret_cast<const quint16 *>(layer.transition.constScanLine(y))[x] / 65535.f;
                        w += (1 - w) * boundary;
                        if (!layer.retained.isNull())
                            // Preserve the original atlas texels in trusted cores;
                            // a camera/UV round trip would soften even identical details.
                            w *= 1 - layer.retained.constScanLine(y)[x] / 255.f;
                        if (w <= 1e-6f)
                            continue;
                        V3 previous = sample(old.color, float(x), float(y));
                        color = previous * (1 - w) + color * w;
                    }
                    a.color.setPixel(x, y, qRgb(srgb(color.x), srgb(color.y), srgb(color.z)));
                    a.coverage.scanLine(y)[x] = 255;
                    const float contentQuality =
                        layer.contentQuality.isNull()
                            ? incoming
                            : reinterpret_cast<const quint16 *>(layer.contentQuality.constScanLine(y))[x] /
                                  65535.f;
                    if (options.recordLayer)
                        strokeStream << quint32(i) << sourceColor.x << sourceColor.y << sourceColor.z << w
                                     << contentQuality << (contentQuality * std::min(1.f, options.strength));
                    setConfidence(a.confidence, x, y,
                                  known ? previousQuality * (1 - w) + contentQuality * w
                                        : contentQuality * std::min(1.f, options.strength));
                    ++result.changed;
                }
            }
            pad(*it, std::clamp(padding, 0, 64), cancel);
            if (!strokes.isEmpty())
                recordedMaterials.append(
                    QJsonObject{{"material", it.key()},
                                {"resolution", n},
                                {"strokes", QString::fromLatin1(qCompress(strokes).toBase64())}});
        }
    if (options.recordLayer)
        result.layer = {
            {"version", 1}, {"padding", std::clamp(padding, 0, 64)}, {"materials", recordedMaterials}};
    return result;
}
QJsonObject aiCameraJson(const AiCamera &c) {
    auto matrix = [](const QMatrix4x4 &m) {
        QJsonArray a;
        for (int i = 0; i < 16; ++i)
            a.append(m.constData()[i]);
        return a;
    };
    return {{"view", matrix(c.view)},
            {"projection", matrix(c.projection)},
            {"size", c.size},
            {"yaw", c.orbit.yaw},
            {"pitch", c.orbit.pitch},
            {"distance", c.orbit.distance},
            {"fieldOfView", c.orbit.fieldOfView},
            {"orthographic", c.orbit.orthographic},
            {"pan", QJsonArray{c.orbit.pan.x, c.orbit.pan.y, c.orbit.pan.z}}};
}
AiCamera aiCameraFromJson(const QJsonObject &j) {
    AiCamera c;
    c.size = j["size"].toInt();
    if (c.size < 1 || c.size > 2048)
        fail("Invalid saved camera size");
    auto matrix = [](const QJsonValue &v) {
        auto a = v.toArray();
        if (a.size() != 16)
            fail("Invalid saved camera matrix");
        QMatrix4x4 m;
        for (int i = 0; i < 16; ++i) {
            float f = float(a[i].toDouble(NAN));
            if (!std::isfinite(f))
                fail("Invalid camera value");
            m.data()[i] = f;
        }
        return m;
    };
    c.view = matrix(j["view"]);
    c.projection = matrix(j["projection"]);
    bool invertible = false;
    c.view.inverted(&invertible);
    if (!invertible)
        fail("Invalid saved view matrix");
    c.projection.inverted(&invertible);
    if (!invertible)
        fail("Invalid saved projection matrix");
    c.orbit.yaw = float(j["yaw"].toDouble());
    c.orbit.pitch = float(j["pitch"].toDouble());
    c.orbit.distance = float(j["distance"].toDouble());
    c.orbit.orthographic = orthographic(c.projection);
    c.orbit.fieldOfView =
        float(j["fieldOfView"].toDouble(c.orbit.orthographic
                                            ? 20 // Saved orthographic cameras always record their lens.
                                            : 2 * std::atan(1. / c.projection(1, 1)) * 180 / 3.14159265359));
    if (!std::isfinite(c.orbit.yaw) || !std::isfinite(c.orbit.pitch) || !std::isfinite(c.orbit.distance) ||
        c.orbit.distance <= 0 || !std::isfinite(c.orbit.fieldOfView) || c.orbit.fieldOfView < 5 ||
        c.orbit.fieldOfView > 90)
        fail("Invalid saved orbit camera");
    auto p = j["pan"].toArray();
    if (p.size() == 3)
        c.orbit.pan = {float(p[0].toDouble()), float(p[1].toDouble()), float(p[2].toDouble())};
    return c;
}
QJsonArray aiSerializeAtlases(const AiAtlases &atlases) {
    QJsonArray images;
    for (auto it = atlases.begin(); it != atlases.end(); ++it)
        images.append(QJsonObject{
            {"material", it.key()},
            {"color", QString::fromLatin1(png(it->color))},
            {"coverage", QString::fromLatin1(png(it->coverage))},
            {"confidence",
             it->confidence.isNull() ? QJsonValue() : QJsonValue(QString::fromLatin1(png(it->confidence)))}});
    return images;
}
AiAtlases aiDeserializeAtlases(const QJsonArray &images) {
    AiAtlases loaded;
    for (const auto &v : images) {
        auto j = v.toObject();
        AiAtlas a{unpng(j["color"]).convertToFormat(QImage::Format_RGB32),
                  unpng(j["coverage"]).convertToFormat(QImage::Format_Grayscale8)};
        if (j["confidence"].isString())
            a.confidence = unpng(j["confidence"]).convertToFormat(QImage::Format_Grayscale16);
        if (a.color.size() != a.coverage.size() || a.color.width() != a.color.height() ||
            a.color.width() > 4096 || (!a.confidence.isNull() && a.confidence.size() != a.color.size()))
            fail("Invalid atlas size");
        loaded[j["material"].toString()] = a;
    }
    return loaded;
}
AiAtlases aiComposeLayers(const AiAtlases &base, const QList<QJsonObject> &records, std::atomic_bool *cancel,
                          QCache<QByteArray, QByteArray> *decoded) {
    AiAtlases result = base;
    QMap<QString, int> padding;
    QSet<QString> touched;
    for (const auto &record : records)
        if (record["visible"].toBool(true))
            for (const auto &value : record["uvLayer"].toObject()["materials"].toArray()) {
                const auto material = value.toObject()["material"].toString();
                const int radius = std::clamp(record["uvLayer"].toObject()["padding"].toInt(8), 0, 64);
                if (!padding.contains(material))
                    padding[material] = radius;
                else if (padding[material] != radius)
                    padding[material] = -1;
            }
    for (const auto &record : records) {
        checkCancel(cancel);
        if (!record["visible"].toBool(true))
            continue;
        const auto layer = record["uvLayer"].toObject();
        if (layer["version"].toInt() != 1)
            fail("Projection layer needs reconstruction");
        for (const auto &value : layer["materials"].toArray()) {
            const auto block = value.toObject();
            const auto material = block["material"].toString();
            const int n = block["resolution"].toInt();
            if (!result.contains(material) || n != result[material].color.width())
                fail("Projection layer does not match the atlas");
            const auto encoded = block["strokes"].toString().toLatin1();
            const auto cacheKey =
                QCryptographicHash::hash(encoded, QCryptographicHash::Sha256) + QByteArray::number(n);
            const auto cached = decoded ? decoded->object(cacheKey) : nullptr;
            const auto compressed = cached ? QByteArray() : QByteArray::fromBase64(encoded);
            constexpr int stride = 28; // UV texel index, linear RGB, opacity, two confidence values.
            if (!cached && (compressed.size() < 4 ||
                            qFromBigEndian<quint32>(compressed.constData()) > quint64(n) * n * stride))
                fail("Invalid projection layer size");
            const QByteArray strokes = cached ? *cached : qUncompress(compressed);
            if (strokes.isEmpty() || strokes.size() % stride)
                fail("Invalid projection layer data");
            QDataStream stream(strokes);
            stream.setByteOrder(QDataStream::LittleEndian);
            stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
            auto &a = result[material];
            if (a.confidence.isNull()) {
                a.confidence = QImage(a.color.size(), QImage::Format_Grayscale16);
                a.confidence.fill(0);
                for (int y = 0; y < n; ++y)
                    for (int x = 0; x < n; ++x)
                        if (a.coverage.constScanLine(y)[x])
                            setConfidence(a.confidence, x, y, .5f);
            }
            a.color.detach();
            a.coverage.detach();
            a.confidence.detach();
            int processed = 0;
            while (!stream.atEnd()) {
                quint32 index;
                V3 color;
                float weight, quality, firstQuality;
                stream >> index >> color.x >> color.y >> color.z >> weight >> quality >> firstQuality;
                if (stream.status() != QDataStream::Ok || index >= quint64(n) * n ||
                    !std::isfinite(color.x) || !std::isfinite(color.y) || !std::isfinite(color.z) ||
                    !std::isfinite(weight) || weight <= 0 || weight > 1 || !std::isfinite(quality) ||
                    quality < 0 || quality > 1 || !std::isfinite(firstQuality) || firstQuality < 0 ||
                    firstQuality > 1)
                    fail("Invalid projection layer paint");
                if (!(processed++ & 4095))
                    checkCancel(cancel);
                const int x = index % n, y = index / n;
                const bool known = a.coverage.constScanLine(y)[x] != 0;
                float confidenceValue = firstQuality;
                if (known) {
                    color = sample(a.color, float(x), float(y)) * (1 - weight) + color * weight;
                    confidenceValue = confidence(a, x, y) * (1 - weight) + quality * weight;
                }
                a.color.setPixel(x, y, qRgb(srgb(color.x), srgb(color.y), srgb(color.z)));
                a.coverage.scanLine(y)[x] = 255;
                setConfidence(a.confidence, x, y, confidenceValue);
            }
            if (decoded && !cached)
                decoded->insert(cacheKey, new QByteArray(strokes), int((strokes.size() + 1023) / 1024));
            touched.insert(material);
            if (padding.value(material) < 0)
                pad(a, std::clamp(layer["padding"].toInt(8), 0, 64), cancel);
        }
    }
    // Padding touches only uncovered texels; it cannot affect later paint blends.
    // With the normal constant radius, one final pass is exactly equivalent.
    for (const auto &material : touched)
        if (padding.value(material) >= 0)
            pad(result[material], padding[material], cancel);
    return result;
}
QList<QJsonObject> aiPrepareLayers(const Scene &scene, int uv, const AiAtlases &base,
                                   const QList<QJsonObject> &records, std::atomic_bool *cancel) {
    AiAtlases composite = base;
    QList<QJsonObject> result;
    for (auto record : records) {
        checkCancel(cancel);
        if (record["uvLayer"].toObject()["version"].toInt() != 1) {
            auto capture =
                aiCapture(scene, uv, composite, aiCameraFromJson(record["camera"].toObject()), cancel);
            const auto blend = record["blending"].toObject();
            AiProjectionOptions options;
            options.overlay = blend["mode"].toString() == "overlay";
            options.lockQuality = blend["mode"].toString() == "lock" ? aiDefaultLockQuality : 0;
            options.strength = float(blend["strength"].toDouble(1));
            options.feather = float(blend["feather"].toDouble(.06));
            options.anglePower = float(blend["anglePower"].toDouble(2));
            options.minCosine = float(blend["minCosine"].toDouble(.15));
            options.fillGaps = blend["fillGaps"].toBool(false);
            options.coverAll = blend["coverAll"].toBool(false);
            options.colorMatch = blend["colorMatch"].toBool(false);
            if (!record["retentionMask"].toString().isEmpty()) {
                options.retainedMask =
                    unpng(record["retentionMask"]).convertToFormat(QImage::Format_Grayscale8);
                capture.confidence =
                    unpng(record["feedbackConfidenceImage"]).convertToFormat(QImage::Format_Grayscale16);
            }
            options.recordLayer = true;
            QSet<QString> materials;
            for (const auto &m : record["materials"].toArray())
                materials.insert(m.toString());
            auto projected =
                aiProject(scene, uv, composite, capture, unpng(record["candidate"]), unpng(record["mask"]),
                          materials, record["fillOnly"].toBool(), 8, cancel, options);
            record["uvLayer"] = projected.layer;
            composite = projected.atlases;
        } else
            composite = aiComposeLayers(composite, {record}, cancel);
        result << record;
    }
    return result;
}
void aiSaveProject(const QString &path, const QJsonObject &settings, const AiAtlases &atlases,
                   const QList<QJsonObject> &projections) {
    QJsonArray images = aiSerializeAtlases(atlases), history;
    for (const auto &p : projections)
        history.append(p);
    QJsonObject root{{"format", "FornaioAI"},
                     {"version", 1},
                     {"settings", settings},
                     {"atlases", images},
                     {"projections", history}};
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        fail(f.errorString());
    QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (f.write(bytes) != bytes.size() || !f.commit())
        fail(f.errorString());
}
QJsonObject aiLoadProject(const QString &path, AiAtlases &atlases, QList<QJsonObject> &projections) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        fail(f.errorString());
    QJsonParseError error;
    auto doc = QJsonDocument::fromJson(f.readAll(), &error);
    auto root = doc.object();
    if (error.error != QJsonParseError::NoError || root["format"] != "FornaioAI" || root["version"] != 1)
        fail("Unsupported AI project");
    AiAtlases loaded;
    QList<QJsonObject> history;
    loaded = aiDeserializeAtlases(root["atlases"].toArray());
    for (const auto &v : root["projections"].toArray())
        history << v.toObject();
    atlases = loaded;
    projections = history;
    return root["settings"].toObject();
}

namespace {
// Square min/max filters (separable) for the binary lock and clay masks.
std::vector<uchar> morph(const std::vector<uchar> &in, int w, int h, int radius, bool dilate) {
    std::vector<uchar> rows(in.size()), out(in.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uchar v = dilate ? 0 : 255;
            for (int k = std::max(0, x - radius); k <= std::min(w - 1, x + radius); ++k)
                v = dilate ? std::max(v, in[size_t(y) * w + k]) : std::min(v, in[size_t(y) * w + k]);
            rows[size_t(y) * w + x] = v;
        }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uchar v = dilate ? 0 : 255;
            for (int k = std::max(0, y - radius); k <= std::min(h - 1, y + radius); ++k)
                v = dilate ? std::max(v, rows[size_t(k) * w + x]) : std::min(v, rows[size_t(k) * w + x]);
            out[size_t(y) * w + x] = v;
        }
    return out;
}
// Approximate Gaussian (three running-sum box passes, O(1) per pixel) of a float field.
std::vector<float> soften(std::vector<float> v, int w, int h, int radius) {
    if (radius < 1)
        return v;
    std::vector<double> prefix(size_t(std::max(w, h)) + 1);
    auto pass = [&](int count, int length, auto index) {
        for (int line = 0; line < count; ++line) {
            prefix[0] = 0;
            for (int i = 0; i < length; ++i)
                prefix[i + 1] = prefix[i] + v[index(line, i)];
            for (int i = 0; i < length; ++i) {
                const int a = std::max(0, i - radius), b = std::min(length - 1, i + radius);
                v[index(line, i)] = float((prefix[b + 1] - prefix[a]) / (b - a + 1));
            }
        }
    };
    for (int k = 0; k < 3; ++k) {
        pass(h, w, [w](int y, int x) { return size_t(y) * w + x; });
        pass(w, h, [w](int x, int y) { return size_t(y) * w + x; });
    }
    return v;
}
bool onMesh(const AiCapture &c, int x, int y) {
    return qGray(c.silhouette.pixel(x, y)) > 127;
}
// Width of the band inside locked paint where a new view crossfades into it.
int lockBand(int size) {
    return std::max(4, size / 32);
}
} // namespace

AiLock aiLockedGuide(const AiCapture &c, float lockQuality) {
    const int w = c.camera.size, h = c.camera.size;
    if (c.normal.size() != QSize(w, h) || c.partial.size() != QSize(w, h) || c.known.size() != QSize(w, h) ||
        c.confidence.size() != QSize(w, h) || c.confidence.format() != QImage::Format_Grayscale16)
        fail("Invalid capture for the locked guide");
    std::vector<uchar> locked(size_t(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        const auto quality = reinterpret_cast<const quint16 *>(c.confidence.constScanLine(y));
        for (int x = 0; x < w; ++x)
            if (onMesh(c, x, y) && qGray(c.known.pixel(x, y)) > 127 && quality[x] / 65535.f >= lockQuality)
                locked[size_t(y) * w + x] = 255;
    }
    // A two-pixel erosion keeps silhouette and UV-seam texels (half-painted at best) out of the lock.
    locked = morph(locked, w, h, 2, false);
    // Distance (chamfer) from unlocked mesh pixels: the outer band of the lock crossfades with new paint.
    // The silhouette is not an edge to blend across, so off-mesh pixels are not sources.
    constexpr float far = 1e9f;
    std::vector<float> distance(size_t(w) * h, far);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (onMesh(c, x, y) && !locked[size_t(y) * w + x])
                distance[size_t(y) * w + x] = 0;
    auto relax = [&](int x, int y, int dx, int dy, float cost) {
        const int nx = x + dx, ny = y + dy;
        if (nx >= 0 && ny >= 0 && nx < w && ny < h)
            distance[size_t(y) * w + x] =
                std::min(distance[size_t(y) * w + x], distance[size_t(ny) * w + nx] + cost);
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            relax(x, y, -1, 0, 1);
            relax(x, y, 0, -1, 1);
            relax(x, y, -1, -1, 1.4142f);
            relax(x, y, 1, -1, 1.4142f);
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x) {
            relax(x, y, 1, 0, 1);
            relax(x, y, 0, 1, 1);
            relax(x, y, 1, 1, 1.4142f);
            relax(x, y, -1, 1, 1.4142f);
        }
    const float band = float(lockBand(w));
    AiLock out;
    out.guide = QImage(w, h, QImage::Format_RGB32);
    out.locked = QImage(w, h, QImage::Format_Grayscale8);
    out.keep = QImage(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const bool lock = locked[size_t(y) * w + x];
            out.locked.scanLine(y)[x] = lock ? 255 : 0;
            out.keep.scanLine(y)[x] = lock ? uchar(qRound(255 * smooth(distance[size_t(y) * w + x] / band))) : 0;
            out.any |= lock;
            if (!onMesh(c, x, y)) {
                out.guide.setPixel(x, y, qRgb(255, 255, 255));
                continue;
            }
            if (lock) {
                out.guide.setPixel(x, y, c.partial.pixel(x, y));
                continue;
            }
            // Camera-lit mid-gray clay, never near white (the same guide as the local worker's clay).
            const QRgb n = c.normal.pixel(x, y);
            QVector3D v(qRed(n) / 127.5f - 1, qGreen(n) / 127.5f - 1, qBlue(n) / 127.5f - 1);
            const float shade = .25f + .35f * std::clamp(v.normalized().z(), 0.f, 1.f);
            const int g = qRound(shade * 255);
            out.guide.setPixel(x, y, qRgb(g, g, g));
        }
    return out;
}
QImage aiMatchLockedTone(const AiCapture &c, const AiLock &lock, const QImage &input) {
    const int w = c.camera.size, h = c.camera.size;
    QImage candidate = input.convertToFormat(QImage::Format_RGB32);
    if (candidate.size() != QSize(w, h) || lock.locked.size() != QSize(w, h))
        fail("Generated image does not match the capture");
    // The model redraws the locked pixels too; comparing its version with the accepted paint measures this
    // view's lighting and colour shift. Gains are measured in linear light, in the log domain, per channel.
    const size_t n = size_t(w) * h;
    std::vector<float> weight(n, 0);
    std::array<std::vector<float>, 3> ratio;
    for (auto &r : ratio)
        r.assign(n, 0);
    std::array<double, 3> total{};
    size_t samples = 0, mesh = 0;
    constexpr float limit = .47f; // ln(1.6): a larger difference is a different detail, not a tone change.
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!onMesh(c, x, y))
                continue;
            ++mesh;
            const QRgb generated = candidate.pixel(x, y);
            if (!lock.locked.constScanLine(y)[x] ||
                (qRed(generated) >= 245 && qGreen(generated) >= 245 && qBlue(generated) >= 245))
                continue;
            const V3 a = decode(c.partial.pixel(x, y)), g = decode(generated);
            const float accepted[] = {a.x, a.y, a.z}, redrawn[] = {g.x, g.y, g.z};
            const size_t i = size_t(y) * w + x;
            weight[i] = 1;
            for (int k = 0; k < 3; ++k) {
                ratio[k][i] = std::clamp(std::log((accepted[k] + .02f) / (redrawn[k] + .02f)), -limit, limit);
                total[k] += ratio[k][i];
            }
            ++samples;
        }
    if (!mesh || samples < mesh / 100)
        return candidate;
    const int radius = std::max(4, w / 64); // Three passes: an effective blur of about w/24.
    const auto coverage = soften(weight, w, h, radius);
    std::array<std::vector<float>, 3> local;
    for (int k = 0; k < 3; ++k)
        local[k] = soften(ratio[k], w, h, radius);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!onMesh(c, x, y))
                continue;
            const size_t i = size_t(y) * w + x;
            // Near locked paint follow its local shift; farther away, this view's average shift.
            const float t = smooth((coverage[i] - .02f) / .23f);
            const V3 g = decode(candidate.pixel(x, y));
            float values[] = {g.x, g.y, g.z};
            for (int k = 0; k < 3; ++k) {
                const float measured = coverage[i] > 1e-4f ? local[k][i] / coverage[i] : 0.f;
                const float shift = t * measured + (1 - t) * float(total[k] / samples);
                values[k] = (values[k] + .02f) * std::exp(std::clamp(shift, -limit, limit)) - .02f;
            }
            candidate.setPixel(x, y, qRgb(srgb(values[0]), srgb(values[1]), srgb(values[2])));
        }
    return candidate;
}
QImage aiComposeLocked(const AiCapture &c, const AiLock &lock, const QImage &input) {
    const int w = c.camera.size, h = c.camera.size;
    QImage candidate = input.convertToFormat(QImage::Format_RGB32);
    if (candidate.size() != QSize(w, h) || lock.keep.size() != QSize(w, h))
        fail("Generated image does not match the capture");
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float keep = lock.keep.constScanLine(y)[x] / 255.f;
            if (keep <= 0 || !onMesh(c, x, y))
                continue;
            // Crossfade in linear light: new paint at the lock edge, accepted paint deeper inside.
            const V3 o = decode(c.partial.pixel(x, y)), n = decode(candidate.pixel(x, y));
            candidate.setPixel(x, y,
                               qRgb(srgb(o.x * keep + n.x * (1 - keep)), srgb(o.y * keep + n.y * (1 - keep)),
                                    srgb(o.z * keep + n.z * (1 - keep))));
        }
    return candidate;
}
QImage aiGeneratedPaintMask(const AiCapture &c, const AiLock &lock, const QImage &input) {
    const int w = c.camera.size, h = c.camera.size;
    const QImage candidate = input.convertToFormat(QImage::Format_RGB32);
    if (candidate.size() != QSize(w, h) || lock.guide.size() != QSize(w, h) || lock.keep.size() != QSize(w, h))
        fail("Generated image does not match the capture");
    std::vector<uchar> clay(size_t(w) * h, 0);
    QImage mask(w, h, QImage::Format_RGB32);
    mask.fill(Qt::black);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            if (!onMesh(c, x, y))
                continue;
            if (lock.locked.constScanLine(y)[x]) {
                // The lock's outer band receives the crossfaded image with a ramped strength; its core is kept.
                const int strength = 255 - lock.keep.constScanLine(y)[x];
                mask.setPixel(x, y, qRgb(strength, strength, strength));
                continue;
            }
            const QRgb g = lock.guide.pixel(x, y), o = candidate.pixel(x, y);
            const bool guideClay =
                std::max({qRed(g), qGreen(g), qBlue(g)}) - std::min({qRed(g), qGreen(g), qBlue(g)}) < 2;
            // Background white left on bare clay is never projected as paint.
            if (guideClay && qRed(o) >= 245 && qGreen(o) >= 245 && qBlue(o) >= 245)
                continue;
            const int chroma = std::max({qRed(o), qGreen(o), qBlue(o)}) - std::min({qRed(o), qGreen(o), qBlue(o)});
            const int difference =
                (std::abs(qRed(o) - qRed(g)) + std::abs(qGreen(o) - qGreen(g)) + std::abs(qBlue(o) - qBlue(g))) / 3;
            if (guideClay && chroma < 12 && difference < 12)
                clay[size_t(y) * w + x] = 255;
            mask.setPixel(x, y, qRgb(255, 255, 255));
        }
    // Only large bare-clay regions count: grey paint (steel, graphite) has scattered clay-like pixels, and
    // holes would lower projection confidence everywhere (it feathers at every mask edge).
    clay = morph(morph(clay, w, h, 4, false), w, h, 4, true);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            if (clay[size_t(y) * w + x])
                mask.setPixel(x, y, qRgb(0, 0, 0));
    return mask;
}

namespace {
// Running-sum mean over a (2r+1)^2 window, clamped at the borders. O(1) per pixel.
void boxMean(const std::vector<float> &in, std::vector<float> &out, std::vector<float> &tmp, int w, int h, int r) {
    for (int y = 0; y < h; ++y) {
        const float *row = &in[size_t(y) * w];
        float sum = 0;
        for (int x = 0; x <= std::min(r, w - 1); ++x)
            sum += row[x];
        for (int x = 0; x < w; ++x) {
            const int a = x - r, b = x + r;
            tmp[size_t(y) * w + x] = sum / float(std::min(b, w - 1) - std::max(a, 0) + 1);
            if (b + 1 < w)
                sum += row[b + 1];
            if (a >= 0)
                sum -= row[a];
        }
    }
    for (int x = 0; x < w; ++x) {
        float sum = 0;
        for (int y = 0; y <= std::min(r, h - 1); ++y)
            sum += tmp[size_t(y) * w + x];
        for (int y = 0; y < h; ++y) {
            const int a = y - r, b = y + r;
            out[size_t(y) * w + x] = sum / float(std::min(b, h - 1) - std::max(a, 0) + 1);
            if (b + 1 < h)
                sum += tmp[size_t(b + 1) * w + x];
            if (a >= 0)
                sum -= tmp[size_t(a) * w + x];
        }
    }
}
std::vector<float> gradientMagnitude(const std::vector<float> &v, int w, int h) {
    std::vector<float> out(v.size());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float gx = v[size_t(y) * w + std::min(x + 1, w - 1)] - v[size_t(y) * w + std::max(x - 1, 0)];
            const float gy = v[size_t(std::min(y + 1, h - 1)) * w + x] - v[size_t(std::max(y - 1, 0)) * w + x];
            out[size_t(y) * w + x] = .5f * std::hypot(gx, gy);
        }
    return out;
}
float bilinear(const std::vector<float> &v, int w, int h, float x, float y) {
    x = std::clamp(x, 0.f, float(w - 1));
    y = std::clamp(y, 0.f, float(h - 1));
    const int x0 = int(x), y0 = int(y), x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
    const float fx = x - x0, fy = y - y0;
    return (v[size_t(y0) * w + x0] * (1 - fx) + v[size_t(y0) * w + x1] * fx) * (1 - fy) +
           (v[size_t(y1) * w + x0] * (1 - fx) + v[size_t(y1) * w + x1] * fx) * fy;
}
} // namespace

QImage aiAlignToGeometry(const AiCapture &c, const QImage &input, double *meanShift) {
    const int s = c.camera.size;
    const QImage generated = input.convertToFormat(QImage::Format_RGB32);
    if (generated.size() != QSize(s, s) || c.normal.size() != QSize(s, s) || c.silhouette.size() != QSize(s, s))
        fail("Generated image does not match the capture");
    // Half resolution keeps the shift search fast; the field is smooth, so nothing is lost.
    const int f = s >= 768 ? 2 : 1, m = s / f;
    const size_t n = size_t(m) * m;
    std::vector<float> geometry(n), painted(n);
    std::vector<uchar> mesh(n);
    for (int y = 0; y < m; ++y)
        for (int x = 0; x < m; ++x) {
            float shade = 0, lum = 0;
            for (int dy = 0; dy < f; ++dy)
                for (int dx = 0; dx < f; ++dx) {
                    const int px = x * f + dx, py = y * f + dy;
                    const QRgb g = generated.pixel(px, py);
                    lum += (.299f * qRed(g) + .587f * qGreen(g) + .114f * qBlue(g)) / 255;
                    if (!onMesh(c, px, py)) {
                        shade += 1; // White background, like the generated image's.
                        continue;
                    }
                    const QRgb nn = c.normal.pixel(px, py);
                    QVector3D v(qRed(nn) / 127.5f - 1, qGreen(nn) / 127.5f - 1, qBlue(nn) / 127.5f - 1);
                    shade += std::pow(std::clamp(v.normalized().z(), 0.f, 1.f), 1.5f);
                }
            geometry[size_t(y) * m + x] = shade / (f * f);
            painted[size_t(y) * m + x] = lum / (f * f);
            mesh[size_t(y) * m + x] = onMesh(c, x * f + f / 2, y * f + f / 2);
        }
    // Edge strength: creases, slits, nostrils, the lip line and the silhouette in the geometry; painted edges in
    // the generated image. Both are lightly smoothed so single-pixel texture does not dominate.
    const auto G = gradientMagnitude(soften(geometry, m, m, 1), m, m);
    const auto P = gradientMagnitude(soften(painted, m, m, 1), m, m);
    const int window = std::max(3, m * 10 / 512), radius = std::max(3, m * 10 / 512);
    std::vector<float> tmp(n), gm(n), gv(n), work(n), qm(n), qq(n), gq(n), q(n);
    boxMean(G, gm, tmp, m, m, window);
    for (size_t i = 0; i < n; ++i)
        work[i] = G[i] * G[i];
    boxMean(work, gv, tmp, m, m, window);
    for (size_t i = 0; i < n; ++i)
        gv[i] = std::max(0.f, gv[i] - gm[i] * gm[i]);
    std::vector<float> best(n, -2), dyBest(n, 0), dxBest(n, 0);
    for (int sy = -radius; sy <= radius; ++sy)
        for (int sx = -radius; sx <= radius; ++sx) {
            if (sy * sy + sx * sx > radius * radius)
                continue;
            for (int y = 0; y < m; ++y) {
                const int yy = std::clamp(y + sy, 0, m - 1);
                for (int x = 0; x < m; ++x)
                    q[size_t(y) * m + x] = P[size_t(yy) * m + std::clamp(x + sx, 0, m - 1)];
            }
            boxMean(q, qm, tmp, m, m, window);
            for (size_t i = 0; i < n; ++i)
                work[i] = q[i] * q[i];
            boxMean(work, qq, tmp, m, m, window);
            for (size_t i = 0; i < n; ++i)
                work[i] = G[i] * q[i];
            boxMean(work, gq, tmp, m, m, window);
            // A slight preference for small shifts keeps flat, ambiguous regions still.
            const float penalty = .004f * std::hypot(float(sx), float(sy)) * f;
            for (size_t i = 0; i < n; ++i) {
                const float variance = std::max(0.f, qq[i] - qm[i] * qm[i]);
                const float ncc = (gq[i] - gm[i] * qm[i]) / std::sqrt(std::max(gv[i] * variance, 1e-12f)) - penalty;
                if (ncc > best[i]) {
                    best[i] = ncc;
                    dyBest[i] = float(sy);
                    dxBest[i] = float(sx);
                }
            }
        }
    // Trust a clear correlation peak where the geometry has structure; elsewhere the field stays near zero.
    std::vector<float> energies;
    for (size_t i = 0; i < n; ++i)
        if (mesh[i])
            energies.push_back(std::sqrt(gv[i]));
    if (energies.empty())
        return generated;
    auto q98 = energies.begin() + std::ptrdiff_t(energies.size() * 98 / 100);
    std::nth_element(energies.begin(), q98, energies.end());
    const float strong = std::max(*q98, 1e-6f);
    std::vector<float> trust(n), ty(n), tx(n);
    for (size_t i = 0; i < n; ++i) {
        const float t = best[i] > .35f ? best[i] * best[i] * std::clamp(std::sqrt(gv[i]) / strong, 0.f, 1.f) : 0.f;
        trust[i] = t;
        ty[i] = t * dyBest[i];
        tx[i] = t * dxBest[i];
    }
    const int smoothing = std::max(2, m * 8 / 512);
    const auto weight = soften(trust, m, m, smoothing);
    const auto fieldY = soften(ty, m, m, smoothing), fieldX = soften(tx, m, m, smoothing);
    std::vector<float> fy(n), fx(n);
    for (size_t i = 0; i < n; ++i) {
        fy[i] = std::clamp(fieldY[i] / (weight[i] + .02f), -float(radius), float(radius));
        fx[i] = std::clamp(fieldX[i] / (weight[i] + .02f), -float(radius), float(radius));
    }
    // Pull each output pixel from where the generation drew it.
    QImage out(s, s, QImage::Format_RGB32);
    double total = 0;
    size_t counted = 0;
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            const float wx = (x + .5f) / f - .5f, wy = (y + .5f) / f - .5f;
            const float dx = bilinear(fx, m, m, wx, wy) * f, dy = bilinear(fy, m, m, wx, wy) * f;
            const V3 color = sample(generated, std::clamp(x + dx, 0.f, float(s - 1)), std::clamp(y + dy, 0.f, float(s - 1)));
            out.setPixel(x, y, qRgb(srgb(color.x), srgb(color.y), srgb(color.z)));
            if (onMesh(c, x, y)) {
                total += std::hypot(dx, dy);
                ++counted;
            }
        }
    if (meanShift)
        *meanShift = counted ? total / counted : 0;
    return out;
}
