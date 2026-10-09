// UV2 layout self-tests: packer correctness (bounds, spacing, density, rotation, determinism) and a
// binary FBX round trip through the real ufbx importer. Fixtures are generated in memory.
#include "fbx_binary.h"
#include "uv2.h"
#include <QDir>
#include <QFile>
#include <QJsonObject>
#include <cmath>
#include <iostream>
#include <random>

static void check(bool b, const QString &msg) {
    if (!b)
        throw std::runtime_error(("UV2 test failed: " + msg).toStdString());
}
static double uvArea(const PackIsland &island) {
    double a = 0;
    const auto &t = island.triangles;
    for (size_t i = 0; i < t.size(); i += 3)
        a += fabs((t[i + 1].x - t[i].x) * (t[i + 2].y - t[i].y) - (t[i + 1].y - t[i].y) * (t[i + 2].x - t[i].x)) * .5;
    return a;
}
static std::vector<PackIsland> randomIslands(int count) {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<double> u(0, 1);
    std::vector<PackIsland> islands(count);
    for (int i = 0; i < count; ++i) {
        double w = .02 + .08 * u(rng), h = .1 + .4 * u(rng), x = u(rng) * .5, y = u(rng) * .4;
        double shear = i % 3 == 1 ? .1 * u(rng) : 0;
        PackPoint a{x, y}, b{x + w, y + shear}, c{x + w, y + h + shear}, d{x, y + h};
        auto &t = islands[i].triangles;
        if (i % 7 == 6)
            t = {a, b, d};
        else
            t = {a, b, c, a, c, d};
        double density = .5 + 1.5 * u(rng);
        islands[i].surfaceArea = uvArea(islands[i]) * density * density;
    }
    return islands;
}
static bool inside(PackPoint a, PackPoint b, PackPoint c, PackPoint p) {
    auto side = [](PackPoint a, PackPoint b, PackPoint p) { return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x); };
    double d0 = side(a, b, p), d1 = side(b, c, p), d2 = side(c, a, p);
    return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
}
// Samples islands on a 2x grid and dilates each by just under half the spacing: any collision
// means two islands are closer than the requested gap (or overlap).
static void checkLayout(const std::vector<PackIsland> &islands, const PackResult &r, const PackSettings &s) {
    const int g = s.resolution * 2, d = s.spacing - 1;
    std::vector<int> owner((size_t)g * g, -1), grown((size_t)g * g, -1);
    for (size_t i = 0; i < islands.size(); ++i) {
        const auto &t = islands[i].triangles;
        std::vector<PackPoint> o(t.size());
        for (size_t j = 0; j < t.size(); ++j) {
            o[j] = r.placements[i].apply(t[j]);
            check(o[j].x >= 0 && o[j].y >= 0 && o[j].x <= 1 && o[j].y <= 1, "island outside the 0-1 tile");
        }
        for (size_t j = 0; j < o.size(); j += 3) {
            int x0 = std::max(0, (int)floor(std::min({o[j].x, o[j + 1].x, o[j + 2].x}) * g));
            int x1 = std::min(g - 1, (int)ceil(std::max({o[j].x, o[j + 1].x, o[j + 2].x}) * g));
            int y0 = std::max(0, (int)floor(std::min({o[j].y, o[j + 1].y, o[j + 2].y}) * g));
            int y1 = std::min(g - 1, (int)ceil(std::max({o[j].y, o[j + 1].y, o[j + 2].y}) * g));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    if (inside(o[j], o[j + 1], o[j + 2], {(x + .5) / g, (y + .5) / g})) {
                        int &cell = owner[(size_t)y * g + x];
                        check(cell < 0 || cell == (int)i, "islands overlap");
                        cell = (int)i;
                    }
        }
    }
    for (int y = 0; y < g; ++y)
        for (int x = 0; x < g; ++x) {
            int i = owner[(size_t)y * g + x];
            if (i < 0)
                continue;
            for (int yy = std::max(0, y - d); yy <= std::min(g - 1, y + d); ++yy)
                for (int xx = std::max(0, x - d); xx <= std::min(g - 1, x + d); ++xx) {
                    int &cell = grown[(size_t)yy * g + xx];
                    check(cell < 0 || cell == i, "islands closer than the requested spacing");
                    cell = i;
                }
        }
}
static fbx::Node leaf(const char *name, fbx::Properties p) {
    return p.node(name);
}
static fbx::Node withChildren(fbx::Node n, std::vector<fbx::Node> children) {
    n.children = std::move(children);
    return n;
}
// Binary FBX 7.4 with three overlapping UV1 cards (two quads and a triangle fan) in one mesh.
static QByteArray cardsFbx() {
    std::vector<double> vertices;
    std::vector<int32_t> polygons, uvIndex;
    std::vector<double> uv = {0, 0, .25, 0, .25, 1, 0, 1, .125, .5};
    for (int card = 0; card < 3; ++card) {
        int32_t base = (int32_t)vertices.size() / 3;
        double z = card * .1, height = 1 + card * .5;
        for (auto [x, y] : std::vector<std::pair<double, double>>{{0, 0}, {.2, 0}, {.2, height}, {0, height}})
            vertices.insert(vertices.end(), {x + card, y, z});
        if (card < 2) {
            polygons.insert(polygons.end(), {base, base + 1, base + 2, ~(base + 3)});
            uvIndex.insert(uvIndex.end(), {0, 1, 2, 3});
        } else {
            vertices.insert(vertices.end(), {card + .1, height / 2, z});
            for (int k = 0; k < 4; ++k) {
                polygons.insert(polygons.end(), {base + 4, base + k, ~(base + (k + 1) % 4)});
                uvIndex.insert(uvIndex.end(), {4, k, (k + 1) % 4});
            }
        }
    }
    fbx::Document doc;
    doc.header = QByteArray("Kaydara FBX Binary  \0\x1a\0", 23) + QByteArray(4, '\0');
    doc.version = 7400;
    doc.nodes.push_back(withChildren(fbx::Properties().node("FBXHeaderExtension"),
                                     {leaf("FBXHeaderVersion", fbx::Properties().integer(1003)),
                                      leaf("FBXVersion", fbx::Properties().integer(7400))}));
    auto geometry =
        withChildren(fbx::Properties().longInteger(100).string(QByteArray("Cards\0\1Geometry", 15)).string("Mesh").node("Geometry"),
                     {leaf("Vertices", fbx::Properties().doubles(vertices)),
                      leaf("PolygonVertexIndex", fbx::Properties().ints(polygons)),
                      leaf("GeometryVersion", fbx::Properties().integer(124)),
                      withChildren(fbx::Properties().integer(0).node("LayerElementUV"),
                                   {leaf("Version", fbx::Properties().integer(101)),
                                    leaf("Name", fbx::Properties().string("map1")),
                                    leaf("MappingInformationType", fbx::Properties().string("ByPolygonVertex")),
                                    leaf("ReferenceInformationType", fbx::Properties().string("IndexToDirect")),
                                    leaf("UV", fbx::Properties().doubles(uv)),
                                    leaf("UVIndex", fbx::Properties().ints(uvIndex))}),
                      withChildren(fbx::Properties().integer(0).node("Layer"),
                                   {leaf("Version", fbx::Properties().integer(100)),
                                    withChildren(fbx::Properties().node("LayerElement"),
                                                 {leaf("Type", fbx::Properties().string("LayerElementUV")),
                                                  leaf("TypedIndex", fbx::Properties().integer(0))})})});
    auto model = withChildren(
        fbx::Properties().longInteger(200).string(QByteArray("Cards\0\1Model", 12)).string("Mesh").node("Model"),
        {leaf("Version", fbx::Properties().integer(232))});
    doc.nodes.push_back(withChildren(fbx::Properties().node("Objects"), {geometry, model}));
    doc.nodes.push_back(withChildren(
        fbx::Properties().node("Connections"),
        {leaf("C", fbx::Properties().string("OO").longInteger(200).longInteger(0)),
         leaf("C", fbx::Properties().string("OO").longInteger(100).longInteger(200))}));
    return fbx::serialize(doc);
}
void uv2Tests(const QString &artifacts, QJsonObject &out) {
    std::atomic_bool cancel{false};
    auto islands = randomIslands(150);
    PackSettings settings;
    settings.resolution = 512;
    settings.spacing = 4;
    auto plain = packIslands(islands, settings, cancel);
    checkLayout(islands, plain, settings);
    check(plain.rotated == 0, "islands rotated with rotation disabled");
    for (const auto &p : plain.placements)
        check(!p.rotated, "rotation flag set with rotation disabled");
    double reference = -1;
    for (size_t i = 0; i < islands.size(); ++i) {
        double density = plain.placements[i].scale * plain.placements[i].scale * uvArea(islands[i]) / islands[i].surfaceArea;
        if (reference < 0)
            reference = density;
        check(fabs(density / reference - 1) < 1e-9, "texel density is not uniform");
    }
    check(plain.utilization > .5, QString("low utilization %1").arg(plain.utilization));
    auto again = packIslands(islands, settings, cancel);
    for (size_t i = 0; i < islands.size(); ++i)
        check(again.placements[i].offset.x == plain.placements[i].offset.x &&
                  again.placements[i].offset.y == plain.placements[i].offset.y &&
                  again.placements[i].scale == plain.placements[i].scale,
              "packing is not deterministic");
    settings.allowRotation = true;
    auto rotated = packIslands(islands, settings, cancel);
    checkLayout(islands, rotated, settings);
    // Unrotated recipes are a subset of the rotated search; allow for scale-search granularity.
    check(rotated.utilization >= plain.utilization * .99, "rotation made the layout worse");
    // Lay every other card on its side: only rotation can stand them back up.
    auto mixed = islands;
    for (size_t i = 0; i < mixed.size(); i += 2)
        for (auto &p : mixed[i].triangles)
            std::swap(p.x, p.y);
    settings.allowRotation = false;
    auto mixedPlain = packIslands(mixed, settings, cancel);
    settings.allowRotation = true;
    auto mixedRotated = packIslands(mixed, settings, cancel);
    checkLayout(mixed, mixedRotated, settings);
    check(mixedRotated.rotated > 0 && mixedRotated.utilization > mixedPlain.utilization,
          QString("rotation did not help sideways cards (%1 vs %2)")
              .arg(mixedRotated.utilization)
              .arg(mixedPlain.utilization));
    for (size_t i = 0; i < mixed.size(); ++i) {
        // A 90 degree turn keeps the island's area and swaps its extents exactly.
        const auto &place = mixedRotated.placements[i];
        PackPoint a = place.apply(mixed[i].triangles[0]), b = place.apply(mixed[i].triangles[1]);
        PackPoint da{b.x - a.x, b.y - a.y}, ds{mixed[i].triangles[1].x - mixed[i].triangles[0].x,
                                             mixed[i].triangles[1].y - mixed[i].triangles[0].y};
        PackPoint expect = place.rotated ? PackPoint{-ds.y * place.scale, ds.x * place.scale}
                                         : PackPoint{ds.x * place.scale, ds.y * place.scale};
        check(fabs(da.x - expect.x) < 1e-12 && fabs(da.y - expect.y) < 1e-12, "island transform is not a 90 degree turn");
    }
    std::cout << "PASS UV2 packing utilization " << plain.utilization << " / rotated " << rotated.utilization
              << "; sideways cards " << mixedPlain.utilization << " -> " << mixedRotated.utilization << " ("
              << mixedRotated.rotated << " rotated)" << std::endl;
    cancel = true;
    bool cancelled = false;
    try {
        packIslands(islands, settings, cancel);
    } catch (const std::exception &e) {
        cancelled = QString(e.what()).contains("cancelled");
    }
    check(cancelled, "packing ignores cancellation");
    cancel = false;
    bool rejected = false;
    try {
        settings.resolution = 64;
        packIslands(randomIslands(2000), settings, cancel);
    } catch (const std::exception &e) {
        rejected = QString(e.what()).contains("do not fit");
    }
    check(rejected, "impossible layout not reported");

    QDir().mkpath(artifacts);
    const QString input = artifacts + "/uv2_cards.fbx", output = artifacts + "/uv2_cards_UV2.fbx";
    const QByteArray bytes = cardsFbx();
    check(fbx::serialize(fbx::parse(bytes)) == bytes, "binary FBX round trip changed bytes");
    QFile f(input);
    check(f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(), "cannot write FBX fixture");
    f.close();
    PackSettings fileSettings;
    fileSettings.resolution = 256;
    auto report = generateUv2(input, output, fileSettings, {}, cancel);
    check(report.meshes == 1 && report.islands == 3 && report.replaced == 0, "unexpected UV2 report");
    Scene source = loadScene(input), scene = loadScene(output);
    check(scene.uvNames.size() == 2 && scene.tris.size() == source.tris.size(), "UV2 set missing after import");
    for (size_t i = 0; i < scene.tris.size(); ++i)
        for (int k = 0; k < 3; ++k)
            check(scene.tris[i].uv[0][k].x == source.tris[i].uv[0][k].x &&
                      scene.tris[i].uv[0][k].y == source.tris[i].uv[0][k].y,
                  "UV1 changed");
    Settings strict;
    strict.maskUV = 0;
    strict.aoUV = 1;
    strict.averageOverlaps = false;
    auto validation = validate(scene, strict);
    check(validation.ok() && validation.overlaps.empty(), "generated UV2 overlaps or leaves the tile");
    auto doc = fbx::parse([&] {
        QFile r(output);
        r.open(QIODevice::ReadOnly);
        return r.readAll();
    }());
    const fbx::Node *geometry = doc.node("Objects")->child("Geometry");
    int uvSets = 0, layers = 0;
    for (const auto &c : geometry->children) {
        uvSets += c.name == "LayerElementUV";
        layers += c.name == "Layer";
    }
    check(uvSets == 2 && layers == 2, "UV2 element or layer link missing");
    auto replaced = generateUv2(output, output, fileSettings, {}, cancel);
    check(replaced.replaced == 1 && loadScene(output).uvNames.size() == 2, "existing UV2 was not replaced");
    generateUv2(output, output, fileSettings, "hairAO_uv", cancel);
    check(loadScene(output).uvNames[1].contains("hairAO_uv"), "custom UV2 name not written");
    check(!uvSetNameError("2uv").isEmpty() && !uvSetNameError("a b").isEmpty() && uvSetNameError("UV_2").isEmpty(),
          "UV set name validation");
    bool badName = false;
    try {
        generateUv2(output, output, fileSettings, "bad name", cancel);
    } catch (const std::exception &) {
        badName = true;
    }
    check(badName, "invalid UV2 name accepted");
    out["uv2"] = QJsonObject{{"utilization", plain.utilization},
                             {"rotated_utilization", rotated.utilization},
                             {"fbx_islands", report.islands}};
    std::cout << "PASS UV2 FBX round trip" << std::endl;
}
