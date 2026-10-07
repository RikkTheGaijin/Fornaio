#pragma once
#include "core.h"
#include <QMatrix4x4>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <memory>
class Viewport : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    friend class UiTests;

  public:
    explicit Viewport(QWidget *parent = nullptr);
    ~Viewport();
    void setScene(std::shared_ptr<Scene> scene);
    void setMask(const Mask &mask, const Settings &settings);
    void setResult(const QImage &);
    void setUV(int mask, int ao, const std::vector<int> &bad);
    void setMode(int);
    void fit();

  protected:
    void initializeGL() override;
    void resizeGL(int, int) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;

  private:
    std::shared_ptr<Scene> scene_;
    std::unique_ptr<QOpenGLShaderProgram> shader_;
    unsigned vao_ = 0, vbo_ = 0, maskTex_ = 0, aoTex_ = 0;
    int maskUV_ = 0, aoUV_ = 0, mode_ = 0, count_ = 0;
    bool dirty_ = true, maskDirty_ = true, aoDirty_ = true, repeat_ = false, flip_ = false;
    std::vector<int> bad_;
    std::vector<float> mask_{1};
    int mw_ = 1, mh_ = 1;
    qint64 maskImageKey_ = 0;
    int maskChannel_ = -1;
    bool maskInverted_ = false;
    QImage ao_;
    QPoint last_;
    float yaw_ = 25, pitch_ = 15, distance_ = 3;
    V3 pan_{};
    V3 displayCenter_{};
    float displayScale_ = 1;
    void uploadGeometry();
};
