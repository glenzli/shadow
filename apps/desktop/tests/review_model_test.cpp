#include "review_model.hpp"

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QVariant>

#include <array>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "review model contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] QVariant value(
    const ReviewModel& model,
    const int row,
    const ReviewModel::Role role
) {
    return model.data(model.index(row, 0), role);
}

void role_names_and_types_are_stable() {
    ReviewItem item;
    item.photo_id = QStringLiteral("photo-a");
    item.representation_id = QStringLiteral("representation-a");
    item.has_technical_observation = true;
    item.technical_input_width = 512;
    item.technical_input_height = 341;
    item.technical_preprocessing_version = QStringLiteral("pre-v1");
    item.technical_implementation_version = QStringLiteral("impl-v1");
    item.mean_luma = 0.42;
    item.p01_luma = 0.01;
    item.p50_luma = 0.4;
    item.p99_luma = 0.97;
    item.near_black_fraction = 0.02;
    item.near_white_fraction = 0.03;
    item.laplacian_variance = 0.004;
    item.edge_energy = 0.005;

    ReviewModel model;
    model.replace({item}, 7);

    struct ExpectedRole final {
        ReviewModel::Role role;
        const char* name;
    };
    constexpr std::array expected_roles{
        ExpectedRole{ReviewModel::HasTechnicalObservationRole, "hasTechnicalObservation"},
        ExpectedRole{ReviewModel::TechnicalInputWidthRole, "technicalInputWidth"},
        ExpectedRole{ReviewModel::TechnicalInputHeightRole, "technicalInputHeight"},
        ExpectedRole{
            ReviewModel::TechnicalPreprocessingVersionRole,
            "technicalPreprocessingVersion",
        },
        ExpectedRole{
            ReviewModel::TechnicalImplementationVersionRole,
            "technicalImplementationVersion",
        },
        ExpectedRole{ReviewModel::MeanLumaRole, "meanLuma"},
        ExpectedRole{ReviewModel::P01LumaRole, "p01Luma"},
        ExpectedRole{ReviewModel::P50LumaRole, "p50Luma"},
        ExpectedRole{ReviewModel::P99LumaRole, "p99Luma"},
        ExpectedRole{ReviewModel::NearBlackFractionRole, "nearBlackFraction"},
        ExpectedRole{ReviewModel::NearWhiteFractionRole, "nearWhiteFraction"},
        ExpectedRole{ReviewModel::LaplacianVarianceRole, "laplacianVariance"},
        ExpectedRole{ReviewModel::EdgeEnergyRole, "edgeEnergy"},
    };
    const auto names = model.roleNames();
    for (const auto& expected : expected_roles) {
        require(
            names.value(expected.role) == QByteArray(expected.name),
            "every technical QML role name must remain stable"
        );
    }

    require(
        value(model, 0, ReviewModel::HasTechnicalObservationRole).typeId()
            == QMetaType::Bool,
        "observation presence must be a QML boolean"
    );
    require(
        value(model, 0, ReviewModel::TechnicalInputWidthRole).typeId()
                == QMetaType::UInt
            && value(model, 0, ReviewModel::TechnicalInputHeightRole).typeId()
                == QMetaType::UInt,
        "analysis dimensions must retain unsigned integer types"
    );
    require(
        value(model, 0, ReviewModel::TechnicalPreprocessingVersionRole).typeId()
                == QMetaType::QString
            && value(model, 0, ReviewModel::TechnicalImplementationVersionRole).typeId()
                == QMetaType::QString,
        "provenance must be exposed as strings"
    );

    constexpr std::array numeric_roles{
        std::pair{ReviewModel::MeanLumaRole, 0.42},
        std::pair{ReviewModel::P01LumaRole, 0.01},
        std::pair{ReviewModel::P50LumaRole, 0.4},
        std::pair{ReviewModel::P99LumaRole, 0.97},
        std::pair{ReviewModel::NearBlackFractionRole, 0.02},
        std::pair{ReviewModel::NearWhiteFractionRole, 0.03},
        std::pair{ReviewModel::LaplacianVarianceRole, 0.004},
        std::pair{ReviewModel::EdgeEnergyRole, 0.005},
    };
    for (const auto& [role, expected] : numeric_roles) {
        const auto actual = value(model, 0, role);
        require(
            actual.typeId() == QMetaType::Double,
            "technical measurements must retain double precision"
        );
        require(
            actual.toDouble() == expected,
            "each technical role must expose its own measurement"
        );
    }
}

void absence_and_legitimate_zero_are_distinct() {
    ReviewItem missing;
    missing.photo_id = QStringLiteral("missing");

    ReviewItem observed_zero;
    observed_zero.photo_id = QStringLiteral("observed-zero");
    observed_zero.has_technical_observation = true;

    ReviewModel model;
    model.replace({missing}, 1);
    model.append({observed_zero});

    require(model.rowCount() == 2, "append must preserve the replaced page");
    require(
        !value(model, 0, ReviewModel::HasTechnicalObservationRole).toBool(),
        "a missing observation must remain explicitly absent"
    );
    require(
        value(model, 1, ReviewModel::HasTechnicalObservationRole).toBool(),
        "an observed all-zero result must not look absent"
    );
    require(
        value(model, 0, ReviewModel::P50LumaRole).toDouble() == 0.0
            && value(model, 1, ReviewModel::P50LumaRole).toDouble() == 0.0
            && value(model, 1, ReviewModel::EdgeEnergyRole).toDouble() == 0.0,
        "zero measurements must stay numeric for both states"
    );
}

void replace_and_append_keep_their_items_intact() {
    ReviewItem first;
    first.photo_id = QStringLiteral("first");
    first.has_technical_observation = true;
    first.p50_luma = 0.25;

    ReviewItem second;
    second.photo_id = QStringLiteral("second");
    second.has_technical_observation = true;
    second.p50_luma = 0.75;

    ReviewModel model;
    model.replace({first}, 10);
    model.append({second});
    require(
        value(model, 0, ReviewModel::P50LumaRole).toDouble() == 0.25
            && value(model, 1, ReviewModel::P50LumaRole).toDouble() == 0.75,
        "append must preserve each item's own observation"
    );

    ReviewItem replacement;
    replacement.photo_id = QStringLiteral("replacement");
    replacement.has_technical_observation = false;
    model.replace({replacement}, 11);
    require(model.rowCount() == 1, "replace must remove the previous page");
    require(
        value(model, 0, ReviewModel::PhotoIdRole).toString()
                == QStringLiteral("replacement")
            && !value(model, 0, ReviewModel::HasTechnicalObservationRole).toBool(),
        "replace must expose only the new item's observation state"
    );
}

} // namespace

int main() {
    role_names_and_types_are_stable();
    absence_and_legitimate_zero_are_distinct();
    replace_and_append_keep_their_items_intact();
    return EXIT_SUCCESS;
}
