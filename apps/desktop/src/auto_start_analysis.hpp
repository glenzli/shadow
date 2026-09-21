#pragma once
#include "auto_start_policy.hpp"
#include "desktop_backend.hpp"
#include <atomic>
#include <functional>

// Worker-owned input and output. No QObject, catalog publication or UI state.
struct AutoStartCancellation final {
    std::atomic_bool cancelled = false;
    std::atomic_uint64_t job = 0, render = 0;
    void cancel(const DesktopBackend& backend);
};
struct AutoStartProposal final {
    QByteArray original;
    AutoStartTone tone;
    BackendRawWhiteBalancePickerResult whiteBalance;
    QVector<BackendGradeNode> skinNodes;
    QVector<BackendAutoStartMask> masks;
    QString scene, model, error;
    bool sceneChecked = false, skinChecked = false;
};
struct AutoStartAnalysisInput final {
    QString photo, source, base;
    BackendGradeStack before;
    BackendGradeNode toneNode;
    std::uint64_t generation = 0;
    bool sceneAllowed = false, skinAllowed = false;
};
AutoStartProposal analyzeAutoStart(
    const std::shared_ptr<DesktopBackend>& backend,
    const AutoStartAnalysisInput& input,
    const std::shared_ptr<AutoStartCancellation>& cancellation,
    const std::function<void(const AutoStartProposal&)>& fastReady
);
BackendGradeStack autoStartStack(
    const BackendGradeStack& before,
    const BackendGradeNode& toneNode,
    const AutoStartProposal& proposal,
    double strength,
    bool whiteBalance,
    bool tone,
    bool skin
);
