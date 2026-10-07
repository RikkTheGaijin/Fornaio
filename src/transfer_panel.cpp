#include "transfer_panel.h"
#include "scroll_controls.h"
#include <QCheckBox>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QDateTime>
#include <algorithm>
#include <cmath>

namespace {
const QString kNone = "(none)", kAny = "(any)";
const QStringList kImageFilters{"*.png", "*.tga", "*.tif", "*.tiff", "*.jpg", "*.jpeg"};
const QStringList kMeshSuffixes{"fbx", "obj"};

QLabel *section(const QString &title) {
    auto l = new QLabel(title);
    l->setStyleSheet("font-size: 14px; font-weight: 600; color: #89d5ca; margin-top: 12px;");
    return l;
}
QLabel *note(const QString &text) {
    auto l = new QLabel(text);
    l->setWordWrap(true);
    l->setStyleSheet("color:#97a9be;");
    return l;
}
QTableWidget *table(const QStringList &headers, int height) {
    auto t = new QTableWidget(0, headers.size());
    t->setHorizontalHeaderLabels(headers);
    t->verticalHeader()->hide();
    t->horizontalHeader()->setStretchLastSection(true);
    t->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setMinimumHeight(height);
    t->setMaximumHeight(height * 2);
    return t;
}
QTableWidgetItem *fixedItem(const QString &text) {
    auto item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    item->setToolTip(text);
    return item;
}
// Zoomable image view; a partner view mirrors zoom and scroll position.
class ZoomView : public QGraphicsView {
  public:
    ZoomView() {
        setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
        setDragMode(QGraphicsView::ScrollHandDrag);
        setBackgroundBrush(QColor("#11151b"));
        setMinimumSize(200, 200);
    }
    ZoomView *partner = nullptr;

  protected:
    void wheelEvent(QWheelEvent *e) override {
        double f = std::pow(1.0015, e->angleDelta().y());
        scale(f, f);
        if (partner)
            partner->setTransform(transform());
        syncScroll();
        e->accept();
    }
    void scrollContentsBy(int dx, int dy) override {
        QGraphicsView::scrollContentsBy(dx, dy);
        syncScroll();
    }

  private:
    void syncScroll() {
        if (!partner || syncing_)
            return;
        syncing_ = partner->syncing_ = true;
        partner->horizontalScrollBar()->setValue(horizontalScrollBar()->value());
        partner->verticalScrollBar()->setValue(verticalScrollBar()->value());
        syncing_ = partner->syncing_ = false;
    }
    bool syncing_ = false;
};
// Both compare views use a 1024-unit wide scene so zoom and scroll map to the same UV region.
void showImage(QGraphicsView *view, QGraphicsScene *scene, const QImage &image) {
    scene->clear();
    auto item = scene->addPixmap(QPixmap::fromImage(image));
    item->setTransformationMode(Qt::SmoothTransformation);
    item->setScale(1024.0 / std::max(1, image.width()));
    scene->setSceneRect(item->sceneBoundingRect());
    if (view->transform().isIdentity())
        view->fitInView(scene->sceneRect(), Qt::KeepAspectRatio);
}
QImage withWire(QImage image, const Scene *scene, int uv, const std::function<bool(const Triangle &)> &keep) {
    image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
    if (!scene)
        return image;
    QVector<QLineF> lines;
    double w = image.width(), h = image.height();
    for (const Triangle &t : scene->tris) {
        if (uv < 0 || uv >= (int)t.uv.size() || !keep(t))
            continue;
        for (int k = 0; k < 3; k++) {
            V2 a = t.uv[uv][k], b = t.uv[uv][(k + 1) % 3];
            lines.push_back({a.x * w, (1 - a.y) * h, b.x * w, (1 - b.y) * h});
        }
    }
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor(255, 196, 80, 170), 0));
    p.drawLines(lines);
    return image;
}
QImage blank() {
    QImage im(1024, 1024, QImage::Format_ARGB32_Premultiplied);
    im.fill(QColor("#151b22"));
    return im;
}
QImage channel(const QImage &src, int c) {
    QImage in = src.convertToFormat(QImage::Format_RGBA8888), out(in.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < in.height(); y++) {
        const uchar *s = in.constScanLine(y);
        uchar *d = out.scanLine(y);
        for (int x = 0; x < in.width(); x++)
            d[x] = s[x * 4 + c];
    }
    return out;
}
} // namespace

