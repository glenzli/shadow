#include "session.hpp"
#include "../ai_preferences.hpp"
#include "../edit_controller.hpp"
#include "../pipeline_run_controller.hpp"
#include <QCoreApplication>
#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <stdexcept>
#ifdef SHADOW_DESKTOP_UI_SMOKE
#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#endif

namespace AiRuntimeSmoke {
bool install(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor,
    AiPreferences& preferences
) {
    const auto mode = qEnvironmentVariable("SHADOW_PIPELINE_SMOKE_ACTION");
    if (mode == QStringLiteral("ai-subject") || mode == QStringLiteral("ai-subject-unavailable")
        || mode == QStringLiteral("ai-semantic") || mode == QStringLiteral("ai-face")) {
        preferences.setSubjectMaskExecutionAllowed(true);
        installSubjectMask(engine, pipeline, editor, mode);
    } else if (mode == QStringLiteral("ai-completion")) {
        preferences.setImageCompletionExecutionAllowed(true);
        installCompletion(engine, pipeline, editor);
    } else if (mode == QStringLiteral("ai-raw-foundation")) {
        preferences.setRawDenoiseExecutionAllowed(true);
        installRawFoundation(engine, pipeline, editor);
    } else {
        return false;
    }
    return true;
}

Session::Session(
    QQmlApplicationEngine& engine,
    PipelineRunController& pipeline,
    EditController& editor,
    QString mode
) :
    QObject(&pipeline), engine_(engine), pipeline_(pipeline), editor_(editor),
    mode_(std::move(mode)) {
    lifetime_.start();
    timer_.setInterval(50);
    connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
    connect(&pipeline_, &PipelineRunController::terminationRequested, this, [this](int code) {
        timer_.stop();
        if (exporting_ && code == 0 && pipeline_.completedCount() == pipeline_.photoCount()
            && pipeline_.errorText().isEmpty()) {
            checkpoint("export passed");
        } else {
            qCritical().noquote() << "AI editor" << mode_ << "export/receipt failed" << code;
        }
    });
    timer_.start();
}

void Session::require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

qint64 Session::elapsed() const {
    return lifetime_.elapsed();
}

void Session::checkpoint(const char* phase) {
    qInfo().noquote() << "AI editor" << mode_ << phase << elapsed() << "ms";
}

void Session::click(const char* name, const QPointF relative) {
#ifdef SHADOW_DESKTOP_UI_SMOKE
    // Candidate visibility changes the dock's layout. Observe a presented
    // window before mapping the hit point, without reentering this state machine.
    timer_.stop();
    QTest::qWait(80);
    const auto items =
        engine_.rootObjects().front()->findChildren<QQuickItem*>(QString::fromLatin1(name));
    for (auto* item : items) {
        auto* window = item->window();
        if (!item->isVisible() || !item->isEnabled() || item->width() < 1 || item->height() < 1
            || !window || !window->isExposed())
            continue;
        const auto point =
            item->mapToScene({item->width() * relative.x(), item->height() * relative.y()})
                .toPoint();
        require(
            window->geometry().size().isValid() && QRect(QPoint{}, window->size()).contains(point),
            "AI pointer control is outside the real window"
        );
        if (item->metaObject()->indexOfSignal("clicked()") >= 0) {
            QSignalSpy spy(item, SIGNAL(clicked()));
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
            require(!spy.isEmpty(), "real pointer did not activate the AI control");
        } else {
            QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
        }
        timer_.start();
        return;
    }
    throw std::runtime_error(std::string("AI control is not available: ") + name);
#else
    Q_UNUSED(name);
    Q_UNUSED(relative);
    throw std::runtime_error("real AI editor acceptance requires the QtTest build");
#endif
}

void Session::tick() {
    try {
        require(elapsed() < 1'800'000, "real AI editor acceptance timed out");
        observe();
        if (pipeline_.finished()) {
            require(
                exporting_ && pipeline_.completedCount() == pipeline_.photoCount()
                    && pipeline_.errorText().isEmpty(),
                "AI edit export was not completed"
            );
            timer_.stop();
            pipeline_.cancel();
            return;
        }
        require(!editor_.autosaveFailed(), "AI edit autosave failed");
        auto* workspace =
            engine_.rootObjects().front()->findChild<QObject*>("pipelinePrecisionWorkspace");
        if (elapsed() - last_wait_report_ >= 5'000) {
            last_wait_report_ = elapsed();
            qInfo().noquote() << "AI editor" << mode_ << "readiness"
                              << "pipeline" << pipeline_.busy() << "editor" << editor_.busy()
                              << "active" << editor_.active() << "save" << editor_.autosavePending()
                              << "workspace" << (workspace != nullptr) << "frame"
                              << (workspace && workspace->property("previewFrameReady").toBool())
                              << "mask" << editor_.aiMaskPromptActive() << editor_.aiMaskBusy()
                              << elapsed() << "ms";
        }
        // Pipeline busy also projects the expected photo-navigation lock while
        // a transient tool is open. Its own points/Apply/Cancel remain usable.
        const bool authoring = editor_.aiMaskPromptActive() || editor_.imageCompletionActive();
        if ((pipeline_.busy() && !authoring) || editor_.busy() || editor_.autosavePending()
            || !editor_.active() || !workspace
            || !workspace->property("previewFrameReady").toBool())
            return;
        if (history_stage_)
            advanceHistory();
        else if (!exporting_)
            advance();
    } catch (const std::exception& error) {
        timer_.stop();
        qCritical().noquote() << "AI editor" << mode_ << "failed:" << error.what()
                              << editor_.statusText() << editor_.foundationAiDenoiseStatusText()
                              << pipeline_.errorText();
        QCoreApplication::exit(3);
    }
}

void Session::slide(const char* name, double position) {
#ifdef SHADOW_DESKTOP_UI_SMOKE
    timer_.stop();
    QTest::qWait(80);
    auto* field = engine_.rootObjects().front()->findChild<QQuickItem*>(QString::fromLatin1(name));
    require(field && field->isVisible() && field->isEnabled(), "AI slider is not available");
    for (auto* slider : field->findChildren<QQuickItem*>()) {
        if (!slider->property("pressed").isValid() || !slider->property("from").isValid())
            continue;
        auto* window = slider->window();
        require(window && window->isExposed(), "AI slider has no exposed window");
        const auto point =
            slider->mapToScene({slider->width() * position, slider->height() / 2}).toPoint();
        require(
            QRect(QPoint{}, window->size()).contains(point),
            "AI slider is outside the real window"
        );
        QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, point);
        timer_.start();
        return;
    }
    require(false, "AI slider has no pointer input control");
#else
    Q_UNUSED(name);
    Q_UNUSED(position);
    require(false, "real AI slider acceptance requires the QtTest build");
#endif
}

