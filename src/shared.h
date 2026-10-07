#pragma once
#include <cmath>
#include <cstdint>
#ifdef __CUDACC__
#define HD __host__ __device__
#else
#define HD
#endif
struct V2 {
    float x = 0, y = 0;
};
struct V3 {
    float x = 0, y = 0, z = 0;
};
HD inline V3 operator+(V3 a, V3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
HD inline V3 operator-(V3 a, V3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
HD inline V3 operator*(V3 a, float b) {
    return {a.x * b, a.y * b, a.z * b};
}
HD inline float dot(V3 a, V3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
HD inline V3 cross(V3 a, V3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
HD inline V3 unit(V3 a) {
    float l = sqrtf(dot(a, a));
    return l > 1e-20f ? a * (1 / l) : V3{0, 0, 1};
}
HD inline float clamp01(float a) {
    return fminf(1, fmaxf(0, a));
}
HD inline V3 blend(const V3 *a, float u, float v) {
    return a[0] * (1 - u - v) + a[1] * u + a[2] * v;
}
HD inline V2 blend(const V2 *a, float u, float v) {
    return {a[0].x * (1 - u - v) + a[1].x * u + a[2].x * v, a[0].y * (1 - u - v) + a[1].y * u + a[2].y * v};
}
HD inline float orient(V2 a, V2 b, V2 c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}
HD inline bool bary(const V2 *t, V2 p, float &u, float &v) {
    float d = orient(t[0], t[1], t[2]);
    if (fabsf(d) < 1e-20f)
        return false;
    u = orient(t[0], p, t[2]) / d;
    v = orient(t[0], t[1], p) / d;
    return u >= 0 && v >= 0 && u + v <= 1;
}
struct GpuTri {
    V3 p[3], n[3];
    V2 maskUV[3], aoUV[3];
    int island;
    int opaque;
};
// Half-open triangle coverage: a shared tessellation edge belongs to exactly one face,
// for either UV winding. Real overlapping interiors are still sampled independently.
HD inline bool ownedEdge(V2 a, V2 b) {
    return b.y > a.y || (b.y == a.y && b.x < a.x);
}
HD inline bool rasterBary(const V2 *t, V2 p, float &u, float &v) {
    if (!bary(t, p, u, v))
        return false;
    bool ccw = orient(t[0], t[1], t[2]) > 0;
    if (u == 0 && !(ccw ? ownedEdge(t[2], t[0]) : ownedEdge(t[0], t[2])))
        return false;
    if (v == 0 && !(ccw ? ownedEdge(t[0], t[1]) : ownedEdge(t[1], t[0])))
        return false;
    if (u + v == 1 && !(ccw ? ownedEdge(t[1], t[2]) : ownedEdge(t[2], t[1])))
        return false;
    return true;
}
struct MaskView {
    const float *data;
    int w, h, repeat, flip;
};
HD inline int texIndex(int i, int n, int repeat) {
    return repeat ? ((i % n + n) % n) : (i < 0 ? 0 : (i >= n ? n - 1 : i));
}
HD inline float opacity(MaskView m, V2 uv) {
    float u = m.repeat ? uv.x - floorf(uv.x) : clamp01(uv.x);
    float v = m.repeat ? uv.y - floorf(uv.y) : clamp01(uv.y);
    float x = u * m.w - 0.5f, y = (m.flip ? v : 1 - v) * m.h - 0.5f;
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy;
    int x0 = texIndex(ix, m.w, m.repeat), x1 = texIndex(ix + 1, m.w, m.repeat),
        y0 = texIndex(iy, m.h, m.repeat), y1 = texIndex(iy + 1, m.h, m.repeat);
    return (m.data[y0 * m.w + x0] * (1 - fx) + m.data[y0 * m.w + x1] * fx) * (1 - fy) +
           (m.data[y1 * m.w + x0] * (1 - fx) + m.data[y1 * m.w + x1] * fx) * fy;
}
HD inline uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    return x ^ (x >> 16);
}
HD inline float random01(uint32_t &s) {
    s = hash32(s);
    return (s >> 8) * (1.f / 16777216.f);
}
HD inline V3 hemisphere(V3 n, uint32_t &seed) {
    float a = random01(seed), b = random01(seed);
    float r = sqrtf(a), p = 6.28318530718f * b;
    V3 t = unit(cross(fabsf(n.z) < .999f ? V3{0, 0, 1} : V3{1, 0, 0}, n));
    return t * (r * cosf(p)) + cross(n, t) * (r * sinf(p)) + n * sqrtf(1 - a);
}
struct Accum {
    float sum = 0, weight = 0;
};
struct LaunchParams {
    uint64_t handle;
    const GpuTri *tris;
    MaskView mask;
    const int *offsets;
    const int *indices;
    Accum *accum;
    int *owners;
    int resolution, grid, pixelStart, pixelCount, ss, rayStart, rayCount, averageOverlaps;
    float bias, distance;
};
