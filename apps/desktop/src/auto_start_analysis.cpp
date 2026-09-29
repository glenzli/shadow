#include "auto_start_analysis.hpp"
#include <QCoreApplication>
#include <algorithm>
#include <cmath>

void AutoStartCancellation::cancel(const DesktopBackend& backend) {
    cancelled.store(true);
    if (auto token = job.load()) {
        try {
            backend.cancelSubjectMaskJob(token);
        } catch (...) {}
    }
    if (auto token = render.load())
        (void)backend.cancelEditPreviewRequest(token);
}

BackendGradeStack autoStartStack(
    const BackendGradeStack& before,
    const BackendGradeNode& toneNode,
    const AutoStartProposal& proposal,
    double strength,
    bool whiteBalance,
    bool tone,
    bool skin
) {
    auto stack = before;
    if (strength <= 0)
        return stack;
    if (whiteBalance && proposal.whiteBalance.available && !proposal.tone.preserveLight) {
        // Mired interpolation gives comparable colour movement across Kelvin values.
        const double current =
            std::clamp(double(before.foundation.temperature_kelvin), 2000., 25000.);
        const double goal =
            std::clamp(double(proposal.whiteBalance.temperature_kelvin), 2000., 25000.);
        const double shift = std::clamp(1e6 / goal - 1e6 / current, -20., 20.) * strength;
        auto& f = stack.foundation;
        f.raw_white_balance_mode = 1;
        f.temperature_kelvin =
            std::uint32_t(std::lround(std::clamp(1e6 / (1e6 / current + shift), 2000., 25000.)));
        f.tint = std::int16_t(
            std::clamp(
                int(before.foundation.tint)
                    + int(std::lround(
                        std::clamp(
                            int(proposal.whiteBalance.tint) - int(before.foundation.tint),
                            -6,
                            6
                        )
                        * strength
                    )),
                -150,
                150
            )
        );
        // Don't materialize an authored white point for an imperceptible/no-op change.
        if (f.temperature_kelvin == before.foundation.temperature_kelvin
            && f.tint == before.foundation.tint)
            f = before.foundation;
    }
    if (tone && proposal.tone.useful) {
        auto node = toneNode;
        node.basic = proposal.tone.basic;
        node.fine = proposal.tone.fine;
        node.opacity = strength;
        stack.grade_nodes.push_back(std::move(node));
    }
    if (skin)
        for (auto node : proposal.skinNodes) {
            node.opacity = strength;
            stack.grade_nodes.push_back(std::move(node));
        }
    return stack;
}

