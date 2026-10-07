#include "core.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <algorithm>
#include <cuda_runtime.h>
#include <optix.h>
#include <optix_function_table_definition.h>
#include <optix_stack_size.h>
#include <optix_stubs.h>
#include <stdexcept>
static void cudaCheck(cudaError_t e) {
    if (e != cudaSuccess)
        throw std::runtime_error(
            std::string("CUDA: ") + cudaGetErrorString(e) +
            (e == cudaErrorMemoryAllocation
                 ? ". Reduce resolution or supersampling and close other GPU applications."
                 : ". Check the NVIDIA driver and compatible GPU."));
}
static void ox(OptixResult e) {
    if (e != OPTIX_SUCCESS)
        throw std::runtime_error(std::string("OptiX: ") + optixGetErrorName(e) +
                                 ". Install a current NVIDIA driver supporting OptiX 9.1 (R590 or newer). "
                                 "See diagnostic log if supplied.");
}
struct Buffer {
    void *ptr = nullptr;
    size_t size = 0;
    Buffer() = default;
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    ~Buffer() {
        if (ptr)
            cudaFree(ptr);
    }
    void alloc(size_t n) {
        if (ptr)
            cudaCheck(cudaFree(ptr));
        ptr = nullptr;
        size = n;
        if (n)
            cudaCheck(cudaMalloc(&ptr, n));
    }
    void upload(const void *data, size_t n) {
        alloc(n);
        if (n)
            cudaCheck(cudaMemcpy(ptr, data, n, cudaMemcpyHostToDevice));
    }
    template <class T> void upload(const std::vector<T> &v) { upload(v.data(), v.size() * sizeof(T)); }
    CUdeviceptr address() const { return reinterpret_cast<CUdeviceptr>(ptr); }
};
struct Engine {
    OptixDeviceContext context = nullptr;
    OptixModule module = nullptr;
    OptixPipeline pipeline = nullptr;
    OptixProgramGroup rg = nullptr, ms = nullptr, hg = nullptr;
    ~Engine() {
        if (pipeline)
            optixPipelineDestroy(pipeline);
        if (rg)
            optixProgramGroupDestroy(rg);
        if (ms)
            optixProgramGroupDestroy(ms);
        if (hg)
            optixProgramGroupDestroy(hg);
        if (module)
            optixModuleDestroy(module);
        if (context)
            optixDeviceContextDestroy(context);
    }
};
struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) Record {
    char header[OPTIX_SBT_RECORD_HEADER_SIZE];
};
Result bakeGpu(const Scene &scene, const Mask &mask, const Settings &s, std::atomic_bool &cancel,
               Progress progress) {
    QElapsedTimer clock;
    clock.start();
    auto report = [&](float f, const QString &m) {
        if (cancel)
            throw std::runtime_error("Bake cancelled.");
        if (progress)
            progress(f, m);
    };
    report(0, "Validating UVs and preparing geometry…");
    auto p = prepare(scene, s);
    if (mask.w <= 0 || mask.h <= 0 || mask.values.size() != (size_t)mask.w * mask.h)
        throw std::runtime_error("Load a valid opacity mask first.");
    int count = 0;
    cudaCheck(cudaGetDeviceCount(&count));
    if (count == 0)
        throw std::runtime_error("No NVIDIA CUDA GPU found.");
    int best = 0;
    size_t bestMem = 0;
    cudaDeviceProp props{};
    for (int i = 0; i < count; i++) {
        cudaDeviceProp d;
        cudaCheck(cudaGetDeviceProperties(&d, i));
        if (d.totalGlobalMem > bestMem) {
            best = i;
            bestMem = d.totalGlobalMem;
            props = d;
        }
    }
    cudaCheck(cudaSetDevice(best));
    cudaCheck(cudaFree(nullptr));
    ox(optixInit());
    Engine e;
    OptixDeviceContextOptions co = {};
    ox(optixDeviceContextCreate(nullptr, &co, &e.context));
    QFile code(QCoreApplication::applicationDirPath() + "/bake.ptx");
    if (!code.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing bake.ptx. Keep the portable app folder intact.");
    auto ptx = code.readAll();
    OptixModuleCompileOptions mo = {};
    mo.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    mo.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;
    OptixPipelineCompileOptions po = {};
    po.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    po.numPayloadValues = 2;
    po.numAttributeValues = 2;
    po.pipelineLaunchParamsVariableName = "params";
    po.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE;
    char log[8192];
    size_t logSize = sizeof(log);
    auto status =
        optixModuleCreate(e.context, &mo, &po, ptx.constData(), ptx.size(), log, &logSize, &e.module);
    if (status != OPTIX_SUCCESS)
        throw std::runtime_error(std::string("OptiX module: ") + log);
    OptixProgramGroupOptions go = {};
    OptixProgramGroupDesc desc = {};
    desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    desc.raygen.module = e.module;
    desc.raygen.entryFunctionName = "__raygen__bake";
    logSize = sizeof(log);
    ox(optixProgramGroupCreate(e.context, &desc, 1, &go, log, &logSize, &e.rg));
    desc = {};
    desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    desc.miss.module = e.module;
    desc.miss.entryFunctionName = "__miss__ao";
    logSize = sizeof(log);
    ox(optixProgramGroupCreate(e.context, &desc, 1, &go, log, &logSize, &e.ms));
    desc = {};
    desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    desc.hitgroup.moduleAH = e.module;
    desc.hitgroup.entryFunctionNameAH = "__anyhit__opacity";
    logSize = sizeof(log);
    ox(optixProgramGroupCreate(e.context, &desc, 1, &go, log, &logSize, &e.hg));
    OptixProgramGroup groups[] = {e.rg, e.ms, e.hg};
    OptixPipelineLinkOptions link = {};
    link.maxTraceDepth = 1;
    logSize = sizeof(log);
    ox(optixPipelineCreate(e.context, &po, &link, groups, 3, log, &logSize, &e.pipeline));
    OptixStackSizes stacks = {};
    for (auto pg : groups)
        ox(optixUtilAccumulateStackSizes(pg, &stacks, e.pipeline));
    uint32_t dt, ds, cs;
    ox(optixUtilComputeStackSizes(&stacks, 1, 0, 0, &dt, &ds, &cs));
    ox(optixPipelineSetStackSize(e.pipeline, dt, ds, cs, 1));
    Buffer triangles, maskData, offsets, indices, verts, accel, scratch, rg, ms, hg, accum, owners, param;
    triangles.upload(p.tris);
    maskData.upload(mask.values);
    offsets.upload(p.offsets);
    indices.upload(p.indices);
    std::vector<V3> vertices;
    vertices.reserve(p.tris.size() * 3);
    for (auto &t : p.tris)
        for (auto pt : t.p)
            vertices.push_back(pt);
    verts.upload(vertices);
    unsigned flags = OPTIX_GEOMETRY_FLAG_REQUIRE_SINGLE_ANYHIT_CALL;
    CUdeviceptr vb = verts.address();
    OptixBuildInput bi = {};
    bi.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
    bi.triangleArray.vertexBuffers = &vb;
    bi.triangleArray.numVertices = (unsigned)vertices.size();
    bi.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
    bi.triangleArray.vertexStrideInBytes = sizeof(V3);
    bi.triangleArray.flags = &flags;
    bi.triangleArray.numSbtRecords = 1;
    OptixAccelBuildOptions abo = {};
    abo.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
    abo.operation = OPTIX_BUILD_OPERATION_BUILD;
    OptixAccelBufferSizes sizes;
    ox(optixAccelComputeMemoryUsage(e.context, &abo, &bi, 1, &sizes));
    scratch.alloc(sizes.tempSizeInBytes);
    accel.alloc(sizes.outputSizeInBytes);
    OptixTraversableHandle handle;
    report(.01f, "Building RTX acceleration structure…");
    ox(optixAccelBuild(e.context, 0, &abo, &bi, 1, scratch.address(), scratch.size, accel.address(),
                       accel.size, &handle, nullptr, 0));
    cudaCheck(cudaDeviceSynchronize());
    size_t buildScratch = scratch.size;
    scratch.alloc(0);
    Record rec;
    ox(optixSbtRecordPackHeader(e.rg, &rec));
    rg.upload(&rec, sizeof(rec));
    ox(optixSbtRecordPackHeader(e.ms, &rec));
    ms.upload(&rec, sizeof(rec));
    ox(optixSbtRecordPackHeader(e.hg, &rec));
    hg.upload(&rec, sizeof(rec));
    OptixShaderBindingTable sbt = {};
    sbt.raygenRecord = rg.address();
    sbt.missRecordBase = ms.address();
    sbt.missRecordStrideInBytes = sizeof(rec);
    sbt.missRecordCount = 1;
    sbt.hitgroupRecordBase = hg.address();
    sbt.hitgroupRecordStrideInBytes = sizeof(rec);
    sbt.hitgroupRecordCount = 1;
    size_t pixels = (size_t)s.resolution * s.resolution;
    accum.alloc(pixels * sizeof(Accum));
    owners.alloc(pixels * sizeof(int));
    cudaCheck(cudaMemset(accum.ptr, 0, accum.size));
    cudaCheck(cudaMemset(owners.ptr, 0x7f, owners.size));
    param.alloc(sizeof(LaunchParams));
    LaunchParams lp = {};
    lp.handle = handle;
    lp.tris = (GpuTri *)triangles.ptr;
    lp.mask = mask.view(s);
    lp.mask.data = (float *)maskData.ptr;
    lp.offsets = (int *)offsets.ptr;
    lp.indices = (int *)indices.ptr;
    lp.accum = (Accum *)accum.ptr;
    lp.owners = (int *)owners.ptr;
    lp.averageOverlaps = s.averageOverlaps;
    lp.resolution = s.resolution;
    lp.grid = p.grid;
    lp.ss = s.ss;
    lp.bias = s.bias ? s.bias : scene.scale * 1e-6f;
    lp.distance = s.distance ? s.distance : 1e30f;
    constexpr int batch = 4096, rayBatch = 32;
    int batches = ((int)pixels + batch - 1) / batch, passes = (s.rays + rayBatch - 1) / rayBatch;
    int done = 0;
    QElapsedTimer feedback;
    feedback.start();
    for (int start = 0; start < (int)pixels; start += batch) {
        lp.pixelStart = start;
        lp.pixelCount = std::min(batch, (int)pixels - start);
        for (int ray = 0; ray < s.rays; ray += rayBatch) {
            if (cancel)
                throw std::runtime_error("Bake cancelled.");
            lp.rayStart = ray;
            lp.rayCount = std::min(rayBatch, s.rays - ray);
            cudaCheck(cudaMemcpy(param.ptr, &lp, sizeof(lp), cudaMemcpyHostToDevice));
            ox(optixLaunch(e.pipeline, 0, param.address(), sizeof(lp), &sbt, lp.pixelCount, s.ss * s.ss, 1));
            cudaCheck(cudaDeviceSynchronize());
            done++;
            if (feedback.elapsed() > 150) {
                report(.02f + .94f * done / (batches * passes),
                       QString("Tracing opacity-aware rays · %1").arg(props.name));
                feedback.restart();
            }
        }
    }
    report(.97f, "Resolving subpixel coverage and padding UV islands…");
    std::vector<Accum> data(pixels);
    cudaCheck(cudaMemcpy(data.data(), accum.ptr, accum.size, cudaMemcpyDeviceToHost));
    Result result;
    result.resolution = s.resolution;
    result.pixels.resize(pixels);
    result.owners.resize(pixels);
    cudaCheck(cudaMemcpy(result.owners.data(), owners.ptr, owners.size, cudaMemcpyDeviceToHost));
    for (auto &owner : result.owners)
        if (owner == 0x7f7f7f7f)
            owner = -1;
    for (size_t i = 0; i < pixels; i++)
        result.pixels[i] = data[i].weight > 0 ? clamp01(data[i].sum / data[i].weight) : 1;
    padResult(result, s.padding, &cancel);
    result.seconds = clock.elapsed() / 1000.;
    result.device = props.name;
    result.gpuBytes = triangles.size + maskData.size + offsets.size + indices.size + verts.size + accel.size +
                      std::max(buildScratch, accum.size + owners.size) + param.size + 3 * sizeof(Record);
    report(1, "Bake complete.");
    return result;
}
