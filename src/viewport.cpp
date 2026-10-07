#include "viewport.h"
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <set>
Viewport::Viewport(QWidget *p) : QOpenGLWidget(p) {
    setMinimumSize(400, 350);
    setFocusPolicy(Qt::StrongFocus);
}
Viewport::~Viewport() {
    if (context()) {
        makeCurrent();
        shader_.reset();
        glDeleteBuffers(1, &vbo_);
        glDeleteVertexArrays(1, &vao_);
        glDeleteTextures(1, &maskTex_);
        glDeleteTextures(1, &aoTex_);
        doneCurrent();
    }
}
void Viewport::setScene(std::shared_ptr<Scene> s) {
    if (scene_ == s)
        return;
    scene_ = std::move(s);
    if (scene_) {
        displayCenter_ = scene_->center;
        displayScale_ = scene_->scale;
        if (scene_->occluder) {
            V3 lo{INFINITY, INFINITY, INFINITY}, hi{-INFINITY, -INFINITY, -INFINITY};
            for (const Scene *part : {static_cast<const Scene *>(scene_.get()), scene_->occluder.get()})
                for (const auto &t : part->tris)
                    for (auto p : t.p) {
                        lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
                        hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
                    }
            displayCenter_ = (hi + lo) * .5f;
            displayScale_ = sqrtf(dot(hi - lo, hi - lo));
        }
    }
    dirty_ = true;
    fit();
}
void Viewport::setMask(const Mask &m, const Settings &s) {
    if (!m.image.isNull() && maskImageKey_ == m.image.cacheKey() && maskChannel_ == s.channel &&
        maskInverted_ == s.invert && repeat_ == s.repeat && flip_ == s.flip)
        return;
    maskImageKey_ = m.image.cacheKey();
    maskChannel_ = s.channel;
    maskInverted_ = s.invert;
    mask_ = m.values;
    mw_ = m.w;
    mh_ = m.h;
    repeat_ = s.repeat;
    flip_ = s.flip;
    maskDirty_ = true;
    update();
}
void Viewport::setResult(const QImage &im) {
    ao_ = im;
    aoDirty_ = true;
    update();
}
void Viewport::setUV(int m, int a, const std::vector<int> &bad) {
    if (maskUV_ == m && aoUV_ == a && bad_ == bad)
        return;
    maskUV_ = m;
    aoUV_ = a;
    bad_ = bad;
    dirty_ = true;
    update();
}
void Viewport::setMode(int m) {
    mode_ = m;
    update();
}
void Viewport::fit() {
    yaw_ = 25;
    pitch_ = 15;
    pan_ = {};
    distance_ = scene_ ? displayScale_ * 1.2f : 3;
    update();
}
void Viewport::initializeGL() {
    initializeOpenGLFunctions();
    glGenVertexArrays(1, &vao_);
    glGenBuffers(1, &vbo_);
    glGenTextures(1, &maskTex_);
    glGenTextures(1, &aoTex_);
    shader_ = std::make_unique<QOpenGLShaderProgram>();
    shader_->addShaderFromSourceCode(QOpenGLShader::Vertex, R"(#version 330 core
layout(location=0) in vec3 position;layout(location=1) in vec3 normal;layout(location=2) in vec2 maskUV;layout(location=3) in vec2 aoUV;layout(location=4) in float bad;
uniform mat4 mvp;out vec3 N;out vec2 M;out vec2 A;out float B;
void main(){gl_Position=mvp*vec4(position,1);N=normal;M=maskUV;A=aoUV;B=bad;})");
    shader_->addShaderFromSourceCode(QOpenGLShader::Fragment, R"(#version 330 core
in vec3 N;in vec2 M;in vec2 A;in float B;out vec4 color;uniform sampler2D maskTex;uniform sampler2D aoTex;uniform int mode;uniform bool flip;
void main(){
 if(B<-.5){float light=.35+.65*abs(dot(normalize(N),normalize(vec3(.4,.7,1))));color=vec4(vec3(.64,.57,.48)*light,1);return;}
 if(B>.5){color=vec4(1,.16,.22,1);return;}
 vec2 uv=vec2(M.x,flip?M.y:1-M.y);float a=texture(maskTex,uv).r;
 if(mode!=1){int bayer[16]=int[16](0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5);ivec2 p=ivec2(gl_FragCoord.xy)%4;if(a<(float(bayer[p.y*4+p.x])+.5)/16.)discard;}
 float light=.35+.65*abs(dot(normalize(N),normalize(vec3(.4,.7,1))));
 vec3 base=mode==2?vec3(texture(aoTex,vec2(A.x,1-A.y)).r):vec3(.64,.78,.81)*light;
 color=vec4(base,1);})");
    shader_->link();
    glEnable(GL_DEPTH_TEST);
    dirty_ = maskDirty_ = aoDirty_ = true;
}
void Viewport::uploadGeometry() {
    std::vector<float> vertices;
    std::set<int> bad(bad_.begin(), bad_.end());
    auto appendScene = [&](const Scene &part, bool opaque) {
        for (int i = 0; i < (int)part.tris.size(); i++) {
            auto &t = part.tris[i];
            for (int k = 0; k < 3; k++) {
                auto p = t.p[k], n = t.n[k];
                V2 m{}, a{};
                if (!opaque && maskUV_ >= 0 && maskUV_ < (int)t.uv.size())
                    m = t.uv[maskUV_][k];
                if (!opaque && aoUV_ >= 0 && aoUV_ < (int)t.uv.size())
                    a = t.uv[aoUV_][k];
                if (!std::isfinite(m.x) || !std::isfinite(m.y))
                    m = {};
                if (!std::isfinite(a.x) || !std::isfinite(a.y))
                    a = {};
                float row[] = {p.x,
                               p.y,
                               p.z,
                               n.x,
                               n.y,
                               n.z,
                               m.x,
                               m.y,
                               a.x,
                               a.y,
                               opaque ? -1.f : (bad.count(i) ? 1.f : 0.f)};
                vertices.insert(vertices.end(), row, row + 11);
            }
        }
    };
    if (scene_) {
        appendScene(*scene_, false);
        if (scene_->occluder)
            appendScene(*scene_->occluder, true);
    }
    count_ = (int)vertices.size() / 11;
    glBindVertexArray(vao_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), vertices.data(), GL_STATIC_DRAW);
    int sizes[] = {3, 3, 2, 2, 1}, offset = 0;
    for (int i = 0; i < 5; i++) {
        glEnableVertexAttribArray(i);
        glVertexAttribPointer(i, sizes[i], GL_FLOAT, GL_FALSE, 11 * sizeof(float),
                              (void *)(intptr_t)(offset * sizeof(float)));
        offset += sizes[i];
    }
    dirty_ = false;
}
void Viewport::resizeGL(int, int) {}
void Viewport::paintGL() {
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glClearColor(.065f, .083f, .105f, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (dirty_)
        uploadGeometry();
    if (maskDirty_) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glBindTexture(GL_TEXTURE_2D, maskTex_);
        float white = 1;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, mw_ ? mw_ : 1, mh_ ? mh_ : 1, 0, GL_RED, GL_FLOAT,
                     mask_.empty() ? &white : mask_.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, repeat_ ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, repeat_ ? GL_REPEAT : GL_CLAMP_TO_EDGE);
        // QPainter's glyph cache assumes the default unpack alignment.
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        maskDirty_ = false;
    }
    if (aoDirty_) {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        glBindTexture(GL_TEXTURE_2D, aoTex_);
        QImage im = ao_.isNull() ? QImage(1, 1, QImage::Format_Grayscale8) : ao_;
        if (ao_.isNull())
            im.fill(255);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, im.bytesPerLine());
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, im.width(), im.height(), 0, GL_RED, GL_UNSIGNED_BYTE,
                     im.constBits());
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        aoDirty_ = false;
    }
    if (scene_ && shader_->isLinked()) {
        QMatrix4x4 projection, view;
        float scale = displayScale_;
        projection.perspective(40.f, (float)width() / std::max(1, height()),
                               std::max(scale * .0001f, distance_ - scale * 2), distance_ + scale * 4);
        view.translate(pan_.x, pan_.y, -distance_);
        view.rotate(pitch_, 1, 0, 0);
        view.rotate(yaw_, 0, 1, 0);
        view.translate(-displayCenter_.x, -displayCenter_.y, -displayCenter_.z);
        shader_->bind();
        shader_->setUniformValue("mvp", projection * view);
        shader_->setUniformValue("mode", mode_);
        shader_->setUniformValue("flip", flip_);
        shader_->setUniformValue("maskTex", 0);
        shader_->setUniformValue("aoTex", 1);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, maskTex_);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, aoTex_);
        glBindVertexArray(vao_);
        glDrawArrays(GL_TRIANGLES, 0, count_);
        shader_->release();
    }
    QPainter painter(this);
    painter.setPen(QColor("#9eacbe"));
    if (!scene_)
        painter.drawText(rect(), Qt::AlignCenter, "Load your hair cards to begin");
    else
        painter.drawText(18, height() - 18, "Left drag: orbit   ·   Right drag: pan   ·   Wheel: zoom");
    if (shader_ && !shader_->isLinked())
        painter.drawText(rect(), Qt::AlignCenter, "OpenGL shader error: " + shader_->log());
}
void Viewport::mousePressEvent(QMouseEvent *e) {
    last_ = e->pos();
}
void Viewport::mouseMoveEvent(QMouseEvent *e) {
    auto d = e->pos() - last_;
    last_ = e->pos();
    if (e->buttons() & Qt::LeftButton) {
        yaw_ += d.x() * .5f;
        pitch_ = std::clamp(pitch_ + d.y() * .5f, -89.f, 89.f);
    }
    if (e->buttons() & (Qt::RightButton | Qt::MiddleButton)) {
        pan_.x += d.x() * distance_ / height();
        pan_.y -= d.y() * distance_ / height();
    }
    update();
}
void Viewport::wheelEvent(QWheelEvent *e) {
    distance_ *= std::pow(.85f, e->angleDelta().y() / 120.f);
    float s = scene_ ? displayScale_ : 1;
    distance_ = std::clamp(distance_, s * .01f, s * 30);
    update();
}
