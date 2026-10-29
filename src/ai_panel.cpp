#include "ai_panel.h"
#include "ai_layers.h"
#include "scroll_controls.h"
#include "transfer.h"
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpressionValidator>
#include <QResizeEvent>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QUuid>
#include <algorithm>

namespace {
class ProjectionLayerDelegate : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        QStyleOptionViewItem item(option);
        initStyleOption(&item, index);
        const bool visible = index.data(Qt::CheckStateRole).toInt() == Qt::Checked;
        p->save();
        if (item.state & QStyle::State_Selected)
            p->fillRect(item.rect, QColor("#264c4a"));
        const QPointF center(item.rect.left() + 13, item.rect.center().y());
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(QColor(visible ? "#80ddcc" : "#71818d"), 1.5));
        QPainterPath eye;
        eye.moveTo(center + QPointF(-8, 0));
        eye.quadTo(center + QPointF(0, -9), center + QPointF(8, 0));
        eye.quadTo(center + QPointF(0, 9), center + QPointF(-8, 0));
        p->drawPath(eye);
        p->drawEllipse(center, 2, 2);
        if (!visible)
            p->drawLine(center + QPointF(-8, -7), center + QPointF(8, 7));
        item.features &= ~QStyleOptionViewItem::HasCheckIndicator;
        item.rect.adjust(28, 0, 0, 0);
        (option.widget ? option.widget->style() : QApplication::style())
            ->drawControl(QStyle::CE_ItemViewItem, &item, p, option.widget);
        p->restore();
    }
    bool editorEvent(QEvent *event, QAbstractItemModel *model, const QStyleOptionViewItem &option,
                     const QModelIndex &index) override {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton && mouse->position().x() < option.rect.left() + 28 &&
                (index.flags() & Qt::ItemIsEnabled))
                return model->setData(index,
                                      index.data(Qt::CheckStateRole).toInt() == Qt::Checked ? Qt::Unchecked
                                                                                            : Qt::Checked,
                                      Qt::CheckStateRole);
        }
        return QStyledItemDelegate::editorEvent(event, model, option, index);
    }
};
QPushButton *button(QVBoxLayout *layout, const QString &name, const std::function<void()> &action) {
    auto b = new QPushButton(name);
    layout->addWidget(b);
    QObject::connect(b, &QPushButton::clicked, b, action);
    return b;
}
QString imageData(const QImage &im) {
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    im.save(&buffer, "PNG");
    return QString::fromLatin1(bytes.toBase64());
}
QByteArray compositionKey(const AiAtlases &base, const QList<QJsonObject> &records,
                          bool parity, const QString &isolation) {
    QByteArray key = parity ? "parity:" : "legacy:";
    key += isolation.toUtf8();
    for (auto it = base.begin(); it != base.end(); ++it)
        key += ':' + it.key().toUtf8() + ':' + QByteArray::number(it->color.cacheKey()) + ':' +
               QByteArray::number(it->coverage.cacheKey()) + ':' + QByteArray::number(it->confidence.cacheKey());
    for (const auto &record : records)
        key += ':' + record["layerId"].toString().toUtf8() +
               (record["visible"].toBool(true) ? "+" : "-") +
               QJsonDocument(record["blending"].toObject()).toJson(QJsonDocument::Compact);
    return QCryptographicHash::hash(key, QCryptographicHash::Sha256);
}
int compositionCost(const AiAtlases &atlases) {
    qint64 bytes = 0;
    for (const auto &atlas : atlases)
        bytes += atlas.color.sizeInBytes() + atlas.coverage.sizeInBytes() +
                 atlas.confidence.sizeInBytes() + atlas.projectionWeight.size() * sizeof(float);
    return int((bytes + 1023) / 1024);
}
void atomicImage(const QImage &im, const QString &path) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || !im.save(&file, "PNG") || !file.commit())
        throw std::runtime_error(("Cannot save " + path).toStdString());
}
class SquareFrame : public QWidget {
    QWidget *view_;
    bool compact_;

  public:
    SquareFrame(QWidget *v, bool compact = false) : view_(v), compact_(compact) {
        view_->setParent(this);
        setMinimumSize(compact ? 210 : 260, compact ? 210 : 260);
        if (compact) {
            auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
            policy.setHeightForWidth(true);
            setSizePolicy(policy);
            setMaximumHeight(280);
        }
    }
    QSize sizeHint() const override { return compact_ ? QSize(280, 280) : QWidget::sizeHint(); }
    int heightForWidth(int w) const override { return compact_ ? std::min(w, 280) : -1; }

  protected:
    void resizeEvent(QResizeEvent *) override {
        int n = std::min(width(), height());
        view_->setGeometry((width() - n) / 2, (height() - n) / 2, n, n);
    }
};
class AiReferenceList : public QListWidget {
  protected:
    void paintEvent(QPaintEvent *event) override {
        QListWidget::paintEvent(event);
        if (!count()) {
            QPainter painter(viewport());
            painter.setPen(QColor("#7f91a5"));
            painter.drawText(viewport()->rect(), Qt::AlignCenter, "No reference images yet");
        }
    }
};
} // namespace

// Candidate-space painting is independent of viewport camera movement.
class AiMaskCanvas : public QWidget {
  public:
    QImage image, mask;
    int brush = 24;
    bool editable = true;
    bool outline = false;
    QImage silhouette;
    std::function<void()> maskChanged;
    AiMaskCanvas() {
        setMinimumSize(240, 240);
        setMouseTracking(false);
    }
    QRectF imageRect() const {
        if (image.isNull())
            return {};
        QSizeF size = image.size();
        size.scale(this->size(), Qt::KeepAspectRatio);
        return QRectF((width() - size.width()) / 2, (height() - size.height()) / 2, size.width(),
                      size.height());
    }
    void set(const QImage &im, const QImage &m) {
        image = im;
        mask = m.convertToFormat(QImage::Format_RGB32);
        update();
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.fillRect(rect(), QColor("#111923"));
        if (image.isNull()) {
            p.setPen(QColor("#97a9be"));
            p.drawText(rect(), Qt::AlignCenter, "Your generated image will appear here");
            return;
        }
        QRectF r = imageRect();
        p.drawImage(r, image);
        QImage overlay(mask.size(), QImage::Format_ARGB32);
        for (int y = 0; y < mask.height(); ++y) {
            auto row = reinterpret_cast<QRgb *>(overlay.scanLine(y));
            for (int x = 0; x < mask.width(); ++x)
                row[x] = qRgba(255, 60, 60, (255 - qGray(mask.pixel(x, y))) / 2);
        }
        p.drawImage(r, overlay);
        if (outline && !silhouette.isNull()) {
            QImage edges(silhouette.size(), QImage::Format_ARGB32);
            edges.fill(Qt::transparent);
            for (int y = 1; y + 1 < edges.height(); ++y)
                for (int x = 1; x + 1 < edges.width(); ++x)
                    if (qGray(silhouette.pixel(x, y)) > 127 &&
                        (qGray(silhouette.pixel(x - 1, y)) < 128 || qGray(silhouette.pixel(x + 1, y)) < 128 ||
                         qGray(silhouette.pixel(x, y - 1)) < 128 || qGray(silhouette.pixel(x, y + 1)) < 128))
                        edges.setPixel(x, y, qRgba(50, 255, 220, 255));
            p.drawImage(r, edges);
        }
    }
    void mousePressEvent(QMouseEvent *e) override { paint(e); }
    void mouseMoveEvent(QMouseEvent *e) override { paint(e); }
    void paint(QMouseEvent *e) {
        if (!editable || image.isNull() || !(e->buttons() & (Qt::LeftButton | Qt::RightButton)))
            return;
        QRectF r = imageRect();
        if (!r.contains(e->position()))
            return;
        QPointF point = (e->position() - r.topLeft()) * (image.width() / r.width());
        QPainter p(&mask);
        p.setPen(Qt::NoPen);
        p.setBrush(e->buttons() & Qt::RightButton ? Qt::white : Qt::black);
        p.drawEllipse(point, brush / 2., brush / 2.);
        p.end();
        update();
        if (maskChanged)
            maskChanged();
    }
};