TransferPanel::TransferPanel(QWidget *parent) : QWidget(parent) {
    setAcceptDrops(true);
    auto outer = new QVBoxLayout(this);
    outer->setContentsMargins(20, 16, 20, 16);
    auto split = new QSplitter;
    controlsScroll_ = new QScrollArea;
    controlsScroll_->setObjectName("transferControls");
    controlsScroll_->setWidgetResizable(true);
    controlsScroll_->setMinimumWidth(470);
    controls_ = new QWidget;
    auto c = new QVBoxLayout(controls_);

    // 01 Meshes --------------------------------------------------------------------------------
    c->addWidget(section("01   MESHES"));
    c->addWidget(note("Two copies of the same geometry that differ only in their UVs. Both must sit in the same "
                      "position. The same FBX can be used twice with different UV sets."));
    auto meshes = new QGridLayout;
    auto loadSource = new QPushButton("Source mesh…");
    auto loadTarget = new QPushButton("Target mesh…");
    auto sameAsSource = new QPushButton("Same file");
    sameAsSource->setToolTip("Use the source mesh file as the target (transfer between UV sets of one mesh).");
    sourceLabel_ = new QLabel("No source mesh");
    targetLabel_ = new QLabel("No target mesh");
    for (auto l : {sourceLabel_, targetLabel_}) {
        l->setStyleSheet("color:#97a9be;");
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        l->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }
    sourceUV_ = new ScrollComboBox;
    targetUV_ = new ScrollComboBox;
    meshes->addWidget(loadSource, 0, 0);
    meshes->addWidget(sourceLabel_, 0, 1, 1, 2);
    meshes->addWidget(new QLabel("Source UV"), 1, 0);
    meshes->addWidget(sourceUV_, 1, 1, 1, 2);
    meshes->addWidget(loadTarget, 2, 0);
    meshes->addWidget(targetLabel_, 2, 1);
    meshes->addWidget(sameAsSource, 2, 2);
    meshes->addWidget(new QLabel("Target UV"), 3, 0);
    meshes->addWidget(targetUV_, 3, 1, 1, 2);
    meshes->setColumnStretch(1, 1);
    c->addLayout(meshes);
    auto meshForm = new QFormLayout;
    mode_ = new ScrollComboBox;
    mode_->addItems({"Auto: exact face match, projection fallback", "Projection only"});
    mode_->setToolTip("Auto maps every target face to its identical source face (lossless, no ray casting) and "
                      "ray-projects only faces without a counterpart.");
    front_ = new ScrollDoubleSpinBox;
    back_ = new ScrollDoubleSpinBox;
    tolerance_ = new ScrollDoubleSpinBox;
    for (auto s : {front_, back_, tolerance_}) {
        s->setRange(0, 1e6);
        s->setDecimals(5);
        s->setSingleStep(.01);
        s->setSpecialValueText("Auto");
    }
    front_->setToolTip("Projection search distance outside the target surface (model units). Auto = 1% of the "
                       "mesh size.");
    back_->setToolTip("Projection search distance inside the target surface (model units). Auto = 1% of the mesh "
                      "size.");
    tolerance_->setToolTip("Vertex weld tolerance for exact face matching (model units). Auto = 0.001% of the mesh "
                           "size.");
    meshForm->addRow("Mode", mode_);
    meshForm->addRow("Front distance", front_);
    meshForm->addRow("Back distance", back_);
    meshForm->addRow("Match tolerance", tolerance_);
    c->addLayout(meshForm);

    // 02 Source textures -----------------------------------------------------------------------
    c->addWidget(section("02   SOURCE TEXTURES"));
    c->addWidget(note("Files named <set>_<SUFFIX>.png form texture sets, e.g. body_ALB.png + body_NRM.png = set "
                      "'body'. PNG, TGA, TIFF and JPEG are read; drop files here or use the buttons."));
    auto texButtons = new QHBoxLayout;
    auto addFiles = new QPushButton("Add textures…");
    auto addFolder = new QPushButton("Add folder…");
    removeSet_ = new QPushButton("Remove");
    auto clearSets = new QPushButton("Clear");
    for (auto b : {addFiles, addFolder, removeSet_, clearSets})
        texButtons->addWidget(b);
    c->addLayout(texButtons);
    setsTree_ = new QTreeWidget;
    setsTree_->setHeaderLabels({"Set / map", "File"});
    setsTree_->header()->setDefaultAlignment(Qt::AlignCenter);
    setsTree_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    setsTree_->setMinimumHeight(150);
    setsTree_->setColumnWidth(0, 170);
    c->addWidget(setsTree_);
    c->addWidget(new QLabel("Source material  >  texture set"));
    materialTable_ = table({"Source material", "Texture set"}, 110);
    materialTable_->setColumnWidth(0, 200);
    c->addWidget(materialTable_);

    // 03 Object pairing ------------------------------------------------------------------------
    c->addWidget(section("03   OBJECT PAIRING"));
    c->addWidget(note("Each target object only samples its paired source object, so neighbouring or overlapping "
                      "meshes never bleed into each other. (any) = all source objects, (none) = skip."));
    auto pairRow = new QHBoxLayout;
    pairing_ = new QCheckBox("Pair objects by name");
    pairing_->setChecked(true);
    auto autoPair = new QPushButton("Auto-pair");
    pairRow->addWidget(pairing_);
    pairRow->addStretch();
    pairRow->addWidget(autoPair);
    c->addLayout(pairRow);
    pairTable_ = table({"Target object", "Source object"}, 110);
    pairTable_->setColumnWidth(0, 200);
    c->addWidget(pairTable_);

    // 04 Map types -----------------------------------------------------------------------------
    c->addWidget(section("04   MAP TYPES"));
    c->addWidget(note("color = sRGB, filtered in linear light with premultiplied alpha. linear = data (roughness, "
                      "metal, AO...). normal = tangent space, re-oriented to the target tangent frames. packed = "
                      "per-channel data such as ORM. Default fills sets that lack the map."));
    suffixTable_ = table({"Suffix", "Type", "Default"}, 150);
    suffixTable_->setColumnWidth(0, 130);
    suffixTable_->setColumnWidth(1, 120);
    c->addWidget(suffixTable_);
    auto suffixButtons = new QHBoxLayout;
    auto addSuffix = new QPushButton("Add");
    auto removeSuffix = new QPushButton("Remove");
    auto resetSuffixes = new QPushButton("Reset defaults");
    suffixButtons->addWidget(addSuffix);
    suffixButtons->addWidget(removeSuffix);
    suffixButtons->addStretch();
    suffixButtons->addWidget(resetSuffixes);
    c->addLayout(suffixButtons);

    // 05 Output --------------------------------------------------------------------------------
    c->addWidget(section("05   OUTPUT"));
    c->addWidget(note("One texture set per target material, written as <prefix>_<SUFFIX>.png. Materials with the "
                      "same prefix share one texture set."));
    singleSet_ = new QCheckBox("Single texture set (all target materials share one prefix)");
    singleSet_->setToolTip("Writes every enabled target material into one texture set. The target UVs of those "
                           "materials should not overlap.");
    c->addWidget(singleSet_);
    outputTable_ = table({"Target material", "On", "Prefix", "Resolution"}, 110);
    outputTable_->setColumnWidth(0, 150);
    outputTable_->setColumnWidth(1, 36);
    outputTable_->setColumnWidth(2, 150);
    c->addWidget(outputTable_);
    auto dirRow = new QHBoxLayout;
    outputDir_ = new QLineEdit;
    outputDir_->setPlaceholderText("Output folder");
    auto browse = new QPushButton("Browse…");
    dirRow->addWidget(outputDir_, 1);
    dirRow->addWidget(browse);
    c->addLayout(dirRow);
    auto out = new QFormLayout;
    ss_ = new ScrollComboBox;
    for (int i = 1; i <= 4; i++)
        ss_->addItem(QString("%1 x %1 (%2 samples)").arg(i).arg(i * i), i);
    ss_->setCurrentIndex(2);
    ss_->setToolTip("Samples per texel. Each sample is mapped to the source exactly, then filtered with its "
                    "projected footprint.");
    filter_ = new ScrollComboBox;
    filter_->addItems({"Auto (bicubic / anisotropic EWA)", "Bicubic", "Bilinear"});
    filter_->setToolTip("Auto uses sharp bicubic sampling when texel density is similar and anisotropic EWA "
                        "filtering with mipmaps when the target minifies the source.");
    padding_ = new ScrollSpinBox;
    padding_->setRange(-1, 256);
    padding_->setValue(16);
    padding_->setSpecialValueText("Fill texture");
    padding_->setSuffix(" px");
    bits_ = new ScrollComboBox;
    bits_->addItem("Same as source", 0);
    bits_->addItem("8-bit", 8);
    bits_->addItem("16-bit", 16);
    out->addRow("Supersampling", ss_);
    out->addRow("Filter", filter_);
    out->addRow("Padding", padding_);
    out->addRow("Bit depth", bits_);
    c->addLayout(out);
    islandAware_ = new QCheckBox("Island-aware sampling and padding");
    islandAware_->setToolTip("Filter footprints and padding never cross into other UV islands.");
    rebuildPadding_ = new QCheckBox("Rebuild source padding before sampling");
    rebuildPadding_->setToolTip("Re-dilates source islands so filtering at seams never picks up background.");
    averageOverlaps_ = new QCheckBox("Average overlapping target UVs");
    sourceDX_ = new QCheckBox("Source normal maps are DirectX (Y-)");
    targetDX_ = new QCheckBox("Write DirectX normal maps (Y-)");
    coverage_ = new QCheckBox("Write coverage map (_COVERAGE)");
    for (auto b : {islandAware_, rebuildPadding_, averageOverlaps_}) {
        b->setChecked(true);
        c->addWidget(b);
    }
    for (auto b : {sourceDX_, targetDX_, coverage_})
        c->addWidget(b);
    c->addStretch();
    controlsScroll_->setWidget(controls_);
    split->addWidget(controlsScroll_);

    // Previews ---------------------------------------------------------------------------------
    previewTabs_ = new QTabWidget;
    auto page3d = new QWidget;
    auto v3 = new QVBoxLayout(page3d);
    auto bar3 = new QHBoxLayout;
    display_ = new QComboBox;
    display_->addItem("Lit");
    split_ = new QCheckBox("Split view: source | target");
    auto frame = new QPushButton("Frame");
    bar3->addWidget(new QLabel("Display"));
    bar3->addWidget(display_);
    bar3->addWidget(split_);
    bar3->addStretch();
    bar3->addWidget(frame);
    v3->addLayout(bar3);
    auto views = new QSplitter;
    sourceView_ = new TransferViewport;
    targetView_ = new TransferViewport;
    sourceView_->setCaption("SOURCE");
    targetView_->setCaption("TARGET");
    sourceView_->hide();
    views->addWidget(sourceView_);
    views->addWidget(targetView_);
    v3->addWidget(views, 1);
    previewTabs_->addTab(page3d, "3D preview");
    auto page2d = new QWidget;
    auto v2 = new QVBoxLayout(page2d);
    auto bar2 = new QHBoxLayout;
    compareSet_ = new QComboBox;
    compareOutput_ = new QComboBox;
    compareSuffix_ = new QComboBox;
    wire_ = new QCheckBox("UV wireframe");
    wire_->setChecked(true);
    auto fit2d = new QPushButton("Fit");
    bar2->addWidget(new QLabel("Map"));
    bar2->addWidget(compareSuffix_);
    bar2->addWidget(new QLabel("Source set"));
    bar2->addWidget(compareSet_);
    bar2->addWidget(new QLabel("Output"));
    bar2->addWidget(compareOutput_);
    bar2->addWidget(wire_);
    bar2->addStretch();
    bar2->addWidget(fit2d);
    v2->addLayout(bar2);
    auto images = new QSplitter;
    auto before = new ZoomView, after = new ZoomView;
    before->partner = after;
    after->partner = before;
    before_ = before;
    after_ = after;
    beforeScene_ = new QGraphicsScene(this);
    afterScene_ = new QGraphicsScene(this);
    before_->setScene(beforeScene_);
    after_->setScene(afterScene_);
    std::pair<QGraphicsView *, const char *> compareViews[] = {{before_, "Before: source UVs"},
                                                               {after_, "After: target UVs"}};
    for (auto [view, title] : compareViews) {
        auto box = new QWidget;
        auto l = new QVBoxLayout(box);
        l->setContentsMargins(0, 0, 0, 0);
        l->addWidget(new QLabel(title));
        l->addWidget(view, 1);
        images->addWidget(box);
    }
    v2->addWidget(images, 1);
    previewTabs_->addTab(page2d, "2D compare");
    split->addWidget(previewTabs_);
    split->setStretchFactor(1, 1);
    split->setSizes({500, 900});
    outer->addWidget(split, 1);

    auto footer = new QHBoxLayout;
    saveJob_ = new QPushButton("Save job…");
    loadJob_ = new QPushButton("Load job…");
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    cancel_ = new QPushButton("Cancel");
    cancel_->setEnabled(false);
    transfer_ = new QPushButton("Transfer");
    transfer_->setObjectName("primary");
    transfer_->setMinimumSize(130, 42);
    footer->addWidget(saveJob_);
    footer->addWidget(loadJob_);
    footer->addWidget(progress_, 1);
    footer->addWidget(cancel_);
    footer->addWidget(transfer_);
    outer->addLayout(footer);
    validation_ = new QLabel;
    validation_->setWordWrap(true);
    validation_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_ = new QLabel("Load a source and a target mesh, then add the source textures.");
    status_->setWordWrap(true);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    outer->addWidget(validation_);
    outer->addWidget(status_);

    // Connections ------------------------------------------------------------------------------
    auto pickMesh = [this](bool source) {
        QSettings st;
        QString path = QFileDialog::getOpenFileName(this, source ? "Source mesh" : "Target mesh",
                                                    st.value("transfer/meshDir").toString(),
                                                    "Meshes (*.fbx *.obj);;All files (*)");
        if (!path.isEmpty())
            loadMesh(source, path);
    };
    connect(loadSource, &QPushButton::clicked, this, [pickMesh] { pickMesh(true); });
    connect(loadTarget, &QPushButton::clicked, this, [pickMesh] { pickMesh(false); });
    connect(sameAsSource, &QPushButton::clicked, this, [this] {
        if (!sourcePath_.isEmpty())
            loadMesh(false, sourcePath_);
    });
    auto uvChanged = [this] {
        if (updating_)
            return;
        sourceView_->setMesh(source_, std::max(0, sourceUV_->currentIndex()));
        targetView_->setMesh(target_, std::max(0, targetUV_->currentIndex()));
        validate();
        updateCompare();
    };
    connect(sourceUV_, &QComboBox::currentIndexChanged, this, uvChanged);
    connect(targetUV_, &QComboBox::currentIndexChanged, this, uvChanged);
    auto changed = [this] {
        if (!updating_)
            validate();
    };
    connect(mode_, &QComboBox::currentIndexChanged, this, changed);
    for (auto s : {front_, back_, tolerance_})
        connect(s, &QDoubleSpinBox::valueChanged, this, changed);
    connect(ss_, &QComboBox::currentIndexChanged, this, changed);
    connect(filter_, &QComboBox::currentIndexChanged, this, changed);
    connect(bits_, &QComboBox::currentIndexChanged, this, changed);
    connect(padding_, &QSpinBox::valueChanged, this, changed);
    connect(outputDir_, &QLineEdit::textChanged, this, changed);
    for (auto b : {islandAware_, rebuildPadding_, averageOverlaps_, coverage_})
        connect(b, &QCheckBox::toggled, this, changed);
    connect(sourceDX_, &QCheckBox::toggled, this, [this] {
        updatePreview3D();
        validate();
    });
    connect(targetDX_, &QCheckBox::toggled, this, [this] { validate(); });
    connect(addFiles, &QPushButton::clicked, this, [this] {
        QSettings st;
        QStringList files = QFileDialog::getOpenFileNames(this, "Source textures",
                                                          st.value("transfer/textureDir").toString(),
                                                          "Images (" + kImageFilters.join(' ') + ");;All files (*)");
        if (!files.isEmpty())
            addTextures(files);
    });
    connect(addFolder, &QPushButton::clicked, this, [this] {
        QSettings st;
        QString dir = QFileDialog::getExistingDirectory(this, "Source texture folder",
                                                        st.value("transfer/textureDir").toString());
        if (dir.isEmpty())
            return;
        QStringList files;
        for (const QString &f : QDir(dir).entryList(kImageFilters, QDir::Files, QDir::Name))
            files << QDir(dir).absoluteFilePath(f);
        if (files.isEmpty())
            status_->setText("No PNG, TGA, TIFF or JPEG files in " + QDir::toNativeSeparators(dir) + ".");
        else
            addTextures(files);
    });
    connect(removeSet_, &QPushButton::clicked, this, [this] {
        for (QTreeWidgetItem *item : setsTree_->selectedItems()) {
            QString set = item->parent() ? item->parent()->text(0) : item->text(0);
            for (int i = 0; i < sets_.size(); i++)
                if (sets_[i].name == set) {
                    if (item->parent())
                        sets_[i].files.remove(item->text(0));
                    if (!item->parent() || sets_[i].files.isEmpty())
                        sets_.removeAt(i);
                    break;
                }
        }
        refreshSets();
    });
    connect(clearSets, &QPushButton::clicked, this, [this] {
        sets_.clear();
        refreshSets();
    });
    connect(pairing_, &QCheckBox::toggled, this, [this](bool on) {
        pairTable_->setEnabled(on);
        if (!updating_)
            validate();
    });
    connect(autoPair, &QPushButton::clicked, this, [this] {
        if (source_ && target_)
            pairs_ = guessPairs(target_->objects, source_->objects);
        refreshPairs();
        validate();
    });
    connect(suffixTable_, &QTableWidget::itemChanged, this, [this] {
        if (!updating_)
            rediscover();
    });
    connect(addSuffix, &QPushButton::clicked, this, [this] {
        auto rules = suffixRules();
        rules.push_back({"NEW", MapType::Linear, "#808080"});
        setSuffixRules(rules);
        suffixTable_->scrollToBottom();
        suffixTable_->editItem(suffixTable_->item(suffixTable_->rowCount() - 1, 0));
    });
    connect(removeSuffix, &QPushButton::clicked, this, [this] {
        int row = suffixTable_->currentRow();
        if (row < 0)
            return;
        auto rules = suffixRules();
        rules.removeAt(row);
        setSuffixRules(rules);
        rediscover();
    });
    connect(resetSuffixes, &QPushButton::clicked, this, [this] {
        setSuffixRules(defaultSuffixes());
        rediscover();
    });
    connect(outputTable_, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (updating_ || item->row() >= outputs_.size())
            return;
        OutputRule &o = outputs_[item->row()];
        if (item->column() == 1)
            o.enabled = item->checkState() == Qt::Checked;
        if (item->column() == 2) {
            o.prefix = item->text().trimmed();
            editedPrefixes_.insert(o.material);
            if (singleSet_->isChecked()) {
                for (const OutputRule &r : outputs_)
                    editedPrefixes_.insert(r.material);
                unifyOutputs(item->row());
                // Rebuilding the table inside its own itemChanged signal is unsafe.
                QMetaObject::invokeMethod(this, [this] { refreshOutputs(); }, Qt::QueuedConnection);
            }
        }
        updateDisplayChoices();
        validate();
    });
    connect(singleSet_, &QCheckBox::toggled, this, [this](bool on) {
        if (updating_)
            return;
        if (!on)
            editedPrefixes_.clear();
        updateDefaultPrefixes();
        refreshOutputs();
        updateDisplayChoices();
        updatePreview3D();
        validate();
        updateCompare();
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        QString dir = QFileDialog::getExistingDirectory(this, "Output folder", outputDir_->text());
        if (!dir.isEmpty())
            outputDir_->setText(QDir::toNativeSeparators(dir));
    });
    connect(display_, &QComboBox::currentIndexChanged, this, [this](int i) {
        QString suffix = i <= 0 ? QString() : display_->currentText();
        sourceView_->setDisplay(suffix);
        targetView_->setDisplay(suffix);
    });
    connect(split_, &QCheckBox::toggled, this, [this](bool on) {
        sourceView_->setVisible(on);
        if (on) {
            sourceView_->setCamera(targetView_->camera());
            updatePreview3D();
        }
    });
    connect(frame, &QPushButton::clicked, targetView_, &TransferViewport::fit);
    connect(sourceView_, &TransferViewport::cameraChanged, targetView_, &TransferViewport::setCamera);
    connect(targetView_, &TransferViewport::cameraChanged, sourceView_, &TransferViewport::setCamera);
    for (auto combo : {compareSet_, compareOutput_, compareSuffix_})
        connect(combo, &QComboBox::currentIndexChanged, this, [this] {
            if (!updating_)
                updateCompare();
        });
    connect(wire_, &QCheckBox::toggled, this, [this] { updateCompare(); });
    connect(fit2d, &QPushButton::clicked, this, [this] {
        before_->fitInView(beforeScene_->sceneRect(), Qt::KeepAspectRatio);
        after_->fitInView(afterScene_->sceneRect(), Qt::KeepAspectRatio);
    });
    connect(transfer_, &QPushButton::clicked, this, [this] { run(); });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        cancelFlag_ = true;
        status_->setText("Cancelling…");
    });
    connect(saveJob_, &QPushButton::clicked, this, [this] {
        QString path = QFileDialog::getSaveFileName(this, "Save transfer job",
                                                    QSettings().value("transfer/jobDir").toString(),
                                                    "Transfer job (*.json)");
        if (path.isEmpty())
            return;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(job().toJson()).toJson()) < 0) {
            QMessageBox::warning(this, "Save job", "Cannot write " + QDir::toNativeSeparators(path));
            return;
        }
        QSettings().setValue("transfer/jobDir", QFileInfo(path).absolutePath());
        status_->setText("Saved job " + QDir::toNativeSeparators(path) + ".");
    });
    connect(loadJob_, &QPushButton::clicked, this, [this] {
        QString path = QFileDialog::getOpenFileName(this, "Load transfer job",
                                                    QSettings().value("transfer/jobDir").toString(),
                                                    "Transfer job (*.json)");
        if (path.isEmpty())
            return;
        try {
            QFile f(path);
            if (!f.open(QIODevice::ReadOnly))
                throw std::runtime_error("Cannot open the file.");
            QJsonParseError error;
            auto doc = QJsonDocument::fromJson(f.readAll(), &error);
            if (!doc.isObject())
                throw std::runtime_error(error.errorString().toStdString());
            QGuiApplication::setOverrideCursor(Qt::WaitCursor);
            TransferJob j = TransferJob::fromJson(doc.object(), QFileInfo(path).absolutePath());
            QGuiApplication::restoreOverrideCursor();
            applyJob(j);
            QSettings().setValue("transfer/jobDir", QFileInfo(path).absolutePath());
            status_->setText("Loaded job " + QDir::toNativeSeparators(path) + ".");
        } catch (const std::exception &e) {
            QGuiApplication::restoreOverrideCursor();
            QMessageBox::warning(this, "Load job", QString::fromUtf8(e.what()));
        }
    });
    setSuffixRules(defaultSuffixes());
    loadSettings();
    pairTable_->setEnabled(pairing_->isChecked());
    validate();
    updateCompare();
}
TransferPanel::~TransferPanel() {
    shutdown();
    saveSettings();
}
void TransferPanel::shutdown() {
    cancelFlag_ = true;
    if (worker_.joinable())
        worker_.join();
}
void TransferPanel::setExternalBusy(bool busy) {
    externalBusy_ = busy;
    validate();
}

