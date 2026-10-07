// Texture transfer self-tests: exactness, analytic accuracy, tangent-space normals, projection, pairing,
// and GPU/CPU parity. Fixtures are generated in memory; images are written to the artifacts directory.
#include "transfer.h"
#include "transfer_shared.h"
#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <iostream>

static void check(bool b, const QString &msg) {
    if (!b)
        throw std::runtime_error(("Transfer test failed: " + msg).toStdString());
}
static float wave(float x, float y, float a) {
    return a * sinf(1.4f * x) * cosf(1.1f * y);
}
static V3 waveNormal(float x, float y, float a) {
    float dx = a * 1.4f * cosf(1.4f * x) * cosf(1.1f * y), dy = -a * 1.1f * sinf(1.4f * x) * sinf(1.1f * y);
    return unit({-dx, -dy, 1});
}
// n x n quads over [-1,1]^2. UV0 is planar; UV1 has four quadrant islands (identity, rotated, mirrored,
// rotated 180 degrees) shuffled into other slots.
static Scene gridScene(int n, bool altDiagonal, float amp, bool twoObjects = false) {
    Scene s;
    s.uvNames << "UV0" << "UV1";
    s.objects << (twoObjects ? QStringList{"left", "right"} : QStringList{"mesh"});
    s.materials << (twoObjects ? QStringList{"matLeft", "matRight"} : QStringList{"mat"});
    s.center = {0, 0, 0};
    s.scale = 2.9f;
    auto vertex = [&](int i, int j, V3 &p, V3 &nn, V2 &uv0) {
        float x = -1 + 2.f * i / n, y = -1 + 2.f * j / n;
        p = {x, y, wave(x, y, amp)};
        nn = waveNormal(x, y, amp);
        uv0 = {.02f + .96f * (x + 1) / 2, .02f + .96f * (y + 1) / 2};
    };
    for (int j = 0; j < n; j++)
        for (int i = 0; i < n; i++) {
            int ci[4] = {i, i + 1, i + 1, i}, cj[4] = {j, j, j + 1, j + 1};
            float cx = -1 + 2.f * (i + .5f) / n, cy = -1 + 2.f * (j + .5f) / n;
            V3 p[4], nn[4];
            V2 u0[4], u1[4];
            for (int k = 0; k < 4; k++) {
                vertex(ci[k], cj[k], p[k], nn[k], u0[k]);
                // UV1 uses the quad's quadrant so shared borders stay in that island.
                float x = -1 + 2.f * ci[k] / n, y = -1 + 2.f * cj[k] / n;
                float qx = cx < 0 ? -1.f : 0.f, qy = cy < 0 ? -1.f : 0.f;
                float lx = x - qx, ly = y - qy;
                int q = (cy < 0 ? 0 : 2) + (cx < 0 ? 0 : 1);
                V2 a;
                switch (q) {
                case 0:
                    a = {lx, ly};
                    break;
                case 1:
                    a = {1 - ly, lx};
                    break;
                case 2:
                    a = {1 - lx, ly};
                    break;
                default:
                    a = {1 - lx, 1 - ly};
                }
                int slot = (q + 1) % 4;
                u1[k] = {(slot % 2) * .5f + .025f + a.x * .45f, (slot / 2) * .5f + .025f + a.y * .45f};
            }
            int ix[6] = {0, 1, 2, 0, 2, 3}, alt[6] = {0, 1, 3, 1, 2, 3};
            for (int t = 0; t < 2; t++) {
                Triangle tr;
                tr.uv.resize(2);
                for (int k = 0; k < 3; k++) {
                    int c = (altDiagonal ? alt : ix)[t * 3 + k];
                    tr.p[k] = p[c];
                    tr.n[k] = nn[c];
                    tr.uv[0][k] = u0[c];
                    tr.uv[1][k] = u1[c];
                }
                tr.object = twoObjects && cx >= 0 ? 1 : 0;
                tr.material = tr.object;
                s.tris.push_back(tr);
            }
        }
    return s;
}
static bool locate(const Scene &s, int uvSet, V2 uv, int &tri, float &u, float &v) {
    for (int i = 0; i < (int)s.tris.size(); i++) {
        V2 t[3] = {s.tris[i].uv[uvSet][0], s.tris[i].uv[uvSet][1], s.tris[i].uv[uvSet][2]};
        if (bary(t, uv, u, v)) {
            tri = i;
            return true;
        }
    }
    return false;
}
static V2 texelUV(int x, int y, int res) {
    return {(x + .5f) / res, 1 - (y + .5f) / res};
}
// True if the texel and a ring of points 2.5 texels around it map to the same island (quadrant).
static bool interior(const Scene &s, int uvSet, int x, int y, int res, int &tri, float &u, float &v) {
    if (!locate(s, uvSet, texelUV(x, y, res), tri, u, v))
        return false;
    V3 p = blend(s.tris[tri].p, u, v);
    int q = (p.x < 0) + 2 * (p.y < 0);
    for (int k = 0; k < 8; k++) {
        float a = k * 0.785398f, ox = 2.5f * cosf(a) / res, oy = 2.5f * sinf(a) / res;
        int t2;
        float u2, v2;
        V2 c = texelUV(x, y, res);
        if (!locate(s, uvSet, {c.x + ox, c.y + oy}, t2, u2, v2))
            return false;
        V3 p2 = blend(s.tris[t2].p, u2, v2);
        if ((p2.x < 0) + 2 * (p2.y < 0) != q)
            return false;
    }
    return true;
}
static float field(V3 p) {
    return .5f + .35f * sinf(2.1f * p.x + .4f) * cosf(1.7f * p.y) + .1f * p.z;
}
static V3 normalField(V3 p, V3 n) {
    return unit(n + V3{.45f * sinf(2.3f * p.x), .45f * cosf(1.9f * p.y + .3f), 0});
}
static void writeFieldTexture(const Scene &s, int uvSet, int res, const QString &path) {
    std::vector<uint16_t> q((size_t)res * res, 0);
    for (int y = 0; y < res; y++)
        for (int x = 0; x < res; x++) {
            int t;
            float u, v;
            if (locate(s, uvSet, texelUV(x, y, res), t, u, v))
                q[(size_t)y * res + x] = (uint16_t)std::lround(clamp01(field(blend(s.tris[t].p, u, v))) * 65535);
        }
    writePng(path, res, res, 1, 16, q.data(), PngSpace::Untagged);
}
static V3 encodeTangent(V3 w, V3 T, V3 B, V3 N) {
    V3 r0 = cross(B, N), r1 = cross(N, T), r2 = cross(T, B);
    float det = dot(T, r0);
    return unit(V3{dot(w, r0), dot(w, r1), dot(w, r2)} * (1 / det));
}
static void frameAt(const Scene &s, const std::vector<V3> &tan, const std::vector<float> &sg, int t, float u, float v,
                    V3 &T, V3 &B, V3 &N) {
    tangentFrameNormal(&tan[t * 3], s.tris[t].n, &sg[t * 3], u, v, T, B);
    N = blend(s.tris[t].n, u, v);
}
static void writeNormalTexture(const Scene &s, int uvSet, int res, const QString &path) {
    std::vector<V3> tan;
    std::vector<float> sg;
    meshTangents(s, uvSet, tan, sg);
    std::vector<uint16_t> q((size_t)res * res * 3, 0);
    for (int y = 0; y < res; y++)
        for (int x = 0; x < res; x++) {
            int t;
            float u, v;
            V3 c{0, 0, 1};
            if (locate(s, uvSet, texelUV(x, y, res), t, u, v)) {
                V3 T, B, N;
                frameAt(s, tan, sg, t, u, v, T, B, N);
                c = encodeTangent(normalField(blend(s.tris[t].p, u, v), unit(N)), T, B, N);
            }
            uint16_t *o = q.data() + ((size_t)y * res + x) * 3;
            o[0] = (uint16_t)std::lround(clamp01(c.x * .5f + .5f) * 65535);
            o[1] = (uint16_t)std::lround(clamp01(c.y * .5f + .5f) * 65535);
            o[2] = (uint16_t)std::lround(clamp01(c.z * .5f + .5f) * 65535);
        }
    writePng(path, res, res, 3, 16, q.data(), PngSpace::Untagged);
}
static void writeConstant(const QString &path, uint16_t value) {
    std::vector<uint16_t> q(8 * 8 * 3, value);
    writePng(path, 8, 8, 3, 8, q.data(), PngSpace::Srgb);
}
static TransferJob makeJob(const Scene &src, const Scene &dst, const QString &out) {
    TransferJob j;
    j.source = std::make_shared<Scene>(src);
    j.target = std::make_shared<Scene>(dst);
    j.outputDir = out;
    j.suffixes = defaultSuffixes();
    j.settings.overwrite = true;
    j.settings.padding = 4;
    return j;
}
struct Stats {
    double mean = 0, max = 0;
    int count = 0;
};
static Stats compareField(const Scene &dst, int uvSet, const QString &path) {
    QImage img = QImage(path).convertToFormat(QImage::Format_Grayscale16);
    check(!img.isNull(), "cannot read " + path);
    Stats st;
    int res = img.width();
    for (int y = 0; y < res; y++) {
        const uint16_t *row = (const uint16_t *)img.constScanLine(y);
        for (int x = 0; x < res; x++) {
            int t;
            float u, v;
            if (!interior(dst, uvSet, x, y, res, t, u, v))
                continue;
            double e = std::abs(row[x] / 65535.0 - field(blend(dst.tris[t].p, u, v)));
            st.mean += e;
            st.max = std::max(st.max, e);
            st.count++;
        }
    }
    st.mean /= std::max(1, st.count);
    return st;
}
static Stats compareNormals(const Scene &dst, int uvSet, const QString &path) {
    std::vector<V3> tan;
    std::vector<float> sg;
    meshTangents(dst, uvSet, tan, sg);
    QImage img = QImage(path).convertToFormat(QImage::Format_RGBA64);
    check(!img.isNull(), "cannot read " + path);
    Stats st;
    int res = img.width();
    for (int y = 0; y < res; y++) {
        const uint16_t *row = (const uint16_t *)img.constScanLine(y);
        for (int x = 0; x < res; x++) {
            int t;
            float u, v;
            if (!interior(dst, uvSet, x, y, res, t, u, v))
                continue;
            V3 c{row[x * 4] / 32767.5f - 1, row[x * 4 + 1] / 32767.5f - 1, row[x * 4 + 2] / 32767.5f - 1};
            V3 T, B, N;
            frameAt(dst, tan, sg, t, u, v, T, B, N);
            V3 w = unit(T * c.x + B * c.y + N * c.z);
            V3 expect = normalField(blend(dst.tris[t].p, u, v), unit(N));
            double deg = acos(std::min(1.f, dot(w, expect))) * 57.29578;
            st.mean += deg;
            st.max = std::max(st.max, deg);
            st.count++;
        }
    }
    st.mean /= std::max(1, st.count);
    return st;
}
static TransferReport run(const TransferJob &j, TransferBackend b) {
    std::atomic_bool cancel{false};
    return runTransfer(j, b, cancel);
}
void transferTests(const QString &artifacts, QJsonObject &out) {
    QString dir = QDir(artifacts).absoluteFilePath("transfer");
    QDir().mkpath(dir + "/src");
    QJsonArray results;
    auto record = [&](const char *name, const Stats &s, const char *unit) {
        results.append(QJsonObject{{"test", name}, {"mean", s.mean}, {"max", s.max}, {"texels", s.count}, {"unit", unit}});
        std::cout << "PASS transfer " << name << " mean " << s.mean << " max " << s.max << " " << unit << " ("
                  << s.count << " texels)" << std::endl;
    };

    // 1. Identity: same mesh, same UVs, one centred sample per texel reproduces the source exactly.
    {
        Scene s = gridScene(8, false, .3f);
        int res = 64;
        std::vector<uint16_t> q((size_t)res * res * 3);
        uint32_t seed = 7;
        for (auto &v : q)
            v = (uint16_t)(random01(seed) * 255.99f);
        writePng(dir + "/src/ident_ALB.png", res, res, 3, 8, q.data(), PngSpace::Srgb);
        TransferJob j = makeJob(s, s, dir + "/identity");
        j.sets = {{"ident", {{"ALB", dir + "/src/ident_ALB.png"}}}};
        j.materialSets = {{"mat", "ident"}};
        j.outputs = {{"mat", true, "ident", res}};
        j.settings.ss = 1;
        auto report = run(j, TransferBackend::Gpu);
        check(report.exactTriangles == report.targetTriangles, "identity topology was not matched exactly");
        QImage a(dir + "/src/ident_ALB.png"), b(dir + "/identity/ident_ALB.png");
        a = a.convertToFormat(QImage::Format_RGB888);
        b = b.convertToFormat(QImage::Format_RGB888);
        Stats st;
        for (int y = 0; y < res; y++)
            for (int x = 0; x < res; x++) {
                int t;
                float u, v;
                if (!locate(s, 0, texelUV(x, y, res), t, u, v))
                    continue;
                for (int c = 0; c < 3; c++) {
                    double e = std::abs(a.constScanLine(y)[x * 3 + c] - b.constScanLine(y)[x * 3 + c]);
                    st.mean += e;
                    st.max = std::max(st.max, e);
                }
                st.count++;
            }
        st.mean /= std::max(1, st.count * 3);
        check(st.count > res * res / 2 && st.max <= 1 && st.mean < .01, "identity transfer is not lossless");
        record("identity", st, "8-bit levels");
    }

    // 2. Analytic field through rotated/mirrored islands with exact topology.
    Scene src = gridScene(16, false, .35f), dst = src;
    writeFieldTexture(src, 0, 256, dir + "/src/field_RGH.png");
    writeNormalTexture(src, 0, 256, dir + "/src/field_NRM.png");
    TransferJob j = makeJob(src, dst, dir + "/exact");
    j.sets = {{"field", {{"RGH", dir + "/src/field_RGH.png"}, {"NRM", dir + "/src/field_NRM.png"}}}};
    j.materialSets = {{"mat", "field"}};
    j.outputs = {{"mat", true, "field", 256}};
    j.settings.sourceUV = 0;
    j.settings.targetUV = 1;
    j.settings.coverageMap = true;
    auto exact = run(j, TransferBackend::Gpu);
    check(exact.exactTriangles == exact.targetTriangles, "UV-only change was not matched exactly");
    check(exact.missedTexels == 0, "exact transfer missed texels");
    Stats st = compareField(dst, 1, dir + "/exact/field_RGH.png");
    check(st.count > 10000 && st.mean < .002 && st.max < .02, "analytic field error too large (exact)");
    record("analytic_exact", st, "unorm");
    Stats nrm = compareNormals(dst, 1, dir + "/exact/field_NRM.png");
    check(nrm.count > 10000 && nrm.mean < .5 && nrm.max < 4, "tangent-space normal error too large");
    record("normals_mirrored_islands", nrm, "degrees");

    // 3. DirectX output: green channel must be inverted relative to the OpenGL result.
    {
        TransferJob dx = j;
        dx.outputDir = dir + "/directx";
        dx.settings.targetNormalDX = true;
        dx.settings.coverageMap = false;
        run(dx, TransferBackend::Gpu);
        QImage gl = QImage(dir + "/exact/field_NRM.png").convertToFormat(QImage::Format_RGBA64);
        QImage d = QImage(dir + "/directx/field_NRM.png").convertToFormat(QImage::Format_RGBA64);
        double err = 0;
        int n = 0;
        for (int y = 0; y < 256; y++)
            for (int x = 0; x < 256; x++) {
                int t;
                float u, v;
                if (!interior(dst, 1, x, y, 256, t, u, v))
                    continue;
                auto a = ((const uint16_t *)gl.constScanLine(y))[x * 4 + 1];
                auto b = ((const uint16_t *)d.constScanLine(y))[x * 4 + 1];
                err += std::abs((a + b) / 65535.0 - 1);
                n++;
            }
        check(n > 0 && err / n < .001, "DirectX green flip is wrong");
        record("normals_directx", Stats{err / n, 0, n}, "unorm");
    }

    // 4. Projection: retriangulated target (different diagonals) cannot match topology.
    Scene retri = gridScene(16, true, .35f);
    TransferJob p = makeJob(src, retri, dir + "/projected");
    p.sets = j.sets;
    p.materialSets = j.materialSets;
    p.outputs = j.outputs;
    p.settings.targetUV = 1;
    auto projected = run(p, TransferBackend::Gpu);
    check(projected.exactTriangles == 0, "retriangulated faces matched exactly");
    check(projected.missedTexels == 0, "projection missed texels");
    st = compareField(retri, 1, dir + "/projected/field_RGH.png");
    check(st.count > 10000 && st.mean < .003 && st.max < .03, "analytic field error too large (projection)");
    record("analytic_projected", st, "unorm");
    nrm = compareNormals(retri, 1, dir + "/projected/field_NRM.png");
    check(nrm.mean < 1 && nrm.max < 6, "projected normal error too large");
    record("normals_projected", nrm, "degrees");

    // 5. GPU/CPU parity on a small projected job.
    {
        TransferJob g = p;
        g.outputs = {{"mat", true, "parity", 64}};
        g.outputDir = dir + "/parity_gpu";
        run(g, TransferBackend::Gpu);
        g.outputDir = dir + "/parity_cpu";
        run(g, TransferBackend::Cpu);
        for (const char *sx : {"RGH", "NRM"}) {
            QImage a = QImage(dir + "/parity_gpu/parity_" + sx + ".png").convertToFormat(QImage::Format_RGBA64);
            QImage b = QImage(dir + "/parity_cpu/parity_" + sx + ".png").convertToFormat(QImage::Format_RGBA64);
            check(a.size() == b.size() && !a.isNull(), "parity images missing");
            Stats ps;
            for (int y = 0; y < a.height(); y++)
                for (int x = 0; x < a.width() * 4; x++) {
                    double e = std::abs(((const uint16_t *)a.constScanLine(y))[x] -
                                        ((const uint16_t *)b.constScanLine(y))[x]) /
                               65535.0;
                    ps.mean += e;
                    ps.max = std::max(ps.max, e);
                    ps.count++;
                }
            ps.mean /= std::max(1, ps.count);
            check(ps.mean < 1e-3 && ps.max < .02, QString("GPU/CPU mismatch for ") + sx);
            record(sx[0] == 'R' ? "gpu_cpu_parity_linear" : "gpu_cpu_parity_normal", ps, "unorm");
        }
    }

    // 6. Name pairing: two coincident objects with different textures; projection must stay on its pair.
    {
        Scene a = gridScene(4, false, 0, true), b = gridScene(4, true, 0, true);
        // Stack both objects on the same spot so only pairing can tell them apart.
        for (auto *s : {&a, &b})
            for (auto &t : s->tris)
                if (t.object == 1)
                    for (auto &pt : t.p)
                        pt.x -= 1;
        writeConstant(dir + "/src/white_ALB.png", 255);
        writeConstant(dir + "/src/black_ALB.png", 0);
        TransferJob q = makeJob(a, b, dir + "/pairing");
        q.sets = {{"white", {{"ALB", dir + "/src/white_ALB.png"}}}, {"black", {{"ALB", dir + "/src/black_ALB.png"}}}};
        q.materialSets = {{"matLeft", "white"}, {"matRight", "black"}};
        q.pairs = guessPairs(b.objects, a.objects);
        check(q.pairs.value("left") == "left" && q.pairs.value("right") == "right", "pair guessing failed");
        q.outputs = {{"matLeft", true, "left", 32}, {"matRight", true, "right", 32}};
        q.settings.mode = 1;
        q.settings.padding = 0;
        run(q, TransferBackend::Gpu);
        auto mean = [&](const QString &file) {
            QImage img = QImage(file).convertToFormat(QImage::Format_RGB888);
            double sum = 0;
            int n = 0;
            for (int y = 0; y < img.height(); y++)
                for (int x = 0; x < img.width(); x++) {
                    int t;
                    float u, v;
                    if (!locate(b, 0, texelUV(x, y, img.width()), t, u, v))
                        continue;
                    if ((b.tris[t].object == 0) != file.contains("left"))
                        continue;
                    sum += img.constScanLine(y)[x * 3];
                    n++;
                }
            return n ? sum / n : -1;
        };
        double left = mean(dir + "/pairing/left_ALB.png"), right = mean(dir + "/pairing/right_ALB.png");
        check(left > 254.5 && right >= 0 && right < .5, "pairing leaked texels between coincident objects");
        record("pairing", Stats{left - right, 0, 1}, "8-bit difference");
    }

    // 7. Discovery and naming helpers.
    {
        QStringList unknown;
        auto sets = discoverSets({"C:/t/doll01_ALB.png", "C:/t/doll01_NRM.png", "C:/t/doll02-ALB.png",
                                  "C:/t/doll03_BASECOLOR.png", "C:/t/notes.png"},
                                 defaultSuffixes(), &unknown);
        check(sets.size() == 3 && sets[0].files.size() == 2 && unknown.size() == 1, "set discovery failed");
        check(sets[2].files.contains("BASECOLOR"), "longest suffix not preferred");
        check(suggestPrefix({"doll01", "doll02", "doll03"}) == "doll01-02-03", "prefix suggestion failed");
        auto guess = guessMaterialSets({"M_doll01", "doll02_mat", "other"}, {"doll01", "doll02"});
        check(guess["M_doll01"] == "doll01" && guess["doll02_mat"] == "doll02" && guess["other"].isEmpty(),
              "material guessing failed");
        auto pairs = guessPairs({"Body_low", "Hat.001", "Extra"}, {"body_high", "Hat", "Other"});
        check(pairs["Body_low"] == "body_high" && pairs["Hat.001"] == "Hat" && pairs["Extra"] == "*",
              "pair guessing failed");
        TransferJob round = TransferJob::fromJson(j.toJson(), dir);
        check(round.sets.size() == 1 && round.outputs.size() == 1 && round.settings.targetUV == 1,
              "job JSON round trip failed");
    }
    out["transfer"] = results;
    std::cout << "All texture transfer tests passed." << std::endl;
}
