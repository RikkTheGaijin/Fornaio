#include "shared.h"
#include <optix.h>
#include <optix_device.h>
extern "C" {
__constant__ LaunchParams params;
}
static __forceinline__ __device__ float3 f3(V3 a) {
    return make_float3(a.x, a.y, a.z);
}
extern "C" __global__ void __anyhit__opacity() {
    unsigned id = optixGetPrimitiveIndex();
    if (id == optixGetPayload_1()) {
        optixIgnoreIntersection();
        return;
    }
    float2 b = optixGetTriangleBarycentrics();
    const auto &t = params.tris[id];
    float a = t.opaque ? 1.f : opacity(params.mask, blend(t.maskUV, b.x, b.y));
    float transmission = __uint_as_float(optixGetPayload_0()) * (1 - a);
    optixSetPayload_0(__float_as_uint(transmission));
    if (transmission == 0) {
        optixTerminateRay();
        return;
    }
    optixIgnoreIntersection();
}
extern "C" __global__ void __miss__ao() {}
extern "C" __global__ void __raygen__bake() {
    uint3 launch = optixGetLaunchIndex();
    int pixel = params.pixelStart + launch.x;
    if ((int)launch.x >= params.pixelCount)
        return;
    int x = pixel % params.resolution, y = pixel / params.resolution, sample = launch.y,
        sx = sample % params.ss, sy = sample / params.ss;
    V2 uv{(x + (sx + .5f) / params.ss) / params.resolution,
          1 - (y + (sy + .5f) / params.ss) / params.resolution};
    int gx = min((int)(uv.x * params.grid), params.grid - 1),
        gy = min((int)(uv.y * params.grid), params.grid - 1), cell = gy * params.grid + gx;
    for (int j = params.offsets[cell]; j < params.offsets[cell + 1]; j++) {
        int id = params.indices[j];
        const auto &t = params.tris[id];
        float u, v;
        if (!rasterBary(t.aoUV, uv, u, v))
            continue;
        float a = opacity(params.mask, blend(t.maskUV, u, v));
        if (a <= 0)
            continue;
        V3 g = unit(cross(t.p[1] - t.p[0], t.p[2] - t.p[0])), n = unit(blend(t.n, u, v));
        if (dot(g, n) < 0)
            g = g * -1;
        V3 origin = blend(t.p, u, v) + g * params.bias;
        float sum = 0;
        for (int r = 0; r < params.rayCount; r++) {
            int ray = params.rayStart + r;
            uint32_t seed = hash32(pixel * 9781U + sample * 6271U + ray * 13007U + 1);
            V3 d = hemisphere(n, seed);
            if (dot(d, g) < 0)
                d = d - g * (2 * dot(d, g));
            unsigned transmission = __float_as_uint(1.f), skip = id;
            optixTrace(params.handle, f3(origin), f3(d), 0.f, params.distance, 0.f, 255,
                       OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT, 0, 1, 0, transmission, skip);
            sum += __uint_as_float(transmission);
        }
        atomicAdd(&params.accum[pixel].sum, sum * a);
        atomicAdd(&params.accum[pixel].weight, params.rayCount * a);
        atomicMin(&params.owners[pixel], t.island);
        if (!params.averageOverlaps)
            return;
    }
}
