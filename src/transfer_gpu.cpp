#include "transfer_gpu.h"
#include <QCoreApplication>
#include <QFile>
#include <algorithm>
#include <cuda_runtime.h>
#include <optix.h>
#include <optix_stack_size.h>
#include <optix_stubs.h>
#include <stdexcept>

namespace {
void cudaCheck(cudaError_t e) {
    if (e != cudaSuccess)
        throw std::runtime_error(std::string("CUDA: ") + cudaGetErrorString(e) +
                                 (e == cudaErrorMemoryAllocation
                                      ? ". Reduce output resolution and close other GPU applications."
                                      : ". Check the NVIDIA driver and compatible GPU."));
}
void ox(OptixResult e) {
    if (e != OPTIX_SUCCESS)
        throw std::runtime_error(std::string("OptiX: ") + optixGetErrorName(e) +
                                 ". Install a current NVIDIA driver supporting OptiX 9.1 (R590 or newer).");
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
struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) Record {
    char header[OPTIX_SBT_RECORD_HEADER_SIZE];
};
} // namespace

struct TransferGpu::Impl {
    OptixDeviceContext context = nullptr;
    OptixModule module = nullptr;
    OptixPipeline pipeline = nullptr;
    OptixProgramGroup rg = nullptr, ms = nullptr, hg = nullptr;
    OptixTraversableHandle handle = 0;
    OptixShaderBindingTable sbt = {};
    Buffer src, tris, allowed, verts, accel, offsets, indices, accum, param, rgRec, msRec, hgRec, levels, sets;
    std::vector<std::unique_ptr<Buffer>> texels, owners;
    QString device;
    size_t buildScratch = 0, peak = 0;
    size_t current() const {
        size_t n = src.size + tris.size + allowed.size + verts.size + accel.size + offsets.size + indices.size +
                   accum.size + param.size + levels.size + sets.size + 3 * sizeof(Record);
        for (auto &b : texels)
            n += b->size;
        for (auto &b : owners)
            n += b->size;
        return n;
    }
    void track() { peak = std::max(peak, current()); }
    ~Impl() {
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

TransferGpu::TransferGpu(const std::vector<XferSrcTri> &src, const std::vector<unsigned char> &allowed,
                         const std::vector<XferTri> &targets)
    : d(new Impl) {
    int count = 0;
    cudaCheck(cudaGetDeviceCount(&count));
    if (count == 0)
        throw std::runtime_error("No NVIDIA CUDA GPU found.");
    int best = 0;
    size_t bestMem = 0;
    cudaDeviceProp props{};
    for (int i = 0; i < count; i++) {
        cudaDeviceProp p;
        cudaCheck(cudaGetDeviceProperties(&p, i));
        if (p.totalGlobalMem > bestMem) {
            best = i;
            bestMem = p.totalGlobalMem;
            props = p;
        }
    }
    d->device = props.name;
    cudaCheck(cudaSetDevice(best));
    cudaCheck(cudaFree(nullptr));
    ox(optixInit());
    OptixDeviceContextOptions co = {};
    ox(optixDeviceContextCreate(nullptr, &co, &d->context));
    QFile code(QCoreApplication::applicationDirPath() + "/transfer.ptx");
    if (!code.open(QIODevice::ReadOnly))
        throw std::runtime_error("Missing transfer.ptx. Keep the portable app folder intact.");
    auto ptx = code.readAll();
    OptixModuleCompileOptions mo = {};
    mo.optLevel = OPTIX_COMPILE_OPTIMIZATION_DEFAULT;
    mo.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;
    OptixPipelineCompileOptions po = {};
    po.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_GAS;
    po.numPayloadValues = 4;
    po.numAttributeValues = 2;
    po.pipelineLaunchParamsVariableName = "params";
    po.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE;
    char log[8192];
    size_t logSize = sizeof(log);
    if (optixModuleCreate(d->context, &mo, &po, ptx.constData(), ptx.size(), log, &logSize, &d->module) !=
        OPTIX_SUCCESS)
        throw std::runtime_error(std::string("OptiX module: ") + log);
    OptixProgramGroupOptions go = {};
    OptixProgramGroupDesc desc = {};
    desc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
    desc.raygen.module = d->module;
    desc.raygen.entryFunctionName = "__raygen__transfer";
    logSize = sizeof(log);
    ox(optixProgramGroupCreate(d->context, &desc, 1, &go, log, &logSize, &d->rg));
    desc = {};
    desc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
    desc.miss.module = d->module;
    desc.miss.entryFunctionName = "__miss__transfer";
    logSize = sizeof(log);
    ox(optixProgramGroupCreate(d->context, &desc, 1, &go, log, &logSize, &d->ms));
    desc = {};
    desc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
    desc.hitgroup.moduleAH = d->module;
    desc.hitgroup.entryFunctionNameAH = "__anyhit__transfer";
    desc.hitgroup.moduleCH = d->module;
    desc.hitgroup.entryFunctionNameCH = "__closesthit__transfer";
    logSize = sizeof(log);
    ox(optixProgramGroupCreate(d->context, &desc, 1, &go, log, &logSize, &d->hg));
    OptixProgramGroup groups[] = {d->rg, d->ms, d->hg};
    OptixPipelineLinkOptions link = {};
    link.maxTraceDepth = 1;
    logSize = sizeof(log);
    ox(optixPipelineCreate(d->context, &po, &link, groups, 3, log, &logSize, &d->pipeline));
    OptixStackSizes stacks = {};
    for (auto pg : groups)
        ox(optixUtilAccumulateStackSizes(pg, &stacks, d->pipeline));
    uint32_t dt, ds, cs;
    ox(optixUtilComputeStackSizes(&stacks, 1, 0, 0, &dt, &ds, &cs));
    ox(optixPipelineSetStackSize(d->pipeline, dt, ds, cs, 1));

    d->src.upload(src);
    d->tris.upload(targets);
    d->allowed.upload(allowed);
    if (!src.empty()) {
        std::vector<V3> vertices;
        vertices.reserve(src.size() * 3);
        for (auto &t : src)
            for (auto p : t.p)
                vertices.push_back(p);
        d->verts.upload(vertices);
        unsigned flags = OPTIX_GEOMETRY_FLAG_NONE;
        CUdeviceptr vb = d->verts.address();
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
        ox(optixAccelComputeMemoryUsage(d->context, &abo, &bi, 1, &sizes));
        Buffer scratch;
        scratch.alloc(sizes.tempSizeInBytes);
        d->accel.alloc(sizes.outputSizeInBytes);
        ox(optixAccelBuild(d->context, 0, &abo, &bi, 1, scratch.address(), scratch.size, d->accel.address(),
                           d->accel.size, &d->handle, nullptr, 0));
        cudaCheck(cudaDeviceSynchronize());
        d->buildScratch = scratch.size;
    }
    Record rec;
    ox(optixSbtRecordPackHeader(d->rg, &rec));
    d->rgRec.upload(&rec, sizeof(rec));
    ox(optixSbtRecordPackHeader(d->ms, &rec));
    d->msRec.upload(&rec, sizeof(rec));
    ox(optixSbtRecordPackHeader(d->hg, &rec));
    d->hgRec.upload(&rec, sizeof(rec));
    d->sbt.raygenRecord = d->rgRec.address();
    d->sbt.missRecordBase = d->msRec.address();
    d->sbt.missRecordStrideInBytes = sizeof(rec);
    d->sbt.missRecordCount = 1;
    d->sbt.hitgroupRecordBase = d->hgRec.address();
    d->sbt.hitgroupRecordStrideInBytes = sizeof(rec);
    d->sbt.hitgroupRecordCount = 1;
    d->param.alloc(sizeof(XferParams));
    d->peak = d->current() + d->buildScratch;
}
TransferGpu::~TransferGpu() = default;
void TransferGpu::setGrid(const std::vector<int> &offsets, const std::vector<int> &indices) {
    d->offsets.upload(offsets);
    d->indices.upload(indices.empty() ? std::vector<int>{0} : indices);
    d->track();
}
void TransferGpu::setTextures(const std::vector<TexLevel> &levels, const std::vector<TexSet> &sets) {
    d->texels.clear();
    d->owners.clear();
    std::vector<TexLevel> deviceLevels = levels;
    for (size_t i = 0; i < levels.size(); i++) {
        d->texels.push_back(std::make_unique<Buffer>());
        d->texels.back()->upload(levels[i].data, (size_t)levels[i].w * levels[i].h * 4 * sizeof(uint16_t));
        deviceLevels[i].data = (const uint16_t *)d->texels.back()->ptr;
    }
    std::vector<TexSet> deviceSets = sets;
    for (size_t i = 0; i < sets.size(); i++) {
        if (!sets[i].present || !sets[i].owners)
            continue;
        const TexLevel &l = levels[sets[i].firstLevel];
        d->owners.push_back(std::make_unique<Buffer>());
        d->owners.back()->upload(sets[i].owners, (size_t)l.w * l.h * sizeof(int));
        deviceSets[i].owners = (const int *)d->owners.back()->ptr;
    }
    d->levels.upload(deviceLevels.empty() ? std::vector<TexLevel>{TexLevel{}} : deviceLevels);
    d->sets.upload(deviceSets);
    d->track();
}
void TransferGpu::launch(const XferParams &host, XAccum *out) {
    XferParams p = host;
    p.handle = d->handle;
    p.tris = (const XferTri *)d->tris.ptr;
    p.src = (const XferSrcTri *)d->src.ptr;
    p.offsets = (const int *)d->offsets.ptr;
    p.indices = (const int *)d->indices.ptr;
    p.allowed = (const unsigned char *)d->allowed.ptr;
    p.sets = (const TexSet *)d->sets.ptr;
    p.levels = (const TexLevel *)d->levels.ptr;
    // Projection needs a source BVH; without source faces every unmatched sample is a miss.
    if (!d->handle)
        p.projection = 0;
    size_t bytes = (size_t)p.texelCount * sizeof(XAccum);
    if (d->accum.size < bytes) {
        d->accum.alloc(bytes);
        d->track();
    }
    p.accum = (XAccum *)d->accum.ptr;
    cudaCheck(cudaMemcpy(d->param.ptr, &p, sizeof(p), cudaMemcpyHostToDevice));
    ox(optixLaunch(d->pipeline, 0, d->param.address(), sizeof(p), &d->sbt, p.texelCount, 1, 1));
    cudaCheck(cudaDeviceSynchronize());
    cudaCheck(cudaMemcpy(out, d->accum.ptr, bytes, cudaMemcpyDeviceToHost));
}
QString TransferGpu::device() const {
    return d->device;
}
size_t TransferGpu::peakBytes() const {
    return d->peak;
}
