#pragma once
#include "viewport.h"
#include "transfer_panel.h"
#include "uv2.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <thread>
class Window : public QMainWindow {
    friend class UiTests;

  public:
    Window();
    ~Window();
    bool loadInputs(const QString &geometry, const QString &mask);
    bool loadOccluder(const QString &path);
    void smokeCapture(const QString &path);
    void smokeBake(const QString &output, const QString &capture);

  protected:
    void closeEvent(QCloseEvent *) override;

  private:
    std::shared_ptr<Scene> scene_;
    std::shared_ptr<Scene> occluder_;
    std::shared_ptr<Mask> mask_, bakedMask_;
    std::shared_ptr<Scene> bakedScene_;
    Settings bakedSettings_;
    QString bakedMeshPath_;
    bool resultStale_ = false;
    const Scene *validatedScene_ = nullptr;
    int validatedMaskUV_ = -1, validatedAoUV_ = -1;
    bool validatedAverage_ = false;
    Validation validationCache_;
    std::shared_ptr<Result> result_;
    QImage originalMask_;
    QString meshPath_, maskPath_;
    Viewport *view_;
    QGraphicsView *imageView_;
    QGraphicsScene *imageScene_;
    QWidget *inputs_;
    QComboBox *maskUV_, *aoUV_, *channel_, *resolution_, *ss_, *bits_, *mode_;
    QSpinBox *rays_, *padding_;
    QDoubleSpinBox *distance_, *bias_;
    QCheckBox *invert_, *flip_, *repeat_, *averageOverlaps_;
    QLabel *geometryLabel_, *maskLabel_, *validation_, *status_, *elapsed_;
    QLabel *occluderLabel_;
    QPushButton *removeOccluder_;
    QPushButton *bake_, *cancel_, *export_, *exportTexture_;
    QSpinBox *uvSpacing_;
    QLineEdit *uv2Name_;
    QCheckBox *uvRotate_;
    QPushButton *generateUv2_, *generateUv2As_;
    QLabel *uv2Target_;
    QString uv2Source_, uv2Output_;
    QTabWidget *previewTabs_;
    QWidget *uvLayoutTab_;
    QGraphicsView *uvView_;
    QGraphicsScene *uvScene_;
    QLabel *uvLayoutLabel_;
    std::weak_ptr<Scene> uvLayoutScene_;
    int uvLayoutSet_ = -1;
    QProgressBar *progress_;
    QElapsedTimer timer_;
    std::thread worker_;
    std::atomic_bool cancelFlag_{false};
    bool busy_ = false, loading_ = false;
    QTabWidget *mainTabs_;
    TransferPanel *transfer_;
    bool transferRunning_ = false;
    QString smokeOutput_, smokeCapture_;
    int cachedChannel_ = -1;
    bool cachedInvert_ = false;
    Settings settings() const;
    void refresh();
    void bake();
    void exportPng();
    void saveSettings();
    void restoreSettings();
    void showError(const QString &);
    void updatePreview();
    void finish(std::shared_ptr<Result>, const QString &);
    QString uv2Target() const;
    void generateUv2(bool chooseFile = false);
    void finishUv2(const Uv2Report &, const QString &output, const QString &error);
    void updateUvLayout();
};
