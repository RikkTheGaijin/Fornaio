#include "window.h"
#include "scroll_controls.h"
#include <QApplication>
#include <QCloseEvent>
#include <QCryptographicHash>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsPixmapItem>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QMessageBox>
#include <QPainter>
#include <QRegularExpressionValidator>
#include <QScrollArea>
#include <QSettings>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
static QString filePreferenceKey(const QString &path) {
    return "files/" +
           QString::fromLatin1(QCryptographicHash::hash(QFileInfo(path).absoluteFilePath().toUtf8(),
                                                        QCryptographicHash::Sha256)
                                   .toHex());
}
class ImageView : public QGraphicsView {
  public:
    using QGraphicsView::QGraphicsView;

  protected:
    void wheelEvent(QWheelEvent *e) override {
        double f = e->angleDelta().y() > 0 ? 1.2 : 1 / 1.2;
        scale(f, f);
    }
};
static QLabel *section(const QString &title) {
    auto l = new QLabel(title);
    l->setStyleSheet("font-size: 14px; font-weight: 600; color: #89d5ca; margin-top: 12px;");
    return l;
}
Window::Window() {
    setWindowTitle("Fornaio " FORNAIO_VERSION);
    resize(1320, 900);
    setMinimumSize(1000, 720);
    auto root = new QWidget;
    auto central = new QWidget;
    auto centralLayout = new QVBoxLayout(central);
    centralLayout->setContentsMargins(0, 0, 0, 0);
    centralLayout->setSpacing(0);
    setCentralWidget(central);
    auto header = new QHBoxLayout;
    header->setContentsMargins(20, 14, 20, 12);
    header->setSpacing(12);
    auto logo = new QLabel;
    logo->setObjectName("logo");
    logo->setPixmap(QIcon(":/fornaio.png").pixmap(48, 48));
    header->addWidget(logo);
    auto titles = new QVBoxLayout;
    titles->setSpacing(0);
    auto title = new QLabel("FORNAIO");
    title->setStyleSheet("font-size: 23px; font-weight: 700; letter-spacing: 2px;");
    titles->addWidget(title);
    auto subtitle = new QLabel("Freshly baked textures  /  NVIDIA RTX");
    subtitle->setStyleSheet("color: #97a9be;");
    titles->addWidget(subtitle);
    header->addLayout(titles);
    header->addStretch();
    centralLayout->addLayout(header);
    mainTabs_ = new QTabWidget;
    mainTabs_->setObjectName("mainTabs");
    mainTabs_->setDocumentMode(true);
    centralLayout->addWidget(mainTabs_, 1);
    mainTabs_->addTab(root, "AO Baker");
    transfer_ = new TransferPanel;
    mainTabs_->addTab(transfer_, "Texture Transfer");
    ai_ = new AiPanel;
    mainTabs_->addTab(ai_, "AI Texturing");
    // All GPU owners are coordinated on the UI thread. Downloads do not own the GPU.
    connect(transfer_, &TransferPanel::runningChanged, this, [this](bool running) {
        transferRunning_ = running;
        ai_->setExternalBusy(busy_ || running);
        refresh();
    });
    connect(ai_, &AiPanel::runningChanged, this, [this](bool running) {
        aiRunning_ = running;
        transfer_->setExternalBusy(busy_ || running);
        refresh();
    });
    auto layout = new QVBoxLayout(root);
    layout->setContentsMargins(20, 16, 20, 16);
    auto split = new QSplitter;
    layout->addWidget(split, 1);
    auto scroll = new QScrollArea;
    scroll->setObjectName("aoControls");
    scroll->setWidgetResizable(true);
    scroll->setMinimumWidth(330);
    scroll->setMaximumWidth(420);
    inputs_ = new QWidget;
    scroll->setWidget(inputs_);
    auto controls = new QVBoxLayout(inputs_);
    controls->setSpacing(9);
    controls->addWidget(section("01   INPUTS"));
    auto meshButton = new QPushButton("Load geometry…  FBX / OBJ");
    controls->addWidget(meshButton);
    geometryLabel_ = new QLabel("No geometry loaded");
    geometryLabel_->setWordWrap(true);
    controls->addWidget(geometryLabel_);
    auto occluderButtons = new QHBoxLayout;
    auto loadOccluderButton = new QPushButton("Load head / occluder…");
    removeOccluder_ = new QPushButton("Remove");
    removeOccluder_->setEnabled(false);
    occluderButtons->addWidget(loadOccluderButton, 1);
    occluderButtons->addWidget(removeOccluder_);
    controls->addLayout(occluderButtons);
    occluderLabel_ = new QLabel("Optional · opaque casting mesh · UVs not required");
    occluderLabel_->setWordWrap(true);
    occluderLabel_->setToolTip("Export hair and head from the same scene with matching coordinates and "
                               "units. No automatic alignment or scaling is applied.");
    controls->addWidget(occluderLabel_);
    auto maskButton = new QPushButton("Load opacity mask…");
    controls->addWidget(maskButton);
    maskLabel_ = new QLabel("PNG / TIFF / TGA · linear opacity");
    maskLabel_->setWordWrap(true);
    for (auto l : {geometryLabel_, occluderLabel_, maskLabel_})
        l->setStyleSheet("color:#97a9be;");
    controls->addWidget(maskLabel_);
    auto form = new QFormLayout;
    maskUV_ = new ScrollComboBox;
    aoUV_ = new ScrollComboBox;
    channel_ = new ScrollComboBox;
    channel_->addItems({"Red", "Green", "Blue", "Alpha"});
    form->addRow("Mask UV", maskUV_);
    form->addRow("AO UV", aoUV_);
    form->addRow("Mask channel", channel_);
    controls->addLayout(form);
    invert_ = new QCheckBox("Invert mask");
    flip_ = new QCheckBox("Flip mask vertically");
    repeat_ = new QCheckBox("Repeat mask outside 0–1");
    controls->addWidget(invert_);
    controls->addWidget(flip_);
    controls->addWidget(repeat_);
    averageOverlaps_ = new QCheckBox("Average AO where UVs overlap");
    averageOverlaps_->setToolTip("Continue baking with unchanged UVs. Faces sharing a pixel contribute "
                                 "opacity-weighted AO. Disable to require unique UVs.");
    controls->addWidget(averageOverlaps_);
    controls->addWidget(section("02   UV2 LAYOUT"));
    auto uvNote = new QLabel("Packs the UV1 islands of the loaded FBX into a new non-overlapping UV2 with "
                             "uniform texel density, then loads the result.");
    uvNote->setWordWrap(true);
    uvNote->setStyleSheet("color:#97a9be;");
    controls->addWidget(uvNote);
    auto uvForm = new QFormLayout;
    uvSpacing_ = new ScrollSpinBox;
    uvSpacing_->setRange(1, 64);
    uvSpacing_->setValue(4);
    uvSpacing_->setSuffix(" px");
    uvSpacing_->setToolTip("Minimum gap between UV2 islands, in pixels at the selected bake resolution.");
    uvForm->addRow("Island spacing", uvSpacing_);
    uv2Name_ = new QLineEdit("UV2");
    uv2Name_->setPlaceholderText("UV2");
    uv2Name_->setMaxLength(64);
    uv2Name_->setValidator(
        new QRegularExpressionValidator(QRegularExpression("[A-Za-z_][A-Za-z0-9_]*"), uv2Name_));
    uv2Name_->setToolTip("Name of the generated UV set. Letters, digits and underscores; it must differ from "
                         "the UV1 name.");
    uvForm->addRow("UV set name", uv2Name_);
    controls->addLayout(uvForm);
    uvRotate_ = new QCheckBox("Allow 90° island rotation");
    uvRotate_->setToolTip("Lets the packer turn islands by 90° when that fits them tighter. Off keeps every "
                          "island in its UV1 orientation.");
    controls->addWidget(uvRotate_);
    generateUv2_ = new QPushButton("Generate UV2");
    generateUv2_->setEnabled(false);
    generateUv2As_ = new QPushButton("Save as…");
    generateUv2As_->setEnabled(false);
    generateUv2As_->setToolTip("Generate UV2 into a different FBX file.");
    auto uvButtons = new QHBoxLayout;
    uvButtons->addWidget(generateUv2_, 1);
    uvButtons->addWidget(generateUv2As_);
    controls->addLayout(uvButtons);
    uv2Target_ = new QLabel;
    uv2Target_->setWordWrap(true);
    uv2Target_->setStyleSheet("color:#97a9be;");
    uv2Target_->hide();
    controls->addWidget(uv2Target_);
    controls->addWidget(section("03   BAKE SETTINGS"));
    auto presets = new QHBoxLayout;
    auto draft = new QPushButton("Draft"), quality = new QPushButton("Quality");
    presets->addWidget(draft);
    presets->addWidget(quality);
    controls->addLayout(presets);
    auto settingsForm = new QFormLayout;
    resolution_ = new ScrollComboBox;
    for (int r = 256; r <= 8192; r *= 2)
        resolution_->addItem(QString::number(r), r);
    resolution_->setCurrentText("2048");
    rays_ = new ScrollSpinBox;
    rays_->setRange(1, 65536);
    rays_->setValue(256);
    ss_ = new ScrollComboBox;
    for (int n = 1; n <= 4; n++)
        ss_->addItem(QString("%1 × %1").arg(n), n);
    ss_->setCurrentIndex(1);
    padding_ = new ScrollSpinBox;
    padding_->setRange(0, 128);
    padding_->setValue(8);
    padding_->setSuffix(" px");
    bits_ = new ScrollComboBox;
    bits_->addItem("16-bit grayscale", 16);
    bits_->addItem("8-bit grayscale", 8);
    distance_ = new ScrollDoubleSpinBox;
    distance_->setDecimals(6);
    distance_->setRange(0, 1e9);
    distance_->setSpecialValueText("Unlimited");
    distance_->setToolTip("Maximum ray distance in imported model units. Zero means unlimited.");
    bias_ = new ScrollDoubleSpinBox;
    bias_->setDecimals(9);
    bias_->setRange(0, 1e6);
    bias_->setSingleStep(.00001);
    bias_->setSpecialValueText("Automatic");
    bias_->setToolTip("Offset in model units. Automatic uses one millionth of the bounding-box diagonal.");
    settingsForm->addRow("Resolution", resolution_);
    settingsForm->addRow("Rays / sample", rays_);
    settingsForm->addRow("Subpixel samples", ss_);
    settingsForm->addRow("Max distance", distance_);
    settingsForm->addRow("Ray bias", bias_);
    settingsForm->addRow("Island padding", padding_);
    settingsForm->addRow("PNG output", bits_);
    controls->addLayout(settingsForm);
    auto note = new QLabel("Front-side AO · two-sided occluders\nWhite = unoccluded · black = occluded\nNo "
                           "denoising · one shared opacity mask");
    note->setStyleSheet("color:#97a9be;");
    controls->addWidget(note);
    controls->addStretch();
    split->addWidget(scroll);
    auto preview = new QWidget;
    auto pv = new QVBoxLayout(preview);
    pv->setContentsMargins(8, 0, 0, 0);
    auto tabs = new QTabWidget;
    auto page3d = new QWidget;
    auto v3 = new QVBoxLayout(page3d);
    auto toolbar = new QHBoxLayout;
    mode_ = new ScrollComboBox;
    mode_->addItems({"Masked geometry", "Solid geometry", "Baked AO"});
    toolbar->addWidget(new QLabel("Display"));
    toolbar->addWidget(mode_);
    toolbar->addStretch();
    auto fit = new QPushButton("Frame");
    toolbar->addWidget(fit);
    v3->addLayout(toolbar);
    view_ = new Viewport;
    v3->addWidget(view_, 1);
    tabs->addTab(page3d, "3D preview");
    auto resultTab = new QWidget;
    auto rl = new QVBoxLayout(resultTab);
    auto imageTools = new QHBoxLayout;
    exportTexture_ = new QPushButton("Export PNG…");
    exportTexture_->setEnabled(false);
    imageTools->addWidget(exportTexture_);
    imageTools->addStretch();
    auto fitImage = new QPushButton("Fit"), actual = new QPushButton("1:1 pixels");
    imageTools->addWidget(actual);
    imageTools->addWidget(fitImage);
    rl->addLayout(imageTools);
    imageScene_ = new QGraphicsScene(this);
    imageView_ = new ImageView(imageScene_);
    imageView_->setDragMode(QGraphicsView::ScrollHandDrag);
    imageView_->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    imageView_->setBackgroundBrush(QColor("#11151b"));
    rl->addWidget(imageView_);
    tabs->addTab(resultTab, "AO texture");
    uvLayoutTab_ = new QWidget;
    auto ul = new QVBoxLayout(uvLayoutTab_);
    auto uvTools = new QHBoxLayout;
    uvLayoutLabel_ = new QLabel("Load geometry to see its AO UV layout.");
    uvLayoutLabel_->setStyleSheet("color:#97a9be;");
    uvTools->addWidget(uvLayoutLabel_, 1);
    auto fitUv = new QPushButton("Fit");
    uvTools->addWidget(fitUv);
    ul->addLayout(uvTools);
    uvScene_ = new QGraphicsScene(this);
    uvView_ = new ImageView(uvScene_);
    uvView_->setDragMode(QGraphicsView::ScrollHandDrag);
    uvView_->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    uvView_->setBackgroundBrush(QColor("#11151b"));
    ul->addWidget(uvView_);
    tabs->addTab(uvLayoutTab_, "UV layout");
    previewTabs_ = tabs;
    connect(fitUv, &QPushButton::clicked, this,
            [this] { uvView_->fitInView(uvScene_->sceneRect(), Qt::KeepAspectRatio); });
    connect(tabs, &QTabWidget::currentChanged, this, [this] { updateUvLayout(); });
    pv->addWidget(tabs);
    split->addWidget(preview);
    split->setStretchFactor(1, 1);
    auto footer = new QHBoxLayout;
    bake_ = new QPushButton("Bake AO");
    bake_->setObjectName("primary");
    bake_->setMinimumSize(130, 42);
    cancel_ = new QPushButton("Cancel");
    cancel_->setEnabled(false);
    export_ = new QPushButton("Export PNG…  Ctrl+S");
    export_->setShortcut(QKeySequence::Save);
    export_->setEnabled(false);
    footer->addWidget(export_);
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setValue(0);
    footer->addWidget(progress_, 1);
    elapsed_ = new QLabel;
    footer->addWidget(elapsed_);
    footer->addWidget(cancel_);
    footer->addWidget(bake_);
    layout->addLayout(footer);
    validation_ = new QLabel("Load geometry and an opacity mask to begin.");
    validation_->setWordWrap(true);
    layout->addWidget(validation_);
    status_ = new QLabel("Ready");
    layout->addWidget(status_);
    connect(meshButton, &QPushButton::clicked, this, [this] {
        QSettings st;
        auto path = QFileDialog::getOpenFileName(this, "Load hair-card geometry",
                                                 st.value("meshDir").toString(), "Geometry (*.fbx *.obj)");
        if (!path.isEmpty())
            loadInputs(path, {});
    });
    connect(maskButton, &QPushButton::clicked, this, [this] {
        QSettings st;
        auto path = QFileDialog::getOpenFileName(this, "Load opacity mask", st.value("maskDir").toString(),
                                                 "Opacity mask (*.png *.tif *.tiff *.tga)");
        if (!path.isEmpty())
            loadInputs({}, path);
    });
    connect(loadOccluderButton, &QPushButton::clicked, this, [this] {
        auto path = QFileDialog::getOpenFileName(this, "Load opaque head / occluder",
                                                 QSettings().value("occluderDir").toString(),
                                                 "Geometry (*.fbx *.obj)");
        if (!path.isEmpty())
            loadOccluder(path);
    });
    connect(removeOccluder_, &QPushButton::clicked, this, [this] { loadOccluder({}); });
    connect(bake_, &QPushButton::clicked, this, &Window::bake);
    connect(generateUv2_, &QPushButton::clicked, this, [this] { generateUv2(); });
    connect(generateUv2As_, &QPushButton::clicked, this, [this] { generateUv2(true); });
    connect(uvRotate_, &QCheckBox::toggled, this, [this] { saveSettings(); });
    connect(uv2Name_, &QLineEdit::editingFinished, this, [this] { saveSettings(); });
    connect(uvSpacing_, &QSpinBox::valueChanged, this, [this] { saveSettings(); });
    connect(cancel_, &QPushButton::clicked, this, [this] {
        cancelFlag_ = true;
        cancel_->setEnabled(false);
        status_->setText("Cancelling…");
    });
    connect(export_, &QPushButton::clicked, this, &Window::exportPng);
    connect(exportTexture_, &QPushButton::clicked, this, &Window::exportPng);
    connect(mode_, &QComboBox::currentIndexChanged, this, [this] { updatePreview(); });
    connect(fit, &QPushButton::clicked, view_, &Viewport::fit);
    connect(fitImage, &QPushButton::clicked, this,
            [this] { imageView_->fitInView(imageScene_->sceneRect(), Qt::KeepAspectRatio); });
    connect(actual, &QPushButton::clicked, this, [this] { imageView_->resetTransform(); });
    connect(draft, &QPushButton::clicked, this, [this] {
        resolution_->setCurrentText("512");
        rays_->setValue(32);
        ss_->setCurrentIndex(0);
    });
    connect(quality, &QPushButton::clicked, this, [this] {
        resolution_->setCurrentText("2048");
        rays_->setValue(256);
        ss_->setCurrentIndex(1);
    });
    for (auto c : {maskUV_, aoUV_, channel_, resolution_, ss_})
        connect(c, &QComboBox::currentIndexChanged, this, [this] { refresh(); });
    for (auto c : {invert_, flip_, repeat_, averageOverlaps_})
        connect(c, &QCheckBox::toggled, this, [this] { refresh(); });
    for (auto c : {rays_, padding_})
        connect(c, &QSpinBox::valueChanged, this, [this] { refresh(); });
    for (auto c : {distance_, bias_})
        connect(c, &QDoubleSpinBox::valueChanged, this, [this] { refresh(); });
    auto tick = new QTimer(this);
    connect(tick, &QTimer::timeout, this, [this] {
        if (busy_)
            elapsed_->setText(QString("%1 s").arg(timer_.elapsed() / 1000));
    });
    tick->start(250);
    restoreSettings();
    refresh();
}
Window::~Window() {
    ai_->shutdown();
    transfer_->shutdown();
    cancelFlag_ = true;
    if (worker_.joinable())
        worker_.join();
    saveSettings();
}
Settings Window::settings() const {
    Settings s;
    s.resolution = resolution_->currentData().toInt();
    s.rays = rays_->value();
    s.ss = ss_->currentData().toInt();
    s.padding = padding_->value();
    s.bits = bits_->currentData().toInt();
    s.maskUV = maskUV_->currentIndex();
    s.aoUV = aoUV_->currentIndex();
    s.channel = channel_->currentIndex();
    s.invert = invert_->isChecked();
    s.flip = flip_->isChecked();
    s.repeat = repeat_->isChecked();
    s.averageOverlaps = averageOverlaps_->isChecked();
    s.distance = (float)distance_->value();
    s.bias = (float)bias_->value();
    return s;
}
void Window::showError(const QString &text) {
    QMessageBox::warning(this, "Fornaio", text);
}
bool Window::loadInputs(const QString &geometry, const QString &path) {
    try {
        loading_ = true;
        if (!geometry.isEmpty()) {
            auto loaded = std::make_shared<Scene>(loadScene(geometry));
            loaded->occluder = occluder_;
            scene_ = loaded;
            validatedScene_ = nullptr;
            meshPath_ = geometry;
            maskUV_->clear();
            aoUV_->clear();
            maskUV_->addItems(scene_->uvNames);
            aoUV_->addItems(scene_->uvNames);
            aoUV_->setCurrentIndex(scene_->uvNames.size() > 1 ? 1 : 0);
            QSettings saved;
            auto key = filePreferenceKey(geometry);
            int previousMask = saved.value(key + "/maskUV", 0).toInt();
            int previousAO = saved.value(key + "/aoUV", aoUV_->currentIndex()).toInt();
            if (previousMask >= 0 && previousMask < maskUV_->count())
                maskUV_->setCurrentIndex(previousMask);
            if (previousAO >= 0 && previousAO < aoUV_->count())
                aoUV_->setCurrentIndex(previousAO);
            geometryLabel_->setText(QString("%1\n%2 triangles · %3 UV sets\nBounds diagonal: %4 model units")
                                        .arg(QFileInfo(geometry).fileName())
                                        .arg(scene_->tris.size())
                                        .arg(scene_->uvNames.size())
                                        .arg(scene_->scale, 0, 'g', 5));
            QSettings().setValue("meshDir", QFileInfo(geometry).absolutePath());
        }
        if (!path.isEmpty()) {
            auto m = loadMask(path);
            originalMask_ = m.image;
            cachedChannel_ = -1;
            maskPath_ = path;
            channel_->setCurrentIndex(m.alpha ? 3 : 0);
            channel_->setCurrentIndex(std::clamp(
                QSettings().value(filePreferenceKey(path) + "/channel", channel_->currentIndex()).toInt(), 0,
                3));
            maskLabel_->setText(QString("%1\n%2 × %3").arg(QFileInfo(path).fileName()).arg(m.w).arg(m.h));
            QSettings().setValue("maskDir", QFileInfo(path).absolutePath());
        }
        loading_ = false;
        refresh();
        return true;
    } catch (const std::exception &e) {
        loading_ = false;
        refresh();
        showError(QString::fromUtf8(e.what()));
        return false;
    }
}
bool Window::loadOccluder(const QString &path) {
    if (busy_)
        return false;
    try {
        std::shared_ptr<Scene> loaded;
        if (!path.isEmpty()) {
            loaded = std::make_shared<Scene>(loadScene(path));
            auto errors = validateOccluder(*loaded);
            if (!errors.empty())
                throw std::runtime_error(errors.join('\n').toStdString());
        }
        // Copy the hair scene so a completed bake retains its original occluder snapshot.
        auto updated = scene_ ? std::make_shared<Scene>(*scene_) : nullptr;
        if (updated)
            updated->occluder = loaded;
        occluder_ = loaded;
        scene_ = updated;
        validatedScene_ = nullptr;
        removeOccluder_->setEnabled(bool(loaded));
        occluderLabel_->setText(loaded ? QString("%1\n%2 triangles · opaque occluder")
                                             .arg(QFileInfo(path).fileName())
                                             .arg(loaded->tris.size())
                                       : "Optional · opaque casting mesh · UVs not required");
        if (loaded)
            QSettings().setValue("occluderDir", QFileInfo(path).absolutePath());
        refresh();
        // Show the new head placement immediately. Baked AO can still show the old snapshot.
        mode_->setCurrentIndex(0);
        return true;
    } catch (const std::exception &e) {
        showError(QString::fromUtf8(e.what()));
        return false;
    }
}
void Window::updatePreview() {
    if (result_ && mode_->currentIndex() == 2) {
        view_->setScene(bakedScene_);
        if (bakedMask_)
            view_->setMask(*bakedMask_, bakedSettings_);
        view_->setUV(bakedSettings_.maskUV, bakedSettings_.aoUV, {});
    } else {
        view_->setScene(scene_);
        if (mask_)
            view_->setMask(*mask_, settings());
        view_->setUV(settings().maskUV, settings().aoUV,
                     validationCache_.ok() ? std::vector<int>{} : validationCache_.faces);
    }
    view_->setMode(mode_->currentIndex());
}
void Window::refresh() {
    if (loading_ || busy_)
        return;
    if (result_) {
        resultStale_ = true;
        status_->setText(
            "Settings changed. Preview and Export retain the last completed bake. Bake AO to update.");
    }
    auto s = settings();
    if (!originalMask_.isNull()) {
        if (cachedChannel_ != s.channel || cachedInvert_ != s.invert) {
            mask_ = std::make_shared<Mask>(maskFromImage(originalMask_, s.channel, s.invert));
            cachedChannel_ = s.channel;
            cachedInvert_ = s.invert;
        }
    }
    bool valid = false;
    if (scene_) {
        if (validatedScene_ != scene_.get() || validatedMaskUV_ != s.maskUV || validatedAoUV_ != s.aoUV ||
            validatedAverage_ != s.averageOverlaps) {
            validationCache_ = validate(*scene_, s);
            validatedScene_ = scene_.get();
            validatedMaskUV_ = s.maskUV;
            validatedAoUV_ = s.aoUV;
            validatedAverage_ = s.averageOverlaps;
        }
        const auto &v = validationCache_;
        valid = v.ok();
        validation_->setText(valid ? "UVs valid · AO uses the selected output UV set. Preview opacity is "
                                     "dithered; baking uses graded transmission."
                                   : v.errors.join('\n'));
        validation_->setStyleSheet(valid ? "color:#89d5ca;" : "color:#ff858c;");
        if (valid && !v.warnings.empty()) {
            validation_->setText(v.warnings.join('\n'));
            validation_->setStyleSheet("color:#f0c477;");
        }
    }
    bake_->setEnabled(valid && !originalMask_.isNull() && !transferRunning_ && !aiRunning_);
    bake_->setToolTip(transferRunning_ || aiRunning_ ? "Waiting for the other GPU job to finish." : QString());
    const bool fbx = scene_ && meshPath_.endsWith(".fbx", Qt::CaseInsensitive);
    const QString target = fbx ? uv2Target() : QString();
    generateUv2_->setEnabled(fbx);
    generateUv2As_->setEnabled(fbx);
    generateUv2_->setText(target.isEmpty() ? "Generate UV2…" : "Generate UV2");
    generateUv2_->setToolTip(!fbx ? "Load an FBX whose UV1 holds the hair-card UVs."
                             : target.isEmpty() ? "Save a copy of the FBX with a packed UV2 and load it."
                                                : "Regenerate UV2 into " + QDir::toNativeSeparators(target));
    uv2Target_->setText("Writes to " + QFileInfo(target).fileName());
    uv2Target_->setToolTip(QDir::toNativeSeparators(target));
    uv2Target_->setVisible(!target.isEmpty());
    updatePreview();
    updateUvLayout();
    saveSettings();
}
void Window::bake() {
    if (!scene_ || !mask_ || busy_ || transferRunning_ || aiRunning_)
        return;
    if (worker_.joinable())
        worker_.join();
    saveSettings();
    progress_->setValue(0);
    busy_ = true;
    transfer_->setExternalBusy(true);
    ai_->setExternalBusy(true);
    cancelFlag_ = false;
    inputs_->setEnabled(false);
    bake_->setEnabled(false);
    cancel_->setEnabled(true);
    timer_.start();
    auto s = settings();
    auto scene = scene_;
    auto mask = mask_;
    worker_ = std::thread([this, s, scene, mask = std::move(mask)] {
        try {
            auto r = std::make_shared<Result>(
                bakeGpu(*scene, *mask, s, cancelFlag_, [this](float p, const QString &text) {
                    QMetaObject::invokeMethod(
                        this,
                        [this, p, text] {
                            progress_->setValue((int)(p * 1000));
                            status_->setText(text);
                        },
                        Qt::QueuedConnection);
                }));
            QMetaObject::invokeMethod(this, [this, r] { finish(r, {}); }, Qt::QueuedConnection);
        } catch (const std::exception &e) {
            QString error = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(this, [this, error] { finish({}, error); }, Qt::QueuedConnection);
        }
    });
}
void Window::finish(std::shared_ptr<Result> r, const QString &error) {
    if (worker_.joinable())
        worker_.join();
    busy_ = false;
    transfer_->setExternalBusy(aiRunning_);
    ai_->setExternalBusy(transferRunning_);
    inputs_->setEnabled(true);
    cancel_->setEnabled(false);
    bake_->setEnabled(true);
    if (!r) {
        status_->setText(error + (result_ ? " Last completed bake is still available to export." : ""));
        progress_->setValue(0);
        if (!smokeOutput_.isEmpty()) {
            QCoreApplication::exit(2);
            return;
        }
        if (!cancelFlag_)
            showError(error);
        return;
    }
    result_ = r;
    resultStale_ = false;
    bakedScene_ = scene_;
    bakedMask_ = mask_;
    bakedSettings_ = settings();
    bakedMeshPath_ = meshPath_;
    auto image = resultImage(*r);
    imageScene_->clear();
    imageScene_->addPixmap(QPixmap::fromImage(image));
    imageScene_->setSceneRect(image.rect());
    imageView_->fitInView(image.rect(), Qt::KeepAspectRatio);
    view_->setResult(image);
    mode_->setCurrentIndex(2);
    updatePreview();
    export_->setEnabled(true);
    exportTexture_->setEnabled(true);
    elapsed_->setText(QString::number(r->seconds, 'f', 1) + " s");
    status_->setText(QString("Complete · %1 · %2 × %2 · %3 MiB explicit GPU buffers")
                         .arg(r->device)
                         .arg(r->resolution)
                         .arg(r->gpuBytes / 1048576));
    if (!smokeOutput_.isEmpty()) {
        try {
            savePng(*r, smokeOutput_, 16);
        } catch (...) {
            QCoreApplication::exit(3);
            return;
        }
        QTimer::singleShot(500, this, [this] {
            if (!smokeCapture_.isEmpty())
                smokeCapture(smokeCapture_);
            QCoreApplication::quit();
        });
    }
}
// QFileInfo::operator== treats any two missing files as equal, so compare absolute paths instead.
static bool samePath(const QString &a, const QString &b) {
    return !a.isEmpty() && !b.isEmpty() &&
           QDir::cleanPath(QFileInfo(a).absoluteFilePath())
                   .compare(QDir::cleanPath(QFileInfo(b).absoluteFilePath()), Qt::CaseInsensitive) == 0;
}
QString Window::uv2Target() const {
    // The remembered target applies while either end of the last generation is loaded.
    return samePath(meshPath_, uv2Output_) || samePath(meshPath_, uv2Source_) ? uv2Output_ : QString();
}
void Window::generateUv2(bool chooseFile) {
    if (!scene_ || busy_ || transferRunning_ || aiRunning_ || !meshPath_.endsWith(".fbx", Qt::CaseInsensitive))
        return;
    QString output = chooseFile ? QString() : uv2Target();
    // Repack from the original file rather than a previously generated copy when it still exists.
    const bool generated = samePath(meshPath_, uv2Output_);
    const QString input = generated && QFileInfo::exists(uv2Source_) ? uv2Source_ : meshPath_;
    if (output.isEmpty()) {
        const QFileInfo source(input);
        const QString suggested = uv2Target().isEmpty()
                                      ? source.absolutePath() + "/" + source.completeBaseName() + "_UV2.fbx"
                                      : uv2Target();
        output = QFileDialog::getSaveFileName(this, "Save FBX with generated UV2", suggested, "FBX (*.fbx)");
        // The save dialog runs a nested event loop; re-check that nothing started meanwhile.
        if (output.isEmpty() || busy_ || transferRunning_ || aiRunning_)
            return;
        if (!output.endsWith(".fbx", Qt::CaseInsensitive))
            output += ".fbx";
    }
    PackSettings pack;
    pack.resolution = resolution_->currentData().toInt();
    pack.spacing = uvSpacing_->value();
    pack.allowRotation = uvRotate_->isChecked();
    const QString name = uv2Name_->text().trimmed();
    if (QString error = name.isEmpty() ? QString() : uvSetNameError(name); !error.isEmpty()) {
        showError(error);
        return;
    }
    if (worker_.joinable())
        worker_.join();
    saveSettings();
    progress_->setValue(0);
    busy_ = true;
    transfer_->setExternalBusy(true);
    ai_->setExternalBusy(true);
    cancelFlag_ = false;
    inputs_->setEnabled(false);
    bake_->setEnabled(false);
    cancel_->setEnabled(true);
    status_->setText("Generating UV2…");
    timer_.start();
    worker_ = std::thread([this, input, output, pack, name] {
        try {
            auto report = ::generateUv2(input, output, pack, name, cancelFlag_, [this](float p, const QString &text) {
                QMetaObject::invokeMethod(
                    this,
                    [this, p, text] {
                        progress_->setValue((int)(p * 1000));
                        status_->setText(text);
                    },
                    Qt::QueuedConnection);
            });
            QMetaObject::invokeMethod(
                this,
                [this, report, input, output] {
                    uv2Source_ = input;
                    uv2Output_ = output;
                    finishUv2(report, output, {});
                },
                Qt::QueuedConnection);
        } catch (const std::exception &e) {
            QString error = QString::fromUtf8(e.what());
            QMetaObject::invokeMethod(this, [this, output, error] { finishUv2({}, output, error); },
                                      Qt::QueuedConnection);
        }
    });
}
void Window::finishUv2(const Uv2Report &report, const QString &output, const QString &error) {
    if (worker_.joinable())
        worker_.join();
    busy_ = false;
    transfer_->setExternalBusy(aiRunning_);
    ai_->setExternalBusy(transferRunning_);
    inputs_->setEnabled(true);
    cancel_->setEnabled(false);
    elapsed_->clear();
    if (!error.isEmpty()) {
        progress_->setValue(0);
        status_->setText(error + (result_ ? " Last completed bake is still available to export." : ""));
        bake_->setEnabled(validationCache_.ok() && scene_ && !originalMask_.isNull() && !transferRunning_ && !aiRunning_);
        if (!cancelFlag_)
            showError(error);
        return;
    }
    progress_->setValue(1000);
    if (!loadInputs(output, {}))
        return;
    // The packer writes UV2 as the second UV set; bake AO into it, keep the mask on UV1.
    if (scene_->uvNames.size() > 1) {
        maskUV_->setCurrentIndex(0);
        aoUV_->setCurrentIndex(1);
    }
    previewTabs_->setCurrentWidget(uvLayoutTab_);
    status_->setText(QString("UV2 generated · %1 islands in %2 meshes · %3% of the tile used%4 · %5 s · saved %6")
                         .arg(report.islands)
                         .arg(report.meshes)
                         .arg(report.utilization * 100, 0, 'f', 1)
                         .arg(report.rotated ? QString(" · %1 rotated").arg(report.rotated) : QString())
                         .arg(report.seconds, 0, 'f', 2)
                         .arg(QDir::toNativeSeparators(output)));
    if (!report.warnings.empty())
        QMessageBox::information(this, "Fornaio", "UV2 generated with notes:\n\n" + report.warnings.join('\n'));
}
void Window::updateUvLayout() {
    if (previewTabs_->currentWidget() != uvLayoutTab_)
        return;
    const int set = aoUV_->currentIndex();
    if (!uvLayoutScene_.expired() && uvLayoutScene_.lock() == scene_ && uvLayoutSet_ == set)
        return;
    uvLayoutScene_ = scene_;
    uvLayoutSet_ = set;
    uvScene_->clear();
    if (!scene_ || set < 0 || set >= scene_->uvNames.size()) {
        uvLayoutLabel_->setText("Load geometry to see its AO UV layout.");
        return;
    }
    constexpr int size = 2048;
    QImage image(size, size, QImage::Format_RGB32);
    image.fill(QColor("#11151b"));
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor(75, 196, 175, 90), 0));
        painter.setBrush(QColor("#6f7c8a"));
        for (const auto &t : scene_->tris) {
            if (set >= (int)t.uv.size())
                continue;
            QPointF p[3];
            for (int k = 0; k < 3; ++k)
                p[k] = {t.uv[set][k].x * size, (1 - t.uv[set][k].y) * size};
            painter.drawPolygon(p, 3);
        }
        painter.setPen(QPen(QColor("#4bc4af"), 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(1, 1, size - 2, size - 2));
    }
    uvScene_->addPixmap(QPixmap::fromImage(image));
    uvScene_->setSceneRect(image.rect());
    uvView_->fitInView(image.rect(), Qt::KeepAspectRatio);
    uvLayoutLabel_->setText(QString("%1 · 0–1 tile · scroll to zoom").arg(scene_->uvNames[set]));
}
void Window::exportPng() {
    // Native dialogs run a nested event loop. Hold the selected result independently
    // so a later bake completion can never replace it halfway through exporting.
    const auto result = result_;
    const int bits = bits_->currentData().toInt();
    if (!result)
        return;
    auto path = QFileDialog::getSaveFileName(this, "Export linear AO PNG",
                                             QFileInfo(bakedMeshPath_).absolutePath() + "/" +
                                                 QFileInfo(bakedMeshPath_).completeBaseName() + "_AO.png",
                                             "PNG (*.png)");
    if (path.isEmpty())
        return;
    if (!path.endsWith(".png", Qt::CaseInsensitive))
        path += ".png";
    try {
        savePng(*result, path, bits);
        status_->setText("Saved " + path);
    } catch (const std::exception &e) {
        showError(e.what());
    }
}
void Window::saveSettings() {
    if (loading_)
        return;
    QSettings q;
    auto s = settings();
    q.setValue("resolution", s.resolution);
    q.setValue("rays", s.rays);
    q.setValue("ss", s.ss);
    q.setValue("padding", s.padding);
    q.setValue("bits", s.bits);
    q.setValue("distance", s.distance);
    q.setValue("bias", s.bias);
    q.setValue("invert", s.invert);
    q.setValue("flip", s.flip);
    q.setValue("repeat", s.repeat);
    q.setValue("averageOverlaps", s.averageOverlaps);
    q.setValue("uv2/spacing", uvSpacing_->value());
    q.setValue("uv2/rotate", uvRotate_->isChecked());
    q.setValue("uv2/name", uv2Name_->text());
    if (!meshPath_.isEmpty() && s.maskUV >= 0 && s.aoUV >= 0) {
        auto key = filePreferenceKey(meshPath_);
        q.setValue(key + "/maskUV", s.maskUV);
        q.setValue(key + "/aoUV", s.aoUV);
    }
    if (!maskPath_.isEmpty())
        q.setValue(filePreferenceKey(maskPath_) + "/channel", s.channel);
}
void Window::restoreSettings() {
    loading_ = true;
    QSettings q;
    resolution_->setCurrentText(q.value("resolution", 2048).toString());
    rays_->setValue(q.value("rays", 256).toInt());
    ss_->setCurrentIndex(std::clamp(q.value("ss", 2).toInt() - 1, 0, 3));
    padding_->setValue(q.value("padding", 8).toInt());
    bits_->setCurrentIndex(q.value("bits", 16).toInt() == 16 ? 0 : 1);
    distance_->setValue(q.value("distance", 0).toDouble());
    bias_->setValue(q.value("bias", 0).toDouble());
    invert_->setChecked(q.value("invert", false).toBool());
    flip_->setChecked(q.value("flip", false).toBool());
    repeat_->setChecked(q.value("repeat", false).toBool());
    averageOverlaps_->setChecked(q.value("averageOverlaps", true).toBool());
    uvSpacing_->setValue(q.value("uv2/spacing", 4).toInt());
    uvRotate_->setChecked(q.value("uv2/rotate", false).toBool());
    if (const QString name = q.value("uv2/name", "UV2").toString(); uvSetNameError(name).isEmpty())
        uv2Name_->setText(name);
    loading_ = false;
}
void Window::closeEvent(QCloseEvent *e) {
    ai_->shutdown();
    transfer_->shutdown();
    cancelFlag_ = true;
    if (worker_.joinable())
        worker_.join();
    saveSettings();
    e->accept();
}
void Window::smokeCapture(const QString &path) {
    grab().save(path);
}
void Window::smokeBake(const QString &output, const QString &capture) {
    smokeOutput_ = output;
    smokeCapture_ = capture;
    bake();
}
