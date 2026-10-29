#include "ai_projection.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QSurfaceFormat>
#include <QThread>
#include <algorithm>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>

namespace {
void require(bool ok, const QString &message) {
    if (!ok)
        throw std::runtime_error(message.toStdString());
}
void cancelled(std::atomic_bool *flag) {
    if (flag && flag->load())
        throw std::runtime_error("Cancelled");
}

const char *cameraVertex = R"(#version 330 core
layout(location=0) in vec3 position;
uniform mat4 mvp;
out vec2 clipZW;
void main(){gl_Position=mvp*vec4(position,1.0);clipZW=gl_Position.zw;}
)";
const char *depthFragment = R"(#version 330 core
in vec2 clipZW;
layout(location=0) out vec4 result;
void main(){result=vec4(0.5*clipZW.x/clipZW.y+0.5,0.0,0.0,1.0);}
)";
const char *screenVertex = R"(#version 330 core
out vec2 screenUv;
void main(){
 vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);
 screenUv=p;gl_Position=vec4(p*2.0-1.0,0.0,1.0);
}
)";
const char *erosionFragment = R"(#version 330 core
uniform sampler2D sourceMap,depthMap;
in vec2 screenUv;
out vec4 result;
void main(){
 ivec2 size=textureSize(depthMap,0),p=ivec2(screenUv*vec2(size));
 for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x){
  ivec2 q=p+ivec2(x,y);
  if(any(lessThan(q,ivec2(0)))||any(greaterThanEqual(q,size))||texelFetch(depthMap,q,0).a<1.0){result=vec4(0);return;}
 }
 result=texture(sourceMap,screenUv);
}
)";
const char *projectionVertex = R"(#version 330 core
layout(location=0) in vec3 position;
layout(location=1) in vec3 normal;
layout(location=2) in vec2 uv;
uniform mat4 mvp;
out vec4 cameraClip;
out vec3 objectPosition,vertexNormal;
out vec2 atlasUv;
void main(){
 cameraClip=mvp*vec4(position,1.0);objectPosition=position;
 vertexNormal=normalize(normal);atlasUv=uv;
 gl_Position=vec4(uv*2.0-1.0,0.0,1.0);
}
)";
const char *projectionFragment = R"(#version 330 core
uniform sampler2D sourceMap,depthMap,cameraMask,uvMask;
uniform vec3 cameraDirection,cameraPosition;
uniform bool orthographic,overlay,hasCameraMask,hasUvMask;
uniform int depthMargin;
in vec4 cameraClip;
in vec3 objectPosition,vertexNormal;
in vec2 atlasUv;
out vec4 result;
void main(){
 if(cameraClip.w<=0.0)discard;
 vec2 uv=cameraClip.xy/cameraClip.w*0.5+0.5;
 if(any(lessThan(uv,vec2(0)))||any(greaterThan(uv,vec2(1))))discard;
 vec4 source=texture(sourceMap,uv);
 if(source.a==0.0)discard;
 vec3 dir=orthographic?cameraDirection:normalize(cameraPosition-objectPosition);
 float alpha=source.a;
 if(!overlay){
  float similarity=dot(vertexNormal,dir);
  if(similarity<0.01)discard;
  similarity*=smoothstep(0.0,0.5,1.0/cameraClip.w);
  alpha*=similarity*similarity;
 }
 if(hasCameraMask){float a=texture(cameraMask,uv).r;if(a<0.01)discard;alpha*=a;}
 if(hasUvMask){float a=texture(uvMask,atlasUv).r;if(a<0.01)discard;alpha*=a;}
 float depth=texture(depthMap,uv).r;
 float z=0.5*cameraClip.z/cameraClip.w+0.5;
 z-=(orthographic?0.00002:0.000001)*dot(normalize(vertexNormal),dir);
 if(z>depth||z<depth-0.0001)discard;
 if(depthMargin!=0){
  ivec2 size=textureSize(depthMap,0),p=ivec2(uv*vec2(size));
  for(int y=-depthMargin;y<=depthMargin;++y)for(int x=-depthMargin;x<=depthMargin;++x){
   if(x==0&&y==0)continue;
   ivec2 q=p+ivec2(x,y);
   if(any(lessThan(q,ivec2(0)))||any(greaterThanEqual(q,size)))discard;
   if(abs(texelFetch(depthMap,q,0).r-depth)>0.001)discard;
  }
 }
 if(alpha==0.0)discard;
 result=vec4(source.rgb/source.a,alpha);
}
)";
const char *uvVertex = R"(#version 330 core
layout(location=2) in vec2 uv;
void main(){gl_Position=vec4(uv*2.0-1.0,0.0,1.0);}
)";
const char *uvFragment = R"(#version 330 core
out vec4 result;
void main(){result=vec4(1);}
)";
const char *paddingFragment = R"(#version 330 core
uniform sampler2D projectionMap,islandMap;
in vec2 screenUv;
out vec4 result;
void main(){
 ivec2 size=textureSize(islandMap,0),p=ivec2(screenUv*vec2(size));
 if(texelFetch(islandMap,p,0).r>0.5){result=texelFetch(projectionMap,p,0);return;}
 int bestDistance=26;ivec2 best=p;
 for(int y=-5;y<=5;++y)for(int x=-5;x<=5;++x){
  int d=x*x+y*y;if(d>=bestDistance)continue;
  ivec2 q=p+ivec2(x,y);
  if(any(lessThan(q,ivec2(0)))||any(greaterThanEqual(q,size)))continue;
  if(texelFetch(islandMap,q,0).r>0.5){best=q;bestDistance=d;}
 }
 result=texelFetch(projectionMap,best,0);
}
)";

