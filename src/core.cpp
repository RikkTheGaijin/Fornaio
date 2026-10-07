#include "core.h"
#include <QElapsedTimer>
#include <QFile>
#include <QImageReader>
#include <QSaveFile>
#include <QtEndian>
#include <algorithm>
#include <map>
#include <memory>
#include <numeric>
#include <set>
#include <stdexcept>
#include <ufbx.h>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_TGA
#include <stb_image.h>
static void fail(const QString &s) {
    throw std::runtime_error(s.toStdString());
}
static V3 vec(ufbx_vec3 a) {
    return {(float)a.x, (float)a.y, (float)a.z};
}
Scene loadScene(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        fail(f.errorString());
    auto bytes = f.readAll();
    ufbx_load_opts opts = {};
    opts.generate_missing_normals = true;
    auto name = path.toUtf8();
    opts.filename = {name.constData(), (size_t)name.size()};
    ufbx_error err = {};
    ufbx_scene *raw = ufbx_load_memory(bytes.data(), bytes.size(), &opts, &err);
    if (!raw) {
        char message[2048];
        ufbx_format_error(message, sizeof(message), &err);
        fail(QString::fromUtf8(message));
    }
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> doc(raw, ufbx_free_scene);
    Scene s;
    s.source = path;
    V3 lo{INFINITY, INFINITY, INFINITY}, hi{-INFINITY, -INFINITY, -INFINITY};
    size_t sets = 0;
    for (auto *node : raw->nodes)
        if (node->mesh)
            sets = std::max(sets, node->mesh->uv_sets.count);
    for (size_t i = 0; i < sets; i++)
        s.uvNames << QString("UV%1").arg(i + 1);
    auto indexOf = [](QStringList &list, const QString &name) {
        int i = list.indexOf(name);
        if (i < 0) {
            list << name;
            i = (int)list.size() - 1;
        }
        return i;
    };
    for (auto *node : raw->nodes) {
        auto *m = node->mesh;
        if (!m)
            continue;
        QString objectName = QString::fromUtf8(node->name.data, (int)node->name.length);
        if (objectName.isEmpty())
            objectName = QString("mesh %1").arg(s.objects.size() + 1);
        int object = indexOf(s.objects, objectName);
        std::vector<int> materials;
        size_t materialCount = std::max(node->materials.count, m->materials.count);
        for (size_t j = 0; j < materialCount; j++) {
            const ufbx_material *mat = j < node->materials.count ? node->materials[j] : m->materials[j];
            QString n = mat ? QString::fromUtf8(mat->name.data, (int)mat->name.length) : QString();
            materials.push_back(indexOf(s.materials, n.isEmpty() ? "(no material)" : n));
        }
        auto normalMatrix = ufbx_matrix_for_normals(&node->geometry_to_world);
        for (size_t j = 0; j < m->uv_sets.count; j++)
            if (m->uv_sets[j].name.length)
                s.uvNames[(int)j] =
                    QString("UV%1 · %2")
                        .arg(j + 1)
                        .arg(QString::fromUtf8(m->uv_sets[j].name.data, (int)m->uv_sets[j].name.length));
        std::vector<uint32_t> ix(m->max_face_triangles * 3);
        for (size_t faceIndex = 0; faceIndex < m->faces.count; ++faceIndex) {
            auto face = m->faces[faceIndex];
            uint32_t num = ufbx_triangulate_face(ix.data(), ix.size(), m, face);
            uint32_t faceMaterial = faceIndex < m->face_material.count ? m->face_material[faceIndex] : 0;
            int material = faceMaterial < materials.size() ? materials[faceMaterial]
                                                            : indexOf(s.materials, "(no material)");
            for (uint32_t t = 0; t < num; t++) {
                Triangle tr;
                tr.sourceMesh = objectName;
                tr.sourcePolygon = (int)faceIndex;
                tr.material = material;
                tr.object = object;
                tr.uv.resize(sets);
                for (auto &uv : tr.uv)
                    for (auto &p : uv)
                        p = {NAN, NAN};
                for (int k = 0; k < 3; k++) {
                    auto i = ix[t * 3 + k];
                    tr.p[k] = vec(ufbx_transform_position(&node->geometry_to_world,
                                                          ufbx_get_vertex_vec3(&m->vertex_position, i)));
                    tr.n[k] = unit(vec(
                        ufbx_transform_direction(&normalMatrix, ufbx_get_vertex_vec3(&m->vertex_normal, i))));
                    for (size_t j = 0; j < m->uv_sets.count; j++) {
                        auto a = ufbx_get_vertex_vec2(&m->uv_sets[j].vertex_uv, i);
                        tr.uv[j][k] = {(float)a.x, (float)a.y};
                    }
                    auto p = tr.p[k];
                    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                }
                if (dot(cross(tr.p[1] - tr.p[0], tr.p[2] - tr.p[0]), tr.n[0] + tr.n[1] + tr.n[2]) < 0) {
                    std::swap(tr.p[1], tr.p[2]);
                    std::swap(tr.n[1], tr.n[2]);
                    for (auto &uv : tr.uv)
                        std::swap(uv[1], uv[2]);
                }
                s.tris.push_back(std::move(tr));
            }
        }
    }
    if (s.tris.empty())
        fail("The file has no triangle geometry.");
    s.center = (hi + lo) * .5f;
    s.scale = sqrtf(dot(hi - lo, hi - lo));
    if (!std::isfinite(s.scale) || s.scale <= 0)
        fail("Invalid or zero-size geometry.");
    return s;
}
Mask maskFromImage(const QImage &image, int channel, bool invert) {
    if (image.isNull())
        fail("The mask image is empty.");
    Mask m;
    m.alpha = image.hasAlphaChannel();
    m.image = image;
    m.w = image.width();
    m.h = image.height();
    m.values.resize((size_t)m.w * m.h);
    if (channel < 0)
        channel = m.alpha ? 3 : 0;
    if (channel > 3)
        fail("Invalid mask channel.");
    auto rgba = image.convertToFormat(QImage::Format_RGBA64);
    for (int y = 0; y < m.h; y++) {
        auto row = reinterpret_cast<const QRgba64 *>(rgba.constScanLine(y));
        for (int x = 0; x < m.w; x++) {
            auto c = row[x];
            float v = (channel == 0   ? c.red()
                       : channel == 1 ? c.green()
                       : channel == 2 ? c.blue()
                                      : c.alpha()) /
                      65535.f;
            m.values[(size_t)y * m.w + x] = invert ? 1 - v : v;
        }
    }
    return m;
}
Mask loadMask(const QString &path, int channel, bool invert) {
    QImageReader reader(path);
    reader.setAutoTransform(false);
    QImage image = reader.read();
    if (image.isNull() && path.endsWith(".tga", Qt::CaseInsensitive)) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            auto bytes = f.readAll();
            int w, h, c;
            auto data =
                stbi_load_from_memory((const stbi_uc *)bytes.data(), (int)bytes.size(), &w, &h, &c, 4);
            if (data) {
                image = QImage(data, w, h, w * 4, QImage::Format_RGBA8888).copy();
                if (c < 4)
                    image = image.convertToFormat(QImage::Format_RGB888);
                stbi_image_free(data);
            }
        }
    }
    if (image.isNull())
        fail("Cannot read mask: " + reader.errorString());
    return maskFromImage(image, channel, invert);
}
void checkSettings(const Settings &s) {
    if (s.resolution < 1 || s.resolution > 8192 || s.rays < 1 || s.rays > 65536 || s.ss < 1 || s.ss > 4 ||
        s.padding < 0 || s.padding > 128 || !std::isfinite(s.distance) || s.distance < 0 ||
        !std::isfinite(s.bias) || s.bias < 0 || (s.bits != 8 && s.bits != 16))
        fail("Invalid bake settings.");
}
// Positive intersection area detects overlaps without rejecting shared edges.
struct D2 {
    double x, y;
};
static double side(D2 a, D2 b, D2 p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}
static double intersectArea(const V2 *a, const V2 *b) {
    std::vector<D2> p;
    for (int i = 0; i < 3; i++)
        p.push_back({a[i].x, a[i].y});
    double sign = orient(b[0], b[1], b[2]) < 0 ? -1 : 1;
    for (int e = 0; e < 3 && !p.empty(); e++) {
        D2 x{b[e].x, b[e].y}, y{b[(e + 1) % 3].x, b[(e + 1) % 3].y};
        std::vector<D2> out;
        D2 last = p.back();
        double dl = sign * side(x, y, last);
        for (auto cur : p) {
            double dc = sign * side(x, y, cur);
            if ((dc >= 0) != (dl >= 0)) {
                double t = dl / (dl - dc);
                out.push_back({last.x + (cur.x - last.x) * t, last.y + (cur.y - last.y) * t});
            }
            if (dc >= 0)
                out.push_back(cur);
            last = cur;
            dl = dc;
        }
        p = std::move(out);
    }
    double area = 0;
    for (size_t i = 0; i < p.size(); i++) {
        auto a = p[i], b = p[(i + 1) % p.size()];
        area += a.x * b.y - a.y * b.x;
    }
    return fabs(area) * .5;
}
static std::array<int, 4> bounds(const V2 *uv, int grid) {
    float x0 = 1, y0 = 1, x1 = 0, y1 = 0;
    for (int k = 0; k < 3; k++) {
        x0 = std::min(x0, uv[k].x);
        y0 = std::min(y0, uv[k].y);
        x1 = std::max(x1, uv[k].x);
        y1 = std::max(y1, uv[k].y);
    }
    return {std::clamp((int)(x0 * grid), 0, grid - 1), std::clamp((int)(y0 * grid), 0, grid - 1),
            std::clamp((int)(x1 * grid), 0, grid - 1), std::clamp((int)(y1 * grid), 0, grid - 1)};
}
QStringList validateOccluder(const Scene &scene) {
    if (scene.tris.empty())
        return {"The occluder has no triangle geometry."};
    for (const auto &t : scene.tris) {
        V3 c = cross(t.p[1] - t.p[0], t.p[2] - t.p[0]);
        if (!std::isfinite(dot(c, c)) || dot(c, c) < 1e-30f)
            return {
                "The occluder contains invalid or degenerate geometry. Correct it or remove the occluder."};
        for (auto p : t.p)
            if (!std::isfinite(dot(p, p)))
                return {"The occluder contains non-finite positions."};
    }
    return {};
}
Validation validate(const Scene &scene, const Settings &s) {
    Validation v;
    if (scene.occluder)
        v.errors.append(validateOccluder(*scene.occluder));
    std::set<int> bad;
    bool geometry = false, uv = false, outside = false, overlap = false;
    if (scene.tris.empty()) {
        v.errors << "No geometry loaded.";
        return v;
    }
    if (s.aoUV < 0 || s.maskUV < 0 || s.aoUV >= scene.uvNames.size() || s.maskUV >= scene.uvNames.size()) {
        v.errors << "Select existing mask and AO UV sets.";
        return v;
    }
    constexpr int grid = 64;
    constexpr size_t maxOverlapReports = 64;
    std::vector<std::vector<int>> cells(grid * grid);
    for (int i = 0; i < (int)scene.tris.size(); i++) {
        const auto &t = scene.tris[i];
        bool invalid = false;
        V3 c = cross(t.p[1] - t.p[0], t.p[2] - t.p[0]);
        if (!std::isfinite(dot(c, c)) || dot(c, c) < 1e-30f) {
            geometry = true;
            invalid = true;
        }
        for (int k = 0; k < 3; k++)
            if (!std::isfinite(dot(t.p[k], t.p[k])) || !std::isfinite(dot(t.n[k], t.n[k])) ||
                dot(t.n[k], t.n[k]) < .1f) {
                geometry = true;
                invalid = true;
            }
        if ((int)t.uv.size() <= std::max(s.maskUV, s.aoUV)) {
            uv = true;
            bad.insert(i);
            continue;
        }
        for (int k = 0; k < 3; k++) {
            auto a = t.uv[s.aoUV][k], m = t.uv[s.maskUV][k];
            if (!std::isfinite(a.x) || !std::isfinite(a.y) || !std::isfinite(m.x) || !std::isfinite(m.y)) {
                uv = true;
                invalid = true;
            }
            if (a.x < 0 || a.y < 0 || a.x > 1 || a.y > 1) {
                outside = true;
                invalid = true;
            }
        }
        auto a = t.uv[s.aoUV].data();
        if (fabsf(orient(a[0], a[1], a[2])) < 1e-16f) {
            uv = true;
            invalid = true;
        }
        if (invalid) {
            bad.insert(i);
            continue;
        }
        // Hard-error checks above always run for every face. Once overlap is established,
        // bounded examples are enough to warn/block; never enumerate millions of UV1 pairs.
        if (v.overlaps.size() >= maxOverlapReports) {
            v.overlapScanLimited = true;
            continue;
        }
        auto b = bounds(a, grid);
        std::set<int> candidates;
        for (int y = b[1]; y <= b[3]; y++)
            for (int x = b[0]; x <= b[2]; x++)
                for (int j : cells[y * grid + x])
                    candidates.insert(j);
        for (int j : candidates) {
            double area = intersectArea(a, scene.tris[j].uv[s.aoUV].data());
            if (area > 1e-14) {
                overlap = true;
                bad.insert(i);
                bad.insert(j);
                v.overlaps.push_back({i, j, area});
                if (v.overlaps.size() >= maxOverlapReports) {
                    v.overlapScanLimited = true;
                    break;
                }
            }
        }
        for (int y = b[1]; y <= b[3]; y++)
            for (int x = b[0]; x <= b[2]; x++)
                cells[y * grid + x].push_back(i);
    }
    if (geometry)
        v.errors << "Invalid or degenerate geometry (highlighted in red).";
    if (uv)
        v.errors << "Missing/non-finite UVs or zero-area AO UV faces (red).";
    if (outside)
        v.errors << "AO UVs must be inside the single 0–1 tile (red).";
    if (overlap) {
        auto &messages = s.averageOverlaps ? v.warnings : v.errors;
        const auto &pair = v.overlaps.front();
        const auto &first = scene.tris[pair.first];
        const auto &second = scene.tris[pair.second];
        messages << QString("AO UV faces overlap: %1%2 triangle pairs. Mirrored islands are allowed.")
                        .arg(v.overlapScanLimited ? "at least " : "")
                        .arg(v.overlaps.size());
        if (s.averageOverlaps)
            messages << "Baking is enabled. Shared pixels will average the faces' AO; the UVs are unchanged.";
        if (first.sourcePolygon >= 0 && second.sourcePolygon >= 0)
            messages
                << QString(
                       "First overlap: %1, polygon %2 / %3, polygon %4 (zero-based imported polygon IDs).")
                       .arg(first.sourceMesh)
                       .arg(first.sourcePolygon)
                       .arg(second.sourceMesh)
                       .arg(second.sourcePolygon);
        else
            messages << "Select a unique, non-overlapping UV set.";
    }
    v.faces.assign(bad.begin(), bad.end());
    return v;
}
Prepared prepare(const Scene &scene, const Settings &s) {
    checkSettings(s);
    auto valid = validate(scene, s);
    if (!valid.ok())
        fail(valid.errors.join('\n'));
    Prepared p;
    p.scale = scene.scale;
    std::vector<int> parent(scene.tris.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&](int a) {
        while (parent[a] != a) {
            parent[a] = parent[parent[a]];
            a = parent[a];
        }
        return a;
    };
    std::map<std::array<float, 10>, int> edges;
    for (int i = 0; i < (int)scene.tris.size(); i++) {
        auto &t = scene.tris[i];
        GpuTri g{};
        for (int k = 0; k < 3; k++) {
            g.p[k] = t.p[k];
            g.n[k] = t.n[k];
            g.maskUV[k] = t.uv[s.maskUV][k];
            g.aoUV[k] = t.uv[s.aoUV][k];
        }
        g.island = i;
        p.tris.push_back(g);
        for (int k = 0; k < 3; k++) {
            int l = (k + 1) % 3;
            std::array<float, 5> a{g.p[k].x, g.p[k].y, g.p[k].z, g.aoUV[k].x, g.aoUV[k].y},
                b{g.p[l].x, g.p[l].y, g.p[l].z, g.aoUV[l].x, g.aoUV[l].y};
            if (b < a)
                std::swap(a, b);
            std::array<float, 10> key;
            std::copy(a.begin(), a.end(), key.begin());
            std::copy(b.begin(), b.end(), key.begin() + 5);
            auto it = edges.find(key);
            if (it != edges.end())
                parent[root(i)] = root(it->second);
            else
                edges[key] = i;
        }
    }
    std::vector<std::vector<int>> cells(p.grid * p.grid);
    for (int i = 0; i < (int)p.tris.size(); i++) {
        p.tris[i].island = root(i);
        auto b = bounds(p.tris[i].aoUV, p.grid);
        for (int y = b[1]; y <= b[3]; y++)
            for (int x = b[0]; x <= b[2]; x++)
                cells[y * p.grid + x].push_back(i);
    }
    p.offsets.push_back(0);
    for (auto &cell : cells) {
        p.indices.insert(p.indices.end(), cell.begin(), cell.end());
        p.offsets.push_back((int)p.indices.size());
    }
    // Add casting-only triangles after building the hair UV raster bins. They participate
    // in ray traversal, but never receive AO or apply the hair's opacity texture.
    if (scene.occluder) {
        for (const auto &t : scene.occluder->tris) {
            GpuTri g{};
            for (int k = 0; k < 3; ++k) {
                g.p[k] = t.p[k];
                g.n[k] = t.n[k];
            }
            g.island = -1;
            g.opaque = 1;
            p.tris.push_back(g);
        }
    }
    return p;
}
float referenceTransmission(const Prepared &p, MaskView mask, V3 o, V3 d, float bias, float distance,
                            int skip) {
    float transmission = 1;
    for (int i = 0; i < (int)p.tris.size(); i++) {
        if (i == skip)
            continue;
        auto &t = p.tris[i];
        V3 e = t.p[1] - t.p[0], f = t.p[2] - t.p[0], q = cross(d, f);
        float det = dot(e, q);
        if (fabsf(det) < 1e-12f)
            continue;
        V3 a = o - t.p[0];
        float u = dot(a, q) / det;
        V3 r = cross(a, e);
        float v = dot(d, r) / det, dist = dot(f, r) / det;
        if (u < 0 || v < 0 || u + v > 1 || dist < bias || dist > distance)
            continue;
        if (t.opaque)
            return 0;
        transmission *= 1 - opacity(mask, blend(t.maskUV, u, v));
    }
    return transmission;
}
Result bakeReference(const Scene &scene, const Mask &mask, const Settings &s) {
    auto p = prepare(scene, s);
    Result r;
    r.resolution = s.resolution;
    r.pixels.assign((size_t)s.resolution * s.resolution, 1);
    r.owners.assign(r.pixels.size(), -1);
    float bias = s.bias ? s.bias : scene.scale * 1e-6f;
    for (int y = 0; y < s.resolution; y++)
        for (int x = 0; x < s.resolution; x++) {
            float sum = 0, weight = 0;
            int pixel = y * s.resolution + x;
            for (int sy = 0; sy < s.ss; sy++)
                for (int sx = 0; sx < s.ss; sx++) {
                    V2 uv{(x + (sx + .5f) / s.ss) / s.resolution, 1 - (y + (sy + .5f) / s.ss) / s.resolution};
                    int cell = std::min((int)(uv.y * p.grid), p.grid - 1) * p.grid +
                               std::min((int)(uv.x * p.grid), p.grid - 1);
                    for (int j = p.offsets[cell]; j < p.offsets[cell + 1]; j++) {
                        int i = p.indices[j];
                        auto &t = p.tris[i];
                        float u, v;
                        if (!rasterBary(t.aoUV, uv, u, v))
                            continue;
                        float a = opacity(mask.view(s), blend(t.maskUV, u, v));
                        V3 n = unit(blend(t.n, u, v)), g = unit(cross(t.p[1] - t.p[0], t.p[2] - t.p[0]));
                        if (dot(n, g) < 0)
                            g = g * -1;
                        V3 o = blend(t.p, u, v) + g * bias;
                        for (int ray = 0; ray < s.rays; ray++) {
                            uint32_t seed =
                                hash32(pixel * 9781U + (sy * s.ss + sx) * 6271U + ray * 13007U + 1);
                            V3 d = hemisphere(n, seed);
                            if (dot(d, g) < 0)
                                d = d - g * (2 * dot(d, g));
                            sum += a * referenceTransmission(p, mask.view(s), o, d, 0,
                                                             s.distance ? s.distance : 1e30f, i);
                            weight += a;
                        }
                        if (a > 0)
                            r.owners[pixel] =
                                r.owners[pixel] < 0 ? t.island : std::min(r.owners[pixel], t.island);
                        if (!s.averageOverlaps)
                            break;
                    }
                }
            if (weight > 0)
                r.pixels[pixel] = sum / weight;
        }
    padResult(r, s.padding);
    return r;
}
void padResult(Result &r, int padding, std::atomic_bool *cancel) {
    // Multi-source breadth-first dilation copies values without mixing UV islands.
    int n = r.resolution;
    std::vector<int> frontier;
    for (int i = 0; i < (int)r.owners.size(); i++)
        if (r.owners[i] >= 0) {
            int x = i % n, y = i / n;
            if ((x > 0 && r.owners[i - 1] < 0) || (x + 1 < n && r.owners[i + 1] < 0) ||
                (y > 0 && r.owners[i - n] < 0) || (y + 1 < n && r.owners[i + n] < 0))
                frontier.push_back(i);
        }
    for (int step = 0; step < padding && !frontier.empty(); step++) {
        if (cancel && *cancel)
            throw std::runtime_error("Bake cancelled.");
        std::vector<int> next;
        for (int i : frontier) {
            int x = i % n, y = i / n;
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    if (x + dx < 0 || x + dx >= n || y + dy < 0 || y + dy >= n)
                        continue;
                    int j = i + dy * n + dx;
                    if (r.owners[j] < 0) {
                        r.owners[j] = r.owners[i];
                        r.pixels[j] = r.pixels[i];
                        next.push_back(j);
                    }
                }
        }
        frontier = std::move(next);
    }
}
QImage resultImage(const Result &r) {
    QImage im(r.resolution, r.resolution, QImage::Format_Grayscale8);
    for (int y = 0; y < r.resolution; y++) {
        auto row = im.scanLine(y);
        for (int x = 0; x < r.resolution; x++)
            row[x] = (uchar)std::lround(clamp01(r.pixels[(size_t)y * r.resolution + x]) * 255);
    }
    return im;
}
static uint32_t crc(const QByteArray &b) {
    uint32_t c = ~0U;
    for (unsigned char v : b) {
        c ^= v;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320U & -(int)(c & 1));
    }
    return ~c;
}
static void chunk(QIODevice &f, const char *type, const QByteArray &data) {
    QByteArray b(type, 4);
    b += data;
    uint32_t n = qToBigEndian((uint32_t)data.size()), c = qToBigEndian(crc(b));
    if (f.write((char *)&n, 4) != 4 || f.write(b) != b.size() || f.write((char *)&c, 4) != 4)
        fail("Failed writing PNG.");
}
void savePng(const Result &r, const QString &path, int bits) {
    if (bits != 8 && bits != 16)
        fail("PNG bit depth must be 8 or 16.");
    std::vector<uint16_t> values(r.pixels.size());
    for (size_t i = 0; i < values.size(); i++)
        values[i] = (uint16_t)std::lround(clamp01(r.pixels[i]) * (bits == 16 ? 65535 : 255));
    // AO exports keep their original unfiltered row layout.
    writePng(path, r.resolution, r.resolution, 1, bits, values.data(), PngSpace::Linear, false);
}
static uint8_t paeth(int a, int b, int c) {
    int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
    return (uint8_t)(pa <= pb && pa <= pc ? a : (pb <= pc ? b : c));
}
void writePng(const QString &path, int w, int h, int channels, int bits, const uint16_t *values,
              PngSpace space, bool adaptiveFilter) {
    if ((bits != 8 && bits != 16) || channels < 1 || channels > 4 || w <= 0 || h <= 0)
        fail("Unsupported PNG format.");
    const int bpp = channels * bits / 8;
    const qsizetype rowBytes = (qsizetype)w * bpp, stride = 1 + rowBytes;
    QByteArray raw;
    raw.resize(stride * h);
    std::vector<uint8_t> prev(rowBytes, 0), cur(rowBytes), trial(rowBytes);
    for (int y = 0; y < h; y++) {
        const uint16_t *src = values + (size_t)y * w * channels;
        for (qsizetype i = 0, n = (qsizetype)w * channels; i < n; i++) {
            if (bits == 16) {
                cur[i * 2] = (uint8_t)(src[i] >> 8);
                cur[i * 2 + 1] = (uint8_t)src[i];
            } else
                cur[i] = (uint8_t)src[i];
        }
        // Adaptive filter choice (minimum sum of absolute differences), as libpng does.
        auto row = (uint8_t *)raw.data() + stride * y;
        uint64_t best = UINT64_MAX;
        for (int f = 0; f < (adaptiveFilter ? 5 : 1); f++) {
            uint64_t cost = 0;
            for (qsizetype i = 0; i < rowBytes; i++) {
                int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
                int predictor = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : paeth(a, b, c);
                trial[i] = (uint8_t)(cur[i] - predictor);
                cost += (uint64_t)abs((int8_t)trial[i]);
            }
            if (cost < best) {
                best = cost;
                row[0] = (uint8_t)f;
                std::copy(trial.begin(), trial.end(), row + 1);
            }
        }
        std::swap(prev, cur);
    }
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        fail(f.errorString());
    f.write("\x89PNG\r\n\x1a\n", 8);
    QByteArray ihdr(13, 0);
    qToBigEndian((uint32_t)w, ihdr.data());
    qToBigEndian((uint32_t)h, ihdr.data() + 4);
    ihdr[8] = (char)bits;
    ihdr[9] = (char)(channels == 1 ? 0 : channels == 2 ? 4 : channels == 3 ? 2 : 6);
    chunk(f, "IHDR", ihdr);
    if (space == PngSpace::Linear) {
        QByteArray gamma(4, 0);
        qToBigEndian((uint32_t)100000, gamma.data());
        chunk(f, "gAMA", gamma);
    } else if (space == PngSpace::Srgb)
        chunk(f, "sRGB", QByteArray(1, 0));
    chunk(f, "IDAT", qCompress(raw, 6).mid(4));
    chunk(f, "IEND", {});
    if (!f.commit())
        fail(f.errorString());
}
Scene testCards(int count) {
    Scene s;
    s.scale = 3;
    s.center = {0, 0, .2f};
    s.uvNames << "UV1 · shared mask" << "UV2 · unique AO";
    int cols = (int)ceil(sqrt((double)count));
    for (int i = 0; i < count; i++) {
        float z = i * .12f;
        V3 pts[4] = {{-1, -1, z}, {1, -1, z}, {1, 1, z}, {-1, 1, z}};
        V2 uv[4] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        int ix[6] = {0, 1, 2, 0, 2, 3};
        for (int t = 0; t < 2; t++) {
            Triangle tr;
            tr.uv.resize(2);
            for (int k = 0; k < 3; k++) {
                int j = ix[t * 3 + k];
                tr.p[k] = pts[j];
                tr.n[k] = {0, 0, 1};
                tr.uv[0][k] = uv[j];
                tr.uv[1][k] = {(i % cols + .05f + uv[j].x * .9f) / cols,
                               (i / cols + .05f + uv[j].y * .9f) / cols};
            }
            s.tris.push_back(tr);
        }
    }
    return s;
}
