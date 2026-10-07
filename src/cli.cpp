#include "core.h"
#include "transfer.h"
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <iostream>
#include <numeric>
#include <thread>
static void require(bool b, const char *msg) {
    if (!b)
        throw std::runtime_error(msg);
}
void transferTests(const QString &artifacts, QJsonObject &out);
static Mask constantMask(float v) {
    Mask m;
    m.w = m.h = 2;
    m.values.assign(4, v);
    return m;
}
static void tests(const QString &dir) {
    QDir().mkpath(dir);
    Settings s;
    s.resolution = 24;
    s.rays = 128;
    s.ss = 2;
    s.padding = 0;
    s.aoUV = 1;
    s.averageOverlaps = false;
    Scene scene = testCards();
    std::atomic_bool cancel{false};
    require(validate(scene, s).ok(), "Unique UVs incorrectly rejected");
    // Mirror only one island in UV2, leaving its geometry, normals, and mask UV1 intact.
    auto mirroredUV = scene;
    for (int i = 0; i < 2; ++i)
        for (auto &uv : mirroredUV.tris[i].uv[1])
            uv.x = .5f - uv.x;
    require(validate(mirroredUV, s).ok(), "Mirrored AO UV island incorrectly rejected");
    auto reversedWinding = mirroredUV;
    for (auto &t : reversedWinding.tris) {
        std::swap(t.p[1], t.p[2]);
        std::swap(t.n[1], t.n[2]);
        for (auto &uv : t.uv)
            std::swap(uv[1], uv[2]);
        for (auto &n : t.n)
            n = n * -1;
    }
    require(validate(reversedWinding, s).ok(), "Reversed normals/winding affect UV validation");
    auto mirroredOverlap = mirroredUV;
    mirroredOverlap.tris.push_back(mirroredOverlap.tris.front());
    require(!validate(mirroredOverlap, s).ok(), "Real overlap inside a mirrored island was missed");
    s.aoUV = 0;
    require(!validate(scene, s).ok(), "Shared output UVs not rejected");
    s.aoUV = 1;
    auto bad = scene;
    bad.tris[0].uv[1][0].x = -.01f;
    require(!validate(bad, s).ok(), "Out-of-tile UV not rejected");
    bad = scene;
    bad.tris[0].uv[1][0].x = NAN;
    require(!validate(bad, s).ok(), "Missing UV not rejected");
    bad = scene;
    bad.tris[0].p[1] = bad.tris[0].p[0];
    require(!validate(bad, s).ok(), "Degenerate geometry not rejected");
    auto prep = prepare(scene, s);
    auto half = constantMask(.5f);
    // Opaque casting-only mesh: no UVs, no output pixels, and no hair mask applied.
    auto head = std::make_shared<Scene>(testCards(1));
    head->uvNames.clear();
    for (auto &t : head->tris) {
        t.uv.clear();
        for (auto &p : t.p) {
            p.x *= 5;
            p.y *= 5;
            p.z = .25f;
        }
    }
    Scene withHead = testCards(1);
    withHead.occluder = head;
    require(validate(withHead, s).ok(), "UV-less opaque occluder was rejected");
    auto headPrepared = prepare(withHead, s);
    require(headPrepared.tris.size() == withHead.tris.size() + head->tris.size(),
            "Occluder missing from BVH input");
    for (int index : headPrepared.indices)
        require(index < (int)withHead.tris.size(), "Occluder was included in output UV rasterization");
    require(referenceTransmission(headPrepared, half.view(s), {.13f, .21f, .1f}, {0, 0, 1}, 0, 1, -1) == 0,
            "Head incorrectly uses the hair opacity mask");
    auto headGpu = bakeGpu(withHead, half, s, cancel);
    auto headCpu = bakeReference(withHead, half, s);
    for (size_t i = 0; i < headGpu.pixels.size(); ++i)
        require(fabs(headGpu.pixels[i] - headCpu.pixels[i]) < .015f, "Head occluder GPU/CPU mismatch");
    require(*std::min_element(headGpu.pixels.begin(), headGpu.pixels.end()) < .1f,
            "Opaque head failed to occlude partially transparent hair");
    Settings shortRays = s;
    shortRays.distance = .05f;
    auto limited = bakeGpu(withHead, half, shortRays, cancel);
    for (float value : limited.pixels)
        require(value == 1, "Occluder ignores maximum distance");
    auto emptyHair = bakeGpu(withHead, constantMask(0), s, cancel);
    for (float value : emptyHair.pixels)
        require(value == 1, "Head wrote into transparent hair pixels");
    auto reversedHead = std::make_shared<Scene>(*head);
    for (auto &t : reversedHead->tris) {
        std::swap(t.p[1], t.p[2]);
        for (auto &n : t.n)
            n = n * -1;
    }
    withHead.occluder = reversedHead;
    auto reversedHeadResult = bakeGpu(withHead, half, s, cancel);
    for (size_t i = 0; i < headGpu.pixels.size(); ++i)
        require(fabs(headGpu.pixels[i] - reversedHeadResult.pixels[i]) < .015f,
                "Head is not a two-sided occluder");
    auto invalidHead = std::make_shared<Scene>(*head);
    invalidHead->tris[0].p[0].x = NAN;
    withHead.occluder = invalidHead;
    require(!validate(withHead, s).ok(), "Invalid occluder geometry was accepted");
    withHead.occluder.reset();
    auto removedHead = bakeGpu(withHead, half, s, cancel);
    for (float value : removedHead.pixels)
        require(value == 1, "Removing head did not restore unoccluded hair");
    Settings averaged = s;
    averaged.averageOverlaps = true;
    auto sharedUV = testCards(2);
    for (auto &t : sharedUV.tris)
        t.uv[1] = t.uv[0];
    auto sharedValidation = validate(sharedUV, averaged);
    require(sharedValidation.ok() && !sharedValidation.warnings.empty(),
            "Overlap averaging should warn without blocking");
    auto sharedGpu = bakeGpu(sharedUV, constantMask(1), averaged, cancel);
    auto sharedCpu = bakeReference(sharedUV, constantMask(1), averaged);
    for (size_t i = 0; i < sharedGpu.pixels.size(); ++i) {
        require(fabs(sharedGpu.pixels[i] - sharedCpu.pixels[i]) < .015f,
                "Overlapping UV average differs from reference");
        // Top card is entirely unoccluded and has equal opacity weight to the lower card.
        require(sharedGpu.pixels[i] >= .4999f, "Shared texel did not include the unoccluded top card");
    }
    require(*std::min_element(sharedGpu.pixels.begin(), sharedGpu.pixels.end()) < .8f,
            "Shared texel did not include the occluded lower card");
    auto invalidShared = sharedUV;
    invalidShared.tris.front().uv[1][0].x = -1;
    require(!validate(invalidShared, averaged).ok(), "Overlap averaging bypassed invalid UV checks");
    for (bool reverse : {false, true}) {
        V2 a[3] = {{0, 0}, {1, 0}, {1, 1}}, b[3] = {{0, 0}, {1, 1}, {0, 1}};
        if (reverse) {
            std::swap(a[1], a[2]);
            std::swap(b[1], b[2]);
        }
        float u, v;
        int coverage = (int)rasterBary(a, {.5f, .5f}, u, v) + (int)rasterBary(b, {.5f, .5f}, u, v);
        require(coverage == 1, "Shared triangulation edge was counted twice or left empty");
    }
    auto mirroredGpu = bakeGpu(mirroredUV, half, s, cancel);
    auto mirroredCpu = bakeReference(mirroredUV, half, s);
    for (size_t i = 0; i < mirroredGpu.pixels.size(); ++i)
        require(fabs(mirroredGpu.pixels[i] - mirroredCpu.pixels[i]) < .015f,
                "Mirrored UV2 bake differs from CPU reference");
    float transmission =
        referenceTransmission(prep, half.view(s), {.123f, .234f, -.1f}, {0, 0, 1}, 0, 10, -1);
    require(fabsf(transmission - .125f) < 1e-6f, "Stacked partial transmission not multiplicative");
    QJsonArray comparisons;
    for (float a : {0.f, .5f, 1.f}) {
        auto m = constantMask(a);
        auto gpu = bakeGpu(scene, m, s, cancel);
        auto cpu = bakeReference(scene, m, s);
        double maxError = 0, mean = 0;
        for (size_t i = 0; i < gpu.pixels.size(); i++) {
            double err = fabs(gpu.pixels[i] - cpu.pixels[i]);
            maxError = std::max(maxError, err);
            mean += err;
            if (a == 0)
                require(gpu.pixels[i] == 1, "Transparent mask occludes light");
        }
        mean /= gpu.pixels.size();
        require(maxError < .015 && mean < .001, "GPU differs from CPU reference");
        if (a == 1)
            require(*std::min_element(gpu.pixels.begin(), gpu.pixels.end()) < .2f,
                    "Opaque cards do not occlude");
        savePng(gpu, dir + QString("/mask_%1.png").arg(a), 16);
        comparisons.append(QJsonObject{
            {"opacity", a}, {"max_error", maxError}, {"mean_error", mean}, {"seconds", gpu.seconds}});
        std::cout << "PASS opacity " << a << " GPU/CPU max " << maxError << " mean " << mean << std::endl;
    }
    // Fine, asymmetric mask checks bilinear addressing, vertical flip and independent UV sets.
    Mask strands;
    strands.w = 17;
    strands.h = 13;
    strands.values.resize(17 * 13);
    for (int y = 0; y < 13; y++)
        for (int x = 0; x < 17; x++)
            strands.values[y * 17 + x] = (x % 3 == 0 ? .9f : .1f) * (y + 1) / 13.f;
    for (int variant = 0; variant < 3; variant++) {
        s.flip = variant == 1;
        s.repeat = variant == 2;
        auto sc = scene;
        if (s.repeat)
            for (auto &t : sc.tris)
                for (auto &uv : t.uv[0]) {
                    uv.x = uv.x * 2 - .25f;
                    uv.y = uv.y * 2 - .125f;
                }
        auto gpu = bakeGpu(sc, strands, s, cancel);
        auto cpu = bakeReference(sc, strands, s);
        double mean = 0;
        for (size_t i = 0; i < gpu.pixels.size(); i++)
            mean += fabs(gpu.pixels[i] - cpu.pixels[i]);
        mean /= gpu.pixels.size();
        require(mean < .001, "Textured GPU/CPU mismatch");
    }
    s.flip = s.repeat = false;
    auto flipped = scene;
    for (auto &t : flipped.tris)
        for (auto &n : t.n)
            n = n * -1;
    auto gpuFlip = bakeGpu(flipped, half, s, cancel), cpuFlip = bakeReference(flipped, half, s);
    for (size_t i = 0; i < gpuFlip.pixels.size(); i++)
        require(fabs(gpuFlip.pixels[i] - cpuFlip.pixels[i]) < .015f, "Flipped normals mismatch");
    auto deep = testCards(80);
    Settings ds = s;
    ds.resolution = 24;
    ds.ss = 1;
    ds.rays = 32;
    auto thin = constantMask(.02f);
    auto deepGpu = bakeGpu(deep, thin, ds, cancel), deepCpu = bakeReference(deep, thin, ds);
    double deepError = 0;
    for (size_t i = 0; i < deepGpu.pixels.size(); i++)
        deepError += fabs(deepGpu.pixels[i] - deepCpu.pixels[i]);
    require(deepError / deepGpu.pixels.size() < .001, "Deep transparency stack mismatch");
    auto grazing = scene;
    for (auto &t : grazing.tris)
        for (auto &n : t.n)
            n = unit({1, 0, .01f});
    auto gg = bakeGpu(grazing, half, s, cancel), gc = bakeReference(grazing, half, s);
    double err = 0;
    for (size_t i = 0; i < gg.pixels.size(); i++)
        err += fabs(gg.pixels[i] - gc.pixels[i]);
    require(err / gg.pixels.size() < .002, "Grazing-normal mismatch");
    s.distance = .01f;
    auto near = bakeGpu(scene, constantMask(1), s, cancel);
    for (float x : near.pixels)
        require(x == 1, "Short ray range/self-intersection failed");
    s.distance = 0;
    auto isolated = bakeGpu(testCards(1), constantMask(1), s, cancel);
    for (float x : isolated.pixels)
        require(x == 1, "Isolated card self-occludes");
    cancel = true;
    bool cancelled = false;
    try {
        bakeGpu(scene, half, s, cancel);
    } catch (const std::exception &e) {
        cancelled = QString(e.what()).contains("cancelled");
    }
    require(cancelled, "Pre-launch cancellation failed");
    cancel = false;
    cancelled = false;
    try {
        bakeGpu(scene, half, s, cancel, [&](float p, const QString &) {
            if (p >= .01f)
                cancel = true;
        });
    } catch (const std::exception &e) {
        cancelled = QString(e.what()).contains("cancelled");
    }
    require(cancelled, "In-progress cancellation failed");
    cancel = false;
    Result ramp;
    ramp.resolution = 256;
    ramp.pixels.resize(65536);
    ramp.owners.assign(65536, 0);
    for (int i = 0; i < 65536; i++)
        ramp.pixels[i] = i / 65535.f;
    savePng(ramp, dir + "/precision16.png", 16);
    savePng(ramp, dir + "/precision8.png", 8);
    auto image = QImage(dir + "/precision16.png").convertToFormat(QImage::Format_RGBA64);
    require(!image.isNull(), "PNG cannot be read");
    for (int y = 0; y < 256; y++) {
        auto row = (const QRgba64 *)image.constScanLine(y);
        for (int x = 0; x < 256; x++)
            require(row[x].red() == y * 256 + x, "PNG loses 16-bit precision");
    }
    Result pad;
    pad.resolution = 7;
    pad.pixels.assign(49, 1);
    pad.owners.assign(49, -1);
    pad.pixels[22] = .2f;
    pad.owners[22] = 1;
    pad.pixels[26] = .8f;
    pad.owners[26] = 2;
    padResult(pad, 1);
    require(pad.pixels[23] == .2f && pad.pixels[25] == .8f && pad.pixels[24] == 1,
            "Padding crosses island gap or blends values");
    QJsonObject report{
        {"passed", true},
        {"comparisons", comparisons},
        {"tests",
         "UV validation, transmission, masks, flip/repeat, independent UVs, front-side normals, grazing "
         "normals, distance, self-intersections, cancellation, padding, lossless 16-bit PNG, texture transfer"}};
    transferTests(dir, report);
    QFile f(dir + "/tests.json");
    f.open(QIODevice::WriteOnly);
    f.write(QJsonDocument(report).toJson());
    std::cout << "All core and GPU tests passed." << std::endl;
}
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QCoreApplication::setApplicationName("Fornaio CLI");
    QCommandLineParser p;
    p.setApplicationDescription("Opacity-aware AO baker; UV numbers are one-based.");
    p.addHelpOption();
    for (auto opt : std::vector<QCommandLineOption>{
             {"self-test", "Run CPU/GPU correctness tests."},
             {"benchmark", "Bake a generated dense 400-card fixture."},
             {"inspect", "Inspect geometry.", "file"},
             {"mesh", "Input FBX/OBJ.", "file"},
             {"occluder", "Optional opaque head/occluder FBX/OBJ; UVs not required.", "file"},
             {"mask", "Opacity texture.", "file"},
             {"out", "Output PNG.", "file", "AO.png"},
             {"size", "Output resolution.", "pixels", "2048"},
             {"rays", "Rays per sample.", "count", "256"},
             {"ss", "Supersampling per axis (1–4).", "count", "2"},
             {"padding", "Padding in pixels.", "pixels", "8"},
             {"bits", "PNG bit depth (8/16).", "bits", "16"},
             {"mask-uv", "Mask UV index (one-based).", "index", "1"},
             {"ao-uv", "AO UV index (one-based, defaults to UV2 if available).", "index"},
             {"channel", "Mask channel: red, green, blue, alpha, auto.", "name", "auto"},
             {"invert", "Invert mask."},
             {"flip", "Flip mask vertically."},
             {"repeat", "Repeat mask."},
             {"strict-uv", "Reject overlapping AO UVs instead of averaging their AO."},
             {"distance", "Maximum distance, 0 = unlimited.", "units", "0"},
             {"bias", "Ray bias, 0 = automatic.", "units", "0"},
             {"reference", "Use slow CPU reference for small validation fixtures."},
             {"transfer", "Run a texture transfer job (JSON saved from the Texture Transfer tab).", "job"},
             {"overwrite", "Allow a transfer job to replace existing output files."},
             {"artifacts", "Test results directory.", "directory", "artifacts"}})
        p.addOption(opt);
    p.process(app);
    try {
        if (p.isSet("self-test")) {
            tests(p.value("artifacts"));
            return 0;
        }
        if (p.isSet("transfer")) {
            QFile f(p.value("transfer"));
            if (!f.open(QIODevice::ReadOnly))
                throw std::runtime_error("Cannot open transfer job.");
            QJsonParseError error;
            auto doc = QJsonDocument::fromJson(f.readAll(), &error);
            if (!doc.isObject())
                throw std::runtime_error(("Invalid transfer job: " + error.errorString()).toStdString());
            TransferJob job = TransferJob::fromJson(doc.object(), QFileInfo(f.fileName()).absolutePath());
            if (p.isSet("overwrite"))
                job.settings.overwrite = true;
            std::atomic_bool cancel{false};
            auto report = runTransfer(job, p.isSet("reference") ? TransferBackend::Cpu : TransferBackend::Gpu, cancel,
                                      [](float f, const QString &msg) {
                                          std::cerr << (int)(f * 100) << "% " << msg.toStdString() << "\n";
                                      });
            for (const auto &w : report.warnings)
                std::cerr << "WARNING: " << w.toStdString() << '\n';
            QJsonArray maps;
            for (const auto &m : report.maps)
                maps.append(QJsonObject{{"path", m.path},
                                        {"prefix", m.prefix},
                                        {"suffix", m.suffix},
                                        {"resolution", m.resolution},
                                        {"bits", m.bits},
                                        {"channels", m.channels}});
            QJsonObject obj{{"maps", maps},
                            {"seconds", report.seconds},
                            {"device", report.device},
                            {"explicit_gpu_buffer_bytes", (double)report.gpuBytes},
                            {"target_triangles", report.targetTriangles},
                            {"exact_triangles", report.exactTriangles},
                            {"covered_texels", report.coveredTexels},
                            {"missed_texels", report.missedTexels},
                            {"warnings", QJsonArray::fromStringList(report.warnings)}};
            std::cout << QJsonDocument(obj).toJson().constData();
            return 0;
        }
        if (p.isSet("inspect")) {
            auto scene = loadScene(p.value("inspect"));
            QJsonArray names;
            for (auto &name : scene.uvNames)
                names.append(name);
            QJsonObject obj{
                {"triangles", (int)scene.tris.size()}, {"uv_sets", names}, {"bounds_diagonal", scene.scale}};
            obj["objects"] = QJsonArray::fromStringList(scene.objects);
            obj["materials"] = QJsonArray::fromStringList(scene.materials);
            if (p.isSet("ao-uv")) {
                Settings settings;
                settings.aoUV = p.value("ao-uv").toInt() - 1;
                settings.maskUV = p.value("mask-uv").toInt() - 1;
                settings.averageOverlaps = !p.isSet("strict-uv");
                auto validation = validate(scene, settings);
                obj["valid"] = validation.ok();
                obj["errors"] = QJsonArray::fromStringList(validation.errors);
                obj["warnings"] = QJsonArray::fromStringList(validation.warnings);
                obj["overlap_count"] = (int)validation.overlaps.size();
                obj["overlap_count_is_lower_bound"] = validation.overlapScanLimited;
                QJsonArray pairs;
                for (auto pair : validation.overlaps) {
                    QJsonArray triangles;
                    for (int index : {pair.first, pair.second}) {
                        const auto &t = scene.tris[index];
                        QJsonArray uv, positions;
                        for (int k = 0; k < 3; ++k) {
                            auto a = t.uv[settings.aoUV][k];
                            auto b = t.p[k];
                            uv.append(QJsonArray{a.x, a.y});
                            positions.append(QJsonArray{b.x, b.y, b.z});
                        }
                        triangles.append(QJsonObject{{"index", index},
                                                     {"mesh", t.sourceMesh},
                                                     {"polygon", t.sourcePolygon},
                                                     {"uv", uv},
                                                     {"positions", positions}});
                    }
                    pairs.append(QJsonObject{{"area", pair.area}, {"triangles", triangles}});
                }
                obj["overlaps"] = pairs;
            }
            std::cout << QJsonDocument(obj).toJson().constData();
            return 0;
        }
        if (!p.isSet("benchmark") && (!p.isSet("mesh") || !p.isSet("mask")))
            p.showHelp(1);
        Settings s;
        auto integer = [&](const char *key) {
            bool ok;
            int v = p.value(key).toInt(&ok);
            if (!ok)
                throw std::runtime_error(std::string("Invalid integer: ") + key);
            return v;
        };
        auto number = [&](const char *key) {
            bool ok;
            float v = p.value(key).toFloat(&ok);
            if (!ok)
                throw std::runtime_error(std::string("Invalid number: ") + key);
            return v;
        };
        s.resolution = integer("size");
        s.rays = integer("rays");
        s.ss = integer("ss");
        s.padding = integer("padding");
        s.bits = integer("bits");
        s.maskUV = integer("mask-uv") - 1;
        s.distance = number("distance");
        s.bias = number("bias");
        s.flip = p.isSet("flip");
        s.repeat = p.isSet("repeat");
        s.invert = p.isSet("invert");
        s.averageOverlaps = !p.isSet("strict-uv");
        QStringList channels{"red", "green", "blue", "alpha", "auto"};
        int ch = channels.indexOf(p.value("channel"));
        if (ch < 0)
            throw std::runtime_error("Unknown mask channel.");
        s.channel = ch == 4 ? -1 : ch;
        Scene scene = p.isSet("benchmark") ? testCards(400) : loadScene(p.value("mesh"));
        if (p.isSet("occluder"))
            scene.occluder = std::make_shared<Scene>(loadScene(p.value("occluder")));
        s.aoUV = p.isSet("ao-uv") ? integer("ao-uv") - 1 : (scene.uvNames.size() > 1 ? 1 : 0);
        for (const auto &warning : validate(scene, s).warnings)
            std::cerr << "WARNING: " << warning.toStdString() << '\n';
        Mask mask;
        if (p.isSet("benchmark")) {
            mask.w = mask.h = 256;
            mask.values.resize(65536);
            for (int y = 0; y < 256; y++)
                for (int x = 0; x < 256; x++)
                    mask.values[y * 256 + x] = x % 8 < 2 ? 1.f : (x % 8 == 2 ? .5f : 0.f);
        } else
            mask = loadMask(p.value("mask"), s.channel, s.invert);
        std::atomic_bool cancel{false};
        Result result = p.isSet("reference")
                            ? bakeReference(scene, mask, s)
                            : bakeGpu(scene, mask, s, cancel, [](float f, const QString &msg) {
                                  std::cerr << (int)(f * 100) << "% " << msg.toStdString() << "\n";
                              });
        savePng(result, p.value("out"), s.bits);
        QJsonObject obj{
            {"output", p.value("out")},         {"seconds", result.seconds},
            {"device", result.device},          {"explicit_gpu_buffer_bytes", (double)result.gpuBytes},
            {"resolution", s.resolution},       {"rays_per_sample", s.rays},
            {"samples_per_pixel", s.ss * s.ss}, {"triangles", (int)scene.tris.size()}};
        obj["occluder_triangles"] = scene.occluder ? (int)scene.occluder->tris.size() : 0;
        std::cout << QJsonDocument(obj).toJson().constData();
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 2;
    }
}
