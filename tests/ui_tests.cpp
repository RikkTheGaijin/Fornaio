#include "window.h"
#include <QApplication>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QGraphicsPixmapItem>
#include <QLineEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSurfaceFormat>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QWheelEvent>

class UiTests : public QObject {
    Q_OBJECT
    static std::shared_ptr<Result> makeResult(float value = .25f) {
        auto r = std::make_shared<Result>();
        r->resolution = 256;
        r->pixels.assign(256 * 256, value);
        r->owners.assign(256 * 256, 0);
        r->device = "UI regression fixture";
        return r;
    }
    static void setup(Window &w) {
        w.loading_ = true;
        w.scene_ = std::make_shared<Scene>(testCards());
        w.meshPath_ = "fixture.fbx";
        w.maskUV_->addItems(w.scene_->uvNames);
        w.aoUV_->addItems(w.scene_->uvNames);
        w.aoUV_->setCurrentIndex(1);
        w.originalMask_ = QImage(16, 16, QImage::Format_RGB32);
        w.originalMask_.fill(Qt::white);
        w.loading_ = false;
        w.refresh();
        w.finish(makeResult(), {});
        w.show();
    }
  private slots:
    void init() { QSettings().clear(); }
    void wheelScrollDoesNotChangeSettingsOrDiscardBake() {
        Window w;
        setup(w);
        QVERIFY(QTest::qWaitForWindowExposed(&w));
        const auto original = w.result_;
        auto scroll = w.findChild<QScrollArea *>("aoControls");
        QVERIFY(scroll);
        // Use a short window so the real scroll area has overflow.
        w.resize(1000, 720);
        w.inputs_->setMinimumHeight(1800);
        QTest::qWait(50);
        QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
        QList<QWidget *> controls{w.maskUV_, w.aoUV_,  w.channel_,  w.resolution_, w.ss_,       w.bits_,
                                  w.rays_,   w.padding_, w.distance_, w.bias_,      w.uvSpacing_};
        for (auto control : controls) {
            scroll->ensureWidgetVisible(control);
            control->setFocus();
            auto combo = qobject_cast<QComboBox *>(control);
            auto spin = qobject_cast<QAbstractSpinBox *>(control);
            QString previous = combo ? combo->currentText() : spin->text();
            for (int i = 0; i < 12; ++i) {
                QPoint center = control->rect().center();
                QWheelEvent wheel(center, control->mapToGlobal(center), {}, {0, -120}, Qt::NoButton,
                                  Qt::NoModifier, Qt::NoScrollPhase, false);
                QApplication::sendEvent(control, &wheel);
            }
            QCOMPARE(combo ? combo->currentText() : spin->text(), previous);
            QCOMPARE(w.result_.get(), original.get());
            QVERIFY(w.export_->isEnabled());
            QCOMPARE(w.imageScene_->items().size(), 1);
        }
        scroll->verticalScrollBar()->setValue(0);
        QPoint p = w.aoUV_->rect().center();
        QWheelEvent wheel(p, w.aoUV_->mapToGlobal(p), {}, {0, -120}, Qt::NoButton, Qt::NoModifier,
                          Qt::NoScrollPhase, false);
        QApplication::sendEvent(w.aoUV_, &wheel);
        QVERIFY(scroll->verticalScrollBar()->value() > 0);
        QVERIFY(!w.resultStale_);
    }
    void explicitChangesAndCancellationKeepBakeAndItsPreviewMapping() {
        Window w;
        setup(w);
        const auto original = w.result_;
        const auto bakedMask = w.bakedMask_;
        w.rays_->setValue(w.rays_->value() + 1);
        w.aoUV_->setCurrentIndex(0);
        w.invert_->setChecked(true);
        QVERIFY(w.resultStale_);
        QCOMPARE(w.result_.get(), original.get());
        QCOMPARE(w.view_->aoUV_, 1);
        QCOMPARE(w.view_->maskInverted_, false);
        w.mode_->setCurrentIndex(0);
        QCOMPARE(w.view_->aoUV_, 0);
        QCOMPARE(w.view_->maskInverted_, true);
        w.mode_->setCurrentIndex(2);
        QCOMPARE(w.view_->aoUV_, 1);
        QCOMPARE(w.view_->maskInverted_, false);
        QCOMPARE(w.bakedMask_.get(), bakedMask.get());
        QVERIFY(!w.view_->ao_.isNull());
        w.cancelFlag_ = true;
        w.finish({}, "Bake cancelled.");
        QCOMPARE(w.result_.get(), original.get());
        QVERIFY(w.export_->isEnabled());
        QVERIFY(w.exportTexture_->isEnabled());
        QVERIFY(w.status_->text().contains("still available"));
    }
    void exportingHoldsResultAcrossNativeDialogEventLoop() {
        Window w;
        setup(w);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString output = temp.filePath("preserved.png");
        bool dialogHandled = false;
        QTimer::singleShot(50, &w, [&] {
            auto dialog = w.findChild<QFileDialog *>();
            if (!dialog)
                return;
            // A different bake completes while the export dialog is open.
            w.finish(makeResult(.75f), {});
            dialog->selectFile(output);
            // selectFile() leaves the name field alone once it has focus, which is timing-dependent.
            auto name = dialog->findChild<QLineEdit *>("fileNameEdit");
            if (!name)
                return;
            name->setText(output);
            dialogHandled = QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
        });
        QTimer::singleShot(2000, &w, [&] {
            if (auto dialog = w.findChild<QFileDialog *>())
                dialog->reject();
        });
        w.exportPng();
        QVERIFY(dialogHandled);
        QVERIFY2(QFileInfo::exists(output), qPrintable(w.status_->text()));
        QImage image(output);
        QVERIFY(!image.isNull());
        QCOMPARE(image.pixelColor(100, 100).red(), 64);
        QCOMPARE(w.result_->pixels.front(), .75f);
    }
    void denseOverlapsAreBoundedButHardErrorsStillChecked() {
        Scene scene = testCards(1);
        const auto t = scene.tris.front();
        scene.tris.assign(20000, t);
        Settings s;
        s.aoUV = 1;
        QElapsedTimer timer;
        timer.start();
        auto valid = validate(scene, s);
        QVERIFY(valid.ok());
        QVERIFY(valid.overlapScanLimited);
        QCOMPARE(valid.overlaps.size(), size_t(64));
        QVERIFY2(timer.elapsed() < 3000, "Dense UV overlap validation stalled the UI");
        scene.tris.back().uv[1][0].x = -1;
        QVERIFY(!validate(scene, s).ok());
        scene.tris.back() = t;
        s.averageOverlaps = false;
        QVERIFY(!validate(scene, s).ok());
    }
    void occluderLoadRemoveAndBakeSnapshot() {
        Window w;
        setup(w);
        QTemporaryDir temp;
        QFile file(temp.filePath("head.obj"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("v -5 -5 .25\nv 5 -5 .25\nv 5 5 .25\nv -5 5 .25\nf 1 2 3 4\n");
        file.close();
        auto original = w.result_;
        QVERIFY(w.loadOccluder(file.fileName()));
        QVERIFY(w.scene_->occluder);
        QVERIFY(w.scene_->occluder->uvNames.empty());
        QVERIFY(w.removeOccluder_->isEnabled());
        QCOMPARE(w.result_.get(), original.get());
        QVERIFY(!w.bakedScene_->occluder);
        QCOMPARE(w.mode_->currentIndex(), 0);
        QCOMPARE(w.view_->scene_->occluder.get(), w.occluder_.get());
        w.finish(makeResult(.1f), {});
        const auto bakedHead = w.bakedScene_->occluder;
        QVERIFY(bakedHead);
        QVERIFY(w.loadOccluder({}));
        QVERIFY(!w.scene_->occluder);
        QVERIFY(!w.removeOccluder_->isEnabled());
        QCOMPARE(w.bakedScene_->occluder.get(), bakedHead.get());
        w.mode_->setCurrentIndex(2);
        QCOMPARE(w.view_->scene_->occluder.get(), bakedHead.get());
        QVERIFY(w.export_->isEnabled());
    }
    void uv2LayoutControlsAndPreview() {
        Window w;
        QVERIFY(!w.uvRotate_->isChecked());
        QCOMPARE(w.uvSpacing_->value(), 4);
        QCOMPARE(w.uv2Name_->text(), QString("UV2"));
        int pos = 0;
        QString bad = "2 uv";
        QVERIFY(w.uv2Name_->validator()->validate(bad, pos) == QValidator::Invalid);
        QVERIFY(!w.generateUv2_->isEnabled());
        setup(w);
        QVERIFY(w.generateUv2_->isEnabled());
        // After one save, regenerating writes to the same file while either end of it is loaded.
        QVERIFY(w.uv2Target().isEmpty());
        QVERIFY(w.uv2Target_->isHidden());
        w.uv2Source_ = "fixture.fbx";
        w.uv2Output_ = "fixture_UV2.fbx";
        w.refresh();
        QCOMPARE(w.uv2Target(), QString("fixture_UV2.fbx"));
        QVERIFY(!w.uv2Target_->isHidden());
        QCOMPARE(w.generateUv2_->text(), QString("Generate UV2"));
        w.meshPath_ = "fixture_UV2.fbx";
        QCOMPARE(w.uv2Target(), QString("fixture_UV2.fbx"));
        w.meshPath_ = "other.fbx";
        w.refresh();
        QVERIFY(w.uv2Target().isEmpty());
        QVERIFY(w.uv2Target_->isHidden());
        w.meshPath_ = "fixture.fbx";
        w.refresh();
        // The layout image is rendered only once its tab is shown, then follows the AO UV choice.
        QCOMPARE(w.uvScene_->items().size(), 0);
        w.previewTabs_->setCurrentWidget(w.uvLayoutTab_);
        QCOMPARE(w.uvScene_->items().size(), 1);
        QVERIFY(w.uvLayoutLabel_->text().startsWith(w.scene_->uvNames[1]));
        w.aoUV_->setCurrentIndex(0);
        QVERIFY(w.uvLayoutLabel_->text().startsWith(w.scene_->uvNames[0]));
        w.uvRotate_->setChecked(true);
        w.uvSpacing_->setValue(9);
        w.uv2Name_->setText("hairAO");
        w.saveSettings();
        QCOMPARE(QSettings().value("uv2/rotate").toBool(), true);
        QCOMPARE(QSettings().value("uv2/spacing").toInt(), 9);
        // A failed or cancelled generation unlocks the inputs and keeps the last bake.
        const auto original = w.result_;
        w.busy_ = true;
        w.inputs_->setEnabled(false);
        w.cancelFlag_ = true;
        w.finishUv2({}, "out.fbx", "UV2 generation cancelled.");
        QVERIFY(!w.busy_);
        QVERIFY(w.inputs_->isEnabled());
        QCOMPARE(w.result_.get(), original.get());
        QVERIFY(w.status_->text().startsWith("UV2 generation cancelled."));
        Window restored;
        QVERIFY(restored.uvRotate_->isChecked());
        QCOMPARE(restored.uvSpacing_->value(), 9);
        QCOMPARE(restored.uv2Name_->text(), QString("hairAO"));
    }
    void transferTabBuildsJobAndExcludesConcurrentGpuJobs() {
        Window w;
        QCOMPARE(w.mainTabs_->count(), 2);
        QCOMPARE(w.mainTabs_->tabText(0), QString("AO Baker"));
        QCOMPARE(w.mainTabs_->tabText(1), QString("Texture Transfer"));
        QCOMPARE(w.mainTabs_->currentIndex(), 0);
        QVERIFY(w.findChild<QScrollArea *>("aoControls"));
        QVERIFY(w.findChild<QScrollArea *>("transferControls"));
        TransferPanel *p = w.transfer_;
        QTemporaryDir temp;
        QFile mesh(temp.filePath("cards.obj"));
        QVERIFY(mesh.open(QIODevice::WriteOnly));
        mesh.write("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 1 1\nvt 0 1\n"
                   "vn 0 0 1\nf 1/1/1 2/2/1 3/3/1 4/4/1\n");
        mesh.close();
        QVERIFY(p->loadMesh(true, mesh.fileName()));
        QVERIFY(p->loadMesh(false, mesh.fileName()));
        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(Qt::red);
        QVERIFY(image.save(temp.filePath("body_ALB.png")));
        image.fill(QColor(128, 128, 255));
        QVERIFY(image.save(temp.filePath("body_NRM.png")));
        p->addTextures({temp.filePath("body_ALB.png"), temp.filePath("body_NRM.png"), temp.filePath("cards.obj")});
        QCOMPARE(p->sets_.size(), 1);
        QCOMPARE(p->sets_[0].files.size(), 2);
        p->outputDir_->setText(temp.filePath("out"));
        TransferJob job = p->job();
        QVERIFY(job.source && job.target);
        QCOMPARE(job.materialSets.value(job.source->materials[0]), QString("body"));
        QCOMPARE(job.outputs.size(), 1);
        QCOMPARE(job.outputs[0].prefix, QString("body"));
        QVERIFY2(checkJob(job).isEmpty(), qPrintable(checkJob(job).join('\n')));
        QVERIFY(p->transfer_->isEnabled());
        p->setExternalBusy(true);
        QVERIFY(!p->transfer_->isEnabled());
        p->setExternalBusy(false);
        QVERIFY(p->transfer_->isEnabled());
        emit p->runningChanged(true);
        QVERIFY(w.transferRunning_);
        QVERIFY(!w.bake_->isEnabled());
        emit p->runningChanged(false);
        QVERIFY(!w.transferRunning_);
        // A saved job restores the same UI state.
        TransferJob copy = TransferJob::fromJson(job.toJson(), temp.path());
        p->applyJob(copy);
        QCOMPARE(p->job().toJson(), job.toJson());
        // Two materials, two sets: each material's set stays selectable, also with a single output set.
        QFile two(temp.filePath("two.obj"));
        QVERIFY(two.open(QIODevice::WriteOnly));
        two.write("v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nv 2 0 0\nv 2 1 0\nvt 0 0\nvt .5 0\nvt .5 1\nvt 0 1\nvt 1 0\n"
                  "vt 1 1\nvn 0 0 1\nusemtl doll01_MAT\nf 1/1/1 2/2/1 3/3/1 4/4/1\nusemtl doll02_MAT\nf 2/2/1 5/5/1 6/6/1 3/3/1\n");
        two.close();
        image.fill(Qt::blue);
        QVERIFY(image.save(temp.filePath("doll01_ALB.png")));
        QVERIFY(image.save(temp.filePath("doll02_ALB.png")));
        p->addTextures({temp.filePath("doll01_ALB.png"), temp.filePath("doll02_ALB.png")});
        QVERIFY(p->loadMesh(true, two.fileName()));
        QVERIFY(p->loadMesh(false, two.fileName()));
        p->singleSet_->setChecked(true);
        QCOMPARE(p->materialTable_->rowCount(), 2);
        QCOMPARE(p->materialSets_.value("doll01_MAT"), QString("doll01"));
        QCOMPARE(p->materialSets_.value("doll02_MAT"), QString("doll02"));
        for (int r = 0; r < 2; r++) {
            auto combo = qobject_cast<QComboBox *>(p->materialTable_->cellWidget(r, 1));
            QVERIFY(combo && combo->isEnabled());
            QString want = r ? "doll01" : "doll02";
            combo->setCurrentIndex(combo->findText(want));
            QCoreApplication::processEvents();
            QCOMPARE(p->materialSets_.value(p->source_->materials[r]), want);
            combo = qobject_cast<QComboBox *>(p->materialTable_->cellWidget(r, 1));
            QVERIFY(combo);
            QCOMPARE(combo->currentText(), want);
        }
        TransferJob merged = p->job();
        QCOMPARE(merged.outputs.size(), 2);
        QCOMPARE(merged.outputs[0].prefix, merged.outputs[1].prefix);
    }
};

int main(int argc, char **argv) {
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setDepthBufferSize(24);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication app(argc, argv);
    app.setOrganizationName("Fornaio-Tests");
    app.setApplicationName("UI-regressions");
    QTemporaryDir prefs;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, prefs.path());
    UiTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "ui_tests.moc"
