#pragma once
#include "shared.h"
// Texture transfer: code shared by the OptiX kernel and the multithreaded CPU reference.
enum XferKind { KindColor = 0, KindLinear = 1, KindNormal = 2 };
enum XferFilter { FilterAuto = 0, FilterBicubic = 1, FilterBilinear = 2 };
constexpr int OwnerShared = -2;
struct V4 {
    float x = 0, y = 0, z = 0, w = 0;
};
HD inline V2 sub(V2 a, V2 b) {
    return {a.x - b.x, a.y - b.y};
}
HD inline V2 operator*(V2 a, float b) {
    return {a.x * b, a.y * b};
}
HD inline V2 operator+(V2 a, V2 b) {
    return {a.x + b.x, a.y + b.y};
}
HD inline int imin(int a, int b) {
    return a < b ? a : b;
}
HD inline V4 operator+(V4 a, V4 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
HD inline V4 operator*(V4 a, float b) {
    return {a.x * b, a.y * b, a.z * b, a.w * b};
}
HD inline V4 vmin(V4 a, V4 b) {
    return {fminf(a.x, b.x), fminf(a.y, b.y), fminf(a.z, b.z), fminf(a.w, b.w)};
}
HD inline V4 vmax(V4 a, V4 b) {
    return {fmaxf(a.x, b.x), fmaxf(a.y, b.y), fmaxf(a.z, b.z), fmaxf(a.w, b.w)};
}
// Target triangle in output UV space. perm maps each target corner to its source corner.
struct XferTri {
    V2 uv[3];
    V3 p[3], n[3], t[3];
    float sign[3];
    int match, perm[3];
    int object, island;
};
struct XferSrcTri {
    V2 uv[3];
    V3 p[3], n[3], t[3];
    float sign[3];
    int set, island, object;
};
// RGBA16 unorm texels in filter space: linear (sRGB decoded) and alpha-premultiplied for colour.
struct TexLevel {
    const uint16_t *data;
    int w, h;
};
struct TexSet {
    int firstLevel, levels;
    const int *owners;
    int present;
    V4 fill;
};
struct XAccum {
    V4 v;
    float weight, miss, projected;
    int owner;
};
struct XferParams {
    uint64_t handle;
    const XferTri *tris;
    const XferSrcTri *src;
    const int *offsets, *indices;
    const unsigned char *allowed;
    const TexSet *sets;
    const TexLevel *levels;
    XAccum *accum;
    int grid, srcObjects, resolution, texelStart, texelCount, ss;
    int kind, filter, islandAware, averageOverlaps, projection, srcGreenDX, dstGreenDX;
    float front, back, eps;
};
HD inline float srgbToLinear(float c) {
    return c <= .04045f ? c / 12.92f : powf((c + .055f) / 1.055f, 2.4f);
}
HD inline float linearToSrgb(float c) {
    return c <= .0031308f ? c * 12.92f : 1.055f * powf(c, 1 / 2.4f) - .055f;
}
HD inline int wrapIndex(int i, int n) {
    i %= n;
    return i < 0 ? i + n : i;
}
// UV tile holding a sample. Filter taps are clamped to that tile, so a 0-1 texture never bleeds
// across its opposite border, while UVs in other tiles still repeat the texture.
struct Tile {
    int x, y;
};
HD inline int tileOrigin(float u) {
    float f = floorf(u);
    if (f == u && u > 0)
        f -= 1; // an exact upper border belongs to the tile below it
    return (int)fminf(fmaxf(f, -100000.f), 100000.f);
}
HD inline Tile tileOf(V2 uv) {
    return {tileOrigin(uv.x), tileOrigin(1 - uv.y)};
}
HD inline size_t texelIndex(const TexLevel &l, Tile t, int x, int y) {
    int x0 = t.x * l.w, y0 = t.y * l.h;
    x = x < x0 ? x0 : imin(x, x0 + l.w - 1);
    y = y < y0 ? y0 : imin(y, y0 + l.h - 1);
    return (size_t)wrapIndex(y, l.h) * l.w + wrapIndex(x, l.w);
}
HD inline V4 fetch(const TexLevel &l, Tile t, int x, int y) {
    const uint16_t *d = l.data + texelIndex(l, t, x, y) * 4;
    const float s = 1.f / 65535.f;
    return {d[0] * s, d[1] * s, d[2] * s, d[3] * s};
}
HD inline bool owned(const int *owners, const TexLevel &l, Tile t, int x, int y, int island) {
    if (!owners)
        return true;
    int o = owners[texelIndex(l, t, x, y)];
    return o == island || o == OwnerShared;
}
HD inline V2 texelCoord(const TexLevel &l, V2 uv) {
    return {uv.x * l.w - .5f, (1 - uv.y) * l.h - .5f};
}
HD inline V4 sampleNearest(const TexLevel &l, V2 uv) {
    V2 c = texelCoord(l, uv);
    return fetch(l, tileOf(uv), (int)floorf(c.x + .5f), (int)floorf(c.y + .5f));
}
HD inline bool sampleBilinear(const TexLevel &l, const int *owners, int island, V2 uv, V4 &out) {
    V2 c = texelCoord(l, uv);
    Tile tile = tileOf(uv);
    int ix = (int)floorf(c.x), iy = (int)floorf(c.y);
    float fx = c.x - ix, fy = c.y - iy, sum = 0;
    V4 acc;
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            float w = (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
            if (w <= 0 || !owned(owners, l, tile, ix + i, iy + j, island))
                continue;
            acc = acc + fetch(l, tile, ix + i, iy + j) * w;
            sum += w;
        }
    if (sum < 1e-6f)
        return false;
    out = acc * (1 / sum);
    return true;
}
HD inline void catmullRom(float t, float *w) {
    float t2 = t * t, t3 = t2 * t;
    w[0] = .5f * (-t3 + 2 * t2 - t);
    w[1] = .5f * (3 * t3 - 5 * t2 + 2);
    w[2] = .5f * (-3 * t3 + 4 * t2 + t);
    w[3] = .5f * (t3 - t2);
}
// Catmull-Rom, clamped to the central 2x2 range to suppress ringing at hard edges.
HD inline bool sampleBicubic(const TexLevel &l, const int *owners, int island, V2 uv, V4 &out) {
    V2 c = texelCoord(l, uv);
    Tile tile = tileOf(uv);
    int ix = (int)floorf(c.x), iy = (int)floorf(c.y);
    float wx[4], wy[4], sum = 0;
    catmullRom(c.x - ix, wx);
    catmullRom(c.y - iy, wy);
    V4 acc, lo{1e30f, 1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f, -1e30f};
    bool inner = false;
    for (int j = 0; j < 4; j++)
        for (int i = 0; i < 4; i++) {
            int x = ix + i - 1, y = iy + j - 1;
            if (!owned(owners, l, tile, x, y, island))
                continue;
            V4 v = fetch(l, tile, x, y);
            float w = wx[i] * wy[j];
            acc = acc + v * w;
            sum += w;
            if ((i == 1 || i == 2) && (j == 1 || j == 2)) {
                lo = vmin(lo, v);
                hi = vmax(hi, v);
                inner = true;
            }
        }
    if (!inner || sum < .5f)
        return sampleBilinear(l, owners, island, uv, out);
    out = vmax(lo, vmin(hi, acc * (1 / sum)));
    return true;
}
// Elliptical weighted average (Heckbert / pbrt) with a Gaussian kernel.
HD inline bool sampleEwa(const TexLevel &l, const int *owners, int island, V2 uv, V2 a0, V2 a1, V4 &out) {
    V2 st = texelCoord(l, uv);
    Tile tile = tileOf(uv);
    a0 = {a0.x * l.w, a0.y * l.h};
    a1 = {a1.x * l.w, a1.y * l.h};
    float A = a0.y * a0.y + a1.y * a1.y + 1, B = -2 * (a0.x * a0.y + a1.x * a1.y),
          C = a0.x * a0.x + a1.x * a1.x + 1;
    float F = A * C - B * B * .25f;
    if (!(F > 0))
        return sampleBilinear(l, owners, island, uv, out);
    A /= F;
    B /= F;
    C /= F;
    float det = 4 * A * C - B * B, invDet = 1 / det;
    float du = 2 * invDet * sqrtf(det * C), dv = 2 * invDet * sqrtf(det * A);
    du = fminf(du, 64.f);
    dv = fminf(dv, 64.f);
    int s0 = (int)ceilf(st.x - du), s1 = (int)floorf(st.x + du);
    int t0 = (int)ceilf(st.y - dv), t1 = (int)floorf(st.y + dv);
    const float tail = expf(-2.f);
    V4 acc;
    float sum = 0;
    for (int t = t0; t <= t1; t++) {
        float v = t - st.y;
        for (int s = s0; s <= s1; s++) {
            float u = s - st.x, r2 = A * u * u + B * u * v + C * v * v;
            if (r2 >= 1 || !owned(owners, l, tile, s, t, island))
                continue;
            float w = expf(-2 * r2) - tail;
            acc = acc + fetch(l, tile, s, t) * w;
            sum += w;
        }
    }
    if (sum < 1e-6f)
        return sampleBilinear(l, owners, island, uv, out);
    out = acc * (1 / sum);
    return true;
}
HD inline float length2(V2 a) {
    return a.x * a.x + a.y * a.y;
}
// a0/a1: footprint axes of one target sample, in source UV units.
HD inline V4 sampleSet(const XferParams &P, const TexSet &set, int island, V2 uv, V2 a0, V2 a1) {
    if (!set.present)
        return set.fill;
    const TexLevel &base = P.levels[set.firstLevel];
    const int *owners = P.islandAware ? set.owners : nullptr;
    V4 out;
    if (P.filter == FilterBilinear) {
        if (sampleBilinear(base, owners, island, uv, out))
            return out;
        return sampleNearest(base, uv);
    }
    V2 t0{a0.x * base.w, a0.y * base.h}, t1{a1.x * base.w, a1.y * base.h};
    float l0 = sqrtf(length2(t0)), l1 = sqrtf(length2(t1));
    float major = fmaxf(l0, l1), minor = fminf(l0, l1);
    // Bicubic up to mild minification; EWA only where several source texels fall into one sample.
    if (P.filter == FilterBicubic || !(major > 1.5f)) {
        if (sampleBicubic(base, owners, island, uv, out))
            return out;
        return sampleNearest(base, uv);
    }
    const float maxAniso = 16;
    if (minor * maxAniso < major) {
        float scale = major / (minor * maxAniso);
        if (l0 < l1)
            a0 = a0 * scale;
        else
            a1 = a1 * scale;
        minor = major / maxAniso;
    }
    float lod = fmaxf(0.f, log2f(fmaxf(minor, 1e-8f)));
    int top = set.levels - 1;
    if (lod >= top) {
        const TexLevel &l = P.levels[set.firstLevel + top];
        if (sampleBilinear(l, nullptr, island, uv, out))
            return out;
        return sampleNearest(l, uv);
    }
    int i = (int)floorf(lod);
    float f = lod - i;
    V4 lo, hi;
    // Island ownership exists at full resolution only; mip levels are built from dilated texels.
    if (!sampleEwa(P.levels[set.firstLevel + i], i == 0 ? owners : nullptr, island, uv, a0, a1, lo))
        lo = sampleNearest(P.levels[set.firstLevel + i], uv);
    if (f <= 0)
        return lo;
    if (!sampleEwa(P.levels[set.firstLevel + i + 1], nullptr, island, uv, a0, a1, hi))
        hi = sampleNearest(P.levels[set.firstLevel + i + 1], uv);
    return lo * (1 - f) + hi * f;
}
// Derivative of source UV with respect to target UV: dUVs/dP * dP/dUVt (both meshes share world space).
HD inline void uvJacobian(const XferTri &t, const XferSrcTri &s, V2 &c0, V2 &c1) {
    V2 d1 = sub(t.uv[1], t.uv[0]), d2 = sub(t.uv[2], t.uv[0]);
    float det = d1.x * d2.y - d1.y * d2.x;
    c0 = c1 = {};
    if (fabsf(det) < 1e-20f)
        return;
    V3 e1 = t.p[1] - t.p[0], e2 = t.p[2] - t.p[0];
    // dP/du and dP/dv of the target parameterisation.
    V3 pu = (e1 * d2.y - e2 * d1.y) * (1 / det), pv = (e2 * d1.x - e1 * d2.x) * (1 / det);
    V3 f1 = s.p[1] - s.p[0], f2 = s.p[2] - s.p[0];
    float a = dot(f1, f1), b = dot(f1, f2), c = dot(f2, f2), g = a * c - b * b;
    if (fabsf(g) < 1e-30f)
        return;
    V2 s1 = sub(s.uv[1], s.uv[0]), s2 = sub(s.uv[2], s.uv[0]);
    auto map = [&](V3 d) {
        // Least-squares barycentric step in the source plane, then to source UV.
        float x = dot(f1, d), y = dot(f2, d);
        float wa = (c * x - b * y) / g, wb = (a * y - b * x) / g;
        return s1 * wa + s2 * wb;
    };
    c0 = map(pu);
    c1 = map(pv);
}
HD inline V3 tangentFrameNormal(const V3 *t, const V3 *n, const float *sign, float u, float v, V3 &T, V3 &B) {
    V3 N = blend(n, u, v);
    T = blend(t, u, v);
    float sg = sign[0] * (1 - u - v) + sign[1] * u + sign[2] * v;
    B = cross(N, T) * (sg < 0 ? -1.f : 1.f);
    return N;
}
HD inline V3 normalToTarget(const XferParams &P, V4 encoded, const XferSrcTri &s, float su, float sv,
                            const XferTri &t, float tu, float tv) {
    V3 ts{encoded.x * 2 - 1, encoded.y * 2 - 1, encoded.z * 2 - 1};
    if (P.srcGreenDX)
        ts.y = -ts.y;
    V3 T, B;
    V3 N = tangentFrameNormal(s.t, s.n, s.sign, su, sv, T, B);
    V3 world = unit(T * ts.x + B * ts.y + N * ts.z);
    V3 Tt, Bt;
    V3 Nt = tangentFrameNormal(t.t, t.n, t.sign, tu, tv, Tt, Bt);
    V3 r0 = cross(Bt, Nt), r1 = cross(Nt, Tt), r2 = cross(Tt, Bt);
    float det = dot(Tt, r0);
    if (fabsf(det) < 1e-20f)
        return {0, 0, 1};
    V3 c = unit(V3{dot(world, r0), dot(world, r1), dot(world, r2)} * (1 / det));
    if (P.dstGreenDX)
        c.y = -c.y;
    return c;
}
// Trace(origin, normal, targetObject, &srcTri, &u, &v) finds the source surface for unmatched faces.
template <class Trace> HD inline void shadeTexel(const XferParams &P, int texel, Trace trace, XAccum &out) {
    out = {};
    out.owner = 0x7fffffff;
    int x = texel % P.resolution, y = texel / P.resolution;
    float footprint = 1.f / (P.resolution * P.ss);
    for (int sy = 0; sy < P.ss; sy++)
        for (int sx = 0; sx < P.ss; sx++) {
            V2 uv{(x + (sx + .5f) / P.ss) / P.resolution, 1 - (y + (sy + .5f) / P.ss) / P.resolution};
            int gx = imin((int)(uv.x * P.grid), P.grid - 1), gy = imin((int)(uv.y * P.grid), P.grid - 1);
            int cell = gy * P.grid + gx;
            for (int j = P.offsets[cell]; j < P.offsets[cell + 1]; j++) {
                const XferTri &t = P.tris[P.indices[j]];
                float u, v;
                if (!rasterBary(t.uv, uv, u, v))
                    continue;
                int src = -1;
                float su = 0, sv = 0;
                if (t.match >= 0) {
                    float w[3] = {1 - u - v, u, v}, ws[3] = {0, 0, 0};
                    for (int k = 0; k < 3; k++)
                        ws[t.perm[k]] = w[k];
                    src = t.match;
                    su = ws[1];
                    sv = ws[2];
                } else if (P.projection) {
                    if (trace(blend(t.p, u, v), unit(blend(t.n, u, v)), t.object, src, su, sv))
                        out.projected += 1;
                    else
                        src = -1;
                }
                if (src < 0) {
                    out.miss += 1;
                } else {
                    const XferSrcTri &s = P.src[src];
                    V2 a0, a1;
                    uvJacobian(t, s, a0, a1);
                    V4 value = sampleSet(P, P.sets[s.set], s.island, blend(s.uv, su, sv), a0 * footprint,
                                         a1 * footprint);
                    if (P.kind == KindNormal) {
                        V3 c = normalToTarget(P, value, s, su, sv, t, u, v);
                        value = {c.x, c.y, c.z, value.w};
                    }
                    out.v = out.v + value;
                    out.weight += 1;
                    out.owner = imin(out.owner, t.island);
                }
                if (!P.averageOverlaps)
                    break;
            }
        }
    if (out.weight <= 0)
        out.owner = -1;
}