// Model <-> UI ---------------------------------------------------------------------------------
QStringList TransferPanel::setNames() const {
    QStringList names;
    for (const SourceSet &s : sets_)
        names << s.name;
    return names;
}
QList<SuffixRule> TransferPanel::suffixRules() const {
    QList<SuffixRule> rules;
    for (int r = 0; r < suffixTable_->rowCount(); r++) {
        SuffixRule rule;
        rule.suffix = suffixTable_->item(r, 0) ? suffixTable_->item(r, 0)->text().trimmed().toUpper() : QString();
        auto combo = qobject_cast<QComboBox *>(suffixTable_->cellWidget(r, 1));
        rule.type = combo ? mapTypeFromName(combo->currentText()) : MapType::Linear;
        rule.fill = suffixTable_->item(r, 2) ? suffixTable_->item(r, 2)->text().trimmed() : QString("#808080");
        rules << rule;
    }
    return rules;
}
void TransferPanel::setSuffixRules(const QList<SuffixRule> &rules) {
    updating_ = true;
    suffixTable_->setRowCount(rules.size());
    for (int r = 0; r < rules.size(); r++) {
        suffixTable_->setItem(r, 0, new QTableWidgetItem(rules[r].suffix));
        auto combo = new ScrollComboBox;
        for (MapType t : {MapType::Color, MapType::Linear, MapType::Normal, MapType::Packed})
            combo->addItem(mapTypeName(t));
        combo->setCurrentText(mapTypeName(rules[r].type));
        connect(combo, &QComboBox::currentIndexChanged, this, [this] {
            if (!updating_) {
                updatePreview3D();
                validate();
            }
        });
        suffixTable_->setCellWidget(r, 1, combo);
        auto fill = new QTableWidgetItem(rules[r].fill);
        fill->setToolTip("File-space value as #RRGGBB or #AARRGGBB, used when a set lacks this map.");
        suffixTable_->setItem(r, 2, fill);
    }
    updating_ = false;
}
TransferSettings TransferPanel::settings() const {
    TransferSettings s;
    s.sourceUV = std::max(0, sourceUV_->currentIndex());
    s.targetUV = std::max(0, targetUV_->currentIndex());
    s.mode = mode_->currentIndex();
    s.front = (float)front_->value();
    s.back = (float)back_->value();
    s.tolerance = (float)tolerance_->value();
    s.ss = ss_->currentData().toInt();
    s.filter = filter_->currentIndex();
    s.padding = padding_->value();
    s.bits = bits_->currentData().toInt();
    s.islandAware = islandAware_->isChecked();
    s.rebuildSourcePadding = rebuildPadding_->isChecked();
    s.averageOverlaps = averageOverlaps_->isChecked();
    s.pairing = pairing_->isChecked();
    s.sourceNormalDX = sourceDX_->isChecked();
    s.targetNormalDX = targetDX_->isChecked();
    s.coverageMap = coverage_->isChecked();
    return s;
}
TransferJob TransferPanel::job() const {
    TransferJob j;
    j.sourceMesh = sourcePath_;
    j.targetMesh = targetPath_;
    j.source = source_;
    j.target = target_;
    j.outputDir = QDir::fromNativeSeparators(outputDir_->text().trimmed());
    j.suffixes = suffixRules();
    j.sets = sets_;
    if (source_)
        for (const QString &m : source_->materials)
            j.materialSets[m] = materialSets_.value(m);
    if (target_)
        for (const QString &o : target_->objects)
            j.pairs[o] = pairs_.value(o, "*");
    j.outputs = outputs_;
    j.settings = settings();
    return j;
}
void TransferPanel::applyJob(const TransferJob &j) {
    updating_ = true;
    source_ = j.source;
    target_ = j.target;
    sourcePath_ = j.sourceMesh;
    targetPath_ = j.targetMesh;
    sourceLabel_->setText(source_ ? QDir::toNativeSeparators(sourcePath_) : "No source mesh");
    targetLabel_->setText(target_ ? QDir::toNativeSeparators(targetPath_) : "No target mesh");
    sourceUV_->clear();
    targetUV_->clear();
    if (source_)
        sourceUV_->addItems(source_->uvNames);
    if (target_)
        targetUV_->addItems(target_->uvNames);
    const TransferSettings &s = j.settings;
    sourceUV_->setCurrentIndex(std::min(s.sourceUV, sourceUV_->count() - 1));
    targetUV_->setCurrentIndex(std::min(s.targetUV, targetUV_->count() - 1));
    mode_->setCurrentIndex(s.mode);
    front_->setValue(s.front);
    back_->setValue(s.back);
    tolerance_->setValue(s.tolerance);
    ss_->setCurrentIndex(std::clamp(s.ss, 1, 4) - 1);
    filter_->setCurrentIndex(std::clamp(s.filter, 0, 2));
    padding_->setValue(s.padding);
    bits_->setCurrentIndex(s.bits == 8 ? 1 : s.bits == 16 ? 2 : 0);
    islandAware_->setChecked(s.islandAware);
    rebuildPadding_->setChecked(s.rebuildSourcePadding);
    averageOverlaps_->setChecked(s.averageOverlaps);
    pairing_->setChecked(s.pairing);
    pairTable_->setEnabled(s.pairing);
    sourceDX_->setChecked(s.sourceNormalDX);
    targetDX_->setChecked(s.targetNormalDX);
    coverage_->setChecked(s.coverageMap);
    QStringList prefixes;
    for (const OutputRule &o : j.outputs)
        if (o.enabled && !prefixes.contains(o.prefix, Qt::CaseInsensitive))
            prefixes << o.prefix;
    singleSet_->setChecked(prefixes.size() == 1 && j.outputs.size() > 1);
    outputDir_->setText(QDir::toNativeSeparators(j.outputDir));
    sets_ = j.sets;
    materialSets_ = j.materialSets;
    pairs_ = j.pairs;
    outputs_ = j.outputs;
    editedPrefixes_.clear();
    for (const OutputRule &o : outputs_)
        editedPrefixes_.insert(o.material);
    report_ = {};
    updating_ = false;
    setSuffixRules(j.suffixes);
    if (target_) {
        // Make sure every target material has an output row.
        auto defaults = defaultOutputs(target_->materials, setNames());
        for (const OutputRule &d : defaults)
            if (std::none_of(outputs_.begin(), outputs_.end(), [&](const OutputRule &o) { return o.material == d.material; }))
                outputs_ << d;
        unifyOutputs(0);
    }
    sourceView_->setMesh(source_, s.sourceUV);
    targetView_->setMesh(target_, s.targetUV);
    if (auto frame = target_ ? target_ : source_) {
        sourceView_->setFraming(frame->center, frame->scale);
        targetView_->setFraming(frame->center, frame->scale);
        targetView_->fit();
    }
    refreshSets();
    refreshPairs();
}
bool TransferPanel::loadMesh(bool source, const QString &path) {
    std::shared_ptr<const Scene> scene;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        scene = std::make_shared<Scene>(loadScene(path));
        QGuiApplication::restoreOverrideCursor();
    } catch (const std::exception &e) {
        QGuiApplication::restoreOverrideCursor();
        QMessageBox::warning(this, "Load mesh", QString::fromUtf8(e.what()));
        return false;
    }
    if (scene->uvNames.isEmpty()) {
        QMessageBox::warning(this, "Load mesh", QDir::toNativeSeparators(path) + " has no UV set.");
        return false;
    }
    QSettings().setValue("transfer/meshDir", QFileInfo(path).absolutePath());
    updating_ = true;
    QComboBox *uv = source ? sourceUV_ : targetUV_;
    QString previous = uv->currentText();
    uv->clear();
    uv->addItems(scene->uvNames);
    int index = scene->uvNames.indexOf(previous);
    // The same file used twice usually means "transfer UV0 to UV1".
    if (!source && QFileInfo(path) == QFileInfo(sourcePath_) && scene->uvNames.size() > 1)
        index = sourceUV_->currentIndex() == 0 ? 1 : 0;
    uv->setCurrentIndex(std::max(0, index));
    (source ? sourceLabel_ : targetLabel_)
        ->setText(QString("%1  (%2 triangles, %3 UV sets)")
                      .arg(QFileInfo(path).fileName())
                      .arg(scene->tris.size())
                      .arg(scene->uvNames.size()));
    (source ? sourceLabel_ : targetLabel_)->setToolTip(QDir::toNativeSeparators(path));
    if (source) {
        source_ = scene;
        sourcePath_ = path;
        auto guess = guessMaterialSets(source_->materials, setNames());
        for (const QString &m : source_->materials)
            if (!setNames().contains(materialSets_.value(m)))
                materialSets_[m] = guess.value(m);
    } else {
        target_ = scene;
        targetPath_ = path;
        QList<OutputRule> next;
        auto defaults = defaultOutputs(target_->materials, setNames());
        for (const OutputRule &d : defaults) {
            auto it = std::find_if(outputs_.begin(), outputs_.end(), [&](const OutputRule &o) { return o.material == d.material; });
            next << (it != outputs_.end() ? *it : d);
        }
        outputs_ = next;
    }
    if (source_ && target_) {
        auto guess = guessPairs(target_->objects, source_->objects);
        for (const QString &o : target_->objects)
            if (!pairs_.contains(o) || (pairs_[o] != "*" && !pairs_[o].isEmpty() && !source_->objects.contains(pairs_[o])))
                pairs_[o] = guess.value(o);
    }
    report_ = {};
    updating_ = false;
    sourceView_->setMesh(source_, std::max(0, sourceUV_->currentIndex()));
    targetView_->setMesh(target_, std::max(0, targetUV_->currentIndex()));
    auto frame = target_ ? target_ : source_;
    sourceView_->setFraming(frame->center, frame->scale);
    targetView_->setFraming(frame->center, frame->scale);
    targetView_->fit();
    updateDefaultPrefixes();
    refreshSets();
    refreshPairs();
    return true;
}
void TransferPanel::addTextures(const QStringList &files) {
    QStringList unknown;
    auto found = discoverSets(files, suffixRules(), &unknown);
    sets_ = mergeSets(sets_, found);
    if (!files.isEmpty()) {
        QString dir = QFileInfo(files.front()).absolutePath();
        QSettings().setValue("transfer/textureDir", dir);
        if (outputDir_->text().trimmed().isEmpty())
            outputDir_->setText(QDir::toNativeSeparators(QDir(dir).absoluteFilePath("transferred")));
    }
    if (source_) {
        auto guess = guessMaterialSets(source_->materials, setNames());
        for (const QString &m : source_->materials)
            if (!setNames().contains(materialSets_.value(m)))
                materialSets_[m] = guess.value(m);
    }
    updateDefaultPrefixes();
    refreshSets();
    QStringList names;
    for (const QString &f : unknown)
        names << QFileInfo(f).fileName();
    if (!unknown.isEmpty())
        status_->setText(QString("Skipped %1 file(s) without a known suffix (add it in Map types): %2")
                             .arg(unknown.size())
                             .arg(names.mid(0, 6).join(", ") + (names.size() > 6 ? ", ..." : "")));
    else
        status_->setText(QString("Added %1 texture(s).").arg(files.size()));
}
void TransferPanel::rediscover() {
    QStringList files;
    for (const SourceSet &s : sets_)
        for (const QString &f : s.files)
            files << f;
    QStringList unknown;
    QMap<QString, QString> keep = materialSets_;
    sets_ = discoverSets(files, suffixRules(), &unknown);
    materialSets_ = keep;
    refreshSets();
    if (!unknown.isEmpty())
        status_->setText(QString("%1 texture(s) no longer match a suffix and were dropped.").arg(unknown.size()));
}
void TransferPanel::updateDefaultPrefixes() {
    if (!target_)
        return;
    QStringList used;
    for (const SourceSet &s : sets_)
        if (source_ && std::any_of(source_->materials.begin(), source_->materials.end(),
                                   [&](const QString &m) { return materialSets_.value(m) == s.name; }))
            used << s.name;
    if (used.isEmpty())
        used = setNames();
    auto defaults = defaultOutputs(target_->materials, used);
    bool single = singleSet_->isChecked();
    int edited = -1;
    for (int r = 0; r < outputs_.size(); r++) {
        OutputRule &o = outputs_[r];
        if (editedPrefixes_.contains(o.material)) {
            if (edited < 0)
                edited = r;
            continue;
        }
        if (single)
            o.prefix = suggestPrefix(used);
        else
            for (const OutputRule &d : defaults)
                if (d.material == o.material)
                    o.prefix = d.prefix;
    }
    unifyOutputs(std::max(0, edited));
}
void TransferPanel::unifyOutputs(int from) {
    if (!singleSet_->isChecked() || from >= outputs_.size())
        return;
    const OutputRule source = outputs_[from];
    for (OutputRule &o : outputs_) {
        o.prefix = source.prefix;
        o.resolution = source.resolution;
    }
}
void TransferPanel::refreshSets() {
    updating_ = true;
    setsTree_->clear();
    QSet<QString> known;
    for (const SuffixRule &r : suffixRules())
        known.insert(r.suffix);
    for (const SourceSet &s : sets_) {
        auto top = new QTreeWidgetItem(setsTree_, {s.name, QString("%1 map(s)").arg(s.files.size())});
        for (auto it = s.files.begin(); it != s.files.end(); ++it) {
            auto child = new QTreeWidgetItem(top, {it.key(), QFileInfo(it.value()).fileName()});
            child->setToolTip(1, QDir::toNativeSeparators(it.value()));
            if (!known.contains(it.key()))
                child->setForeground(0, QColor("#f0c477"));
        }
        top->setExpanded(sets_.size() <= 4);
    }
    QString currentSet = compareSet_->currentText();
    compareSet_->clear();
    compareSet_->addItems(setNames());
    compareSet_->setCurrentText(currentSet);
    updating_ = false;
    refreshMaterials();
    refreshOutputs();
    updateDisplayChoices();
    updatePreview3D();
    validate();
    updateCompare();
}
void TransferPanel::refreshMaterials() {
    updating_ = true;
    materialTable_->setRowCount(0);
    if (source_) {
        materialTable_->setRowCount(source_->materials.size());
        for (int r = 0; r < source_->materials.size(); r++) {
            QString m = source_->materials[r];
            materialTable_->setItem(r, 0, fixedItem(m));
            auto combo = new ScrollComboBox;
            if (sets_.isEmpty()) {
                combo->addItem("(add textures first)");
                combo->setEnabled(false);
                combo->setToolTip("Texture sets come from the source textures added above.");
                materialTable_->setCellWidget(r, 1, combo);
                continue;
            }
            combo->addItem(kNone);
            combo->addItems(setNames());
            int index = combo->findText(materialSets_.value(m));
            combo->setCurrentIndex(materialSets_.value(m).isEmpty() || index < 0 ? 0 : index);
            connect(combo, &QComboBox::currentIndexChanged, this, [this, m, combo](int i) {
                if (updating_)
                    return;
                materialSets_[m] = i <= 0 ? QString() : combo->currentText();
                updateDefaultPrefixes();
                refreshOutputs();
                updateDisplayChoices();
                updatePreview3D();
                validate();
                updateCompare();
            });
            materialTable_->setCellWidget(r, 1, combo);
        }
    }
    updating_ = false;
}
void TransferPanel::refreshPairs() {
    updating_ = true;
    pairTable_->setRowCount(0);
    if (target_) {
        pairTable_->setRowCount(target_->objects.size());
        for (int r = 0; r < target_->objects.size(); r++) {
            QString o = target_->objects[r];
            pairTable_->setItem(r, 0, fixedItem(o));
            auto combo = new ScrollComboBox;
            combo->addItems({kAny, kNone});
            if (source_)
                combo->addItems(source_->objects);
            QString value = pairs_.value(o, "*");
            int index = value == "*" ? 0 : value.isEmpty() ? 1 : combo->findText(value, Qt::MatchExactly);
            combo->setCurrentIndex(std::max(0, index));
            connect(combo, &QComboBox::currentIndexChanged, this, [this, o, combo](int i) {
                if (updating_)
                    return;
                pairs_[o] = i <= 0 ? QString("*") : i == 1 ? QString() : combo->currentText();
                validate();
            });
            pairTable_->setCellWidget(r, 1, combo);
        }
    }
    updating_ = false;
}
void TransferPanel::refreshOutputs() {
    updating_ = true;
    outputTable_->setRowCount(outputs_.size());
    for (int r = 0; r < outputs_.size(); r++) {
        const OutputRule &o = outputs_[r];
        outputTable_->setItem(r, 0, fixedItem(o.material));
        auto on = new QTableWidgetItem;
        on->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        on->setCheckState(o.enabled ? Qt::Checked : Qt::Unchecked);
        outputTable_->setItem(r, 1, on);
        outputTable_->setItem(r, 2, new QTableWidgetItem(o.prefix));
        auto res = new ScrollComboBox;
        res->addItem("Auto (source)", 0);
        for (int size : {256, 512, 1024, 2048, 4096, 8192, 16384})
            res->addItem(QString::number(size), size);
        res->setCurrentIndex(std::max(0, res->findData(o.resolution)));
        res->setToolTip("Auto = the largest source texture used by this output.");
        connect(res, &QComboBox::currentIndexChanged, this, [this, r, res] {
            if (updating_ || r >= outputs_.size())
                return;
            outputs_[r].resolution = res->currentData().toInt();
            if (singleSet_->isChecked()) {
                unifyOutputs(r);
                QMetaObject::invokeMethod(this, [this] { refreshOutputs(); }, Qt::QueuedConnection);
            }
            validate();
        });
        outputTable_->setCellWidget(r, 3, res);
    }
    updating_ = false;
}