void Session::acceptEdit(const BackendGradeStack& before) {
    before_ = before;
    after_ = editor_.gradeStackForInterchange();
    require(before_ != after_ && editor_.canUndo(), "AI apply did not publish an undoable edit");
    click("pipelineUndoButton");
    history_stage_ = 1;
}

void Session::advanceHistory() {
    const auto report = [](const char* identity, const BackendGradeStack& stack) {
        const auto& foundation = stack.foundation;
        qInfo() << "AI editor history" << identity << "foundation" << foundation.enabled
                << "white balance" << static_cast<int>(foundation.raw_white_balance_mode)
                << foundation.temperature_kelvin << foundation.tint << "camera"
                << foundation.as_shot_white_balance_available
                << foundation.as_shot_temperature_kelvin << foundation.as_shot_tint;
        const auto& denoise = stack.raw_ai_denoise;
        qInfo() << "AI editor history" << identity << "RAW denoise" << denoise.present
                << denoise.enabled << denoise.bypassed << static_cast<int>(denoise.model)
                << static_cast<int>(denoise.amount_percent) << "nodes" << stack.grade_nodes.size();
    };
    if (history_stage_ == 1) {
        if (editor_.gradeStackForInterchange() != before_) {
            report("before", before_);
            report("undo", editor_.gradeStackForInterchange());
        }
        require(
            editor_.gradeStackForInterchange() == before_,
            "one undo did not restore the exact pre-AI edit"
        );
        require(editor_.canRedo(), "AI edit cannot be redone");
        click("pipelineRedoButton");
        history_stage_ = 2;
        return;
    }
    if (editor_.gradeStackForInterchange() != after_) {
        report("accepted", after_);
        report("redo", editor_.gradeStackForInterchange());
    }
    require(
        editor_.gradeStackForInterchange() == after_,
        "redo did not restore the exact accepted AI edit"
    );
    history_stage_ = 0;
    qInfo().noquote() << "AI editor" << mode_ << "apply/undo/redo passed";
    historyVerified();
}

void Session::historyVerified() {
    exportNow();
}

void Session::exportNow() {
    exporting_ = true;
    pipeline_.complete();
}
} // namespace AiRuntimeSmoke
