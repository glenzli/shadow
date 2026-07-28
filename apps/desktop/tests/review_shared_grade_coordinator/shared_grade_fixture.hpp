#pragma once

#include "review_shared_grade_coordinator.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace review_shared_grade_test {

inline void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "Shared Grade coordinator contract failed: "
                  << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

struct ApplyCall final {
    QString layer_id;
    QVector<BackendBatchPhotoTarget> targets;
};

struct SharedGradeBackendState final {
    QVector<BackendSharedGradeNode> nodes;
    QVector<ApplyCall> apply_calls;
    BackendBatchGradeReceipt receipt;
    int node_calls = 0;
    bool fail_nodes = false;
    bool fail_apply = false;
};

inline BackendSharedGradeNode node(
    const QString& layer_id,
    const QString& revision_id,
    const std::uint32_t revision_number,
    const QString& label
) {
    return {
        .layer_id = layer_id,
        .revision_id = revision_id,
        .label = label,
        .revision_number = revision_number,
    };
}

inline QVariantMap target(
    const QString& photo_id,
    const QString& source_path
) {
    return {
        {QStringLiteral("photoId"), photo_id},
        {QStringLiteral("sourcePath"), source_path},
    };
}

inline ReviewSharedGradeCoordinator::Operations operations(
    const std::shared_ptr<SharedGradeBackendState>& state
) {
    return {
        .nodes =
            [state]() {
                ++state->node_calls;
                if (state->fail_nodes) {
                    throw std::runtime_error("shared nodes failed");
                }
                return state->nodes;
            },
        .apply =
            [state](
                const QString& layer_id,
                const QVector<BackendBatchPhotoTarget>& targets
            ) {
                state->apply_calls.push_back({
                    .layer_id = layer_id,
                    .targets = targets,
                });
                if (state->fail_apply) {
                    throw std::runtime_error("shared apply failed");
                }
                return state->receipt;
            },
    };
}

} // namespace review_shared_grade_test