struct Vertex {
    float p[3], n[3], uv[2];
};

class ProjectionWorker final : public QObject, protected QOpenGLFunctions_3_3_Core {
    QOffscreenSurface *surface_;
    std::unique_ptr<QOpenGLContext> context_;
    std::unique_ptr<QOpenGLShaderProgram> depth_, erosion_, projection_, islands_, padding_;
    unsigned vao_ = 0, vbo_ = 0;

    std::unique_ptr<QOpenGLShaderProgram> program(const char *vertex, const char *fragment) {
        auto p = std::make_unique<QOpenGLShaderProgram>();
        const bool ok = p->addShaderFromSourceCode(QOpenGLShader::Vertex, vertex) &&
                        p->addShaderFromSourceCode(QOpenGLShader::Fragment, fragment) && p->link();
        require(ok, "Projection shader failed: " + p->log());
        return p;
    }
    // Every render job owns its transient GL resources. Cleanup also runs when a
    // cancellation or validation exception interrupts a stage.
    struct Resources {
        ProjectionWorker &gl;
        std::vector<unsigned> textures, framebuffers, renderbuffers;
        ~Resources() {
            gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
            if (!textures.empty())
                gl.glDeleteTextures(int(textures.size()), textures.data());
            if (!framebuffers.empty())
                gl.glDeleteFramebuffers(int(framebuffers.size()), framebuffers.data());
            if (!renderbuffers.empty())
                gl.glDeleteRenderbuffers(int(renderbuffers.size()), renderbuffers.data());
        }
    };
    unsigned texture(Resources &r, int width, int height, int internal, unsigned format, unsigned type,
                     const void *data = nullptr, bool linear = true) {
        unsigned id;
        glGenTextures(1, &id);
        r.textures.push_back(id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, internal, width, height, 0, format, type, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
        return id;
    }
    unsigned imageTexture(Resources &r, const QImage &input, bool premultiply, bool mask = false) {
        auto im = input
                      .convertToFormat(mask          ? QImage::Format_Grayscale8
                                       : premultiply ? QImage::Format_RGBA8888_Premultiplied
                                                     : QImage::Format_RGBA8888)
                      .mirrored();
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, im.bytesPerLine() / (mask ? 1 : 4));
        const unsigned t = texture(r, im.width(), im.height(), mask ? GL_R8 : GL_RGBA8,
                                   mask ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, im.constBits());
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        return t;
    }
    unsigned framebuffer(Resources &r, unsigned image, int width, int height, bool depth = false) {
        unsigned f;
        glGenFramebuffers(1, &f);
        r.framebuffers.push_back(f);
        glBindFramebuffer(GL_FRAMEBUFFER, f);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, image, 0);
        if (depth) {
            unsigned b;
            glGenRenderbuffers(1, &b);
            r.renderbuffers.push_back(b);
            glBindRenderbuffer(GL_RENDERBUFFER, b);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, b);
        }
        require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
                "Projection framebuffer is unavailable (OpenGL 3.3 / Float32 targets required)");
        glViewport(0, 0, width, height);
        return f;
    }
    void bindTexture(QOpenGLShaderProgram &p, const char *name, unsigned unit, unsigned texture) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, texture);
        p.setUniformValue(name, int(unit));
    }
    void drawStrips(int count, int height, std::atomic_bool *cancel) {
        glEnable(GL_SCISSOR_TEST);
        for (int y = 0; y < height; y += 128) {
            cancelled(cancel);
            glScissor(0, y, viewportWidth_, std::min(128, height - y));
            glDrawArrays(GL_TRIANGLES, 0, count);
        }
        glDisable(GL_SCISSOR_TEST);
    }
    int viewportWidth_ = 0;
    void clearTarget(int width, int height, bool depth = false) {
        viewportWidth_ = width;
        glViewport(0, 0, width, height);
        glClearColor(0, 0, 0, 0);
        glClearDepth(1);
        glClear(GL_COLOR_BUFFER_BIT | (depth ? GL_DEPTH_BUFFER_BIT : 0));
    }

  public:
    explicit ProjectionWorker(QOffscreenSurface *surface) : surface_(surface) {}
    void initialize() {
        context_ = std::make_unique<QOpenGLContext>();
        context_->setFormat(surface_->format());
        require(context_->create() && context_->makeCurrent(surface_),
                "Cannot create offscreen OpenGL projection context");
        require(initializeOpenGLFunctions(), "Projection requires OpenGL 3.3 core");
        depth_ = program(cameraVertex, depthFragment);
        erosion_ = program(screenVertex, erosionFragment);
        projection_ = program(projectionVertex, projectionFragment);
        islands_ = program(uvVertex, uvFragment);
        padding_ = program(screenVertex, paddingFragment);
        glGenVertexArrays(1, &vao_);
        glGenBuffers(1, &vbo_);
        context_->doneCurrent();
    }
    void destroy() {
        if (context_ && context_->makeCurrent(surface_)) {
            depth_.reset();
            erosion_.reset();
            projection_.reset();
            islands_.reset();
            padding_.reset();
            if (vao_)
                glDeleteVertexArrays(1, &vao_);
            if (vbo_)
                glDeleteBuffers(1, &vbo_);
            vao_ = vbo_ = 0;
            context_->doneCurrent();
        }
        context_.reset();
    }
    AiLayerContribution render(const Scene &scene, int uv, int resolution, const AiCamera &camera,
                               const QImage &source, const QImage &mask, const QImage &uvMask,
                               const QSet<QString> &materials, bool overlay, int margin,
                               std::atomic_bool *cancel) {
        require(context_ && context_->makeCurrent(surface_), "Projection context was lost");
        struct DoneCurrent {
            QOpenGLContext *context;
            ~DoneCurrent() { context->doneCurrent(); }
        } done{context_.get()};
        Resources resources{*this};
        glDisable(GL_MULTISAMPLE);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_DITHER);
        glDisable(GL_BLEND);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_TRUE);
        glDepthFunc(GL_LEQUAL);
        glFrontFace(GL_CCW);
        glCullFace(GL_BACK);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        std::vector<Vertex> vertices;
        vertices.reserve(scene.tris.size() * 3);
        QMap<QString, std::vector<Vertex>> byMaterial;
        for (const auto &triangle : scene.tris) {
            cancelled(cancel);
            require(uv >= 0 && uv < int(triangle.uv.size()), "Missing selected UV set");
            const QString material = aiMaterial(scene, triangle);
            for (int k = 0; k < 3; ++k) {
                const auto p = triangle.p[k], n = triangle.n[k];
                const auto t = triangle.uv[uv][k];
                Vertex v{{p.x, p.y, p.z}, {n.x, n.y, n.z}, {t.x, t.y}};
                vertices.push_back(v);
                if (materials.contains(material))
                    byMaterial[material].push_back(v);
            }
        }
        require(vertices.size() <= size_t(std::numeric_limits<int>::max()), "Projection mesh is too large");
        glBindVertexArray(vao_);
        glBindBuffer(GL_ARRAY_BUFFER, vbo_);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<const void *>(3 * sizeof(float)));
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                              reinterpret_cast<const void *>(6 * sizeof(float)));
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(Vertex), vertices.data(), GL_STREAM_DRAW);
        const int width = source.width(), height = source.height();
        // Alpha in this Float32 target doubles as the exact front-side silhouette.
        const auto depthTexture = texture(resources, width, height, GL_RGBA32F, GL_RGBA, GL_FLOAT);
        framebuffer(resources, depthTexture, width, height, true);
        clearTarget(width, height, true);
        glEnable(GL_DEPTH_TEST);
        glEnable(GL_CULL_FACE);
        depth_->bind();
        depth_->setUniformValue("mvp", camera.projection * camera.view);
        drawStrips(int(vertices.size()), height, cancel);
        depth_->release();
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        cancelled(cancel);
        const auto sourceTexture = imageTexture(resources, source, true);
        const auto erodedTexture = texture(resources, width, height, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
        framebuffer(resources, erodedTexture, width, height);
        clearTarget(width, height);
        erosion_->bind();
        bindTexture(*erosion_, "sourceMap", 0, sourceTexture);
        bindTexture(*erosion_, "depthMap", 1, depthTexture);
        drawStrips(3, height, cancel);
        erosion_->release();
        unsigned maskTexture = 0, uvMaskTexture = 0;
        if (!mask.isNull())
            maskTexture = imageTexture(resources, mask, false, true);
        if (!uvMask.isNull())
            uvMaskTexture = imageTexture(resources, uvMask, false, true);
        const bool parallel = camera.projection(3, 2) == 0 && camera.projection(3, 3) == 1;
        const auto inverseView = camera.view.inverted();
        const QVector3D direction = inverseView.mapVector(QVector3D(0, 0, 1)).normalized();
        const QVector3D eye = inverseView.map(QVector3D());
        AiLayerContribution contribution;
        for (auto it = byMaterial.begin(); it != byMaterial.end(); ++it) {
            cancelled(cancel);
            Resources materialResources{*this};
            const auto &mesh = it.value();
            glBufferData(GL_ARRAY_BUFFER, mesh.size() * sizeof(Vertex), mesh.data(), GL_STREAM_DRAW);
            const auto projectedTexture =
                texture(materialResources, resolution, resolution, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
            framebuffer(materialResources, projectedTexture, resolution, resolution);
            clearTarget(resolution, resolution);
            projection_->bind();
            projection_->setUniformValue("mvp", camera.projection * camera.view);
            projection_->setUniformValue("orthographic", parallel);
            projection_->setUniformValue("overlay", overlay);
            projection_->setUniformValue("cameraDirection", direction);
            projection_->setUniformValue("cameraPosition", eye);
            projection_->setUniformValue("depthMargin", (margin + 1) / 2);
            projection_->setUniformValue("hasCameraMask", !mask.isNull());
            projection_->setUniformValue("hasUvMask", !uvMask.isNull());
            bindTexture(*projection_, "sourceMap", 0, erodedTexture);
            bindTexture(*projection_, "depthMap", 1, depthTexture);
            bindTexture(*projection_, "cameraMask", 2, maskTexture);
            bindTexture(*projection_, "uvMask", 3, uvMaskTexture);
            drawStrips(int(mesh.size()), resolution, cancel);
            projection_->release();
            const auto islandTexture = texture(materialResources, resolution, resolution, GL_R8, GL_RED,
                                               GL_UNSIGNED_BYTE, nullptr, false);
            framebuffer(materialResources, islandTexture, resolution, resolution);
            clearTarget(resolution, resolution);
            islands_->bind();
            drawStrips(int(mesh.size()), resolution, cancel);
            islands_->release();
            const auto paddedTexture =
                texture(materialResources, resolution, resolution, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
            framebuffer(materialResources, paddedTexture, resolution, resolution);
            clearTarget(resolution, resolution);
            padding_->bind();
            bindTexture(*padding_, "projectionMap", 0, projectedTexture);
            bindTexture(*padding_, "islandMap", 1, islandTexture);
            drawStrips(3, resolution, cancel);
            padding_->release();
            cancelled(cancel);
            QImage image(resolution, resolution, QImage::Format_RGBA8888);
            require(!image.isNull(), "Cannot allocate projected UV image");
            glReadPixels(0, 0, resolution, resolution, GL_RGBA, GL_UNSIGNED_BYTE, image.bits());
            require(glGetError() == GL_NO_ERROR, "OpenGL projection failed");
            contribution.materials.insert(it.key(), image.mirrored());
        }
        return contribution;
    }
};

struct Renderer {
    QThread thread;
    std::unique_ptr<QOffscreenSurface> surface;
    ProjectionWorker *worker = nullptr;
};
std::unique_ptr<Renderer> renderer;
std::mutex rendererMutex;

template <class F> void onRenderer(ProjectionWorker *worker, F &&f) {
    std::exception_ptr error;
    auto call = [&] {
        try {
            f();
        } catch (...) {
            error = std::current_exception();
        }
    };
    if (QThread::currentThread() == worker->thread())
        call();
    else
        require(QMetaObject::invokeMethod(worker, call, Qt::BlockingQueuedConnection),
                "Cannot dispatch projection work");
    if (error)
        std::rethrow_exception(error);
}
} // namespace

