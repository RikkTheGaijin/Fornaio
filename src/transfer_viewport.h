#pragma once
#include "core.h"
#include <QMap>
#include <QMatrix4x4>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <memory>
// Texture maps for one material. Images are 8-bit previews; base colour is sRGB, the rest are raw.
struct PreviewMaterial {
    QImage base, normal, roughness, metallic, occlusion, coverage;
    QMap<QString, QImage> maps; // every map by suffix, for single-map display
};
struct OrbitCamera {
    float yaw = 25, pitch = 15, distance = 3;
    V3 pan{};
    float fieldOfView = 40;
    // Orthographic views keep the same framing: half height = distance * tan(fieldOfView / 2).
    bool orthographic = false;
};
// Textured preview of a mesh with per-material maps.
class TransferViewport : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
    friend class UiTests;
    friend class AiTests;

  public:
    explicit TransferViewport(QWidget *parent = nullptr);
    ~TransferViewport();
    void setMesh(std::shared_ptr<const Scene> scene, int uv);
    void setMaterials(const QMap<QString, PreviewMaterial> &materials, bool normalDirectX);
    // Empty suffix = lit shading; otherwise shows that map unlit.
    void setDisplay(const QString &suffix);
    void setCaption(const QString &caption);
    // Preview only; coverage is independent of padded texture colors.
    void setUncoveredOverlay(bool enabled);
    void setCamera(const OrbitCamera &);
    void setFieldOfView(float degrees); // Dolly to preserve framing when changing the lens.
    void setFraming(V3 center, float scale);
    OrbitCamera camera() const { return camera_; }
    void fit();

  signals:
    void cameraChanged(const OrbitCamera &);

  protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

  private:
    struct Range {
        QString material;
        int first = 0, count = 0;
    };
    struct GpuMaterial {
        unsigned base = 0, normal = 0, roughness = 0, metallic = 0, occlusion = 0, coverage = 0;
        QMap<QString, unsigned> maps;
    };
    std::shared_ptr<const Scene> scene_;
    int uv_ = 0;
    QMap<QString, PreviewMaterial> materials_;
    QMap<QString, GpuMaterial> gpu_;
    struct CachedTexture {
        unsigned id = 0;
        quint64 bytes = 0, used = 0;
    };
    QMap<QPair<qint64, bool>, CachedTexture> textureCache_;
    quint64 textureBytes_ = 0, textureClock_ = 0;
    bool cacheUploads_ = false;
    std::vector<Range> ranges_;
    std::unique_ptr<QOpenGLShaderProgram> shader_;
    unsigned vao_ = 0, vbo_ = 0, white_ = 0, flatNormal_ = 0, gray_ = 0, black_ = 0;
    bool geometryDirty_ = true, texturesDirty_ = true, normalDX_ = false;
    bool uncoveredOverlay_ = false;
    QString display_, caption_;
    OrbitCamera camera_;
    V3 center_{};
    float scale_ = 1;
    QPoint last_;
    void uploadGeometry();
    void uploadTextures();
    void releaseTextures();
    unsigned upload(const QImage &, bool mask = false);
};
