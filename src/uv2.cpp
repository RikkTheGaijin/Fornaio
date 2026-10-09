#include "uv2.h"
#include "fbx_binary.h"
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <numeric>
#include <tuple>
#include <stdexcept>
#include <unordered_map>
#include <ufbx.h>
namespace {
[[noreturn]] void fail(const QString &message) {
    throw std::runtime_error(message.toStdString());
}
QString text(const ufbx_string &s) {
    return QString::fromUtf8(s.data, (int)s.length);
}
struct Edge {
    uint32_t a, b;
    double ua[2], ub[2];
    uint32_t face;
    auto key() const { return std::tie(a, b, ua[0], ua[1], ub[0], ub[1]); }
};
int root(std::vector<int> &parent, int a) {
    while (parent[a] != a)
        a = parent[a] = parent[parent[a]];
    return a;
}
struct MeshWork {
    const ufbx_mesh *mesh;
    fbx::Node *geometry;
    QString name;
    std::vector<int> faceIsland;
};
struct UvKey {
    uint64_t x, y;
    bool operator==(const UvKey &o) const { return x == o.x && y == o.y; }
};
struct UvKeyHash {
    size_t operator()(const UvKey &k) const { return std::hash<uint64_t>()(k.x * 0x9e3779b97f4a7c15ull ^ k.y); }
};
uint64_t bits(double v) {
    v = v == 0 ? 0 : v; // fold -0 into +0 so equal UVs share an index
    uint64_t b;
    memcpy(&b, &v, sizeof b);
    return b;
}
fbx::Node leaf(const char *name, fbx::Properties p) {
    return p.node(name);
}
// Adds UV2 as LayerElementUV (or replaces the second-lowest UV set) and links it to a Layer.
bool writeUv2(fbx::Node &geometry, const std::vector<PackPoint> &uv, const QByteArray &requestedName) {
    std::vector<double> values;
    std::vector<int32_t> indices(uv.size());
    std::unordered_map<UvKey, int32_t, UvKeyHash> unique;
    unique.reserve(uv.size());
    for (size_t i = 0; i < uv.size(); ++i) {
        auto [it, inserted] = unique.try_emplace({bits(uv[i].x), bits(uv[i].y)}, (int32_t)(values.size() / 2));
        if (inserted) {
            values.push_back(uv[i].x);
            values.push_back(uv[i].y);
        }
        indices[i] = it->second;
    }
    std::vector<std::pair<int64_t, size_t>> sets;
    size_t lastSet = 0;
    for (size_t i = 0; i < geometry.children.size(); ++i)
        if (geometry.children[i].name == "LayerElementUV") {
            sets.push_back({fbx::integer(geometry.children[i], 0), i});
            lastSet = i;
        }
    if (sets.empty())
        fail("FBX geometry has no LayerElementUV.");
    std::sort(sets.begin(), sets.end());
    const bool replace = sets.size() >= 2;
    const int64_t index = replace ? sets[1].first : sets[0].first + 1;
    QByteArray name = "UV2";
    if (!requestedName.isEmpty())
        name = requestedName;
    else if (replace)
        if (auto n = geometry.children[sets[1].second].child("Name"); n && n->propertyCount)
            name = fbx::string(*n, 0);
    if (auto n = geometry.children[sets[0].second].child("Name"); n && n->propertyCount && fbx::string(*n, 0) == name)
        fail(QString("UV2 name \"%1\" is already used by UV1. Choose a different name.").arg(QString::fromUtf8(name)));
    fbx::Node element = fbx::Properties().integer((int32_t)index).node("LayerElementUV");
    element.children = {leaf("Version", fbx::Properties().integer(101)), leaf("Name", fbx::Properties().string(name)),
                        leaf("MappingInformationType", fbx::Properties().string("ByPolygonVertex")),
                        leaf("ReferenceInformationType", fbx::Properties().string("IndexToDirect")),
                        leaf("UV", fbx::Properties().doubles(values)),
                        leaf("UVIndex", fbx::Properties().ints(indices))};
    if (replace)
        geometry.children[sets[1].second] = std::move(element);
    else
        geometry.children.insert(geometry.children.begin() + lastSet + 1, std::move(element));

    // Readers such as the FBX SDK only see elements referenced from a Layer.
    std::vector<std::pair<int64_t, size_t>> layers;
    for (size_t i = 0; i < geometry.children.size(); ++i) {
        const auto &layer = geometry.children[i];
        if (layer.name != "Layer")
            continue;
        bool hasUv = false;
        for (const auto &e : layer.children) {
            auto type = e.child("Type"), typed = e.child("TypedIndex");
            if (e.name != "LayerElement" || !type || !type->propertyCount || fbx::string(*type, 0) != "LayerElementUV")
                continue;
            if (typed && typed->propertyCount && fbx::integer(*typed, 0) == index)
                return replace;
            hasUv = true;
        }
        layers.push_back({hasUv ? INT64_MAX : fbx::integer(layer, 0), i});
    }
    if (layers.empty())
        return replace;
    fbx::Node link;
    link.name = "LayerElement";
    link.children = {leaf("Type", fbx::Properties().string("LayerElementUV")),
                     leaf("TypedIndex", fbx::Properties().integer((int32_t)index))};
    auto free = std::min_element(layers.begin(), layers.end());
    if (free->first != INT64_MAX) {
        geometry.children[free->second].children.push_back(std::move(link));
        return replace;
    }
    int64_t next = 0;
    size_t lastLayer = 0;
    for (size_t i = 0; i < geometry.children.size(); ++i)
        if (geometry.children[i].name == "Layer") {
            next = std::max(next, fbx::integer(geometry.children[i], 0) + 1);
            lastLayer = i;
        }
    fbx::Node layer = fbx::Properties().integer((int32_t)next).node("Layer");
    layer.children = {leaf("Version", fbx::Properties().integer(100)), std::move(link)};
    geometry.children.insert(geometry.children.begin() + lastLayer + 1, std::move(layer));
    return replace;
}
} // namespace

