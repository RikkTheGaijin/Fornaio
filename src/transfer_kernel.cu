#include "transfer_shared.h"
#include <optix.h>
#include <optix_device.h>
extern "C" {
__constant__ XferParams params;
}
struct GpuTracer {
    __device__ bool operator()(V3 o, V3 n, int object, int &src, float &su, float &sv) const {
        float best = 1e30f;
        src = -1;
        for (int dir = 0; dir < 2; dir++) {
            // dir 0 looks inward (behind the target surface), dir 1 outward.
            V3 origin = dir ? o - n * params.eps : o + n * params.eps, d = dir ? n : n * -1.f;
            float tmax = (dir ? params.front : params.back) + params.eps;
            unsigned p0 = (unsigned)object, p1 = 0, p2 = 0, p3 = __float_as_uint(-1.f);
            optixTrace(params.handle, make_float3(origin.x, origin.y, origin.z), make_float3(d.x, d.y, d.z), 0.f,
                       tmax, 0.f, OptixVisibilityMask(255), OPTIX_RAY_FLAG_NONE, 0, 1, 0, p0, p1, p2, p3);
            float t = __uint_as_float(p3);
            if (t >= 0 && fabsf(t - params.eps) < best) {
                best = fabsf(t - params.eps);
                src = (int)p0;
                su = __uint_as_float(p1);
                sv = __uint_as_float(p2);
            }
        }
        return src >= 0;
    }
};
extern "C" __global__ void __raygen__transfer() {
    int i = optixGetLaunchIndex().x;
    if (i >= params.texelCount)
        return;
    XAccum a;
    shadeTexel(params, params.texelStart + i, GpuTracer{}, a);
    params.accum[i] = a;
}
extern "C" __global__ void __miss__transfer() {}
// Rejects source objects that are not paired with the target object being traced.
extern "C" __global__ void __anyhit__transfer() {
    unsigned object = optixGetPayload_0();
    const XferSrcTri &s = params.src[optixGetPrimitiveIndex()];
    if (!params.allowed[(size_t)object * params.srcObjects + s.object])
        optixIgnoreIntersection();
}
extern "C" __global__ void __closesthit__transfer() {
    float2 b = optixGetTriangleBarycentrics();
    optixSetPayload_0(optixGetPrimitiveIndex());
    optixSetPayload_1(__float_as_uint(b.x));
    optixSetPayload_2(__float_as_uint(b.y));
    optixSetPayload_3(__float_as_uint(optixGetRayTmax()));
}