AiPanel::AiPanel(QWidget *parent) : QWidget(parent), installer_(this), engine_(this), cloud_(this) {
    try {
        aiInitializeProjectionRenderer();
    } catch (const std::exception &error) {
        projectionError_ = QString::fromUtf8(error.what());
    }
    setObjectName("aiTexturingPanel");
    QSettings settings;
    installer_.setRoot(settings.value("ai/location", AiInstaller::defaultRoot()).toString());
    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    auto split = new QSplitter;
    layout->addWidget(split, 1);
    auto scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setMinimumWidth(340);
    scroll->setMaximumWidth(470);
    split->addWidget(scroll);
    auto controls = new QWidget;
    auto left = new QVBoxLayout(controls);
    scroll->setWidget(controls);
    auto heading = new QLabel("AI TEXTURING");
    heading->setStyleSheet("font-size:18px;font-weight:600;color:#89d5ca");
    left->addWidget(heading);
    auto modelBox = new QGroupBox("Image model");
    auto modelLayout = new QVBoxLayout(modelBox);
    model_ = new ScrollComboBox;
    model_->setObjectName("aiModel");
    model_->addItem("GPT Image 2 · cloud", "gpt");
    model_->addItem("Fibo 2.0 Edit Turbo · local (optional download)", "fibo");
    model_->setCurrentIndex(std::max(0, model_->findData(settings.value("ai/model", "gpt").toString())));
    modelLayout->addWidget(model_);
    cloudInfo_ = new QLabel;
    cloudInfo_->setWordWrap(true);
    modelLayout->addWidget(cloudInfo_);
    left->addWidget(modelBox);
    auto setup = new QGroupBox("Local AI engine");
    localEngine_ = setup;
    auto setupLayout = new QVBoxLayout(setup);
    setup->setCheckable(true);
    auto setupBody = new QWidget;
    setupLayout->addWidget(setupBody);
    setupLayout = new QVBoxLayout(setupBody);
    setup->setChecked(!installer_.installed());
    setupBody->setVisible(setup->isChecked());
    connect(setup, &QGroupBox::toggled, setupBody, &QWidget::setVisible);
    setupInfo_ = new QLabel;
    setupInfo_->setWordWrap(true);
    setupLayout->addWidget(setupInfo_);
    location_ = new QLineEdit(installer_.root());
    location_->setReadOnly(true);
    setupLayout->addWidget(location_);
    button(setupLayout, "Change storage location…", [this] { chooseLocation(); });
    hfToken_ = new QLineEdit;
    hfToken_->setObjectName("aiHfToken");
    hfToken_->setEchoMode(QLineEdit::Password);
    hfToken_->setPlaceholderText("Hugging Face token (optional; HF_TOKEN or the HF CLI login is used)");
    hfToken_->setToolTip("Fibo is gated: accept its license on huggingface.co/briaai/Fibo-2.0-edit-turbo first. "
                         "The token is used for this download only and is not saved.");
    setupLayout->addWidget(hfToken_);
    install_ = button(setupLayout, "Download & Enable AI Texturing", [this] { beginInstall(); });
    install_->setObjectName("aiInstall");
    auto tools = new QHBoxLayout;
    setupLayout->addLayout(tools);
    auto repair = new QPushButton("Repair"), offline = new QPushButton("Import offline pack…"),
         remove = new QPushButton("Remove");
    for (auto b : {repair, offline, remove})
        tools->addWidget(b);
    connect(repair, &QPushButton::clicked, this, [this] { beginInstall({}, true); });
    connect(offline, &QPushButton::clicked, this, [this] {
        QString path = QFileDialog::getOpenFileName(this, "Import offline AI pack manifest", {},
                                                    "Fornaio AI pack (manifest.json)");
        if (!path.isEmpty())
            beginInstall(QFileInfo(path).absolutePath());
    });
    connect(remove, &QPushButton::clicked, this, [this] {
        if (busy_ || installer_.busy())
            return;
        if (QMessageBox::question(
                this, "Remove AI components",
                "Remove downloaded AI components from this location? Saved texturing projects are kept.") !=
            QMessageBox::Yes)
            return;
        try {
            engine_.stop();
            installer_.remove();
            status_->setText("AI components removed.");
        } catch (const std::exception &e) {
            status_->setText(QString::fromUtf8(e.what()));
        }
        refresh();
    });
    auto notices = button(setupLayout, "Licenses and component information", [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(aiAssetsPath("AI_NOTICES.md")));
    });
    Q_UNUSED(notices);
    inputs_ = new QWidget;
    auto inputLayout = new QVBoxLayout(inputs_);
    inputLayout->setContentsMargins(0, 0, 0, 0);
    left->addWidget(inputs_);
    button(inputLayout, "Load mesh…  FBX / OBJ", [this] {
        QString path = QFileDialog::getOpenFileName(
            this, "Load mesh", QSettings().value("ai/meshDir").toString(), "Meshes (*.fbx *.obj)");
        if (!path.isEmpty())
            loadMesh(path);
    });
    meshInfo_ = new QLabel("No mesh loaded");
    meshInfo_->setWordWrap(true);
    inputLayout->addWidget(meshInfo_);
    auto form = new QFormLayout;
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    auto advanced = new QGroupBox("Mesh, materials and generation settings");
    advanced->setCheckable(true);
    auto advancedLayout = new QVBoxLayout(advanced);
    auto advancedBody = new QWidget;
    auto advancedContents = new QVBoxLayout(advancedBody);
    advancedContents->addLayout(form);
    advancedLayout->addWidget(advancedBody);
    advanced->setChecked(false);
    advancedBody->hide();
    connect(advanced, &QGroupBox::toggled, advancedBody, &QWidget::setVisible);
    uv_ = new ScrollComboBox;
    uv_->setObjectName("aiUV");
    form->addRow("Texture UV", uv_);
    resolution_ = new ScrollComboBox;
    for (int n : {1024, 2048, 4096})
        resolution_->addItem(QString::number(n), n);
    resolution_->setCurrentIndex(1);
    form->addRow("Atlas size", resolution_);
    generationResolution_ = new ScrollComboBox;
    generationResolution_->setObjectName("aiGenerationResolution");
    generationResolution_->addItem("1024 · Full detail", 1024);
    generationResolution_->addItem("512 · Fast draft", 512);
    generationResolution_->setToolTip(
        "Fast draft generates a smaller camera image for quicker iterations. Atlas size stays independent.");
    form->addRow("Generation size", generationResolution_);
    profile_ = new ScrollComboBox;
    profile_->addItem("Standard · 24 GB", "standard");
    profile_->addItem("Memory saving · 12–16 GB", "memory");
    form->addRow("Memory profile", profile_);
    seed_ = new ScrollSpinBox;
    seed_->setRange(0, 2147483647);
    randomSeed_ = new QCheckBox("New variation each generation");
    randomSeed_->setObjectName("aiRandomSeed");
    randomSeed_->setChecked(true);
    form->addRow(randomSeed_);
    form->addRow("Fixed seed", seed_);
    seed_->setEnabled(false);
    connect(randomSeed_, &QCheckBox::toggled, seed_, [this](bool on) { seed_->setEnabled(!on); });
    auto generation = new QGroupBox("Generate");
    generation->setObjectName("aiGenerationCard");
    auto generationLayout = new QVBoxLayout(generation);
    generationLayout->setSpacing(8);
    inputLayout->addWidget(generation);
    generationMode_ = new QTabBar;
    generationMode_->setObjectName("aiGenerationMode");
    generationMode_->addTab("Single");
    generationMode_->addTab("Multiview");
    generationMode_->setExpanding(true);
    generationMode_->setDrawBase(false);
    generationMode_->setStyleSheet(
        "QTabBar::tab { background:#17232e; color:#b5c4d4; padding:8px 20px; border-radius:4px; }"
        "QTabBar::tab:selected { background:#365b58; color:#c8fff1; }"
        "QTabBar::tab:hover { background:#29413f; }");
    generationLayout->addWidget(generationMode_);
    sideCanvas_ = new AiMaskCanvas;
    sideCanvas_->setObjectName("aiSideCandidate");
    sideCanvas_->setMinimumSize(210, 210);
    candidateFrame_ = new SquareFrame(sideCanvas_, true);
    generationLayout->addWidget(candidateFrame_);
    // Multiview shows the cameras it will generate, as normal-map thumbnails, instead of one candidate.
    multiviewBox_ = new QWidget;
    multiviewBox_->setObjectName("aiMultiviewViews");
    auto multiviewLayout = new QVBoxLayout(multiviewBox_);
    multiviewLayout->setContentsMargins(0, 0, 0, 0);
    auto viewsHeader = new QHBoxLayout;
    auto viewsTitle = new QLabel("Camera views");
    viewsTitle->setStyleSheet("font-size:15px;font-weight:600");
    viewsHeader->addWidget(viewsTitle, 1);
    auto resetViews = new QPushButton("↺");
    resetViews->setToolTip("Reset to the default views: front, the ring around it, back and top");
    auto addView = new QPushButton("+");
    addView->setToolTip("Add the current 3D preview camera as a view");
    for (auto control : {resetViews, addView}) {
        control->setFixedSize(30, 30);
        control->setStyleSheet("QPushButton { padding:2px; font-size:18px; }");
        viewsHeader->addWidget(control);
    }
    multiviewLayout->addLayout(viewsHeader);
    viewGrid_ = new QListWidget;
    viewGrid_->setObjectName("aiViewGrid");
    viewGrid_->setViewMode(QListView::IconMode);
    viewGrid_->setIconSize(QSize(96, 96));
    viewGrid_->setGridSize(QSize(110, 110));
    viewGrid_->setResizeMode(QListView::Adjust);
    viewGrid_->setMovement(QListView::Static);
    viewGrid_->setMinimumHeight(240);
    viewGrid_->setContextMenuPolicy(Qt::CustomContextMenu);
    viewGrid_->setToolTip("Generated in this order. Double-click to look through a view; right-click to remove it.");
    multiviewLayout->addWidget(viewGrid_);
    generationLayout->addWidget(multiviewBox_);
    multiviewBox_->hide();
    connect(resetViews, &QPushButton::clicked, this, [this] {
        resetMultiviewCameras();
        refresh();
    });
    connect(addView, &QPushButton::clicked, this, [this] {
        if (!scene_)
            return;
        multiviewCameras_ << aiFrameCamera(*scene_, view_->camera(), true);
        refreshMultiviewViews();
        refresh();
    });
    connect(viewGrid_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        const int index = viewGrid_->row(item);
        if (index >= 0 && index < multiviewCameras_.size())
            view_->setCamera(multiviewCameras_[index]);
    });
    connect(viewGrid_, &QListWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        const int index = viewGrid_->row(viewGrid_->itemAt(point));
        if (index < 0 || busy_)
            return;
        QMenu menu;
        auto look = menu.addAction("Look through this view");
        auto remove = menu.addAction("Remove view");
        auto chosen = menu.exec(viewGrid_->viewport()->mapToGlobal(point));
        if (chosen == look)
            view_->setCamera(multiviewCameras_[index]);
        else if (chosen == remove) {
            multiviewCameras_.removeAt(index);
            refreshMultiviewViews();
            refresh();
        }
    });
    generationLayout->addWidget(new QLabel("Prompt"));
    prompt_ = new QPlainTextEdit;
    prompt_->setObjectName("aiPrompt");
    prompt_->setStyleSheet("QPlainTextEdit { background:#0d1620; color:#eef3ff; border:1px solid "
                           "#52647c; border-radius:5px; padding:8px; } "
                           "QPlainTextEdit:focus { border:1px solid #4bc4af; }");
    prompt_->setPlaceholderText("Weathered painted metal, chipped red edges…");
    prompt_->setMinimumHeight(85);
    prompt_->setMaximumHeight(96);
    generationLayout->addWidget(prompt_);
    auto referenceHeader = new QHBoxLayout;
    referenceHeader->addWidget(new QLabel("Reference images"), 1);
    generationLayout->addLayout(referenceHeader);
    references_ = new AiReferenceList;
    references_->setObjectName("aiReferences");
    references_->setViewMode(QListView::IconMode);
    references_->setIconSize(QSize(64, 64));
    references_->setGridSize(QSize(82, 82));
    references_->setWordWrap(false);
    references_->setTextElideMode(Qt::ElideMiddle);
    references_->setResizeMode(QListView::Adjust);
    references_->setMinimumHeight(84);
    references_->setMaximumHeight(84);
    generationLayout->addWidget(references_);
    auto continuityForm = new QFormLayout;
    continuity_ = new ScrollSpinBox;
    continuity_->setObjectName("aiContinuity");
    continuity_->setRange(0, 100);
    continuity_->setValue(80);
    continuity_->setSuffix(" %");
    continuity_->setToolTip(
        "Reuse more accepted paint from reliable previous projections. Strong camera-facing "
        "details stay sharp; weak falloff and empty areas are regenerated. "
        "Set 0% to generate a completely new appearance.");
    continuityForm->addRow("Keep previous details", continuity_);
    generationLayout->addLayout(continuityForm);
    protectDetails_ = new QCheckBox("Protect trusted details");
    protectDetails_->setObjectName("aiProtectDetails");
    protectDetails_->setChecked(true);
    protectDetails_->setToolTip(
        "Anchor trusted paint during generation and preserve its original atlas details. "
        "The confidence ramp softens the boundary. Disable to let the AI reinterpret all details.");
    advancedContents->addWidget(protectDetails_);
    auto addReference = button(generationLayout, "+", [this] {
        for (const auto &path : QFileDialog::getOpenFileNames(
                 this, "Appearance references", {}, "Images (*.png *.jpg *.jpeg *.tif *.tiff *.tga)")) {
            QString err;
            if (readTextureImage(path, &err).isNull()) {
                status_->setText(err);
                continue;
            }
            if (references_->findItems(path, Qt::MatchExactly).empty()) {
                auto item =
                    new QListWidgetItem(QIcon(QPixmap::fromImage(readTextureImage(path))), path, references_);
                item->setToolTip(path);
            }
        }
        refresh();
    });
    auto removeReference = button(generationLayout, "−", [this] {
        delete references_->takeItem(references_->currentRow());
        refresh();
    });
    generationLayout->removeWidget(addReference);
    generationLayout->removeWidget(removeReference);
    addReference->setToolTip("Add appearance reference images");
    removeReference->setToolTip("Remove selected reference");
    addReference->setFixedWidth(30);
    removeReference->setFixedWidth(30);
    for (auto control : {addReference, removeReference}) {
        control->setFixedHeight(30);
        control->setStyleSheet("QPushButton { padding:2px; font-size:20px; }");
    }
    referenceHeader->addWidget(removeReference);
    referenceHeader->addWidget(addReference);
    advancedContents->addWidget(new QLabel("Materials to paint / export"));
    materials_ = new QListWidget;
    materials_->setMaximumHeight(110);
    advancedContents->addWidget(materials_);
    generate_ = button(generationLayout, "Generate image",
                       [this] { generate(generationMode_->currentIndex() == 1); });
    generate_->setObjectName("aiGenerate");
    generate_->setMinimumHeight(40);
    generate_->setStyleSheet(
        "QPushButton { background:#4bc4af; color:#10252b; font-size:15px; "
        "font-weight:600; border:1px solid #54847d; border-radius:5px; } "
        "QPushButton:hover { background:#69d3c0; } "
        "QPushButton:disabled { background:#2c655f; color:#9aafab; border-color:#354c50; }");
    generate_->setToolTip(
        "Generate a fresh camera-aligned image. Single mode lets you inspect and project it; "
        "Multiview generates and projects a sequence of cameras.");
    auto geometryOutline = new QCheckBox("Show captured geometry outline");
    geometryOutline->setChecked(false);
    advancedContents->addWidget(geometryOutline);
    auto painting = new QLabel("Candidate mask: left drag excludes, right drag restores.");
    painting->setWordWrap(true);
    advancedContents->addWidget(painting);
    brush_ = new ScrollSpinBox;
    brush_->setRange(2, 256);
    brush_->setValue(32);
    form->addRow("Mask brush size", brush_);
    auto blending = new QGroupBox("Projection blending");
    blending->setCheckable(true);
    auto blendingLayout = new QVBoxLayout(blending);
    auto blendingBody = new QWidget;
    auto blendForm = new QFormLayout(blendingBody);
    blendForm->setRowWrapPolicy(QFormLayout::WrapLongRows);
    blendingLayout->addWidget(blendingBody);
    blending->setChecked(false);
    blendingBody->hide();
    connect(blending, &QGroupBox::toggled, blendingBody, &QWidget::setVisible);
    blendMode_ = new ScrollComboBox;
    blendMode_->setObjectName("aiBlendMode");
    blendMode_->addItem("Keep existing paint (paint around it)", "lock");
    blendMode_->addItem("Camera projection (repaint)", "overlay");
    blendMode_->addItem("Blend by camera quality", "blend");
    blendMode_->addItem("Fill uncovered only", "fill");
    blendMode_->setCurrentIndex(blendMode_->findData("blend"));
    blendMode_->setToolTip(
        "Keep existing paint: paint an earlier view saw well is locked, and this view only paints around it "
        "(uncovered or grazing areas). Camera projection paints over existing texture with soft camera falloff. "
        "Quality blending can keep stronger old projections. Fill preserves all existing paint.");
    blendForm->addRow("Mode", blendMode_);
    projectionStrength_ = new ScrollSpinBox;
    projectionStrength_->setRange(0, 200);
    projectionStrength_->setSingleStep(5);
    projectionStrength_->setValue(100);
    projectionStrength_->setSuffix(" %");
    projectionStrength_->setToolTip("Below 100% reduces overlap opacity. Above 100% increases the "
                                    "influence of oblique parts of this projection.");
    blendForm->addRow("Strength", projectionStrength_);
    connect(projectionStrength_, &QSpinBox::valueChanged, this, &AiPanel::refresh);
    projectionFeather_ = new ScrollSpinBox;
    projectionFeather_->setRange(0, 25);
    projectionFeather_->setValue(6);
    projectionFeather_->setSuffix(" %");
    projectionFeather_->setToolTip("Softens camera, silhouette, occlusion and painted mask edges. "
                                   "Also controls the overlap band around newly covered areas.");
    blendForm->addRow("Feather width", projectionFeather_);
    angleFalloff_ = new ScrollSpinBox;
    angleFalloff_->setRange(1, 8);
    angleFalloff_->setValue(2);
    angleFalloff_->setToolTip("Higher values favor surfaces facing the camera more strongly.");
    blendForm->addRow("Angle falloff", angleFalloff_);
    projectionAngle_ = new ScrollSpinBox;
    projectionAngle_->setObjectName("aiProjectionAngle");
    projectionAngle_->setRange(35, 80);
    projectionAngle_->setValue(60);
    projectionAngle_->setSuffix(" degrees");
    projectionAngle_->setToolTip("Leave grazing surfaces unpainted instead of stretching a few image pixels "
                                 "over them. Add another camera view to cover those areas sharply.");
    blendForm->addRow("Maximum projection angle", projectionAngle_);
    fillGaps_ = new QCheckBox("Fill enclosed visible gaps");
    fillGaps_->setObjectName("aiFillGaps");
    fillGaps_->setChecked(true);
    fillGaps_->setToolTip("Use the current image to fill holes surrounded by existing paint, up to "
                          "80 degrees. Keeps the angle limit on fresh outer edges and existing paint.");
    blendForm->addRow(fillGaps_);
    coverAll_ = new QCheckBox("Paint every visible face");
    coverAll_->setObjectName("aiCoverAll");
    coverAll_->setChecked(true);
    coverAll_->setToolTip("Untextured surfaces beyond the maximum angle still receive this view's paint, at low "
                          "confidence, so a later camera-facing view replaces them. Existing paint keeps the limit.");
    blendForm->addRow(coverAll_);
    colorMatch_ = new QCheckBox("Match colors in overlap");
    colorMatch_->setChecked(false);
    colorMatch_->setToolTip("Use corresponding overlap pixels for a small, bounded color correction. "
                            "Disable when intentionally changing the surface color.");
    blendForm->addRow(colorMatch_);
    inputLayout->addWidget(blending);
    project_ = button(inputLayout, "Project camera view", [this] { projectCandidate(false); });
    project_->setObjectName("aiProject");
    inputLayout->removeWidget(project_);
    connect(generationMode_, &QTabBar::currentChanged, this, [this](int mode) {
        generate_->setText(mode == 0 ? "Generate image" : "Generate");
        candidateFrame_->setVisible(mode == 0);
        multiviewBox_->setVisible(mode == 1);
        if (mode == 1 && multiviewCameras_.empty())
            resetMultiviewCameras();
        refresh();
    });
    connect(blendMode_, &QComboBox::currentIndexChanged, this, [this] {
        project_->setText(blendMode_->currentData() == "fill"    ? "Fill uncovered texture"
                          : blendMode_->currentData() == "blend" ? "Blend by camera quality"
                                                                 : "Project camera view");
    });
    auto history = new QHBoxLayout;
    inputLayout->addLayout(history);
    undoButton_ = new QPushButton("Undo");
    redoButton_ = new QPushButton("Redo");
    history->addWidget(undoButton_);
    history->addWidget(redoButton_);
    connect(undoButton_, &QPushButton::clicked, this, [this] {
        if (undo_.empty() || busy_)
            return;
        redo_ << historyState();
        const auto state = undo_.takeLast();
        restoreHistory(state);
    });
    connect(redoButton_, &QPushButton::clicked, this, [this] {
        if (redo_.empty() || busy_)
            return;
        undo_ << historyState();
        const auto state = redo_.takeLast();
        restoreHistory(state);
    });
    button(inputLayout, "Save texturing project…", [this] { saveProject(); });
    button(inputLayout, "Open texturing project…", [this] { openProject(); });
    inputLayout->addWidget(advanced);
    left->addWidget(setup);
    left->addStretch();
    previews_ = new QTabWidget;
    split->addWidget(previews_);
    split->setStretchFactor(1, 1);
    auto preview3d = new QWidget;
    auto viewLayout = new QVBoxLayout(preview3d);
    auto displayBar = new QHBoxLayout;
    viewLayout->addLayout(displayBar);
    display_ = new ScrollComboBox;
    display_->addItems({"Textured", "Unlit base color", "Coverage"});
    display_->setCurrentIndex(1);
    displayBar->addWidget(display_);
    auto frame = new QPushButton("Frame mesh");
    displayBar->addWidget(frame);
    connect(frame, &QPushButton::clicked, this, [this] {
        if (scene_) {
            view_->setCamera(aiFrameCamera(*scene_, view_->camera()));
        }
    });
    auto capturedCamera = new QPushButton("Generated camera");
    capturedCamera->setToolTip("Restore the frozen camera used for the current generated image.");
    displayBar->addWidget(capturedCamera);
    connect(capturedCamera, &QPushButton::clicked, this, [this] {
        if (capture_)
            view_->setCamera(capture_->camera.orbit);
    });
    view_ = new TransferViewport;
    view_->setObjectName("aiViewport");
    view_->setFieldOfView(20);
    {
        auto camera = view_->camera();
        camera.orthographic = true;
        view_->setCamera(camera);
    }
    cameraFov_ = new ScrollSpinBox;
    cameraFov_->setObjectName("aiCameraFov");
    cameraFov_->setRange(5, 90);
    cameraFov_->setValue(20);
    cameraFov_->setSuffix(" degrees");
    cameraFov_->setMaximumWidth(110);
    cameraFov_->setToolTip("Narrow lenses reduce perspective differences between views. "
                           "Changing the lens preserves framing by moving the camera.");
    auto lensBar = new QHBoxLayout;
    viewLayout->addLayout(lensBar);
    orthographic_ = new QCheckBox("Orthographic");
    orthographic_->setObjectName("aiOrthographic");
    orthographic_->setChecked(true);
    orthographic_->setToolTip("Project without perspective, as a parallel view. Generation frames the visible "
                              "mesh bounds to use the full image resolution.");
    lensBar->addWidget(orthographic_);
    auto fovLabel = new QLabel("FOV");
    fovLabel->setObjectName("aiCameraFovLabel");
    lensBar->addWidget(fovLabel);
    lensBar->addWidget(cameraFov_);
    fovLabel->setEnabled(false);
    cameraFov_->setEnabled(false);
    connect(orthographic_, &QCheckBox::toggled, this, [this](bool enabled) {
        auto camera = view_->camera();
        camera.orthographic = enabled;
        view_->setCamera(camera);
    });
    lensBar->addStretch();
    connect(cameraFov_, &QSpinBox::valueChanged, view_, &TransferViewport::setFieldOfView);
    connect(view_, &TransferViewport::cameraChanged, this, [this](const OrbitCamera &camera) {
        const QSignalBlocker blocker(cameraFov_), orthoBlocker(orthographic_);
        cameraFov_->setValue(qRound(camera.fieldOfView));
        orthographic_->setChecked(camera.orthographic);
        cameraFov_->setEnabled(!camera.orthographic);
        if (auto label = findChild<QLabel *>("aiCameraFovLabel"))
            label->setEnabled(!camera.orthographic);
    });
    showUncovered_ = new QCheckBox("Untextured areas");
    showUncovered_->setObjectName("aiShowUncovered");
    showUncovered_->setToolTip("Red hatching marks surfaces without an accepted projection. Preview only.");
    showUncovered_->setChecked(true);
    view_->setUncoveredOverlay(true);
    lensBar->addWidget(showUncovered_);
    connect(showUncovered_, &QCheckBox::toggled, view_, &TransferViewport::setUncoveredOverlay);
    viewLayout->addWidget(new SquareFrame(view_), 1);
    previews_->addTab(preview3d, "3D preview");
    canvas_ = new AiMaskCanvas;
    canvas_->setObjectName("aiFullCandidate");
    sideCanvas_->maskChanged = [this] { canvas_->set(sideCanvas_->image, sideCanvas_->mask); };
    canvas_->maskChanged = [this] { sideCanvas_->set(canvas_->image, canvas_->mask); };
    connect(geometryOutline, &QCheckBox::toggled, this, [this](bool on) {
        canvas_->outline = sideCanvas_->outline = on;
        canvas_->update();
        sideCanvas_->update();
    });
    previews_->addTab(canvas_, "Generated view / mask");
    auto atlasTab = new QWidget;
    auto atlasLayout = new QVBoxLayout(atlasTab);
    atlasChoice_ = new ScrollComboBox;
    atlasLayout->addWidget(atlasChoice_);
    auto exportForm = new QFormLayout;
    exportName_ = new QLineEdit;
    exportName_->setValidator(
        new QRegularExpressionValidator(QRegularExpression("[A-Za-z0-9_-]+"), exportName_));
    exportForm->addRow("Export name (without _ALB.png)", exportName_);
    atlasLayout->addLayout(exportForm);
    connect(exportName_, &QLineEdit::textEdited, this,
            [this](const QString &name) { exportNames_[atlasChoice_->currentText()] = name; });
    atlasImage_ = new QLabel;
    atlasImage_->setAlignment(Qt::AlignCenter);
    atlasImage_->setMinimumSize(240, 240);
    atlasLayout->addWidget(atlasImage_, 1);
    previews_->addTab(atlasTab, "UV texture");
    guideImage_ = new QLabel;
    guideImage_->setAlignment(Qt::AlignCenter);
    previews_->addTab(guideImage_, "Normal guide");
    connect(previews_, &QTabWidget::currentChanged, this, [this](int index) {
        if (index == 2)
            updatePreview();
    });
    auto layersPanel = new QGroupBox("Projection layers");
    layersPanel->setObjectName("aiLayersPanel");
    layersPanel->setMinimumWidth(210);
    layersPanel->setMaximumWidth(330);
    auto layerLayout = new QVBoxLayout(layersPanel);
    layersEmpty_ = new QLabel("Accepted projections appear here.\nToggle visibility to inspect their joins.");
    layersEmpty_->setWordWrap(true);
    layersEmpty_->setStyleSheet("color:#8ea3b6;padding:8px");
    layerLayout->addWidget(layersEmpty_);
    layers_ = new QListWidget;
    layers_->setObjectName("aiProjectionLayers");
    layers_->setItemDelegate(new ProjectionLayerDelegate(layers_));
    layers_->setIconSize(QSize(64, 64));
    layers_->setSpacing(2);
    layers_->setMinimumHeight(84);
    layers_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layerLayout->addWidget(layers_, 1);
    layerTools_ = new QWidget;
    auto layerToolsLayout = new QVBoxLayout(layerTools_);
    layerToolsLayout->setContentsMargins(0, 0, 0, 0);
    auto layerForm = new QFormLayout;
    layerMode_ = new ScrollComboBox;
    layerMode_->setObjectName("aiLayerMode");
    layerMode_->addItem("Blend", "blend");
    layerMode_->addItem("Overlay", "overlay");
    layerStrength_ = new ScrollSpinBox;
    layerStrength_->setObjectName("aiLayerStrength");
    layerStrength_->setRange(0, 200);
    layerStrength_->setSuffix(" %");
    layerDepthMargin_ = new ScrollSpinBox;
    layerDepthMargin_->setObjectName("aiLayerDepthMargin");
    layerDepthMargin_->setRange(0, 20);
    layerForm->addRow("Mode", layerMode_);
    layerForm->addRow("Strength", layerStrength_);
    layerForm->addRow("Depth margin", layerDepthMargin_);
    layerToolsLayout->addLayout(layerForm);
    auto applySettings = button(layerToolsLayout, "Apply layer settings", [this] {
        const int index = selectedLayer();
        if (index < 0 || !parity_)
            return;
        auto records = projections_;
        auto blend = records[index]["blending"].toObject();
        const bool rebake = blend["mode"].toString() != layerMode_->currentData().toString() ||
                            blend["depthMargin"].toInt(2) != layerDepthMargin_->value();
        blend["mode"] = layerMode_->currentData().toString();
        blend["strength"] = layerStrength_->value() / 100.;
        blend["depthMargin"] = layerDepthMargin_->value();
        records[index]["blending"] = blend;
        applyLayerEdit(records, rebake ? index : -1, index, "Layer settings updated.", isolatedLayer_);
    });
    applySettings->setObjectName("aiApplyLayerSettings");
    auto moveButtons = new QHBoxLayout;
    auto moveUp = new QPushButton("Move up");
    auto moveDown = new QPushButton("Move down");
    moveUp->setObjectName("aiLayerMoveUp");
    moveDown->setObjectName("aiLayerMoveDown");
    moveButtons->addWidget(moveUp);
    moveButtons->addWidget(moveDown);
    layerToolsLayout->addLayout(moveButtons);
    connect(moveUp, &QPushButton::clicked, this, [this] { moveLayer(selectedLayer(), 1); });
    connect(moveDown, &QPushButton::clicked, this, [this] { moveLayer(selectedLayer(), -1); });
    layerIsolate_ = new QCheckBox("Isolate selected layer");
    layerIsolate_->setObjectName("aiLayerIsolate");
    layerToolsLayout->addWidget(layerIsolate_);
    connect(layerIsolate_, &QCheckBox::toggled, this, [this](bool on) {
        const int index = selectedLayer();
        if (index >= 0)
            applyLayerEdit(projections_, -1, index, on ? "Layer isolated." : "Layer isolation cleared.",
                           on ? projections_[index]["layerId"].toString() : QString());
    });
    auto maskButtons = new QHBoxLayout;
    auto cameraMask = new QPushButton("Camera mask");
    auto uvMask = new QPushButton("UV mask");
    maskButtons->addWidget(cameraMask);
    maskButtons->addWidget(uvMask);
    layerToolsLayout->addLayout(maskButtons);
    connect(cameraMask, &QPushButton::clicked, this, [this] { editLayerMask(selectedLayer(), false); });
    connect(uvMask, &QPushButton::clicked, this, [this] { editLayerMask(selectedLayer(), true); });
    button(layerToolsLayout, "Replace image...", [this] { replaceLayerImage(selectedLayer()); });
    button(layerToolsLayout, "Go to captured camera", [this] {
        const int index = selectedLayer();
        if (index >= 0)
            view_->setCamera(aiCameraFromJson(projections_[index]["camera"].toObject()).orbit);
    });
    auto layerToolScroll = new QScrollArea;
    layerToolScroll->setObjectName("aiLayerControlsScroll");
    layerToolScroll->setWidgetResizable(true);
    layerToolScroll->setFrameShape(QFrame::NoFrame);
    layerToolScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layerToolScroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    layerToolScroll->setMinimumHeight(180);
    layerToolScroll->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    layerToolScroll->setWidget(layerTools_);
    layerLayout->addWidget(layerToolScroll);
    deleteLayer_ = button(layerLayout, "Delete layer", [this] { deleteLayer(selectedLayer()); });
    deleteLayer_->setObjectName("aiDeleteLayer");
    rebuildLayers_ = button(layerLayout, "Rebuild projection layers", [this] { rebuildProjectionLayers(); });
    rebuildLayers_->setObjectName("aiRebuildLayers");
    rebuildLayers_->setToolTip("Rebuild every saved view using the corrected projection pipeline. No AI generation. Undo restores the previous stack.");
    connect(layers_, &QListWidget::currentRowChanged, this, [this] { refreshLayerTools(); });
    split->addWidget(layersPanel);
    split->setSizes({350, 700, 240});
    connect(layers_, &QListWidget::itemChanged, this, [this](QListWidgetItem *item) {
        setLayerVisible(item->data(Qt::UserRole).toInt(), item->checkState() == Qt::Checked);
    });
    layers_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(layers_, &QListWidget::customContextMenuRequested, this, [this](const QPoint &point) {
        auto item = layers_->itemAt(point);
        if (!item || busy_)
            return;
        const int index = item->data(Qt::UserRole).toInt();
        QMenu menu;
        auto look = menu.addAction("Go to this layer's camera");
        auto regenerate = menu.addAction("Regenerate with a new seed");
        regenerate->setEnabled(modelReady() && !externalBusy_);
        auto remove = menu.addAction("Delete layer");
        remove->setEnabled(!externalBusy_);
        auto chosen = menu.exec(layers_->viewport()->mapToGlobal(point));
        if (chosen == look)
            view_->setCamera(aiCameraFromJson(projections_[index]["camera"].toObject()).orbit);
        else if (chosen == regenerate)
            regenerateLayer(index);
        else if (chosen == remove)
            deleteLayer(index);
    });
    auto bar = new QHBoxLayout;
    layout->addLayout(bar);
    status_ = new QLabel("Load a mesh and describe its appearance.");
    status_->setWordWrap(true);
    bar->addWidget(status_, 1);
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setMaximumWidth(180);
    bar->addWidget(progress_);
    cancelButton_ = new QPushButton("Cancel");
    bar->addWidget(cancelButton_);
    connect(cancelButton_, &QPushButton::clicked, this, &AiPanel::cancelJob);
    // Keep acceptance reachable while the generation panel is scrolled or the full-size image is open.
    bar->addWidget(project_);
    export_ = new QPushButton("Export base colors…");
    bar->addWidget(export_);
    connect(export_, &QPushButton::clicked, this, &AiPanel::exportTextures);
    connect(uv_, &QComboBox::currentIndexChanged, this, [this] {
        if (!loading_)
            resetAtlases();
    });
    connect(resolution_, &QComboBox::currentIndexChanged, this, [this] {
        if (!loading_)
            resetAtlases();
    });
    connect(prompt_, &QPlainTextEdit::textChanged, this, &AiPanel::refresh);
    connect(materials_, &QListWidget::itemChanged, this, &AiPanel::refresh);
    canvas_->brush = sideCanvas_->brush = brush_->value();
    connect(brush_, &QSpinBox::valueChanged, this,
            [this](int n) { canvas_->brush = sideCanvas_->brush = n; });
    connect(profile_, &QComboBox::currentIndexChanged, this, [this] { engine_.stop(); });
    connect(display_, &QComboBox::currentIndexChanged, this, &AiPanel::updatePreview);
    connect(atlasChoice_, &QComboBox::currentIndexChanged, this, &AiPanel::updatePreview);
    connect(&engine_, &AiWorker::event, this, &AiPanel::workerEvent);
    connect(&cloud_, &AiCloudImage::event, this, &AiPanel::workerEvent);
    connect(model_, &QComboBox::currentIndexChanged, this, [this] {
        QSettings().setValue("ai/model", model_->currentData().toString());
        if (cloudModel())
            engine_.stop(); // Return VRAM; the cloud model does not use the local engine.
        refresh();
    });
    connect(&engine_, &AiWorker::failed, this, [this](const QString &e) {
        if (busy_)
            fail(e);
    });
    connect(&installer_, &AiInstaller::progress, this, [this](qint64 n, qint64 total, const QString &s) {
        progress_->setValue(total ? int(double(n) / total * 1000) : 0);
        status_->setText(s);
    });
    connect(&installer_, &AiInstaller::checkingEngine, this, [this](bool checking) {
        setupLease_ = checking;
        emit runningChanged(checking);
        refresh();
    });
    connect(&installer_, &AiInstaller::finished, this, [this](bool success, const QString &message) {
        if (setupLease_) {
            setupLease_ = false;
            emit runningChanged(false);
        }
        if (!migrationSource_.isEmpty()) {
            if (success)
                QSettings().setValue("ai/location", installer_.root());
            else
                installer_.setRoot(migrationSource_);
            migrationSource_.clear();
            location_->setText(installer_.root());
        }
        status_->setText(message);
        refresh();
    });
    try {
        manifest_ = AiManifest::load(aiAssetsPath("manifest.json"));
        installer_.setManifest(manifest_);
    } catch (const std::exception &e) {
        setupInfo_->setText(QString::fromUtf8(e.what()));
    }
    // Hardware probing runs once and does not affect the two existing modules.
    hardware_ = aiHardware();
    profile_->setCurrentIndex(hardware_.profile() == "memory" ? 1 : 0);
    refresh();
    if (!projectionError_.isEmpty())
        status_->setText("Projection renderer unavailable: " + projectionError_ +
                         ". Saved paint can still be inspected and exported.");
}
AiPanel::~AiPanel() {
    shutdown();
}
void AiPanel::shutdown() {
    cancel_ = true;
    installer_.cancel();
    engine_.stop();
    cloud_.cancel();
    if (native_.joinable())
        native_.join();
}
void AiPanel::setExternalBusy(bool b) {
    externalBusy_ = b;
    if (b && !busy_)
        engine_.stop(); // Return VRAM to AO / Texture Transfer before they launch.
    installer_.setGpuAvailable(!b);
    refresh();
}
bool AiPanel::cloudModel() const {
    return model_->currentData() == "gpt";
}
bool AiPanel::modelReady() const {
    return cloudModel() ? cloud_.backend() != AiCloudImage::Backend::None : installer_.installed();
}
void AiPanel::refresh() {
    bool idle = !busy_ && !installer_.busy() && !setupLease_;
    inputs_->setEnabled(idle);
    layers_->setEnabled(idle && !externalBusy_);
    canvas_->editable = idle;
    sideCanvas_->editable = idle;
    install_->setEnabled(idle && !manifest_.id.isEmpty() && hardware_.compatible && !externalBusy_);
    install_->setText(installer_.installed() ? "Check / Repair AI installation"
                                             : "Download && Enable AI Texturing");
    references_->setVisible(true);
    if (!manifest_.id.isEmpty())
        setupInfo_->setText(QString("%1 · %2 GB VRAM\n%3\nDownload: %4 GB · additional free space: %5 GB")
                                .arg(hardware_.name.isEmpty() ? "No compatible GPU found" : hardware_.name)
                                .arg(hardware_.vramMiB / 1024., 0, 'f', 0)
                                .arg(installer_.installed()      ? "Installed · offline generation available"
                                     : hardware_.error.isEmpty() ? "Optional engine · no account required"
                                                                 : hardware_.error)
                                .arg(installer_.missingBytes() / 1e9, 0, 'f', 1)
                                .arg(installer_.requiredBytes() / 1e9, 0, 'f', 1));
    localEngine_->setVisible(!cloudModel());
    cloudInfo_->setText(cloudModel() ? cloud_.backendDescription()
                                     : "Runs locally on your GPU after a one-time download (about 25 GB). "
                                       "Licensed for non-commercial use (CC BY-NC).");
    bool ready = idle && !externalBusy_ && scene_ && meshErrors_.empty() && modelReady() &&
                 (!prompt_->toPlainText().trimmed().isEmpty() || references_->count()) &&
                 !selectedMaterials().empty();
    generate_->setEnabled(ready && (generationMode_->currentIndex() == 0 ||
                                    (projectionStrength_->value() > 0 && !multiviewCameras_.empty())));
    project_->setEnabled(idle && !externalBusy_ && scene_ && meshErrors_.empty() && capture_ &&
                         !candidate_.isNull() && !selectedMaterials().empty() &&
                         projectionStrength_->value() > 0);
    undoButton_->setEnabled(idle && !undo_.empty());
    redoButton_->setEnabled(idle && !redo_.empty());
    export_->setEnabled(idle && !projections_.empty() && !selectedMaterials().empty());
    cancelButton_->setEnabled(busy_ || installer_.busy());
    refreshLayerTools();
}
QSet<QString> AiPanel::selectedMaterials() const {
    QSet<QString> set;
    for (int i = 0; i < materials_->count(); ++i)
        if (materials_->item(i)->checkState() == Qt::Checked)
            set.insert(materials_->item(i)->text());
    return set;
}
QStringList AiPanel::referencePaths() const {
    QStringList paths;
    for (int i = 0; i < references_->count(); ++i)
        paths << references_->item(i)->text();
    return paths;
}
void AiPanel::invalidate() {
    capture_.reset();
    candidate_ = QImage();
    candidateRetention_ = {};
    canvas_->set({}, {});
    sideCanvas_->set({}, {});
    canvas_->silhouette = sideCanvas_->silhouette = {};
    anchor_.clear();
    ++revision_;
}
void AiPanel::resetAtlases() {
    if (!scene_)
        return;
    try {
        parity_ = true;
        isolatedLayer_.clear();
        blendMode_->setCurrentIndex(blendMode_->findData("blend"));
        layerFrames_.clear();
        layerDecoded_.clear();
        meshErrors_ = validateAiMesh(*scene_, uv_->currentIndex());
        if (!meshErrors_.empty()) {
            atlases_.clear();
            undo_.clear();
            redo_.clear();
            projections_.clear();
            layerBase_.clear();
            refreshLayers();
            invalidate();
            meshInfo_->setText(meshErrors_.join('\n'));
            updatePreview();
            refresh();
            return;
        }
        atlases_ = aiEmptyAtlases(*scene_, resolution_->currentData().toInt());
        layerBase_ = atlases_;
        meshInfo_->setText(QString("%1\n%2 triangles · %3 materials")
                               .arg(QFileInfo(meshPath_).fileName())
                               .arg(scene_->tris.size())
                               .arg(atlases_.size()));
        undo_.clear();
        redo_.clear();
        projections_.clear();
        refreshLayers();
        invalidate();
        exportNames_.clear();
        QSet<QString> used;
        for (const auto &material : atlases_.keys()) {
            QString name = material;
            name.replace(QRegularExpression("[^A-Za-z0-9_-]"), "_");
            if (name.isEmpty())
                name = "material";
            QString base = name;
            int i = 2;
            while (used.contains(name.toLower()))
                name = base + "_" + QString::number(i++);
            used.insert(name.toLower());
            exportNames_[material] = name;
        }
        atlasChoice_->clear();
        atlasChoice_->addItems(atlases_.keys());
        view_->setMesh(scene_, uv_->currentIndex());
        updatePreview();
        refresh();
    } catch (const std::exception &e) {
        status_->setText(QString::fromUtf8(e.what()));
    }
}
bool AiPanel::loadMesh(const QString &path) {
    if (busy_)
        return false;
    try {
        auto s = std::make_shared<Scene>(loadScene(path));
        if (s->tris.empty())
            throw std::runtime_error("Mesh contains no triangles");
        scene_ = s;
        meshPath_ = QFileInfo(path).absoluteFilePath();
        loading_ = true;
        uv_->clear();
        uv_->addItems(s->uvNames);
        materials_->clear();
        QSet<QString> names;
        for (const auto &t : s->tris)
            names.insert(aiMaterial(*s, t));
        QStringList sorted = names.values();
        sorted.sort();
        for (const QString &name : sorted) {
            auto item = new QListWidgetItem(name, materials_);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
            item->setCheckState(Qt::Checked);
        }
        loading_ = false;
        meshInfo_->setText(QString("%1\n%2 triangles · %3 materials")
                               .arg(QFileInfo(path).fileName())
                               .arg(s->tris.size())
                               .arg(names.size()));
        QSettings().setValue("ai/meshDir", QFileInfo(path).absolutePath());
        view_->setMesh(scene_, uv_->currentIndex());
        resetAtlases();
        OrbitCamera framed;
        framed.fieldOfView = view_->camera().fieldOfView;
        framed.orthographic = view_->camera().orthographic;
        view_->setCamera(aiFrameCamera(*s, framed));
        resetMultiviewCameras();
        return true;
    } catch (const std::exception &e) {
        status_->setText(QString::fromUtf8(e.what()));
        return false;
    }
}
void AiPanel::updatePreview() {
    if (!scene_)
        return;
    QMap<QString, PreviewMaterial> maps;
    for (auto it = atlases_.begin(); it != atlases_.end(); ++it) {
        PreviewMaterial m;
        m.base = it->color;
        m.coverage = it->coverage;
        m.maps["ALB"] = it->color;
        m.maps["COVERAGE"] = it->coverage;
        maps[it.key()] = m;
    }
    view_->setMaterials(maps, false);
    view_->setDisplay(display_->currentIndex() == 0   ? QString()
                      : display_->currentIndex() == 1 ? "ALB"
                                                      : "COVERAGE");
    auto a = atlases_.value(atlasChoice_->currentText());
    QImage im = display_->currentIndex() == 2 ? a.coverage : a.color;
    exportName_->setText(exportNames_.value(atlasChoice_->currentText()));
    if (!im.isNull() && previews_->currentIndex() == 2)
        atlasImage_->setPixmap(
            QPixmap::fromImage(im.scaled(700, 700, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}
void AiPanel::refreshLayers() {
    const QSignalBlocker blocker(layers_);
    const int selected = selectedLayer();
    QSet<QString> identities;
    for (auto &record : projections_) {
        if (record["layerId"].toString().isEmpty() || identities.contains(record["layerId"].toString()))
            record["layerId"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        identities.insert(record["layerId"].toString());
    }
    int frontLayer = -1; // The first visible layer is the front for view-aware generation.
    for (int j = 0; j < projections_.size() && frontLayer < 0; ++j)
        if (projections_[j]["visible"].toBool(true))
            frontLayer = j;
    auto title = [frontLayer](int index, const QJsonObject &record) {
        const auto camera = record["camera"].toObject();
        return QString("Projection %1\n%2° yaw · %3° pitch")
                   .arg(index + 1)
                   .arg(camera["yaw"].toDouble(), 0, 'f', 0)
                   .arg(camera["pitch"].toDouble(), 0, 'f', 0) +
               (index == frontLayer ? "  [front]" : "");
    };
    bool sameItems = layers_->count() == projections_.size();
    for (int row = 0; sameItems && row < layers_->count(); ++row) {
        const auto &record = projections_[projections_.size() - 1 - row];
        sameItems = layers_->item(row)->data(Qt::UserRole + 1).toString() == record["layerId"].toString() &&
                    layers_->item(row)->data(Qt::UserRole + 2).toString() == record["candidate"].toString();
    }
    if (sameItems) {
        for (int row = 0; row < layers_->count(); ++row) {
            auto item = layers_->item(row);
            const int index = projections_.size() - 1 - row;
            const bool visible = projections_[index]["visible"].toBool(true);
            item->setCheckState(visible ? Qt::Checked : Qt::Unchecked);
            item->setForeground(QColor(visible ? "#d9e7ef" : "#7e909e"));
            item->setText(title(index, projections_[index]));
        }
        layersEmpty_->setVisible(projections_.empty());
        refreshLayerTools();
        return;
    }
    layers_->clear();
    layersEmpty_->setVisible(projections_.empty());
    for (int i = projections_.size() - 1; i >= 0; --i) {
        auto &record = projections_[i];
        QImage thumbnail =
            QImage::fromData(QByteArray::fromBase64(record["candidate"].toString().toLatin1()), "PNG");
        if (!thumbnail.isNull())
            thumbnail = thumbnail.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        auto item = new QListWidgetItem(QIcon(QPixmap::fromImage(thumbnail)),
                                        title(i, record),
                                        layers_);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        item->setData(Qt::UserRole, i);
        item->setData(Qt::UserRole + 1, record["layerId"].toString());
        item->setData(Qt::UserRole + 2, record["candidate"].toString());
        item->setCheckState(record["visible"].toBool(true) ? Qt::Checked : Qt::Unchecked);
        item->setSizeHint(QSize(200, 76));
        item->setToolTip("Toggle visibility in the composed texture. Newest projection is at the top. Right-click to "
                         "regenerate, delete, or look through its camera. The first visible layer is the front.\n" +
                         record["prompt"].toString());
        if (!record["visible"].toBool(true))
            item->setForeground(QColor("#7e909e"));
    }
    if (!projections_.empty())
        layers_->setCurrentRow(projections_.size() - 1 - std::clamp(selected, 0, int(projections_.size()) - 1));
    refreshLayerTools();
}
void AiPanel::setLayerVisible(int index, bool visible) {
    if (busy_ || externalBusy_ || index < 0 || index >= projections_.size())
        return;
    if (projections_[index]["visible"].toBool(true) == visible)
        return;
    auto requested = projections_;
    requested[index]["visible"] = visible;
    applyLayerEdit(requested, -1, index, "Layer visibility updated.", isolatedLayer_);
}
int AiPanel::selectedLayer() const {
    auto item = layers_->currentItem();
    return item ? item->data(Qt::UserRole).toInt() : -1;
}
AiPanel::HistoryState AiPanel::historyState() const {
    return {atlases_, projections_, layerBase_, parity_, isolatedLayer_, selectedLayer(), blendMode_->currentData().toString()};
}
void AiPanel::restoreHistory(const HistoryState &state) {
    atlases_ = state.atlases;
    projections_ = state.projections;
    layerBase_ = state.base;
    parity_ = state.parity;
    isolatedLayer_ = state.isolatedLayer;
    if (!state.mode.isEmpty())
        blendMode_->setCurrentIndex(std::max(0, blendMode_->findData(state.mode)));
    layerFrames_.clear();
    layerDecoded_.clear();
    ++revision_;
    refreshLayers();
    if (state.selected >= 0 && state.selected < projections_.size())
        layers_->setCurrentRow(projections_.size() - 1 - state.selected);
    updatePreview();
    refresh();
}
void AiPanel::refreshLayerTools() {
    const int index = selectedLayer();
    const bool idle = !busy_ && !externalBusy_ && !installer_.busy();
    layerTools_->setEnabled(idle && index >= 0);
    deleteLayer_->setEnabled(idle && index >= 0);
    rebuildLayers_->setEnabled(idle && scene_ && !projections_.empty());
    layerMode_->setEnabled(parity_);
    layerStrength_->setEnabled(parity_);
    layerDepthMargin_->setEnabled(parity_);
    if (auto apply = findChild<QPushButton *>("aiApplyLayerSettings"))
        apply->setEnabled(parity_);
    const QSignalBlocker a(layerMode_), b(layerStrength_), c(layerDepthMargin_), d(layerIsolate_);
    if (index >= 0) {
        const auto blend = projections_[index]["blending"].toObject();
        layerMode_->setCurrentIndex(blend["mode"].toString() == "overlay" ? 1 : 0);
        layerStrength_->setValue(qRound(blend["strength"].toDouble(1) * 100));
        layerDepthMargin_->setValue(blend["depthMargin"].toInt(2));
        layerIsolate_->setChecked(projections_[index]["layerId"].toString() == isolatedLayer_ && !isolatedLayer_.isEmpty());
    } else
        layerIsolate_->setChecked(false);
    for (auto widget : {static_cast<QWidget *>(projectionFeather_), static_cast<QWidget *>(angleFalloff_),
                        static_cast<QWidget *>(projectionAngle_), static_cast<QWidget *>(fillGaps_),
                        static_cast<QWidget *>(coverAll_), static_cast<QWidget *>(colorMatch_)})
        widget->setEnabled(!parity_);
    blendMode_->setToolTip(parity_ ? "New projection layers use Blend or Overlay. Legacy paint-protection modes require a legacy project."
                                  : "Legacy projection behavior. Rebuild projection layers to use the corrected pipeline.");
    auto model = qobject_cast<QStandardItemModel *>(blendMode_->model());
    if (model)
        for (int i = 0; i < blendMode_->count(); ++i)
            model->item(i)->setEnabled(!parity_ || blendMode_->itemData(i) == "blend" || blendMode_->itemData(i) == "overlay");
    blendMode_->setItemText(blendMode_->findData("blend"), parity_ ? "Blend" : "Blend by camera quality");
    blendMode_->setItemText(blendMode_->findData("overlay"), parity_ ? "Overlay" : "Camera projection (repaint)");
}
void AiPanel::applyLayerEdit(QList<QJsonObject> records, int rebakeIndex, int selected,
                             const QString &message, const QString &isolation, bool rebuild) {
    if (busy_ || externalBusy_ || installer_.busy() || setupLease_ || !scene_)
        return;
    if ((rebuild || (parity_ && rebakeIndex >= 0)) && !projectionError_.isEmpty()) {
        status_->setText("Projection renderer unavailable: " + projectionError_ +
                         ". Accepted projections remain unchanged.");
        return;
    }
    if (native_.joinable())
        native_.join();
    const auto previous = historyState();
    const auto scene = scene_;
    const auto base = layerBase_;
    const bool parity = parity_ || rebuild;
    const int uv = uv_->currentIndex(), resolution = resolution_->currentData().toInt();
    const bool contentChange = rebuild || rebakeIndex >= 0;
    if (contentChange) {
        layerFrames_.clear();
        layerDecoded_.clear();
    } else {
        layerFrames_.insert(compositionKey(base, projections_, parity_, isolatedLayer_),
                            new AiAtlases(atlases_), compositionCost(atlases_));
        if (const auto cached = layerFrames_.object(compositionKey(base, records, parity, isolation))) {
            undo_ << previous;
            if (undo_.size() > 10)
                undo_.removeFirst();
            redo_.clear();
            projections_ = records;
            atlases_ = *cached;
            isolatedLayer_ = isolation;
            ++revision_;
            refreshLayers();
            if (selected >= 0 && selected < records.size())
                layers_->setCurrentRow(records.size() - 1 - selected);
            updatePreview();
            refresh();
            status_->setText(message);
            return;
        }
    }
    cancel_ = false;
    setBusy(true);
    status_->setText(rebuild ? "Rebuilding every saved projection layer. No AI generation is needed..."
                            : "Updating projection layers...");
    native_ = std::thread([this, previous, scene, base, records, parity, uv, resolution,
                           rebakeIndex, selected, message, isolation, rebuild]() mutable {
        try {
            if (rebuild)
                records = aiRebuildProjectionLayers(*scene, uv, resolution, records, &cancel_);
            else if (parity && rebakeIndex >= 0)
                records[rebakeIndex] = aiBakeProjectionLayer(*scene, uv, resolution, records[rebakeIndex], &cancel_);
            else if (!parity && std::any_of(records.begin(), records.end(), [](const QJsonObject &r) {
                         return r["uvLayer"].toObject()["version"].toInt() != 1;
                     })) {
                // Legacy paint operations depend on the stack below them. Freeze
                // the original order before applying visibility, order or deletion.
                const auto frozen = aiPrepareLayers(*scene, uv, base, previous.projections, &cancel_);
                QMap<QString, QJsonObject> layersById;
                for (const auto &record : frozen)
                    layersById.insert(record["layerId"].toString(), record["uvLayer"].toObject());
                for (auto &record : records) {
                    if (record["uvLayer"].toObject()["version"].toInt() == 1)
                        continue;
                    const auto id = record["layerId"].toString();
                    if (id.isEmpty() || !layersById.contains(id))
                        throw std::runtime_error("Saved projection identity is missing");
                    record["uvLayer"] = layersById.value(id);
                }
            }
            auto effective = records;
            if (!isolation.isEmpty())
                for (auto &record : effective)
                    record["visible"] = record["layerId"].toString() == isolation;
            auto composed = parity ? aiComposeParityLayers(base, effective, &cancel_)
                                   : aiComposeLayers(base, effective, &cancel_, &layerDecoded_);
            QMetaObject::invokeMethod(
                this,
                [this, previous, records, composed, parity, isolation, selected, message, rebuild] {
                    if (native_.joinable())
                        native_.join();
                    if (cancel_) {
                        fail("Cancelled");
                        return;
                    }
                    undo_ << previous;
                    if (undo_.size() > 10)
                        undo_.removeFirst();
                    redo_.clear();
                    projections_ = records;
                    atlases_ = composed;
                    parity_ = parity;
                    isolatedLayer_ = isolation;
                    layerDecoded_.clear();
                    layerFrames_.insert(compositionKey(layerBase_, projections_, parity_, isolatedLayer_),
                                        new AiAtlases(atlases_), compositionCost(atlases_));
                    ++revision_;
                    if (rebuild)
                        blendMode_->setCurrentIndex(blendMode_->findData("blend"));
                    updatePreview();
                    refreshLayers();
                    if (selected >= 0 && selected < projections_.size())
                        layers_->setCurrentRow(projections_.size() - 1 - selected);
                    status_->setText(message);
                    setBusy(false);
                },
                Qt::QueuedConnection);
        } catch (const std::exception &e) {
            const QString message = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(
                this, [this, message] { fail(message); }, Qt::QueuedConnection);
        }
    });
}
void AiPanel::deleteLayer(int index) {
    if (busy_ || externalBusy_ || index < 0 || index >= projections_.size())
        return;
    auto records = projections_;
    const QString deleted = records[index]["layerId"].toString();
    records.removeAt(index);
    applyLayerEdit(records, -1, records.empty() ? -1 : std::min(index, int(records.size()) - 1),
                   "Layer deleted. Undo restores it.", isolatedLayer_ == deleted ? QString() : isolatedLayer_);
}
void AiPanel::moveLayer(int index, int offset) {
    if (busy_ || externalBusy_ || index < 0 || index >= projections_.size() ||
        index + offset < 0 || index + offset >= projections_.size())
        return;
    auto records = projections_;
    records.move(index, index + offset);
    applyLayerEdit(records, -1, index + offset, "Layer order updated.", isolatedLayer_);
}
void AiPanel::replaceLayerImage(int index) {
    if (busy_ || externalBusy_ || index < 0 || index >= projections_.size())
        return;
    if (!parity_) {
        status_->setText("Rebuild projection layers before replacing a layer image.");
        return;
    }
    const QString path = QFileDialog::getOpenFileName(this, "Replace layer image", {},
                                                     "Images (*.png *.jpg *.jpeg *.tif *.tiff *.tga)");
    if (path.isEmpty())
        return;
    QString error;
    QImage image = readTextureImage(path, &error);
    if (image.isNull() || image.width() != image.height()) {
        status_->setText(image.isNull() ? error : "Use a square image with the captured camera's framing.");
        return;
    }
    auto records = projections_;
    records[index]["sourceImage"] = imageData(image);
    records[index]["candidate"] = imageData(image.scaled(
        aiCameraFromJson(records[index]["camera"].toObject()).size,
        aiCameraFromJson(records[index]["camera"].toObject()).size,
        Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    applyLayerEdit(records, index, index, "Layer image replaced. Undo restores the previous image.", isolatedLayer_);
}
void AiPanel::editLayerMask(int index, bool uv) {
    if (busy_ || externalBusy_ || index < 0 || index >= projections_.size())
        return;
    if (!parity_) {
        status_->setText("Rebuild projection layers before editing a saved layer mask.");
        return;
    }
    const auto record = projections_[index];
    const QString key = uv ? "uvMask" : "userMask";
    QImage image = uv ? atlases_.value(atlasChoice_->currentText()).color
                     : QImage::fromData(QByteArray::fromBase64(
                           record["sourceImage"].toString(record["candidate"].toString()).toLatin1()), "PNG");
    if (image.isNull())
        return;
    QImage mask = QImage::fromData(QByteArray::fromBase64(record[key].toString().toLatin1()), "PNG");
    if (mask.isNull() && !uv)
        mask = QImage::fromData(QByteArray::fromBase64(record["mask"].toString().toLatin1()), "PNG");
    if (mask.isNull()) {
        mask = QImage(image.size(), QImage::Format_RGB32);
        mask.fill(Qt::white);
    }
    if (mask.size() != image.size())
        mask = mask.scaled(image.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QDialog dialog(this);
    dialog.setWindowTitle(uv ? "Edit layer UV mask" : "Edit layer camera mask");
    auto layout = new QVBoxLayout(&dialog);
    auto instruction = new QLabel("Left drag excludes paint; right drag restores it. White keeps paint.");
    instruction->setWordWrap(true);
    layout->addWidget(instruction);
    auto editor = new AiMaskCanvas;
    editor->brush = brush_->value();
    editor->set(image, mask);
    layout->addWidget(editor, 1);
    auto buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(650, 730);
    if (dialog.exec() != QDialog::Accepted)
        return;
    auto records = projections_;
    records[index][key] = imageData(editor->mask);
    if (!uv)
        records[index]["mask"] = records[index][key];
    applyLayerEdit(records, index, index, "Layer mask updated.", isolatedLayer_);
}
void AiPanel::rebuildProjectionLayers() {
    if (busy_ || externalBusy_ || !scene_ || projections_.empty())
        return;
    applyLayerEdit(projections_, -1, selectedLayer(),
                   "Projection layers rebuilt. Saved images were reused; Undo restores the previous stack.",
                   isolatedLayer_, true);
}
void AiPanel::setBusy(bool b) {
    busy_ = b;
    if (!b) {
        automatic_ = false;
    }
    emit runningChanged(b);
    refresh();
}
int AiPanel::generationSeed() {
    if (!randomSeed_->isChecked())
        return int((qint64(seed_->value()) + cameraIndex_) % 2147483648LL);
    int value;
    do {
        value = int(QRandomGenerator::global()->generate() & 0x7fffffff);
    } while (value == lastGenerationSeed_);
    lastGenerationSeed_ = value;
    return value;
}
void AiPanel::resetMultiviewCameras() {
    multiviewCameras_.clear();
    if (scene_) {
        // Keep the established front once layers exist; otherwise the mesh's own front (yaw 0).
        OrbitCamera front;
        front.yaw = frontYaw().value_or(0);
        front.pitch = 0;
        front.fieldOfView = view_->camera().fieldOfView;
        front.orthographic = view_->camera().orthographic;
        multiviewCameras_ = aiAutomaticCameras(*scene_, front);
    }
    refreshMultiviewViews();
}
void AiPanel::refreshMultiviewViews() {
    viewGrid_->clear();
    if (!scene_)
        return;
    for (int i = 0; i < multiviewCameras_.size(); ++i) {
        QImage thumbnail;
        try {
            const auto c = aiCapture(*scene_, uv_->currentIndex(), {}, aiCamera(*scene_, multiviewCameras_[i], 128));
            thumbnail = c.normal.copy();
            for (int y = 0; y < thumbnail.height(); ++y)
                for (int x = 0; x < thumbnail.width(); ++x)
                    if (qGray(c.silhouette.pixel(x, y)) < 128)
                        thumbnail.setPixel(x, y, qRgb(42, 46, 52));
        } catch (const std::exception &) {
        }
        auto item = new QListWidgetItem(QIcon(QPixmap::fromImage(thumbnail)), QString(), viewGrid_);
        item->setToolTip(QString("View %1 · %2° yaw · %3° pitch")
                             .arg(i + 1)
                             .arg(multiviewCameras_[i].yaw, 0, 'f', 0)
                             .arg(multiviewCameras_[i].pitch, 0, 'f', 0));
    }
}
std::optional<float> AiPanel::frontYaw(int excludeLayer) const {
    // The first visible accepted view defines the front for view-aware generation.
    for (int i = 0; i < projections_.size(); ++i)
        if (i != excludeLayer && (isolatedLayer_.isEmpty() ? projections_[i]["visible"].toBool(true)
                                                           : projections_[i]["layerId"].toString() == isolatedLayer_))
            return float(projections_[i]["camera"].toObject()["yaw"].toDouble());
    return std::nullopt;
}
void AiPanel::regenerateLayer(int index) {
    if (busy_ || externalBusy_ || !scene_ || !modelReady() || index < 0 || index >= projections_.size())
        return;
    if (parity_ && !projectionError_.isEmpty()) {
        status_->setText("Projection renderer unavailable: " + projectionError_ +
                         ". Accepted projections remain unchanged.");
        return;
    }
    if (prompt_->toPlainText().trimmed().isEmpty() && referencePaths().empty()) {
        status_->setText("Describe the appearance or add a reference image before regenerating.");
        return;
    }
    if (native_.joinable())
        native_.join();
    work_ = std::make_unique<QTemporaryDir>();
    if (!work_->isValid()) {
        fail("Cannot create AI work directory");
        return;
    }
    cancel_ = false;
    automatic_ = false;
    cameras_.clear();
    cameraIndex_ = 0;
    regenerateIndex_ = index;
    setBusy(true);
    status_->setText(QString("Preparing to regenerate projection %1 without its own paint…").arg(index + 1));
    auto scene = scene_;
    const auto base = layerBase_;
    const auto records = projections_;
    const int uv = uv_->currentIndex();
    const bool parity = parity_;
    const QString isolation = isolatedLayer_;
    native_ = std::thread([this, scene, base, records, uv, index, parity, isolation] {
        try {
            // The layer must not feed its own replacement: compose the stack without it.
            const bool legacy = !parity && std::any_of(records.begin(), records.end(), [](const QJsonObject &r) {
                return r["uvLayer"].toObject()["version"].toInt() != 1;
            });
            auto prepared = legacy ? aiPrepareLayers(*scene, uv, base, records, &cancel_) : records;
            if (!isolation.isEmpty())
                for (auto &record : prepared)
                    record["visible"] = record["layerId"].toString() == isolation;
            prepared[index]["visible"] = false;
            const auto without = aiComposeLayers(base, prepared, &cancel_, &layerDecoded_);
            const auto camera = aiCameraFromJson(records[index]["camera"].toObject());
            auto capture = std::make_shared<AiCapture>(aiCapture(*scene, uv, without, camera, &cancel_));
            QMetaObject::invokeMethod(
                this, [this, capture] { receiveCapture(capture, 1); }, Qt::QueuedConnection);
        } catch (const std::exception &e) {
            QString msg = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(
                this, [this, msg] { fail(msg); }, Qt::QueuedConnection);
        }
    });
}
void AiPanel::generate(bool automatic) {
    if (busy_ || externalBusy_ || !scene_ || !modelReady())
        return;
    auto errors = validateAiMesh(*scene_, uv_->currentIndex());
    if (!errors.empty()) {
        status_->setText(errors.join('\n'));
        return;
    }
    if (prompt_->toPlainText().trimmed().isEmpty() && referencePaths().empty())
        return;
    if (native_.joinable())
        native_.join();
    work_ = std::make_unique<QTemporaryDir>();
    if (!work_->isValid()) {
        fail("Cannot create AI work directory");
        return;
    }
    anchor_.clear();
    cancel_ = false;
    automatic_ = automatic;
    cameraIndex_ = 0;
    regenerateIndex_ = -1;
    // A manual orthographic view is reframed to the visible mesh bounds; the direction stays as orbited.
    if (automatic && multiviewCameras_.empty())
        resetMultiviewCameras();
    cameras_ = automatic ? multiviewCameras_ : QList<OrbitCamera>{aiFrameCamera(*scene_, view_->camera(), true)};
    setBusy(true);
    nextCamera();
}
void AiPanel::nextCamera() {
    if (cancel_) {
        fail("Cancelled. Accepted projections are preserved.");
        return;
    }
    if (cameraIndex_ >= cameras_.size()) {
        status_->setText("Automatic pass complete. Inspect coverage and add views for remaining holes.");
        setBusy(false);
        return;
    }
    if (native_.joinable())
        native_.join();
    auto scene = scene_;
    auto atlases = atlases_;
    int uv = uv_->currentIndex();
    AiCamera c = aiCamera(*scene, cameras_[cameraIndex_], generationResolution_->currentData().toInt());
    const bool automatic = automatic_, fill = blendMode_->currentData() == "fill";
    const auto options = projectionOptions();
    const auto materials = selectedMaterials();
    status_->setText(QString("Capturing view %1/%2").arg(cameraIndex_ + 1).arg(cameras_.size()));
    native_ = std::thread([this, scene, atlases, uv, c, automatic, fill, options, materials] {
        try {
            auto capture = std::make_shared<AiCapture>(aiCapture(*scene, uv, atlases, c, &cancel_));
            const double contribution =
                !automatic ? 1
                : fill     ? aiNewCoverage(*scene, uv, atlases, *capture, materials)
                           : aiViewContribution(*scene, uv, *capture, materials, options, &cancel_);
            QMetaObject::invokeMethod(
                this, [this, capture, contribution] { receiveCapture(capture, contribution); },
                Qt::QueuedConnection);
        } catch (const std::exception &e) {
            QString msg = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(
                this, [this, msg] { fail(msg); }, Qt::QueuedConnection);
        }
    });
}
void AiPanel::receiveCapture(std::shared_ptr<AiCapture> capture, double contribution) {
    if (native_.joinable())
        native_.join();
    if (cancel_) {
        fail("Cancelled");
        return;
    }
    if (automatic_ && contribution < .005) {
        ++cameraIndex_;
        nextCamera();
        return;
    }
    capture_ = capture;
    if (automatic_)
        view_->setCamera(capture_->camera.orbit);
    candidate_ = QImage();
    candidateRetention_ = {};
    canvas_->set({}, {});
    sideCanvas_->set({}, {});
    canvas_->silhouette = sideCanvas_->silhouette = capture_->silhouette;
    QString dir = work_->path() + "/view-" + QString::number(cameraIndex_);
    QDir().mkpath(dir);
    // View-aware generation: name this camera relative to the front (the first visible accepted view), so a
    // face in a reference is not painted onto the back. Without layers, this view becomes the front.
    const auto front = frontYaw(regenerateIndex_);
    const double turn =
        front ? std::abs(std::remainder(double(capture_->camera.orbit.yaw) - double(*front), 360.0)) : 0.0;
    if (cloudModel()) {
        // GPT Image 2 with ownership: this view sees the locked paint of earlier views and bare clay elsewhere,
        // and only paints the clay; the result is composited back over the locked pixels before projection.
        try {
            lock_ = std::make_shared<AiLock>(aiLockedGuide(*capture_));
            const QString guide = dir + "/guide.png";
            atomicImage(lock_->guide, guide);
            const QStringList references = referencePaths();
            QJsonArray images{guide};
            if (!references.empty())
                images.append(references.first()); // The prompt names one reference image (image 2).
            const QString prompt = aiGptPrompt(!lock_->any, !references.empty(), prompt_->toPlainText(),
                                               aiGptViewDescription(turn, capture_->camera.orbit.pitch, bool(front)));
            QJsonObject request{{"id", QString::number(++requestSerial_)},
                                {"model", "gpt-image-2"},
                                {"images", images},
                                {"prompt", prompt},
                                {"output", dir + "/candidate.png"},
                                {"workDir", dir},
                                {"references", QJsonArray::fromStringList(references)},
                                {"userPrompt", prompt_->toPlainText()},
                                {"viewTurn", turn},
                                {"viewPitch", capture_->camera.orbit.pitch},
                                {"size", capture_->camera.size}};
            guideImage_->setPixmap(QPixmap::fromImage(lock_->guide.scaled(700, 700, Qt::KeepAspectRatio)));
            candidateSettings_ = request;
            candidateSettings_["camera"] = aiCameraJson(capture_->camera);
            status_->setText("Generating with GPT Image 2…");
            cloud_.generate(request);
        } catch (const std::exception &e) {
            fail(QString::fromUtf8(e.what()));
        }
        return;
    }
    QJsonObject request{{"command", "generate"},
                        {"id", QString::number(++requestSerial_)},
                        {"output", dir + "/candidate.png"},
                        {"size", capture_->camera.size},
                        {"seed", generationSeed()},
                        {"prompt", prompt_->toPlainText()},
                        {"continuityStrength", continuity_->value() / 100.0},
                        {"continuityVersion", 2},
                        {"protectDetails", protectDetails_->isChecked()},
                        {"references", QJsonArray::fromStringList(referencePaths())},
                        {"viewTurn", turn},
                        {"viewPitch", capture_->camera.orbit.pitch}};
    QJsonObject feedbackSettings;
    try {
        const auto feedback = aiProjectionFeedback(*scene_, *capture_, continuity_->value() / 100.f,
                                                   projectionOptions(), &cancel_);
        request["conditioning"] = feedback.active ? "confidence" : "fresh";
        for (auto pair :
             {qMakePair(QString("normal"), capture_->normal), qMakePair(QString("neutral"), capture_->neutral),
              qMakePair(QString("silhouette"), capture_->silhouette)}) {
            QString path = dir + "/" + pair.first + ".png";
            atomicImage(pair.second, path);
            request[pair.first] = path;
        }
        if (feedback.active) {
            for (auto pair : {qMakePair(QString("feedback"), feedback.image),
                              qMakePair(QString("feedbackMask"), feedback.mask),
                              qMakePair(QString("accepted"), capture_->partial)}) {
                const QString path = dir + "/" + pair.first + ".png";
                atomicImage(pair.second, path);
                request[pair.first] = path;
            }
            feedbackSettings["feedbackGuideImage"] = imageData(feedback.image);
            feedbackSettings["feedbackMaskImage"] = imageData(feedback.mask);
            feedbackSettings["feedbackConfidenceImage"] = imageData(capture_->confidence);
        }
    } catch (const std::exception &e) {
        fail(QString::fromUtf8(e.what()));
        return;
    }
    guideImage_->setPixmap(QPixmap::fromImage(capture_->normal.scaled(700, 700, Qt::KeepAspectRatio)));
    pending_ = request;
    candidateSettings_ = request;
    for (auto it = feedbackSettings.begin(); it != feedbackSettings.end(); ++it)
        candidateSettings_[it.key()] = it.value();
    candidateSettings_["camera"] = aiCameraJson(capture_->camera);
    const QString enginePath = installer_.activePath();
    const QString profile = profile_->currentData().toString();
    if (enginePath_ != enginePath || engineProfile_ != profile)
        engine_.stop();
    enginePath_ = enginePath;
    engineProfile_ = profile;
    if (engine_.active()) {
        status_->setText("Generating with the loaded model…");
        engine_.send(pending_);
        pending_ = {};
    } else {
        // FORNAIO_AI_MODEL is a maintainer override for testing another model folder with the installed runtime.
        QString model = qEnvironmentVariable("FORNAIO_AI_MODEL");
        if (model.isEmpty())
            model = installer_.activePath() + "/model";
        engine_.start(installer_.activePath() + "/" + manifest_.json["python"].toString(), model,
                      profile_->currentData().toString());
    }
}
void AiPanel::workerEvent(const QJsonObject &event) {
    if (!busy_)
        return;
    QString type = event["event"].toString();
    if (!event["id"].toString().isEmpty() && event["id"] != candidateSettings_["id"])
        return;
    if (type == "ready" && !pending_.empty()) {
        engine_.send(pending_);
        pending_ = {};
    } else if (type == "progress") {
        status_->setText(event["message"].toString());
        progress_->setValue(int(event["fraction"].toDouble() * 1000));
    } else if (type == "error")
        fail(event["message"].toString());
    else if (type == "result") {
        if (cancel_) {
            fail("Cancelled");
            return;
        }
        candidate_ = readTextureImage(event["output"].toString());
        if (!candidate_.isNull()) {
            const QImage original = event["sourceImage"].toString().isEmpty()
                                        ? candidate_ : readTextureImage(event["sourceImage"].toString());
            if (original.isNull()) {
                fail("AI returned an invalid original source image");
                return;
            }
            candidateSettings_["sourceImage"] = imageData(original);
            if (parity_)
                candidate_ = original;
        }
        const bool cloud = candidateSettings_["model"] == "gpt-image-2";
        QImage cloudMask;
        if (cloud && capture_ && lock_ && !candidate_.isNull()) {
            if (candidate_.width() != candidate_.height()) {
                fail("GPT Image 2 returned a non-square image; regenerate this view.");
                return;
            }
            try {
                // GPT Image returns its own square size with the guide's framing, so a resize aligns it.
                candidate_ = candidate_.convertToFormat(parity_ ? QImage::Format_RGBA8888 : QImage::Format_RGB32)
                                 .scaled(capture_->camera.size, capture_->camera.size, Qt::IgnoreAspectRatio,
                                         Qt::SmoothTransformation);
                // Register the painted features onto the mesh before anything else uses the image.
                if (!parity_) {
                    double shift = 0;
                    candidate_ = aiAlignToGeometry(*capture_, candidate_, &shift);
                    candidateSettings_["alignmentShift"] = shift;
                    candidate_ = aiComposeLocked(*capture_, *lock_, aiMatchLockedTone(*capture_, *lock_, candidate_));
                    cloudMask = aiGeneratedPaintMask(*capture_, *lock_, candidate_);
                }
            } catch (const std::exception &e) {
                fail(QString::fromUtf8(e.what()));
                return;
            }
        }
        if (!capture_ || candidate_.size() != QSize(capture_->camera.size, capture_->camera.size)) {
            fail("AI returned an invalid candidate image");
            return;
        }
        candidateRetention_ = {};
        if (!event["detailReference"].toString().isEmpty()) {
            const auto detail = readTextureImage(event["detailReference"].toString());
            if (!detail.isNull())
                candidateSettings_["detailReferenceImage"] = imageData(detail);
        }
        if (!parity_ && !event["preservationMask"].toString().isEmpty()) {
            candidateRetention_ = readTextureImage(event["preservationMask"].toString());
            if (candidateRetention_.size() != candidate_.size()) {
                fail("AI returned an invalid retained-detail mask");
                return;
            }
            candidateRetention_ = candidateRetention_.convertToFormat(QImage::Format_Grayscale8);
            candidateSettings_["retentionMask"] = imageData(candidateRetention_);
            candidateSettings_["detailLockApplied"] = true;
        }
        candidateSettings_["timings"] = event["timings"];
        // The worker guards surfaces it left as background white on bare clay; they start excluded
        // from projection (red in the mask) and can be restored with a right drag.
        QImage mask = capture_->silhouette.convertToFormat(QImage::Format_RGB32);
        if (parity_ && regenerateIndex_ >= 0 && regenerateIndex_ < projections_.size()) {
            const auto previous = projections_[regenerateIndex_];
            const auto originalMask = previous["userMask"].isString() ? previous["userMask"] : previous["mask"];
            const auto saved = QImage::fromData(QByteArray::fromBase64(originalMask.toString().toLatin1()), "PNG");
            if (!saved.isNull())
                mask = saved.convertToFormat(QImage::Format_RGB32).scaled(
                    mask.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        if (!parity_ && !event["guardMask"].toString().isEmpty()) {
            const QImage guard = readTextureImage(event["guardMask"].toString()).convertToFormat(QImage::Format_Grayscale8);
            if (guard.size() == mask.size())
                for (int y = 0; y < mask.height(); ++y)
                    for (int x = 0; x < mask.width(); ++x)
                        if (guard.constScanLine(y)[x] < 128)
                            mask.setPixel(x, y, qRgb(0, 0, 0));
        }
        // Cloud views never project over locked paint, nor bare clay or background the model left unpainted.
        if (!cloudMask.isNull())
            mask = cloudMask;
        canvas_->set(candidate_, mask);
        sideCanvas_->set(candidate_, mask);
        progress_->setValue(1000);
        const double alignment = event["alignment"].isDouble() ? event["alignment"].toDouble() : 1;
        if (alignment < .75) {
            status_->setText(QString("Generated geometry differs from the capture (%1% silhouette "
                                     "overlap). Automatic projection paused. Inspect the cyan outline; "
                                     "regenerate or mask incorrect areas before projecting.")
                                 .arg(qRound(alignment * 100)));
            setBusy(false);
            return;
        }
        if (automatic_)
            projectCandidate(true);
        else {
            status_->setText(QString("Candidate ready in %1 s. Inspect the geometry outline and paint "
                                     "the mask before projecting. Left drag excludes; right restores.")
                                 .arg(event["seconds"].toDouble(), 0, 'f', 1));
            setBusy(false);
        }
    }
}
AiProjectionOptions AiPanel::projectionOptions() const {
    AiProjectionOptions options;
    options.overlay = blendMode_->currentData() == "overlay";
    options.lockQuality = blendMode_->currentData() == "lock" ? aiDefaultLockQuality : 0;
    options.strength = projectionStrength_->value() / 100.f;
    options.feather = projectionFeather_->value() / 100.f;
    options.anglePower = float(angleFalloff_->value());
    options.minCosine = std::cos(projectionAngle_->value() * 3.14159265359f / 180);
    options.fillGaps = fillGaps_->isChecked();
    options.coverAll = coverAll_->isChecked();
    options.colorMatch = colorMatch_->isChecked();
    return options;
}
void AiPanel::projectCandidate(bool automatic) {
    if ((busy_ && !automatic) || externalBusy_ || !scene_ || !capture_ || candidate_.isNull())
        return;
    if (parity_ && !projectionError_.isEmpty()) {
        fail("Projection renderer unavailable: " + projectionError_);
        return;
    }
    if (native_.joinable())
        native_.join();
    if (!automatic) {
        cancel_ = false;
        setBusy(true);
    }
    auto scene = scene_;
    auto capture = capture_;
    auto atlases = atlases_;
    auto candidate = candidate_;
    auto mask = canvas_->mask;
    auto materials = selectedMaterials();
    int uv = uv_->currentIndex();
    bool fill = blendMode_->currentData() == "fill";
    auto options = projectionOptions();
    // Legacy cloud views carry ownership in their mask: the lock core is excluded and its outer band ramps,
    // crossfading into accepted paint. Independent parity layers do not use this legacy mask behavior.
    const bool ownedByMask = !parity_ && candidateSettings_["model"] == "gpt-image-2" && blendMode_->currentData() == "lock";
    if (ownedByMask) {
        options.lockQuality = 0;
        options.overlay = true;
    }
    options.recordLayer = true;
    options.retainedMask = candidateRetention_;
    QJsonObject record = candidateSettings_;
    record["candidate"] = imageData(candidate);
    record["mask"] = imageData(mask);
    if (parity_)
        record["userMask"] = imageData(mask);
    record["layerId"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    record["materials"] = QJsonArray::fromStringList(materials.values());
    record["fillOnly"] = fill;
    record["blending"] = QJsonObject{{"mode", ownedByMask ? QString("overlay") : blendMode_->currentData().toString()},
                                     {"ownedByMask", ownedByMask},
                                     {"strength", options.strength},
                                     {"feather", options.feather},
                                     {"anglePower", options.anglePower},
                                     {"minCosine", options.minCosine},
                                     {"fillGaps", options.fillGaps},
                                     {"coverAll", options.coverAll},
                                     {"colorMatch", options.colorMatch}};
    if (parity_)
        record["blending"] = QJsonObject{{"mode", options.overlay ? "overlay" : "blend"},
                                          {"strength", options.strength}, {"depthMargin", 2}};
    status_->setText("Projecting visible surfaces into UV space…");
    const int replace = regenerateIndex_;
    const auto base = layerBase_;
    const auto records = projections_;
    const bool parity = parity_;
    const QString isolation = isolatedLayer_;
    const int resolution = resolution_->currentData().toInt();
    native_ = std::thread([this, scene, capture, atlases, candidate, mask, materials, uv, fill, record, options,
                           replace, base, records, parity, resolution, isolation] {
        try {
            if (parity) {
                auto prepared = records;
                auto toBake = record;
                if (replace >= 0) {
                    const auto previous = prepared[replace];
                    toBake["blending"] = previous["blending"];
                    toBake["materials"] = previous["materials"];
                    if (previous.contains("uvMask"))
                        toBake["uvMask"] = previous["uvMask"];
                }
                auto baked = aiBakeProjectionLayer(*scene, uv, resolution, toBake, &cancel_);
                baked["visible"] = true;
                if (replace >= 0) {
                    baked["layerId"] = prepared[replace]["layerId"];
                    baked["visible"] = prepared[replace]["visible"];
                    prepared[replace] = baked;
                } else
                    prepared.append(baked);
                auto effective = prepared;
                if (!isolation.isEmpty())
                    for (auto &r : effective)
                        r["visible"] = r["layerId"].toString() == isolation;
                auto result = std::make_shared<AiProjection>();
                result->atlases = aiComposeParityLayers(base, effective, &cancel_);
                result->layer = baked["uvLayer"].toObject();
                const auto contribution = aiDecodeProjectionLayer(result->layer);
                for (const auto &image : contribution.materials)
                    for (int y = 0; y < image.height(); ++y) {
                        const uchar *pixels = image.constScanLine(y);
                        for (int x = 0; x < image.width(); ++x)
                            result->changed += pixels[x * 4 + 3] != 0;
                    }
                if (replace >= 0)
                    QMetaObject::invokeMethod(this, [this, result, prepared] { finishRegeneration(*result, prepared); },
                                               Qt::QueuedConnection);
                else
                    QMetaObject::invokeMethod(this, [this, result, baked] { finishProjection(*result, baked); },
                                               Qt::QueuedConnection);
                return;
            }
            if (replace >= 0) {
                // Regenerated layer: project onto the stack below it, put it back at the same position, then
                // replay the layers above it on top.
                auto prepared = std::any_of(records.begin(), records.end(),
                                            [](const QJsonObject &r) {
                                                return r["uvLayer"].toObject()["version"].toInt() != 1;
                                            })
                                    ? aiPrepareLayers(*scene, uv, base, records, &cancel_)
                                    : records;
                const auto below = aiComposeLayers(base, prepared.mid(0, replace), &cancel_, &layerDecoded_);
                auto result = std::make_shared<AiProjection>(aiProject(
                    *scene, uv, below, *capture, candidate, mask, materials, fill, 8, &cancel_, options));
                QJsonObject layerRecord = record;
                layerRecord["layerId"] = prepared[replace]["layerId"];
                layerRecord["uvLayer"] = result->layer;
                layerRecord["visible"] = prepared[replace]["visible"];
                layerRecord["regeneratedFrom"] = prepared[replace]["seed"];
                prepared[replace] = layerRecord;
                result->atlases = aiComposeLayers(base, prepared, &cancel_, &layerDecoded_);
                QMetaObject::invokeMethod(
                    this, [this, result, prepared] { finishRegeneration(*result, prepared); }, Qt::QueuedConnection);
                return;
            }
            auto result = std::make_shared<AiProjection>(aiProject(
                *scene, uv, atlases, *capture, candidate, mask, materials, fill, 8, &cancel_, options));
            QMetaObject::invokeMethod(
                this, [this, result, record] { finishProjection(*result, record); }, Qt::QueuedConnection);
        } catch (const std::exception &e) {
            QString msg = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(
                this, [this, msg] { fail(msg); }, Qt::QueuedConnection);
        }
    });
}
void AiPanel::finishProjection(const AiProjection &result, const QJsonObject &record) {
    if (native_.joinable())
        native_.join();
    if (cancel_) {
        fail("Cancelled. Accepted projections are preserved.");
        return;
    }
    if (result.changed) {
        layerFrames_.clear();
        layerDecoded_.clear();
        if (projections_.empty())
            layerBase_ = atlases_;
        undo_ << historyState();
        if (undo_.size() > 10)
            undo_.removeFirst();
        redo_.clear();
        atlases_ = result.atlases;
        QJsonObject layerRecord = record;
        layerRecord["uvLayer"] = result.layer;
        layerRecord["visible"] = true;
        projections_ << layerRecord;
        ++revision_;
    }
    updatePreview();
    refreshLayers();
    if (result.changed)
        layers_->setCurrentRow(0);
    if (result.changed)
        status_->setText(
            QString("Projected %1 texels from the generated camera. Undo is available.").arg(result.changed));
    else if (!candidateRetention_.isNull())
        status_->setText(
            "Trusted previous details were kept. Lower Keep previous details or disable its lock "
            "before generating to repaint those areas.");
    else if (blendMode_->currentData() != "overlay")
        status_->setText(
            "Existing paint was preserved. Choose Camera projection (repaint) to paint over it.");
    else
        status_->setText(
            "No visible unmasked texels were projected. Check the candidate mask and selected materials.");
    if (automatic_) {
        ++cameraIndex_;
        nextCamera();
    } else
        setBusy(false);
}
void AiPanel::finishRegeneration(const AiProjection &result, const QList<QJsonObject> &records) {
    if (native_.joinable())
        native_.join();
    if (cancel_) {
        fail("Cancelled. Accepted projections are preserved.");
        return;
    }
    const int index = regenerateIndex_;
    regenerateIndex_ = -1;
    layerFrames_.clear(); // Same visibility key, different content.
    layerDecoded_.clear();
    undo_ << historyState();
    if (undo_.size() > 10)
        undo_.removeFirst();
    redo_.clear();
    atlases_ = result.atlases;
    projections_ = records;
    ++revision_;
    invalidate();
    updatePreview();
    layers_->clear(); // Same layer count: force new thumbnails.
    refreshLayers();
    if (index >= 0 && index < projections_.size())
        layers_->setCurrentRow(projections_.size() - 1 - index);
    status_->setText(QString("Projection %1 regenerated in place; the layers above it were reapplied. Undo restores "
                             "the previous version.")
                         .arg(index + 1));
    setBusy(false);
}
void AiPanel::fail(const QString &message) {
    regenerateIndex_ = -1;
    engine_.stop();
    cloud_.cancel();
    if (native_.joinable())
        native_.join();
    status_->setText(message + " Accepted projections remain available.");
    setBusy(false);
    refreshLayers();
}
void AiPanel::cancelJob() {
    if (installer_.busy()) {
        installer_.cancel();
        return;
    }
    cancel_ = true;
    engine_.stop();
    cloud_.cancel();
    if (!native_.joinable())
        fail("Cancelled");
}
void AiPanel::beginInstall(const QString &offline, bool repair) {
    if (busy_ || externalBusy_ || installer_.busy())
        return;
    engine_.stop();
    installer_.setHuggingFaceToken(hfToken_->text());
    if (!hardware_.compatible) {
        status_->setText(hardware_.error);
        return;
    }
    if (!offline.isEmpty()) {
        try {
            auto imported = AiManifest::load(offline + "/manifest.json");
            if (imported.json != manifest_.json)
                throw std::runtime_error("Offline pack does not match this Fornaio release");
        } catch (const std::exception &e) {
            status_->setText(QString::fromUtf8(e.what()));
            return;
        }
    }
    installer_.setMemoryProfile(profile_->currentData().toString());
    installer_.install(offline, repair);
    refresh();
}
void AiPanel::chooseLocation() {
    if (busy_ || installer_.busy())
        return;
    QString location = QFileDialog::getExistingDirectory(this, "AI component storage", installer_.root());
    if (location.isEmpty())
        return;
    engine_.stop();
    const QString previous = installer_.root();
    const bool migrate = installer_.installed();
    if (QDir(location).absolutePath().compare(previous, Qt::CaseInsensitive) == 0)
        return;
    installer_.setRoot(location);
    location_->setText(installer_.root());
    if (migrate && !installer_.installed()) {
        migrationSource_ = previous;
        installer_.setMemoryProfile(profile_->currentData().toString());
        installer_.install(previous);
        refresh();
        return;
    }
    QSettings().setValue("ai/location", installer_.root());
    status_->setText(
        "Storage location selected. Existing components at the previous location are preserved.");
    refresh();
}
void AiPanel::saveProject() {
    if (!scene_ || busy_)
        return;
    QString path = QFileDialog::getSaveFileName(this, "Save AI texturing project", {},
                                                "Fornaio AI project (*.fornaioai)");
    if (path.isEmpty())
        return;
    try {
        QFile mesh(meshPath_);
        if (!mesh.open(QIODevice::ReadOnly))
            throw std::runtime_error("Cannot read mesh for project fingerprint");
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&mesh);
        QJsonObject settings{{"mesh", QDir(QFileInfo(path).absolutePath()).relativeFilePath(meshPath_)},
                             {"projectionBackend", parity_ ? "modddif-v1" : "legacy"},
                             {"isolatedLayer", isolatedLayer_},
                             {"meshSha256", QString::fromLatin1(hash.result().toHex())},
                             {"uv", uv_->currentIndex()},
                             {"resolution", resolution_->currentData().toInt()},
                             {"generationResolution", generationResolution_->currentData().toInt()},
                             {"prompt", prompt_->toPlainText()},
                             {"references", QJsonArray::fromStringList(referencePaths())},
                             {"seed", seed_->value()},
                             {"continuityStrength", continuity_->value()},
                             {"protectDetails", protectDetails_->isChecked()},
                             {"randomSeed", randomSeed_->isChecked()},
                             {"generationMode", generationMode_->currentIndex()},
                             {"showUncovered", showUncovered_->isChecked()},
                             {"profile", profile_->currentData().toString()},
                             {"materials", QJsonArray::fromStringList(selectedMaterials().values())},
                             {"blendMode", blendMode_->currentData().toString()},
                             {"projectionBehaviorVersion", 2},
                             {"projectionStrength", projectionStrength_->value()},
                             {"projectionFeather", projectionFeather_->value()},
                             {"angleFalloff", angleFalloff_->value()},
                             {"projectionAngle", projectionAngle_->value()},
                             {"projectionFillGaps", fillGaps_->isChecked()},
                             {"projectionCoverAll", coverAll_->isChecked()},
                             {"colorMatch", colorMatch_->isChecked()},
                             {"brush", brush_->value()},
                             {"camera", aiCameraJson(aiCamera(*scene_, view_->camera()))},
                             {"model", manifest_.json["model"]},
                             {"revision", manifest_.json["revision"]}};
        QJsonObject names;
        for (auto it = exportNames_.begin(); it != exportNames_.end(); ++it)
            names[it.key()] = it.value();
        settings["exportNames"] = names;
        settings["layerBase"] = aiSerializeAtlases(layerBase_);
        auto records = projections_;
        if (!candidate_.isNull()) {
            QJsonObject saved = candidateSettings_;
            saved["candidate"] = imageData(candidate_);
            saved["mask"] = imageData(canvas_->mask);
            settings["pendingCandidate"] = saved;
        }
        aiSaveProject(path, settings, atlases_, records);
        status_->setText("Project saved.");
    } catch (const std::exception &e) {
        status_->setText(QString::fromUtf8(e.what()));
    }
}
void AiPanel::openProject() {
    if (busy_)
        return;
    QString path = QFileDialog::getOpenFileName(this, "Open AI texturing project", {},
                                                "Fornaio AI project (*.fornaioai)");
    if (path.isEmpty())
        return;
    try {
        AiAtlases loaded;
        QList<QJsonObject> records;
        auto settings = aiLoadProject(path, loaded, records);
        QString meshPath = QDir(QFileInfo(path).absolutePath()).absoluteFilePath(settings["mesh"].toString());
        QFile mesh(meshPath);
        if (!mesh.open(QIODevice::ReadOnly))
            throw std::runtime_error("Project mesh is missing: restore it to its saved location");
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(&mesh);
        if (QString::fromLatin1(hash.result().toHex()) != settings["meshSha256"].toString())
            throw std::runtime_error(
                "Project mesh has changed; restore the original mesh to preserve projection alignment");
        if (!loadMesh(meshPath))
            return;
        parity_ = settings["projectionBackend"].toString() == "modddif-v1";
        isolatedLayer_ = settings["isolatedLayer"].toString();
        loading_ = true;
        uv_->setCurrentIndex(settings["uv"].toInt());
        resolution_->setCurrentIndex(resolution_->findData(settings["resolution"].toInt()));
        generationResolution_->setCurrentIndex(
            std::max(0, generationResolution_->findData(settings["generationResolution"].toInt(1024))));
        prompt_->setPlainText(settings["prompt"].toString());
        seed_->setValue(settings["seed"].toInt());
        continuity_->setValue(settings["continuityStrength"].toInt(80));
        protectDetails_->setChecked(settings["protectDetails"].toBool(true));
        randomSeed_->setChecked(settings["randomSeed"].toBool(true));
        generationMode_->setCurrentIndex(settings["generationMode"].toInt() == 1 ? 1 : 0);
        showUncovered_->setChecked(settings["showUncovered"].toBool(true));
        profile_->setCurrentIndex(profile_->findData(settings["profile"].toString()));
        QSet<QString> selected;
        for (const auto &v : settings["materials"].toArray())
            selected.insert(v.toString());
        for (int i = 0; i < materials_->count(); ++i)
            materials_->item(i)->setCheckState(
                selected.contains(materials_->item(i)->text()) ? Qt::Checked : Qt::Unchecked);
        // Migrate the former quality-protected default so old projects can repaint.
        const bool legacy = settings["projectionBehaviorVersion"].toInt() < 2;
        QString mode = settings["blendMode"].toString("overlay");
        if (legacy && mode == "blend")
            mode = "overlay";
        int blend = blendMode_->findData(mode);
        blendMode_->setCurrentIndex(std::max(0, blend));
        projectionStrength_->setValue(settings["projectionStrength"].toInt(100));
        projectionFeather_->setValue(settings["projectionFeather"].toInt(6));
        angleFalloff_->setValue(settings["angleFalloff"].toInt(2));
        projectionAngle_->setValue(settings["projectionAngle"].toInt(60));
        fillGaps_->setChecked(settings["projectionFillGaps"].toBool(true));
        coverAll_->setChecked(settings["projectionCoverAll"].toBool(true));
        colorMatch_->setChecked(legacy && mode == "overlay" ? false : settings["colorMatch"].toBool(false));
        brush_->setValue(settings["brush"].toInt(32));
        references_->clear();
        for (auto v : settings["references"].toArray()) {
            auto item = new QListWidgetItem(QIcon(QPixmap::fromImage(readTextureImage(v.toString()))),
                                            v.toString(), references_);
            item->setToolTip(v.toString());
        }
        loading_ = false;
        meshErrors_ = validateAiMesh(*scene_, uv_->currentIndex());
        if (!meshErrors_.empty())
            throw std::runtime_error(meshErrors_.join('\n').toStdString());
        atlases_ = loaded;
        meshInfo_->setText(QString("%1\n%2 triangles · %3 materials")
                               .arg(QFileInfo(meshPath_).fileName())
                               .arg(scene_->tris.size())
                               .arg(atlases_.size()));
        projections_ = records;
        layerBase_ = settings.contains("layerBase") ? aiDeserializeAtlases(settings["layerBase"].toArray())
                                                    : aiEmptyAtlases(*scene_, atlases_.first().color.width());
        if (projections_.empty())
            layerBase_ = loaded;
        if (layerBase_.keys() != atlases_.keys())
            throw std::runtime_error("Saved layer base does not match the mesh materials");
        refreshLayers();
        undo_.clear();
        redo_.clear();
        view_->setMesh(scene_, uv_->currentIndex());
        view_->setCamera(aiCameraFromJson(settings["camera"].toObject()).orbit);
        if (!settings["camera"].toObject().contains("fieldOfView"))
            view_->setFieldOfView(20); // Future views use the narrower lens; saved projections stay frozen.
        auto savedNames = settings["exportNames"].toObject();
        for (auto it = savedNames.begin(); it != savedNames.end(); ++it)
            exportNames_[it.key()] = it.value().toString();
        atlasChoice_->clear();
        atlasChoice_->addItems(atlases_.keys());
        if (settings.contains("pendingCandidate")) {
            candidateSettings_ = settings["pendingCandidate"].toObject();
            candidate_ = QImage::fromData(
                QByteArray::fromBase64(candidateSettings_["candidate"].toString().toLatin1()), "PNG");
            QImage mask = QImage::fromData(
                QByteArray::fromBase64(candidateSettings_["mask"].toString().toLatin1()), "PNG");
            capture_ = std::make_shared<AiCapture>(
                aiCapture(*scene_, uv_->currentIndex(), atlases_,
                          aiCameraFromJson(candidateSettings_["camera"].toObject())));
            candidateRetention_ = QImage::fromData(
                QByteArray::fromBase64(candidateSettings_["retentionMask"].toString().toLatin1()), "PNG");
            if (!candidateRetention_.isNull()) {
                QImage sourceConfidence =
                    QImage::fromData(QByteArray::fromBase64(
                                         candidateSettings_["feedbackConfidenceImage"].toString().toLatin1()),
                                     "PNG");
                if (candidateRetention_.size() != candidate_.size() ||
                    sourceConfidence.size() != candidate_.size())
                    throw std::runtime_error("Saved detail continuity data does not match the candidate");
                candidateRetention_ = candidateRetention_.convertToFormat(QImage::Format_Grayscale8);
                capture_->confidence = sourceConfidence.convertToFormat(QImage::Format_Grayscale16);
            }
            canvas_->set(candidate_, mask);
            sideCanvas_->set(candidate_, mask);
            canvas_->silhouette = sideCanvas_->silhouette = capture_->silhouette;
        }
        updatePreview();
        refresh();
        status_->setText(parity_ ? "Project restored without running inference."
                                : "Legacy project restored unchanged. Rebuild projection layers to use the corrected projection pipeline.");
    } catch (const std::exception &e) {
        loading_ = false;
        status_->setText(QString::fromUtf8(e.what()));
    }
}
void AiPanel::exportTextures() {
    if (atlases_.empty() || busy_)
        return;
    QString directory = QFileDialog::getExistingDirectory(this, "Export per-material base colors");
    if (directory.isEmpty())
        return;
    try {
        QMap<QString, QString> names;
        QSet<QString> used;
        const auto selected = selectedMaterials();
        for (auto it = atlases_.begin(); it != atlases_.end(); ++it) {
            if (!selected.contains(it.key()))
                continue;
            QString name = exportNames_.value(it.key());
            if (!QRegularExpression("^[A-Za-z0-9_-]+$").match(name).hasMatch() ||
                used.contains(name.toLower()))
                throw std::runtime_error("Choose unique export names in the UV texture tab.");
            used.insert(name.toLower());
            names[it.key()] = directory + "/" + name + "_ALB.png";
        }
        bool existing = false;
        for (const auto &path : names)
            existing |= QFileInfo::exists(path);
        if (existing && QMessageBox::question(this, "Replace textures",
                                              "Some output files exist. Replace them?") != QMessageBox::Yes)
            return;
        for (auto it = atlases_.begin(); it != atlases_.end(); ++it) {
            if (!selected.contains(it.key()))
                continue;
            const auto &im = it->color;
            std::vector<uint16_t> pixels(size_t(im.width()) * im.height() * 3);
            for (int y = 0; y < im.height(); ++y)
                for (int x = 0; x < im.width(); ++x) {
                    QRgb p = im.pixel(x, y);
                    size_t i = (size_t(y) * im.width() + x) * 3;
                    pixels[i] = qRed(p);
                    pixels[i + 1] = qGreen(p);
                    pixels[i + 2] = qBlue(p);
                }
            writePng(names[it.key()], im.width(), im.height(), 3, 8, pixels.data(), PngSpace::Srgb);
        }
        status_->setText("Base-color textures exported. Use the selected UV set in your material.");
    } catch (const std::exception &e) {
        status_->setText(QString::fromUtf8(e.what()));
    }
}