void aiInitializeProjectionRenderer() {
    std::lock_guard<std::mutex> guard(rendererMutex);
    if (renderer)
        return;
    require(qobject_cast<QGuiApplication *>(QCoreApplication::instance()) != nullptr,
            "Projection requires QGuiApplication and an OpenGL 3.3 environment");
    require(QThread::currentThread() == QCoreApplication::instance()->thread(),
            "Initialize the projection renderer on the GUI thread before projecting");
    auto service = std::make_unique<Renderer>();
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setSamples(0);
    format.setDepthBufferSize(24);
    format.setStencilBufferSize(0);
    service->surface = std::make_unique<QOffscreenSurface>();
    service->surface->setFormat(format);
    service->surface->create();
    require(service->surface->isValid(), "Cannot create offscreen projection surface");
    service->worker = new ProjectionWorker(service->surface.get());
    service->worker->moveToThread(&service->thread);
    QObject::connect(&service->thread, &QThread::finished, service->worker, &QObject::deleteLater);
    service->thread.setObjectName("AI projection renderer");
    service->thread.start();
    try {
        onRenderer(service->worker, [&] { service->worker->initialize(); });
    } catch (...) {
        onRenderer(service->worker, [&] { service->worker->destroy(); });
        service->thread.quit();
        service->thread.wait();
        throw;
    }
    renderer = std::move(service);
}

