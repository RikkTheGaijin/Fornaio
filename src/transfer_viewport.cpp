#include "transfer_viewport.h"
#include "transfer.h"
#include <QMouseEvent>
#include <QPainter>
#include <QSet>
#include <QWheelEvent>
#include <algorithm>
#include <numeric>

TransferViewport::TransferViewport(QWidget *p) : QOpenGLWidget(p) {
    setMinimumSize(240, 240);
    setFocusPolicy(Qt::StrongFocus);
}
TransferViewport::~TransferViewport() {
    if (context()) {
        makeCurrent();
        releaseTextures();
        shader_.reset();
        unsigned defaults[] = {white_, flatNormal_, gray_, black_};
        glDeleteTextures(4, defaults);
        glDeleteBuffers(1, &vbo_);
        glDeleteVertexArrays(1, &vao_);
        doneCurrent();
    }
}
void TransferViewport::setMesh(std::shared_ptr<const Scene> scene, int uv) {
    if (scene_ == scene && uv_ == uv)
        return;
    bool newScene = scene_ != scene;
    scene_ = std::move(scene);
    uv_ = uv;
    geometryDirty_ = true;
    if (newScene && scene_) {
        center_ = scene_->center;
        scale_ = scene_->scale;
        fit();
    }
    update();
}
void TransferViewport::setMaterials(const QMap<QString, PreviewMaterial> &materials, bool normalDirectX) {
    materials_ = materials;
    normalDX_ = normalDirectX;
    texturesDirty_ = true;
    update();
}
void TransferViewport::setDisplay(const QString &suffix) {
    display_ = suffix;
    update();
}
void TransferViewport::setCaption(const QString &caption) {
    caption_ = caption;
    update();
}
void TransferViewport::setUncoveredOverlay(bool enabled) {
    uncoveredOverlay_ = enabled;
    update();
}
void TransferViewport::setCamera(const OrbitCamera &c) {
    const bool changed = camera_.yaw != c.yaw || camera_.pitch != c.pitch || camera_.distance != c.distance ||
                         camera_.fieldOfView != c.fieldOfView || camera_.pan.x != c.pan.x ||
                         camera_.pan.y != c.pan.y || camera_.pan.z != c.pan.z ||
                         camera_.orthographic != c.orthographic;
    camera_ = c;
    update();
    if (changed)
        emit cameraChanged(camera_); // Linked viewports must not bounce identical cameras forever.
}
void TransferViewport::setFieldOfView(float degrees) {
    if (!std::isfinite(degrees) || degrees < 5 || degrees > 90)
        return;
    constexpr float radians = 3.14159265359f / 360;
    camera_.distance *= std::tan(camera_.fieldOfView * radians) / std::tan(degrees * radians);
    camera_.fieldOfView = degrees;
    update();
    emit cameraChanged(camera_);
}
void TransferViewport::setFraming(V3 center, float scale) {
    center_ = center;
    scale_ = scale;
}
void TransferViewport::fit() {
    const float fov = camera_.fieldOfView;
    const bool orthographic = camera_.orthographic;
    camera_ = {};
    camera_.fieldOfView = fov;
    camera_.orthographic = orthographic;
    camera_.distance =
        scale_ * 1.2f * std::tan(20.f * 3.14159265359f / 180) / std::tan(fov * 3.14159265359f / 360);
    update();
    emit cameraChanged(camera_);
}
void TransferViewport::initializeGL() {
    initializeOpenGLFunctions();
    cacheUploads_ = false;
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    auto solid = [&](QColor c) {
        QImage im(1, 1, QImage::Format_RGBA8888);
        im.fill(c);
        return upload(im);
    };
    white_ = solid(Qt::white);
    flatNormal_ = solid(QColor(128, 128, 255));
    gray_ = solid(QColor(128, 128, 128));
    black_ = solid(Qt::black);
    cacheUploads_ = true;
    shader_ = std::make_unique<QOpenGLShaderProgram>();
    shader_->addShaderFromSourceCode(QOpenGLShader::Vertex, R"(#version 330 core
layout(location=0) in vec3 position;layout(location=1) in vec3 normal;layout(location=2) in vec2 uv;layout(location=3) in vec4 tangent;
uniform mat4 mvp;uniform mat4 view;out vec3 N;out vec4 T;out vec2 UV;out vec3 V;
void main(){gl_Position=mvp*vec4(position,1);N=normal;T=tangent;UV=uv;V=(view*vec4(position,1)).xyz;})");
    shader_->addShaderFromSourceCode(QOpenGLShader::Fragment, R"(#version 330 core
in vec3 N;in vec4 T;in vec2 UV;in vec3 V;out vec4 color;
uniform sampler2D baseTex,normalTex,roughTex,metalTex,occTex,displayTex,coverageTex;
uniform bool raw,normalDX,showUncovered,orthographic;uniform mat3 viewRot;uniform float stripePeriod;
void main(){
 vec2 uv=vec2(UV.x,1-UV.y);
 vec4 base=texture(baseTex,uv);
 int bayer[16]=int[16](0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5);ivec2 p=ivec2(gl_FragCoord.xy)%4;
 if(base.a<(float(bayer[p.y*4+p.x])+.5)/16.)discard;
 if(showUncovered && texture(coverageTex,uv).r<.5){
  float stripe=step(.5,fract((gl_FragCoord.x+gl_FragCoord.y)/stripePeriod));
  color=vec4(mix(vec3(.07,.035,.045),vec3(.92,.12,.19),stripe),1);return;
 }
 if(raw){color=vec4(texture(displayTex,uv).rgb,1);return;}
 vec3 n=normalize(N);vec3 t=T.xyz-n*dot(n,T.xyz);
 if(dot(t,t)>1e-12){t=normalize(t);vec3 b=T.w*cross(n,t);vec3 tn=texture(normalTex,uv).xyz*2-1;if(normalDX)tn.y=-tn.y;n=normalize(t*tn.x+b*tn.y+n*tn.z);}
 vec3 nv=normalize(viewRot*n);if(!gl_FrontFacing)nv=-nv;
 vec3 v=orthographic?vec3(0,0,1):normalize(-V);if(dot(nv,v)<0)nv=-nv;
 vec3 albedo=pow(base.rgb,vec3(2.2));float r=clamp(texture(roughTex,uv).r,.04,1);float m=texture(metalTex,uv).r;float ao=texture(occTex,uv).r;
 vec3 f0=mix(vec3(.04),albedo,m);vec3 diffuse=albedo*(1-m);
 vec3 L=normalize(vec3(.45,.7,.55));vec3 H=normalize(L+v);float nl=max(dot(nv,L),0),nh=max(dot(nv,H),0);
 float a2=r*r*r*r;float d=a2/(3.14159*pow(nh*nh*(a2-1)+1,2));float k=(r+1)*(r+1)/8;
 float g=nl/(nl*(1-k)+k)*max(dot(nv,v),0)/(max(dot(nv,v),0)*(1-k)+k);
 vec3 F=f0+(1-f0)*pow(1-max(dot(H,v),0),5);
 vec3 spec=d*g*F/max(4*nl*max(dot(nv,v),0),1e-4);
 vec3 sky=mix(vec3(.18,.16,.14),vec3(.32,.38,.46),nv.y*.5+.5);
 vec3 lit=(diffuse/3.14159+spec)*nl*2.6+(diffuse+f0*.3)*sky*ao;
 color=vec4(pow(lit,vec3(1/2.2)),1);})");
    shader_->link();
    geometryDirty_ = texturesDirty_ = true;
}
unsigned TransferViewport::upload(const QImage &image, bool mask) {
    const auto key = qMakePair(image.cacheKey(), mask);
    auto cached = textureCache_.find(key);
    if (cacheUploads_ && cached != textureCache_.end()) {
        cached->used = ++textureClock_;
        return cached->id;
    }
    QImage im = image.convertToFormat(mask ? QImage::Format_Grayscale8 : QImage::Format_RGBA8888);
    unsigned tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, im.bytesPerLine() / (mask ? 1 : 4));
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, mask ? GL_R8 : GL_RGBA8, im.width(), im.height(), 0,
                 mask ? GL_RED : GL_RGBA, GL_UNSIGNED_BYTE, im.constBits());
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    if (mask) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_RED);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
    }
    if (!mask)
        glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mask ? GL_NEAREST : GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mask ? GL_NEAREST : GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, mask ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, mask ? GL_CLAMP_TO_EDGE : GL_REPEAT);
    // GL_TEXTURE_MAX_ANISOTROPY (core in 4.6, universally available as an extension).
    if (!mask)
        glTexParameterf(GL_TEXTURE_2D, 0x84FE, 8.f);
    glGetError();
    if (cacheUploads_) {
        const quint64 bytes = quint64(im.width()) * im.height() * (mask ? 1 : 4) * (mask ? 1. : 4. / 3.);
        textureCache_[key] = {tex, bytes, ++textureClock_};
        textureBytes_ += bytes;
    }
    return tex;
}
void TransferViewport::releaseTextures() {
    for (const auto &t : textureCache_)
        glDeleteTextures(1, &t.id);
    textureCache_.clear();
    textureBytes_ = 0;
    gpu_.clear();
}
void TransferViewport::uploadTextures() {
    gpu_.clear();
    for (auto it = materials_.begin(); it != materials_.end(); ++it) {
        const PreviewMaterial &m = it.value();
        GpuMaterial g;
        auto up = [&](const QImage &im) { return im.isNull() ? 0u : upload(im); };
        g.base = up(m.base);
        g.normal = up(m.normal);
        g.roughness = up(m.roughness);
        g.metallic = up(m.metallic);
        g.occlusion = up(m.occlusion);
        g.coverage = m.coverage.isNull() ? 0u : upload(m.coverage, true);
        for (auto map = m.maps.begin(); map != m.maps.end(); ++map)
            g.maps[map.key()] = !m.coverage.isNull() && map.value().cacheKey() == m.coverage.cacheKey()
                                    ? g.coverage
                                    : up(map.value());
        gpu_[it.key()] = g;
    }
    QSet<unsigned> active;
    for (const auto &m : gpu_) {
        for (unsigned id : {m.base, m.normal, m.roughness, m.metallic, m.occlusion, m.coverage})
            active.insert(id);
        for (unsigned id : m.maps)
            active.insert(id);
    }
    while (textureBytes_ > 256ull * 1024 * 1024) {
        auto oldest = textureCache_.end();
        for (auto it = textureCache_.begin(); it != textureCache_.end(); ++it)
            if (!active.contains(it->id) && (oldest == textureCache_.end() || it->used < oldest->used))
                oldest = it;
        if (oldest == textureCache_.end())
            break; // Active maps may exceed the cache budget; never delete a bound map.
        glDeleteTextures(1, &oldest->id);
        textureBytes_ -= oldest->bytes;
        textureCache_.erase(oldest);
    }
    texturesDirty_ = false;
}
void TransferViewport::uploadGeometry() {
    std::vector<float> v;
    ranges_.clear();
    if (scene_) {
        std::vector<V3> tangents;
        std::vector<float> signs;
        bool hasUV = uv_ >= 0 && uv_ < scene_->uvNames.size();
        if (hasUV)
            meshTangents(*scene_, uv_, tangents, signs);
        std::vector<int> order(scene_->tris.size());
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(),
                         [&](int a, int b) { return scene_->tris[a].material < scene_->tris[b].material; });
        v.reserve(order.size() * 36);
        for (int i : order) {
            const Triangle &t = scene_->tris[i];
            QString material =
                t.material >= 0 && t.material < scene_->materials.size() ? scene_->materials[t.material] : "";
            if (ranges_.empty() || ranges_.back().material != material)
                ranges_.push_back({material, (int)(v.size() / 12), 0});
            for (int k = 0; k < 3; k++) {
                V2 uv = hasUV && uv_ < (int)t.uv.size() ? t.uv[uv_][k] : V2{};
                if (!std::isfinite(uv.x) || !std::isfinite(uv.y))
                    uv = {};
                V3 tan = hasUV ? tangents[i * 3 + k] : V3{};
                float s = hasUV ? signs[i * 3 + k] : 1.f;
                float row[] = {t.p[k].x, t.p[k].y, t.p[k].z, t.n[k].x, t.n[k].y, t.n[k].z,
                               uv.x,     uv.y,     tan.x,    tan.y,    tan.z,    s};
                v.insert(v.end(), row, row + 12);
            }
            ranges_.back().count += 3;
        }
    }
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    int sizes[] = {3, 3, 2, 4}, offset = 0;
    for (int i = 0; i < 4; i++) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, sizes[i], GL_FLOAT, GL_FALSE, 12 * sizeof(float),
                              (void *)(intptr_t)(offset * sizeof(float)));
        offset += sizes[i];
    }
    geometryDirty_ = false;
}
void TransferViewport::paintGL() {
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);
    glClearColor(.065f, .083f, .105f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (geometryDirty_)
        uploadGeometry();
    if (texturesDirty_)
        uploadTextures();
    if (scene_ && shader_->isLinked()) {
        QMatrix4x4 projection, view;
        const float aspect = (float)width() / std::max(1, height());
        if (camera_.orthographic) {
            const float half = camera_.distance * std::tan(camera_.fieldOfView * 3.14159265359f / 360);
            projection.ortho(-half * aspect, half * aspect, -half, half, camera_.distance - scale_ * 2,
                             camera_.distance + scale_ * 4);
        } else
            projection.perspective(camera_.fieldOfView, aspect,
                                   std::max(scale_ * .0001f, camera_.distance - scale_ * 2),
                                   camera_.distance + scale_ * 4);
        view.translate(camera_.pan.x, camera_.pan.y, -camera_.distance);
        view.rotate(camera_.pitch, 1, 0, 0);
        view.rotate(camera_.yaw, 0, 1, 0);
        view.translate(-center_.x, -center_.y, -center_.z);
        shader_->bind();
        shader_->setUniformValue("mvp", projection * view);
        shader_->setUniformValue("view", view);
        shader_->setUniformValue("viewRot", view.normalMatrix());
        shader_->setUniformValue("normalDX", normalDX_);
        shader_->setUniformValue("orthographic", camera_.orthographic);
        shader_->setUniformValue("raw", !display_.isEmpty());
        shader_->setUniformValue("showUncovered", uncoveredOverlay_ && display_ != "COVERAGE");
        shader_->setUniformValue("stripePeriod", float(12 * devicePixelRatioF()));
        const char *names[] = {"baseTex", "normalTex",  "roughTex",   "metalTex",
                               "occTex",  "displayTex", "coverageTex"};
        for (int i = 0; i < 7; i++)
            shader_->setUniformValue(names[i], i);
        glBindVertexArray(vao_);
        for (const Range &r : ranges_) {
            GpuMaterial g = gpu_.value(r.material);
            unsigned display = g.maps.value(display_, 0);
            if (!display)
                display = display_ == "NRM" || display_ == "NORMAL" ? flatNormal_ : black_;
            unsigned bound[] = {g.base ? g.base : gray_,
                                g.normal ? g.normal : flatNormal_,
                                g.roughness ? g.roughness : gray_,
                                g.metallic ? g.metallic : black_,
                                g.occlusion ? g.occlusion : white_,
                                display,
                                g.coverage ? g.coverage : white_};
            for (int i = 0; i < 7; i++) {
                glActiveTexture(GL_TEXTURE0 + i);
                glBindTexture(GL_TEXTURE_2D, bound[i]);
            }
            glDrawArrays(GL_TRIANGLES, r.first, r.count);
        }
        glActiveTexture(GL_TEXTURE0);
        shader_->release();
    }
    QPainter painter(this);
    painter.setPen(QColor("#9eacbe"));
    if (!scene_)
        painter.drawText(rect(), Qt::AlignCenter, "No mesh loaded");
    if (!caption_.isEmpty())
        painter.drawText(14, 22, caption_);
    if (shader_ && !shader_->isLinked())
        painter.drawText(rect(), Qt::AlignCenter, "OpenGL shader error: " + shader_->log());
}
void TransferViewport::mousePressEvent(QMouseEvent *e) {
    last_ = e->pos();
}
void TransferViewport::mouseMoveEvent(QMouseEvent *e) {
    auto d = e->pos() - last_;
    last_ = e->pos();
    if (e->buttons() & Qt::LeftButton) {
        camera_.yaw += d.x() * .5f;
        camera_.pitch = std::clamp(camera_.pitch + d.y() * .5f, -89.f, 89.f);
    }
    if (e->buttons() & (Qt::RightButton | Qt::MiddleButton)) {
        const float lens =
            std::tan(camera_.fieldOfView * 3.14159265359f / 360) / std::tan(20.f * 3.14159265359f / 180);
        camera_.pan.x += d.x() * camera_.distance * lens / std::max(1, height());
        camera_.pan.y -= d.y() * camera_.distance * lens / std::max(1, height());
    }
    update();
    emit cameraChanged(camera_);
}
void TransferViewport::wheelEvent(QWheelEvent *e) {
    camera_.distance *= std::pow(.85f, e->angleDelta().y() / 120.f);
    camera_.distance = std::clamp(camera_.distance, scale_ * .01f, scale_ * 30);
    update();
    emit cameraChanged(camera_);
}