// Validation and running -----------------------------------------------------------------------
void TransferPanel::validate() {
    if (updating_)
        return;
    QStringList warnings;
    QStringList errors = checkJob(job(), &warnings);
    QStringList lines;
    for (const QString &e : errors.mid(0, 6))
        lines << "<span style='color:#ff858c;'>" + e.toHtmlEscaped() + "</span>";
    for (const QString &w : warnings.mid(0, std::max(0, 8 - (int)lines.size())))
        lines << "<span style='color:#f0c477;'>" + w.toHtmlEscaped() + "</span>";
    int hidden = errors.size() + warnings.size() - lines.size();
    if (hidden > 0)
        lines << QString("<span style='color:#97a9be;'>... and %1 more</span>").arg(hidden);
    if (lines.isEmpty())
        lines << "<span style='color:#89d5ca;'>Ready to transfer.</span>";
    validation_->setText(lines.join("<br>"));
    validation_->setToolTip((errors + warnings).join('\n'));
    transfer_->setEnabled(errors.isEmpty() && !busy_ && !externalBusy_);
    transfer_->setToolTip(externalBusy_ ? "Waiting for the AO bake to finish." : QString());
}
void TransferPanel::run(TransferBackend backend, bool confirmed) {
    if (busy_ || externalBusy_)
        return;
    if (worker_.joinable())
        worker_.join();
    TransferJob j = job();
    if (!checkJob(j).isEmpty()) {
        validate();
        return;
    }
    QStringList existing = existingOutputs(j);
    if (!existing.isEmpty() && !confirmed) {
        QStringList names;
        for (const QString &f : existing.mid(0, 8))
            names << QFileInfo(f).fileName();
        auto answer = QMessageBox::question(
            this, "Overwrite textures",
            QString("%1 output file(s) already exist:\n%2%3\n\nOverwrite them?")
                .arg(existing.size())
                .arg(names.join('\n'), existing.size() > 8 ? "\n..." : ""));
        if (answer != QMessageBox::Yes)
            return;
    }
    j.settings.overwrite = true;
    saveSettings();
    busy_ = true;
    cancelFlag_ = false;
    controls_->setEnabled(false);
    saveJob_->setEnabled(false);
    loadJob_->setEnabled(false);
    transfer_->setEnabled(false);
    cancel_->setEnabled(true);
    progress_->setValue(0);
    status_->setText(backend == TransferBackend::Cpu ? "Transferring on the CPU…" : "Transferring…");
    emit runningChanged(true);
    worker_ = std::thread([this, j, backend] {
        try {
            TransferReport r = runTransfer(j, backend, cancelFlag_, [this](float p, const QString &text) {
                QMetaObject::invokeMethod(
                    this,
                    [this, p, text] {
                        progress_->setValue((int)(p * 1000));
                        status_->setText(text);
                    },
                    Qt::QueuedConnection);
            });
            QMetaObject::invokeMethod(this, [this, r, backend] { finish(r, {}, backend); }, Qt::QueuedConnection);
        } catch (const std::exception &e) {
            QString error = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(this, [this, error, backend] { finish({}, error, backend); }, Qt::QueuedConnection);
        }
    });
}
void TransferPanel::finish(const TransferReport &r, const QString &error, TransferBackend backend) {
    if (worker_.joinable())
        worker_.join();
    busy_ = false;
    controls_->setEnabled(true);
    saveJob_->setEnabled(true);
    loadJob_->setEnabled(true);
    cancel_->setEnabled(false);
    emit runningChanged(false);
    validate();
    if (!error.isEmpty()) {
        progress_->setValue(0);
        if (cancelFlag_) {
            status_->setText("Transfer cancelled.");
            return;
        }
        status_->setText("Transfer failed: " + error);
        if (backend == TransferBackend::Gpu &&
            QMessageBox::question(this, "Transfer failed",
                                  error + "\n\nRun the transfer on the CPU instead? It produces the same result but "
                                          "is much slower.") == QMessageBox::Yes)
            run(TransferBackend::Cpu, true);
        return;
    }
    report_ = r;
    progress_->setValue(1000);
    double total = r.coveredTexels + r.missedTexels;
    QString summary = QString("Wrote %1 map(s) in %2 s on %3. %4 of %5 target triangles matched exactly; %6% of "
                              "samples projected; %7% texel coverage.")
                          .arg(r.maps.size())
                          .arg(r.seconds, 0, 'f', 2)
                          .arg(r.device)
                          .arg(r.exactTriangles)
                          .arg(r.targetTriangles)
                          .arg(r.samples > 0 ? 100 * r.projectedSamples / r.samples : 0, 0, 'f', 1)
                          .arg(total > 0 ? 100 * r.coveredTexels / total : 100, 0, 'f', 2);
    for (const QString &w : r.warnings.mid(0, 5))
        summary += "\n- " + w;
    if (r.warnings.size() > 5)
        summary += QString("\n- ... %1 more warning(s)").arg(r.warnings.size() - 5);
    status_->setText(summary);
    status_->setToolTip(r.warnings.join('\n'));
    updateDisplayChoices();
    updatePreview3D();
    updateCompare();
}