void aiShutdownProjectionRenderer() {
    std::lock_guard<std::mutex> guard(rendererMutex);
    if (!renderer)
        return;
    require(QThread::currentThread() == QCoreApplication::instance()->thread(),
            "Shut down the projection renderer on the GUI thread");
    onRenderer(renderer->worker, [] { renderer->worker->destroy(); });
    renderer->thread.quit();
    renderer->thread.wait();
    renderer.reset();
}

AiLayerContribution aiRenderProjection(const Scene &scene, int uv, int resolution, const AiCamera &camera,
                                       const QImage &source, const QImage &mask, const QImage &uvMask,
                                       const QSet<QString> &materials, bool overlay, int depthMargin,
                                       std::atomic_bool *cancel) {
    cancelled(cancel);
    require(resolution > 0 && resolution <= 4096, "Invalid projection atlas resolution");
    require(!source.isNull() && source.width() <= 8192 && source.height() <= 8192,
            "Invalid projection source image");
    require(depthMargin >= 0 && depthMargin <= 64, "Invalid depth discontinuity margin");
    // A generated image can have a different resolution from its captured guide
    // and editable camera mask. Both textures address the same normalized view.
    require(mask.isNull() || (mask.width() <= 8192 && mask.height() <= 8192),
            "Invalid camera mask dimensions");
    require(uvMask.isNull() || (uvMask.width() <= 8192 && uvMask.height() <= 8192),
            "Invalid UV mask dimensions");
    std::lock_guard<std::mutex> guard(rendererMutex);
    require(renderer != nullptr, "Projection renderer is not initialized");
    AiLayerContribution result;
    onRenderer(renderer->worker, [&] {
        result = renderer->worker->render(scene, uv, resolution, camera, source, mask, uvMask, materials,
                                          overlay, depthMargin, cancel);
    });
    return result;
}