QString uvSetNameError(const QString &name) {
    static const QRegularExpression valid("^[A-Za-z_][A-Za-z0-9_]*$");
    if (name.isEmpty())
        return "The UV set name is empty.";
    if (name.size() > 64)
        return "The UV set name is longer than 64 characters.";
    if (!valid.match(name).hasMatch())
        return QString("\"%1\" is not a valid UV set name. Use letters, digits and underscores, "
                       "not starting with a digit.")
            .arg(name);
    return {};
}

Uv2Report generateUv2(const QString &input, const QString &output, const PackSettings &settings, const QString &name,
                      std::atomic_bool &cancel, const Progress &progress) {
    if (!name.isEmpty())
        if (QString error = uvSetNameError(name); !error.isEmpty())
            fail(error);
    QElapsedTimer timer;
    timer.start();
    auto report = [&](float f, const QString &message) {
        if (progress)
            progress(f, message);
    };
    report(0, "Reading FBX…");
    QFile file(input);
    if (!file.open(QIODevice::ReadOnly))
        fail(QString("Cannot open %1: %2").arg(input, file.errorString()));
    const QByteArray bytes = file.readAll();
    file.close();
    if (!fbx::isBinary(bytes))
        fail(QString("%1 is not a binary FBX. UV2 generation writes binary FBX only; re-export the mesh as "
                     "binary FBX.")
                 .arg(QFileInfo(input).fileName()));
    fbx::Document doc = fbx::parse(bytes);
    if (doc.version < 7000)
        fail(QString("FBX version %1 is not supported. Export FBX 2011 (7.1) or newer.").arg(doc.version));
    std::map<int64_t, fbx::Node *> geometries;
    if (auto objects = doc.node("Objects"))
        for (auto &n : objects->children)
            if (n.name == "Geometry" && n.propertyCount && fbx::propertyType(n, 0) == 'L')
                geometries[fbx::integer(n, 0)] = &n;

    ufbx_load_opts opts = {};
    opts.retain_dom = true;
    opts.ignore_animation = true;
    opts.ignore_embedded = true;
    const QByteArray path = input.toUtf8();
    opts.filename = {path.constData(), (size_t)path.size()};
    ufbx_error error = {};
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
        ufbx_load_memory(bytes.constData(), (size_t)bytes.size(), &opts, &error), ufbx_free_scene);
    if (!scene) {
        char message[2048];
        ufbx_format_error(message, sizeof message, &error);
        fail(QString::fromUtf8(message));
    }

    report(.05f, "Finding UV1 islands…");
    Uv2Report result;
    std::vector<MeshWork> meshes;
    std::vector<PackIsland> islands;
    int unused = 0, instanced = 0;
    for (const ufbx_mesh *mesh : scene->meshes) {
        if (!mesh->instances.count) {
            ++unused;
            continue;
        }
        MeshWork work{mesh, nullptr, text(mesh->instances.data[0]->name), {}};
        if (work.name.isEmpty())
            work.name = text(mesh->name);
        const ufbx_dom_node *dom = mesh->element.dom_node;
        if (!dom || !dom->values.count || dom->values.data[0].type != UFBX_DOM_VALUE_NUMBER ||
            !geometries.count(dom->values.data[0].value_int))
            fail(QString("Cannot locate the FBX geometry of mesh '%1'.").arg(work.name));
        work.geometry = geometries[dom->values.data[0].value_int];
        if (!mesh->uv_sets.count)
            fail(QString("Mesh '%1' has no UV1 set to build UV2 from.").arg(work.name));
        // UV2 is written per polygon vertex in file order; prove ufbx kept that order.
        const fbx::Node *pvi = work.geometry->child("PolygonVertexIndex");
        if (!pvi || !pvi->propertyCount)
            fail(QString("Mesh '%1' has no polygon index array.").arg(work.name));
        auto order = fbx::intArray(*pvi, 0);
        bool same = order.size() == mesh->num_indices;
        for (size_t i = 0; same && i < order.size(); ++i)
            same = uint32_t(order[i] < 0 ? ~order[i] : order[i]) == mesh->vertex_indices.data[i];
        if (!same)
            fail(QString("Mesh '%1' uses an unsupported polygon layout (invalid or out-of-range indices).")
                     .arg(work.name));
        instanced += mesh->instances.count > 1;

        const ufbx_vertex_vec2 &uv = mesh->uv_sets.data[0].vertex_uv;
        std::vector<int> parent(mesh->faces.count);
        std::iota(parent.begin(), parent.end(), 0);
        std::vector<Edge> edges;
        edges.reserve(mesh->num_indices);
        for (size_t f = 0; f < mesh->faces.count; ++f) {
            ufbx_face face = mesh->faces.data[f];
            if (face.num_indices < 3)
                continue;
            for (uint32_t k = 0; k < face.num_indices; ++k) {
                uint32_t i0 = face.index_begin + k, i1 = face.index_begin + (k + 1) % face.num_indices;
                ufbx_vec2 ua = ufbx_get_vertex_vec2(&uv, i0), ub = ufbx_get_vertex_vec2(&uv, i1);
                if (!std::isfinite(ua.x) || !std::isfinite(ua.y))
                    fail(QString("Mesh '%1' has non-finite UV1 coordinates.").arg(work.name));
                uint32_t a = mesh->vertex_indices.data[i0], b = mesh->vertex_indices.data[i1];
                if (a == b)
                    continue;
                if (a > b) {
                    std::swap(a, b);
                    std::swap(ua, ub);
                }
                edges.push_back({a, b, {ua.x, ua.y}, {ub.x, ub.y}, (uint32_t)f});
            }
        }
        // Faces join an island across edges that share both vertices and both UV1 coordinates.
        std::sort(edges.begin(), edges.end(), [](const Edge &x, const Edge &y) { return x.key() < y.key(); });
        for (size_t i = 1; i < edges.size(); ++i)
            if (edges[i].key() == edges[i - 1].key())
                parent[root(parent, (int)edges[i].face)] = root(parent, (int)edges[i - 1].face);
        if (cancel)
            fail("UV2 generation cancelled.");

        const ufbx_matrix &world = mesh->instances.data[0]->geometry_to_world;
        std::vector<int> islandOf(mesh->faces.count, -1);
        std::vector<uint32_t> tri(mesh->max_face_triangles * 3);
        work.faceIsland.assign(mesh->faces.count, -1);
        for (size_t f = 0; f < mesh->faces.count; ++f) {
            ufbx_face face = mesh->faces.data[f];
            if (face.num_indices < 3)
                continue;
            int &id = islandOf[root(parent, (int)f)];
            if (id < 0) {
                id = (int)islands.size();
                islands.emplace_back();
            }
            work.faceIsland[f] = id;
            PackIsland &island = islands[id];
            uint32_t count = ufbx_triangulate_face(tri.data(), tri.size(), mesh, face);
            for (uint32_t t = 0; t < count; ++t) {
                ufbx_vec3 p[3];
                for (int k = 0; k < 3; ++k) {
                    ufbx_vec2 q = ufbx_get_vertex_vec2(&uv, tri[t * 3 + k]);
                    island.triangles.push_back({q.x, q.y});
                    p[k] = ufbx_transform_position(&world, ufbx_get_vertex_vec3(&mesh->vertex_position, tri[t * 3 + k]));
                }
                double ax = p[1].x - p[0].x, ay = p[1].y - p[0].y, az = p[1].z - p[0].z;
                double bx = p[2].x - p[0].x, by = p[2].y - p[0].y, bz = p[2].z - p[0].z;
                double cx = ay * bz - az * by, cy = az * bx - ax * bz, cz = ax * by - ay * bx;
                double area = .5 * std::sqrt(cx * cx + cy * cy + cz * cz);
                if (std::isfinite(area))
                    island.surfaceArea += area;
            }
        }
        meshes.push_back(std::move(work));
    }
    if (meshes.empty())
        fail("The FBX contains no mesh geometry.");

    PackResult pack = packIslands(islands, settings, cancel, [&](float f, const QString &message) {
        report(.1f + f * .8f, message);
    });

    report(.92f, "Writing FBX…");
    int degenerate = 0;
    for (size_t i = 0; i < islands.size(); ++i) {
        const auto &t = islands[i].triangles;
        double s = pack.placements[i].scale;
        for (size_t j = 0; j + 2 < t.size(); j += 3) {
            double cross = (t[j + 1].x - t[j].x) * (t[j + 2].y - t[j].y) - (t[j + 1].y - t[j].y) * (t[j + 2].x - t[j].x);
            // Same threshold the bake validator applies to float UVs.
            degenerate += std::fabs(cross) * s * s < 1e-16;
        }
    }
    for (const auto &work : meshes) {
        const ufbx_mesh *mesh = work.mesh;
        const ufbx_vertex_vec2 &uv = mesh->uv_sets.data[0].vertex_uv;
        std::vector<PackPoint> uv2(mesh->num_indices);
        for (size_t f = 0; f < mesh->faces.count; ++f) {
            if (work.faceIsland[f] < 0)
                continue;
            const PackPlacement &place = pack.placements[work.faceIsland[f]];
            ufbx_face face = mesh->faces.data[f];
            for (uint32_t k = 0; k < face.num_indices; ++k) {
                ufbx_vec2 q = ufbx_get_vertex_vec2(&uv, face.index_begin + k);
                PackPoint p = place.apply({q.x, q.y});
                uv2[face.index_begin + k] = {std::clamp(p.x, 0.0, 1.0), std::clamp(p.y, 0.0, 1.0)};
            }
        }
        result.replaced += writeUv2(*work.geometry, uv2, name.toUtf8());
    }
    QByteArray out = fbx::serialize(doc);
    if (cancel)
        fail("UV2 generation cancelled.");
    QSaveFile save(output);
    if (!save.open(QIODevice::WriteOnly) || save.write(out) != out.size() || !save.commit())
        fail(QString("Cannot write %1: %2").arg(output, save.errorString()));

    result.meshes = (int)meshes.size();
    result.islands = (int)islands.size();
    result.rotated = pack.rotated;
    result.utilization = pack.utilization;
    if (result.replaced)
        result.warnings << QString("Replaced the existing UV2 on %1 mesh(es).").arg(result.replaced);
    if (instanced)
        result.warnings << QString("%1 instanced mesh(es) share one UV2 layout between their copies.").arg(instanced);
    if (unused)
        result.warnings << QString("%1 mesh(es) not used by any node were left unchanged.").arg(unused);
    if (pack.densityClamped)
        result.warnings << QString("%1 island(s) with extreme UV1 stretch had their texel density limited.")
                               .arg(pack.densityClamped);
    if (degenerate)
        result.warnings << QString("%1 triangle(s) have zero UV1 area and stay degenerate in UV2.").arg(degenerate);
    result.seconds = timer.elapsed() / 1000.0;
    report(1, "UV2 written.");
    return result;
}
