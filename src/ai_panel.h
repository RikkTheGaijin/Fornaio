#pragma once
#include "ai_cloud.h"
#include "ai_runtime.h"
#include "ai_texture.h"
#include <QPointer>
#include <QTemporaryDir>
#include <QWidget>
#include <optional>
#include <thread>
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QProgressBar;
class QSpinBox;
class QTabWidget;
class QTabBar;
class AiMaskCanvas;
class QDialog;

class AiPanel : public QWidget {
    Q_OBJECT
    friend class AiTests;

  public:
    explicit AiPanel(QWidget *parent = nullptr);
    ~AiPanel();
    void setExternalBusy(bool);
    void shutdown();
    bool loadMesh(const QString &);
    bool running() const { return busy_; }
  signals:
    void runningChanged(bool);

  private:
    std::shared_ptr<const Scene> scene_;
    QString meshPath_, anchor_;
    QString migrationSource_, engineProfile_, enginePath_;
    QString projectionError_;
    AiAtlases atlases_, layerBase_;
    struct HistoryState {
        AiAtlases atlases;
        QList<QJsonObject> projections;
        AiAtlases base;
        bool parity = true;
        QString isolatedLayer;
        int selected = -1;
        QString mode;
    };
    bool parity_ = true;
    QString isolatedLayer_;
    QList<HistoryState> undo_, redo_;
    QList<QJsonObject> projections_;
    QCache<QByteArray, AiAtlases> layerFrames_{256 * 1024}; // KiB, bounded across visibility combinations.
    QCache<QByteArray, QByteArray> layerDecoded_{128 * 1024};
    std::shared_ptr<AiCapture> capture_;
    QImage candidate_, candidateRetention_;
    QJsonObject candidateSettings_, pending_;
    QJsonObject pendingSmartFill_; // Independent of the pending camera candidate.
    QPointer<QDialog> smartFillDialog_;
    AiMaskCanvas *smartFillCanvas_ = nullptr;
    QCheckBox *smartFillTargets_ = nullptr;
    TransferViewport *smartFillView_ = nullptr;
    QComboBox *smartFillMaterial_ = nullptr;
    QLabel *smartFillInfo_ = nullptr;
    QPushButton *smartFillGenerate_ = nullptr, *smartFillApply_ = nullptr, *smartFillDiscard_ = nullptr,
                *smartFillReplan_ = nullptr, *smartFillReprocess_ = nullptr;
    bool smartFillActive_ = false;
    enum class SmartFillPhase { Idle, Planning, Preview, Request, Provider, Processing, Applying };
    SmartFillPhase smartFillPhase_ = SmartFillPhase::Idle;
    struct SmartFillPreviewData {
        QJsonObject plan;
        AiAtlases atlases;
        QMap<QString, QImage> masks, eligibility;
        QByteArray stateKey, fingerprint;
    };
    std::shared_ptr<SmartFillPreviewData> smartFillPreview_;
    const SmartFillPreviewData *smartFillRendered_ = nullptr;
    QString smartFillRenderedMaterial_;
    bool smartFillPreviewDeferred_ = false;
    int smartFillPatch_ = -1;
    QString smartFillRequestId_;
    std::unique_ptr<QTemporaryDir> smartFillWork_;
    AiManifest manifest_;
    AiHardware hardware_;
    AiInstaller installer_;
    AiWorker engine_;
    AiCloudImage cloud_;
    std::shared_ptr<AiLock> lock_; // Locked paint of the current cloud capture.
    std::unique_ptr<QTemporaryDir> work_;
    std::thread native_;
    std::atomic_bool cancel_{false};
    bool busy_ = false, externalBusy_ = false, automatic_ = false, loading_ = false, setupLease_ = false;
    QList<OrbitCamera> cameras_, multiviewCameras_;
    int cameraIndex_ = 0;
    int regenerateIndex_ = -1;  // Layer being regenerated in place; -1 for a new projection.
    QString regenerateLayerId_; // Stable while a camera replacement candidate remains pending.
    int lastGenerationSeed_ = -1;
    quint64 revision_ = 0, requestSerial_ = 0;
    QStringList meshErrors_;
    QWidget *inputs_;
    TransferViewport *view_;
    AiMaskCanvas *canvas_, *sideCanvas_;
    QLabel *setupInfo_, *status_, *meshInfo_, *atlasImage_, *guideImage_, *cloudInfo_;
    QLineEdit *location_, *hfToken_;
    QComboBox *model_;
    QWidget *localEngine_;
    QLineEdit *exportName_;
    QMap<QString, QString> exportNames_;
    QPlainTextEdit *prompt_;
    QListWidget *references_, *materials_, *layers_, *viewGrid_;
    QWidget *candidateFrame_, *multiviewBox_;
    QLabel *layersEmpty_;
    QWidget *layerTools_;
    QComboBox *layerMode_;
    QSpinBox *layerStrength_, *layerDepthMargin_;
    QCheckBox *layerIsolate_;
    QPushButton *deleteLayer_, *rebuildLayers_, *smartFill_;
    QComboBox *uv_, *resolution_, *generationResolution_, *profile_, *atlasChoice_, *display_, *blendMode_;
    QSpinBox *seed_, *brush_, *projectionStrength_, *projectionFeather_, *angleFalloff_, *continuity_,
        *projectionAngle_, *cameraFov_;
    QCheckBox *colorMatch_, *randomSeed_, *showUncovered_, *protectDetails_, *fillGaps_, *coverAll_,
        *orthographic_, *autoApply_;
    QProgressBar *progress_;
    QPushButton *install_, *cancelButton_, *generate_, *project_, *undoButton_, *redoButton_, *export_;
    QTabWidget *previews_;
    QTabBar *generationMode_;
    int generationSeed();
    bool cloudModel() const; // GPT Image 2 (default) instead of the optional local Fibo engine
    bool modelReady() const;
    void refresh();
    void resetAtlases();
    void invalidate();
    void updatePreview();
    void refreshLayers();
    void setLayerVisible(int index, bool visible);
    int selectedLayer() const;
    HistoryState historyState() const;
    void restoreHistory(const HistoryState &);
    void refreshLayerTools();
    void applyLayerEdit(QList<QJsonObject>, int rebakeIndex, int selected, const QString &message,
                        const QString &isolation, bool rebuild = false);
    void deleteLayer(int index);
    void moveLayer(int index, int offset);
    void replaceLayerImage(int index);
    void editLayerMask(int index, bool uv);
    void rebuildProjectionLayers();
    void startSmartFill();
    void prepareSmartFill(const QString &replaceLayer = {});
    void showSmartFill();
    void refreshSmartFill();
    QByteArray smartFillStateKey() const;
    bool smartFillPreviewMatches() const;
    static std::shared_ptr<SmartFillPreviewData>
    makeSmartFillPreview(const QJsonObject &, const Scene &, int uv, const AiAtlases &,
                         const QList<QJsonObject> &, bool parity, const QByteArray &stateKey,
                         std::atomic_bool *cancel, const QByteArray &fingerprint = {});
    void prepareSmartFillPreview(QJsonObject plan, bool replacePending = false);
    bool smartFillStale() const;
    void smartFillGenerate();
    void smartFillRequest();
    void smartFillEvent(const QJsonObject &);
    void processSmartFillResult(const QString &path = {});
    void smartFillReprocess();
    void smartFillStop(const QString &);
    void smartFillApply();
    void smartFillDiscard();
    void setBusy(bool);
    QSet<QString> selectedMaterials() const;
    QStringList referencePaths() const;
    void generate(bool automatic);
    void resetMultiviewCameras();
    void refreshMultiviewViews();
    std::optional<float> frontYaw(int excludeLayer = -1) const;
    void regenerateLayer(int index);
    void rememberRegenerationTarget();
    void reindexRegenerationTarget();
    void nextCamera();
    void receiveCapture(std::shared_ptr<AiCapture>, double contribution);
    void workerEvent(const QJsonObject &);
    void projectCandidate(bool automatic);
    AiProjectionOptions projectionOptions() const;
    void finishProjection(const AiProjection &, const QJsonObject &);
    void finishRegeneration(const AiProjection &, const QList<QJsonObject> &records);
    void fail(const QString &);
    void cancelJob();
    void saveProject();
    void openProject();
    void exportTextures();
    void beginInstall(const QString &offline = {}, bool repair = false);
    void chooseLocation();
};
