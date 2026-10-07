#pragma once
#include "transfer.h"
#include "transfer_viewport.h"
#include <QHash>
#include <QSet>
#include <QWidget>
#include <atomic>
#include <thread>
class QCheckBox;
class QComboBox;
class QGraphicsScene;
class QGraphicsView;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QDoubleSpinBox;
class QTableWidget;
class QTreeWidget;
class QScrollArea;
class QTabWidget;
// "Texture Transfer" tab: reprojects texture sets from the source mesh UVs to the target mesh UVs.
class TransferPanel : public QWidget {
    Q_OBJECT
    friend class UiTests;

  public:
    explicit TransferPanel(QWidget *parent = nullptr);
    ~TransferPanel();
    bool running() const { return busy_; }
    // Another GPU job (the AO bake) is running; transfers are blocked until it ends.
    void setExternalBusy(bool);
    // Cancels and joins a running transfer.
    void shutdown();
    bool loadMesh(bool source, const QString &path);
    void addTextures(const QStringList &files);
    TransferJob job() const;
    void applyJob(const TransferJob &);

  signals:
    void runningChanged(bool);

  protected:
    void dragEnterEvent(QDragEnterEvent *) override;
    void dropEvent(QDropEvent *) override;

  private:
    // Model
    std::shared_ptr<const Scene> source_, target_;
    QString sourcePath_, targetPath_;
    QList<SourceSet> sets_;
    QMap<QString, QString> materialSets_, pairs_;
    QList<OutputRule> outputs_;
    QSet<QString> editedPrefixes_;
    TransferReport report_;
    QHash<QString, QImage> previewCache_;
    // Controls
    QScrollArea *controlsScroll_;
    QWidget *controls_;
    QLabel *sourceLabel_, *targetLabel_, *validation_, *status_;
    QComboBox *sourceUV_, *targetUV_, *mode_, *ss_, *filter_, *bits_;
    QDoubleSpinBox *front_, *back_, *tolerance_;
    QSpinBox *padding_;
    QTreeWidget *setsTree_;
    QTableWidget *materialTable_, *pairTable_, *suffixTable_, *outputTable_;
    QCheckBox *pairing_, *islandAware_, *rebuildPadding_, *averageOverlaps_, *sourceDX_, *targetDX_, *coverage_;
    QLineEdit *outputDir_;
    QPushButton *transfer_, *cancel_, *saveJob_, *loadJob_, *removeSet_;
    QProgressBar *progress_;
    // Previews
    QTabWidget *previewTabs_;
    TransferViewport *sourceView_, *targetView_;
    QCheckBox *split_, *wire_, *singleSet_;
    QComboBox *display_, *compareOutput_, *compareSuffix_, *compareSet_;
    QGraphicsView *before_, *after_;
    QGraphicsScene *beforeScene_, *afterScene_;
    // State
    std::thread worker_;
    std::atomic_bool cancelFlag_{false};
    bool busy_ = false, externalBusy_ = false, updating_ = false;

    QList<SuffixRule> suffixRules() const;
    TransferSettings settings() const;
    void setSuffixRules(const QList<SuffixRule> &);
    void refreshSets();
    void refreshMaterials();
    void refreshPairs();
    void refreshOutputs();
    void unifyOutputs(int from);
    void updateDefaultPrefixes();
    void rediscover();
    void validate();
    void run(TransferBackend backend = TransferBackend::Gpu, bool confirmed = false);
    void finish(const TransferReport &, const QString &error, TransferBackend backend);
    void updateDisplayChoices();
    void updatePreview3D();
    void updateCompare();
    void loadSettings();
    void saveSettings() const;
    QImage previewOf(const QString &path);
    PreviewMaterial previewMaterial(const QMap<QString, QImage> &maps) const;
    QStringList setNames() const;
};
