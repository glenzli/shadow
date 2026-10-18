#include "document_smoke_harness.hpp"
#include "edit_controller.hpp"
#include "editable_document_history.hpp"
#include "pipeline_run_controller.hpp"

#include <QCoreApplication>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <cmath>
#include <memory>
#ifdef SHADOW_DESKTOP_UI_SMOKE
#include <QtTest/QTest>
#endif

void installDocumentSmokeHarness(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor
) {
    const auto mode = qEnvironmentVariable("SHADOW_DOCUMENT_SMOKE_MODE");
    auto stage = std::make_shared<int>(0), ticks = std::make_shared<int>(0);
    auto* timer = new QTimer(&pipeline);
    timer->setInterval(40);
    QObject::connect(timer, &QTimer::timeout, &pipeline, [&, timer, stage, ticks, mode] {
        const auto fail = [&](const char* message) {
            qCritical() << message << pipeline.errorText();
            timer->stop();
            QCoreApplication::exit(3);
        };
        if (++*ticks > 2250) {
            fail("Document smoke timed out");
            return;
        }
        if (mode == "conflict" && *stage == 1 && !pipeline.busy()
            && !pipeline.errorText().isEmpty()) {
            qInfo() << "Document conflicting save rejected";
            timer->stop();
            pipeline.cancel();
            return;
        }
        auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
        auto* workspace = window->findChild<QObject*>("pipelinePrecisionWorkspace");
        if (pipeline.busy() || editor.busy() || !editor.active() || !workspace
            || !workspace->property("previewFrameReady").toBool())
            return;
        if (*stage == 0 && mode == "create") {
            editor.beginParameterEdit("exposure");
            editor.setExposureStops(.25);
            editor.setExposureStops(.5);
            editor.endParameterEdit("exposure");
            editor.setExposureStops(.75);
            editor.undo();
            editor.setExposureStops(1.25);
            editor.undo();
            if (std::abs(editor.exposureStops() - .5) > 1e-6 || !editor.canUndo()
                || !editor.canRedo()) {
                fail("Document gesture/branch setup failed");
                return;
            }
            *stage = 2;
            return;
        }
        if (*stage == 0 && mode == "resume") {
            if (std::abs(editor.exposureStops() - .5) > 1e-6 || !editor.canUndo()
                || !editor.canRedo()) {
                fail("Document did not restore current state and undo/redo");
                return;
            }
            editor.redo();
            if (std::abs(editor.exposureStops() - 1.25) > 1e-6) {
                fail("Document redo did not restore exact state");
                return;
            }
            editor.undo();
            auto* history = qobject_cast<EditableDocumentHistory*>(
                window->property("documentHistory").value<QObject*>()
            );
            if (!history) {
                fail("Document timeline missing");
                return;
            }
            history->restore("2");
            *stage = 1;
            return;
        }
        if (*stage == 0 && mode == "roundtrip") {
            if (std::abs(editor.exposureStops() - .5) > 1e-6 || !editor.canUndo()
                || !editor.canRedo()) {
                fail("Document round trip lost editable state");
                return;
            }
            *stage = 2;
            return;
        }
        if (*stage == 0 && mode == "metadata") {
            if (pipeline.documentDirty()) {
                fail("Reopening a document incorrectly marked it modified");
                return;
            }
            editor.renameVariant(editor.activeVariantId(), "Portable variant");
            if (!pipeline.documentDirty()) {
                fail("Variant metadata changes were not marked for document saving");
                return;
            }
            *stage = 2;
            return;
        }
        if (*stage == 1 && mode == "resume") {
            if (std::abs(editor.exposureStops() - .75) > 1e-6) {
                fail("Abandoned document branch cannot be restored");
                return;
            }
            editor.setExposureStops(1.1);
            *stage = 2;
            return;
        }
        if (*stage == 0 && mode == "conflict") {
            qInfo() << "DOCUMENT_READY_FOR_CONFLICT";
            *stage = 1;
            QTimer::singleShot(1000, &pipeline, [&pipeline] { pipeline.saveCurrentDocument(); });
            return;
        }
        if (*stage == 2 && pipeline.documentProtocol()) {
#ifdef SHADOW_DESKTOP_UI_SMOKE
            auto* button = window->findChild<QQuickItem*>("externalEditReturnButton");
            if (!button || !button->isVisible() || !button->isEnabled()) {
                fail("Document return button is unreachable");
                return;
            }
            const QPoint center =
                button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint();
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, center);
            if (!pipeline.savingDocument()) {
                fail("Document return pointer did not begin saving");
                return;
            }
            qInfo() << "Document UI return admitted";
            *stage = 3;
#else
            fail("Document pointer acceptance requires a testing build");
#endif
        }
    });
    timer->start();
}