// Previews -------------------------------------------------------------------------------------
QImage TransferPanel::previewOf(const QString &path) {
    QFileInfo info(path);
    QString key = info.absoluteFilePath() + "|" + QString::number(info.lastModified().toMSecsSinceEpoch());
    auto it = previewCache_.find(key);
    if (it != previewCache_.end())
        return it.value();
    QImage image = readTextureImage(path);
    if (!image.isNull()) {
        if (image.width() > 2048 || image.height() > 2048)
            image = image.scaled(2048, 2048, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        image = image.convertToFormat(QImage::Format_RGBA8888);
    }
    if (previewCache_.size() > 64)
        previewCache_.clear();
    previewCache_.insert(key, image);
    return image;
}
PreviewMaterial TransferPanel::previewMaterial(const QMap<QString, QImage> &maps) const {
    PreviewMaterial m;
    m.maps = maps;
    QList<SuffixRule> rules = suffixRules();
    auto typeOf = [&](const QString &suffix) {
        for (const SuffixRule &r : rules)
            if (r.suffix == suffix)
                return r.type;
        return MapType::Linear;
    };
    auto first = [&](const QStringList &names) {
        for (const QString &n : names)
            if (maps.contains(n))
                return maps[n];
        return QImage();
    };
    m.base = first({"ALB", "BC", "BASECOLOR", "COL", "DIFF", "ALBEDO", "DIFFUSE", "BASE"});
    for (auto it = maps.begin(); it != maps.end(); ++it) {
        MapType t = typeOf(it.key());
        if (m.base.isNull() && t == MapType::Color && it.key() != "EMI" && it.key() != "SSS")
            m.base = it.value();
        if (m.normal.isNull() && t == MapType::Normal)
            m.normal = it.value();
    }
    m.roughness = first({"RGH", "ROUGH", "ROUGHNESS"});
    m.metallic = first({"MET", "METAL", "METALLIC", "METALNESS"});
    m.occlusion = first({"OCC", "AO", "OCCLUSION"});
    QImage orm = first({"ORM"});
    if (!orm.isNull()) {
        if (m.occlusion.isNull())
            m.occlusion = channel(orm, 0);
        if (m.roughness.isNull())
            m.roughness = channel(orm, 1);
        if (m.metallic.isNull())
            m.metallic = channel(orm, 2);
    }
    QImage opacity = first({"OPA", "ALPHA", "OPACITY"});
    if (!opacity.isNull() && !m.base.isNull()) {
        QImage base = m.base.convertToFormat(QImage::Format_RGBA8888);
        QImage a = channel(opacity.scaled(base.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation), 0);
        for (int y = 0; y < base.height(); y++) {
            uchar *d = base.scanLine(y);
            const uchar *s = a.constScanLine(y);
            for (int x = 0; x < base.width(); x++)
                d[x * 4 + 3] = s[x];
        }
        m.base = base;
    }
    return m;
}
void TransferPanel::updateDisplayChoices() {
    QStringList suffixes;
    QSet<QString> present;
    for (const SourceSet &s : sets_)
        for (auto it = s.files.begin(); it != s.files.end(); ++it)
            present.insert(it.key());
    for (const TransferMap &m : report_.maps)
        present.insert(m.suffix);
    for (const SuffixRule &r : suffixRules())
        if (present.contains(r.suffix))
            suffixes << r.suffix;
    if (present.contains("COVERAGE"))
        suffixes << "COVERAGE";
    updating_ = true;
    QString display = display_->currentText(), compare = compareSuffix_->currentText(),
            output = compareOutput_->currentText();
    display_->clear();
    display_->addItem("Lit");
    display_->addItems(suffixes);
    display_->setCurrentIndex(std::max(0, display_->findText(display)));
    compareSuffix_->clear();
    compareSuffix_->addItems(suffixes);
    compareSuffix_->setCurrentIndex(std::max(0, compareSuffix_->findText(compare)));
    QStringList prefixes;
    for (const OutputRule &o : outputs_)
        if (o.enabled && !o.prefix.isEmpty() && !prefixes.contains(o.prefix, Qt::CaseInsensitive))
            prefixes << o.prefix;
    compareOutput_->clear();
    compareOutput_->addItems(prefixes);
    compareOutput_->setCurrentIndex(std::max(0, compareOutput_->findText(output)));
    updating_ = false;
    QString suffix = display_->currentIndex() <= 0 ? QString() : display_->currentText();
    sourceView_->setDisplay(suffix);
    targetView_->setDisplay(suffix);
}
void TransferPanel::updatePreview3D() {
    QMap<QString, PreviewMaterial> target;
    if (target_)
        for (const OutputRule &o : outputs_) {
            if (!o.enabled)
                continue;
            QMap<QString, QImage> maps;
            for (const TransferMap &m : report_.maps)
                if (m.prefix.compare(o.prefix, Qt::CaseInsensitive) == 0 && !m.preview.isNull())
                    maps[m.suffix] = m.preview;
            if (!maps.isEmpty())
                target[o.material] = previewMaterial(maps);
        }
    targetView_->setMaterials(target, targetDX_->isChecked());
    if (!split_->isChecked() || !source_)
        return;
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    QMap<QString, PreviewMaterial> source;
    for (const QString &material : source_->materials) {
        for (const SourceSet &s : sets_)
            if (s.name == materialSets_.value(material)) {
                QMap<QString, QImage> maps;
                for (auto it = s.files.begin(); it != s.files.end(); ++it) {
                    QImage image = previewOf(it.value());
                    if (!image.isNull())
                        maps[it.key()] = image;
                }
                source[material] = previewMaterial(maps);
            }
    }
    QGuiApplication::restoreOverrideCursor();
    sourceView_->setMaterials(source, sourceDX_->isChecked());
}
void TransferPanel::updateCompare() {
    QString suffix = compareSuffix_->currentText(), setName = compareSet_->currentText();
    bool wire = wire_->isChecked();
    QImage before;
    for (const SourceSet &s : sets_)
        if (s.name == setName && s.files.contains(suffix))
            before = previewOf(s.files[suffix]);
    if (before.isNull())
        before = blank();
    if (wire && source_) {
        const Scene *scene = source_.get();
        before = withWire(before, scene, std::max(0, sourceUV_->currentIndex()), [&](const Triangle &t) {
            return t.material >= 0 && t.material < scene->materials.size() &&
                   materialSets_.value(scene->materials[t.material]) == setName;
        });
    }
    showImage(before_, beforeScene_, before);
    QString prefix = compareOutput_->currentText();
    QImage after;
    for (const TransferMap &m : report_.maps)
        if (m.prefix.compare(prefix, Qt::CaseInsensitive) == 0 && m.suffix == suffix)
            after = m.preview;
    if (after.isNull())
        after = blank();
    if (wire && target_) {
        QSet<QString> materials;
        for (const OutputRule &o : outputs_)
            if (o.enabled && o.prefix.compare(prefix, Qt::CaseInsensitive) == 0)
                materials.insert(o.material);
        const Scene *scene = target_.get();
        after = withWire(after, scene, std::max(0, targetUV_->currentIndex()), [&](const Triangle &t) {
            return t.material >= 0 && t.material < scene->materials.size() &&
                   materials.contains(scene->materials[t.material]);
        });
    }
    showImage(after_, afterScene_, after);
}

// Persistence and drag & drop ------------------------------------------------------------------
void TransferPanel::loadSettings() {
    QSettings q;
    q.beginGroup("transfer");
    auto doc = QJsonDocument::fromJson(q.value("suffixes").toByteArray());
    if (doc.isArray() && !doc.array().isEmpty()) {
        QList<SuffixRule> rules;
        for (const auto &v : doc.array()) {
            QJsonObject o = v.toObject();
            rules.push_back({o["suffix"].toString(), mapTypeFromName(o["type"].toString()), o["fill"].toString()});
        }
        setSuffixRules(rules);
    }
    updating_ = true;
    mode_->setCurrentIndex(std::clamp(q.value("mode", 0).toInt(), 0, 1));
    ss_->setCurrentIndex(std::clamp(q.value("ss", 3).toInt(), 1, 4) - 1);
    filter_->setCurrentIndex(std::clamp(q.value("filter", 0).toInt(), 0, 2));
    padding_->setValue(q.value("padding", 16).toInt());
    bits_->setCurrentIndex(std::clamp(q.value("bitsIndex", 0).toInt(), 0, 2));
    islandAware_->setChecked(q.value("islandAware", true).toBool());
    rebuildPadding_->setChecked(q.value("rebuildPadding", true).toBool());
    averageOverlaps_->setChecked(q.value("averageOverlaps", true).toBool());
    pairing_->setChecked(q.value("pairing", true).toBool());
    sourceDX_->setChecked(q.value("sourceDX", false).toBool());
    targetDX_->setChecked(q.value("targetDX", false).toBool());
    coverage_->setChecked(q.value("coverage", false).toBool());
    split_->setChecked(q.value("split", false).toBool());
    singleSet_->setChecked(q.value("singleTextureSet", true).toBool());
    outputDir_->setText(q.value("outputDir").toString());
    updating_ = false;
}
void TransferPanel::saveSettings() const {
    QSettings q;
    q.beginGroup("transfer");
    QJsonArray rules;
    for (const SuffixRule &r : suffixRules())
        rules.append(QJsonObject{{"suffix", r.suffix}, {"type", mapTypeName(r.type)}, {"fill", r.fill}});
    q.setValue("suffixes", QJsonDocument(rules).toJson(QJsonDocument::Compact));
    q.setValue("mode", mode_->currentIndex());
    q.setValue("ss", ss_->currentData().toInt());
    q.setValue("filter", filter_->currentIndex());
    q.setValue("padding", padding_->value());
    q.setValue("bitsIndex", bits_->currentIndex());
    q.setValue("islandAware", islandAware_->isChecked());
    q.setValue("rebuildPadding", rebuildPadding_->isChecked());
    q.setValue("averageOverlaps", averageOverlaps_->isChecked());
    q.setValue("pairing", pairing_->isChecked());
    q.setValue("sourceDX", sourceDX_->isChecked());
    q.setValue("targetDX", targetDX_->isChecked());
    q.setValue("coverage", coverage_->isChecked());
    q.setValue("split", split_->isChecked());
    q.setValue("singleTextureSet", singleSet_->isChecked());
    q.setValue("outputDir", outputDir_->text());
}
void TransferPanel::dragEnterEvent(QDragEnterEvent *e) {
    if (!busy_ && e->mimeData()->hasUrls())
        e->acceptProposedAction();
}
void TransferPanel::dropEvent(QDropEvent *e) {
    QStringList textures;
    for (const QUrl &url : e->mimeData()->urls()) {
        QFileInfo info(url.toLocalFile());
        if (info.isDir()) {
            for (const QString &f : QDir(info.absoluteFilePath()).entryList(kImageFilters, QDir::Files, QDir::Name))
                textures << QDir(info.absoluteFilePath()).absoluteFilePath(f);
        } else if (kMeshSuffixes.contains(info.suffix().toLower())) {
            loadMesh(!source_, info.absoluteFilePath());
        } else if (info.isFile())
            textures << info.absoluteFilePath();
    }
    if (!textures.isEmpty())
        addTextures(textures);
    e->acceptProposedAction();
}