AutoStartProposal analyzeAutoStart(
    const std::shared_ptr<DesktopBackend>& backend,
    const AutoStartAnalysisInput& input,
    const std::shared_ptr<AutoStartCancellation>& control,
    const std::function<void(const AutoStartProposal&)>& fastReady
) {
    AutoStartProposal result;
    std::uint64_t session = 0;
    const auto cancelled = [&] { return control->cancelled.load(); };
    const auto render = [&](const BackendGradeStack& stack) {
        const auto token = backend->beginEditPreviewRequest();
        control->render.store(token);
        if (cancelled())
            (void)backend->cancelEditPreviewRequest(token);
        auto preview =
            backend
                ->renderAutoStartPreview(input.photo, input.source, input.base, stack, token, {});
        control->render.store(0);
        if (preview.terminal != EditPreviewTerminal::Completed)
            throw std::runtime_error("cancelled");
        return preview.bytes;
    };
    try {
        if (cancelled())
            return result;
        result.original = render(input.before);
        const auto image = QImage::fromData(result.original);
        if (image.isNull())
            throw std::runtime_error("Invalid analysis preview");
        result.tone = measureAutoStartTone(image);
        if (input.before.foundation.enabled) {
            try {
                result.whiteBalance = backend->autoRawWhiteBalance(
                    input.photo,
                    input.source,
                    input.base,
                    input.before,
                    1024
                );
            } catch (...) { /* Capability failure is expected for RGB or unsupported RAW. */
            }
        }
        if (cancelled())
            return result;
        fastReady(result);
        if (input.sceneAllowed || input.skinAllowed)
            session = backend->beginSubjectMaskInputSession();
        auto analysisStack = input.before;
        analysisStack.grade_nodes.push_back(input.toneNode);
        const auto run = [&](BackendSubjectMaskKind kind, std::uint32_t person = 0) {
            BackendSubjectMaskRequest request;
            request.input_session_token = session;
            request.job_token = backend->beginSubjectMaskJob();
            control->job.store(request.job_token);
            request.generation = input.generation;
            request.base_commit_id = input.base;
            request.grade_stack = analysisStack;
            request.target_grade_node_index = std::uint32_t(analysisStack.grade_nodes.size() - 1);
            request.target_grade_node_id = input.toneNode.grade_node_id;
            request.kind = kind;
            request.person_index = person;
            request.face_region_mask = 1U << 1;
            if (cancelled())
                backend->cancelSubjectMaskJob(request.job_token);
            auto output = backend->executeSubjectMaskJob(input.photo, input.source, request);
            control->job.store(0);
            return output;
        };
        if (input.sceneAllowed && !cancelled()) {
            try {
                const auto scene = run(BackendSubjectMaskKind::SubjectAnalysis);
                if (scene.terminal == BackendSubjectMaskTerminal::AnalysisReady) {
                    result.scene = scene.description;
                    result.model = scene.analysis_model;
                    result.sceneChecked = true;
                    result.tone = measureAutoStartTone(image, result.scene);
                }
            } catch (...) { /* Keep measured starting values when a local model is unavailable. */
            }
        }
        QImage graded = image;
        if (!cancelled()
            && autoStartStack(input.before, input.toneNode, result, 1, true, true, false)
                   != input.before) {
            const double baselineClipping = autoStartDisplayClippedFraction(image);
            for (int trial = 0; trial < 2 && !cancelled(); ++trial) {
                graded = QImage::fromData(render(
                    autoStartStack(input.before, input.toneNode, result, 1, true, true, false)
                ));
                if (autoStartDisplayClippedFraction(graded)
                    <= baselineClipping + std::max(.004, baselineClipping * .1))
                    break;
                result.whiteBalance.available = false;
                result.tone.basic.exposure_stops = std::min(0., result.tone.basic.exposure_stops);
                if (trial == 1) {
                    result.tone = {};
                    result.tone.preserveLight = true;
                    graded = image;
                }
            }
        }
        // Skin is assessed under the proposed global grade, but the exact staged
        // masks remain in source coordinates. Preview and apply share those bytes.
        if (input.skinAllowed && !result.tone.preserveLight && !cancelled()) {
            try {
                auto people = run(BackendSubjectMaskKind::PeopleDiscovery);
                if (people.terminal == BackendSubjectMaskTerminal::PeopleReady) {
                    result.skinChecked = true;
                    const int limit = std::min(4, 15 - int(input.before.grade_nodes.size()));
                    for (const auto personIndex : autoStartSkinPeople(people.people, limit)) {
                        if (cancelled())
                            break;
                        auto mask = run(BackendSubjectMaskKind::PeopleRegions, personIndex);
                        if (!mask.proposal_token)
                            continue;
                        const auto correction = measureAutoStartSkin(
                            graded,
                            mask.preview_samples,
                            int(mask.preview_width),
                            int(mask.preview_height)
                        );
                        if (mask.terminal != BackendSubjectMaskTerminal::Staged
                            || !correction.useful || cancelled()) {
                            backend->discardSubjectMaskProposal(mask.proposal_token);
                            continue;
                        }
                        BackendGradeNode node;
                        try {
                            node = backend->newBasicGradeNode(
                                QCoreApplication::translate(
                                    "EditAutoStartController",
                                    "Auto · skin %1"
                                )
                                    .arg(personIndex + 1)
                            );
                        } catch (...) {
                            backend->discardSubjectMaskProposal(mask.proposal_token);
                            throw;
                        }
                        node.basic = correction.basic;
                        node.fine = correction.fine;
                        result.masks.push_back(
                            {mask.proposal_token, mask.generation, node.grade_node_id}
                        );
                        result.skinNodes.push_back(std::move(node));
                    }
                }
            } catch (...) { /* A partial, clearly reported suggestion remains usable. */
            }
        }
    } catch (const std::exception& error) {
        result.error = QString::fromUtf8(error.what());
    }
    if (session) {
        try {
            backend->finishSubjectMaskInputSession(session);
        } catch (...) {}
    }
    control->job.store(0);
    control->render.store(0);
    return result;
}
