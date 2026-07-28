#include "review_model_fixture.hpp"

namespace review_model_test {

void role_names_and_types_are_stable() {
    ReviewItem item;
    item.photo_id = QStringLiteral("photo-a");
    item.representation_id = QStringLiteral("representation-a");
    item.visual_handle = QStringLiteral("visual-handle-a");
    item.decision_head_sequence = 42;
    item.decision_flag = QStringLiteral("picked");
    item.decision_rating = 4;
    item.liked = true;
    item.color_label = QStringLiteral("blue");
    item.library_state_updated_at_ms = 1'724'000'000'123;
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
        ExpectedRole{ReviewModel::VisualHandleRole, "visualHandle"},
        ExpectedRole{ReviewModel::DecisionHeadSequenceRole, "decisionHeadSequence"},
        ExpectedRole{ReviewModel::DecisionFlagRole, "decisionFlag"},
        ExpectedRole{ReviewModel::DecisionRatingRole, "decisionRating"},
        ExpectedRole{ReviewModel::LikedRole, "liked"},
        ExpectedRole{ReviewModel::ColorLabelRole, "colorLabel"},
        ExpectedRole{ReviewModel::LibraryStateUpdatedAtMsRole, "libraryStateUpdatedAtMs"},
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
        value(model, 0, ReviewModel::VisualHandleRole).typeId()
                == QMetaType::QString
            && value(model, 0, ReviewModel::VisualHandleRole).toString()
                == QStringLiteral("visual-handle-a"),
        "opaque visual identity must be exposed as a string"
    );
    require(
        value(model, 0, ReviewModel::DecisionHeadSequenceRole).typeId()
                == QMetaType::ULongLong
            && value(model, 0, ReviewModel::DecisionHeadSequenceRole).toULongLong()
                == 42,
        "decision head must retain its full unsigned sequence"
    );
    require(
        value(model, 0, ReviewModel::DecisionFlagRole).typeId()
                == QMetaType::QString
            && value(model, 0, ReviewModel::DecisionFlagRole).toString()
                == QStringLiteral("picked")
            && value(model, 0, ReviewModel::DecisionRatingRole).typeId()
                == QMetaType::Int
            && value(model, 0, ReviewModel::DecisionRatingRole).toInt() == 4,
        "manual flag and rating must expose stable QML types"
    );
    require(
        value(model, 0, ReviewModel::LikedRole).typeId() == QMetaType::Bool
            && value(model, 0, ReviewModel::LikedRole).toBool()
            && value(model, 0, ReviewModel::ColorLabelRole).toString()
                == QStringLiteral("blue")
            && value(model, 0, ReviewModel::LibraryStateUpdatedAtMsRole).toLongLong()
                == 1'724'000'000'123,
        "Catalog-backed Library organization state must retain its QML types"
    );
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

} // namespace review_model_test
