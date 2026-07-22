#include "tone_curve_point_model.hpp"

#include <QAbstractItemModel>
#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QModelIndex>
#include <QPointF>
#include <QVariant>
#include <QVector>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace {

void require(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "tone curve point model contract failed: " << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

[[nodiscard]] QVariant value(
    const ToneCurvePointModel& model,
    const int row,
    const ToneCurvePointModel::Role role
) {
    return model.data(model.index(row, 0), role);
}

void default_endpoints_and_roles_are_stable() {
    ToneCurvePointModel model;
    require(model.rowCount() == 2, "the default curve must have two endpoints");
    require(model.pointCount() == 2, "pointCount must match rowCount");
    require(model.isEditable(), "the default identity curve must be editable");
    require(model.selectedIndex() == -1, "the default curve must have no selection");

    const auto names = model.roleNames();
    require(
        names.value(ToneCurvePointModel::XRole) == QByteArrayLiteral("xValue"),
        "xValue role"
    );
    require(
        names.value(ToneCurvePointModel::YRole) == QByteArrayLiteral("yValue"),
        "yValue role"
    );
    require(
        names.value(ToneCurvePointModel::EndpointRole) == QByteArrayLiteral("endpoint"),
        "endpoint role"
    );
    require(
        names.value(ToneCurvePointModel::XMovableRole) == QByteArrayLiteral("xMovable"),
        "xMovable role"
    );
    require(
        names.value(ToneCurvePointModel::DeletableRole) == QByteArrayLiteral("deletable"),
        "deletable role"
    );
    require(
        names.value(ToneCurvePointModel::SelectedRole) == QByteArrayLiteral("selected"),
        "selected role"
    );

    for (const int row : {0, 1}) {
        require(value(model, row, ToneCurvePointModel::XRole).typeId() == QMetaType::Double, "x type");
        require(value(model, row, ToneCurvePointModel::YRole).typeId() == QMetaType::Double, "y type");
        require(value(model, row, ToneCurvePointModel::EndpointRole).toBool(), "endpoint flag");
        require(!value(model, row, ToneCurvePointModel::XMovableRole).toBool(), "endpoint x fixed");
        require(!value(model, row, ToneCurvePointModel::DeletableRole).toBool(), "endpoint retained");
        require(!value(model, row, ToneCurvePointModel::SelectedRole).toBool(), "not selected");
    }
    require(value(model, 0, ToneCurvePointModel::XRole).toDouble() == 0.0, "left x");
    require(value(model, 0, ToneCurvePointModel::YRole).toDouble() == 0.0, "left y");
    require(value(model, 1, ToneCurvePointModel::XRole).toDouble() == 1.0, "right x");
    require(value(model, 1, ToneCurvePointModel::YRole).toDouble() == 1.0, "right y");
}

void replacement_is_validated_without_touching_legacy_values() {
    ToneCurvePointModel model;
    int resets = 0;
    int points_changed = 0;
    int counts_changed = 0;
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&resets]() { ++resets; });
    QObject::connect(
        &model,
        &ToneCurvePointModel::pointsChanged,
        [&points_changed]() { ++points_changed; }
    );
    QObject::connect(
        &model,
        &ToneCurvePointModel::pointCountChanged,
        [&counts_changed]() { ++counts_changed; }
    );

    const QVector<ToneCurvePoint> legacy{{0.0, -0.25}, {0.4, 1.5}, {1.0, 1.25}};
    require(model.replace(legacy), "a finite unclipped persisted curve must load");
    require(resets == 1 && points_changed == 1 && counts_changed == 1, "replace signals");
    require(!model.isEditable(), "out-of-range y must make the loaded curve read-only");
    require(model.points() == legacy, "loading must not clamp or round persisted values");
    require(value(model, 1, ToneCurvePointModel::YRole).toDouble() == 1.5, "legacy y exact");
    require(!value(model, 1, ToneCurvePointModel::XMovableRole).toBool(), "read-only x role");
    require(!value(model, 1, ToneCurvePointModel::DeletableRole).toBool(), "read-only delete role");

    const auto before_invalid = model.points();
    require(!model.replace({{0.0, 0.0}}), "a one-point curve must be rejected");
    require(
        !model.replace({{0.0, 0.0}, {0.7, 0.5}, {0.6, 0.4}, {1.0, 1.0}}),
        "unordered x must be rejected"
    );
    require(
        !model.replace({
            {0.0, 0.0},
            {1.0, std::numeric_limits<double>::quiet_NaN()},
        }),
        "non-finite values must be rejected"
    );
    require(model.points() == before_invalid, "invalid loads must leave state untouched");
    require(resets == 1 && points_changed == 1, "invalid loads must be signal-free");
}

void add_and_move_enforce_the_interactive_envelope() {
    ToneCurvePointModel model;
    int rows_inserted = 0;
    int data_changes = 0;
    int selection_changes = 0;
    QObject::connect(
        &model,
        &QAbstractItemModel::rowsInserted,
        [&rows_inserted](const QModelIndex&, int, int) { ++rows_inserted; }
    );
    QObject::connect(
        &model,
        &QAbstractItemModel::dataChanged,
        [&data_changes](const QModelIndex&, const QModelIndex&, const QList<int>&) {
            ++data_changes;
        }
    );
    QObject::connect(
        &model,
        &ToneCurvePointModel::selectedIndexChanged,
        [&selection_changes]() { ++selection_changes; }
    );

    const int row = model.addPoint(0.0, 2.0);
    require(row == 1, "an insertion near the left endpoint must use the interior row");
    require(rows_inserted == 1, "add must use row insertion signals");
    require(model.selectedIndex() == row && selection_changes == 1, "add selects its point");
    require(value(model, row, ToneCurvePointModel::SelectedRole).toBool(), "selected role");
    require(value(model, row, ToneCurvePointModel::XMovableRole).toBool(), "interior x movable");
    require(value(model, row, ToneCurvePointModel::DeletableRole).toBool(), "interior deletable");
    require(
        value(model, row, ToneCurvePointModel::XRole).toDouble()
            == ToneCurvePointModel::minimum_x_spacing,
        "added x must retain endpoint spacing"
    );
    require(value(model, row, ToneCurvePointModel::YRole).toDouble() == 1.0, "added y clamps high");

    require(model.movePoint(row, 1.0, -4.0), "an interior point must move within constraints");
    require(
        value(model, row, ToneCurvePointModel::XRole).toDouble()
            == 1.0 - ToneCurvePointModel::minimum_x_spacing,
        "moved x must retain right endpoint spacing"
    );
    require(value(model, row, ToneCurvePointModel::YRole).toDouble() == 0.0, "moved y clamps low");
    require(data_changes >= 2, "selection and movement must notify role changes");

    require(model.movePoint(0, 0.8, 0.6), "endpoint y must remain editable");
    require(value(model, 0, ToneCurvePointModel::XRole).toDouble() == 0.0, "endpoint x fixed");
    require(value(model, 0, ToneCurvePointModel::YRole).toDouble() == 0.6, "endpoint y moves");
    require(!model.removePoint(0), "the left endpoint cannot be deleted");
    require(!model.removePoint(model.pointCount() - 1), "the right endpoint cannot be deleted");
    require(
        model.addPoint(std::numeric_limits<double>::infinity(), 0.5) == -1,
        "non-finite insertion must be rejected"
    );
}

void removal_and_reset_keep_selection_and_signals_coherent() {
    ToneCurvePointModel model;
    require(model.addPoint(0.25, 0.3) == 1, "first interior point");
    require(model.addPoint(0.75, 0.7) == 2, "second interior point");
    require(model.selectedIndex() == 2, "latest point selected");

    int rows_removed = 0;
    int resets = 0;
    int editable_changes = 0;
    QObject::connect(
        &model,
        &QAbstractItemModel::rowsRemoved,
        [&rows_removed](const QModelIndex&, int, int) { ++rows_removed; }
    );
    QObject::connect(&model, &QAbstractItemModel::modelReset, [&resets]() { ++resets; });
    QObject::connect(
        &model,
        &ToneCurvePointModel::editableChanged,
        [&editable_changes]() { ++editable_changes; }
    );

    require(model.removePoint(2), "selected interior point must be removable");
    require(rows_removed == 1 && model.pointCount() == 3, "remove row contract");
    require(model.selectedIndex() == 2, "selection must move to the surviving neighbor");
    require(value(model, 2, ToneCurvePointModel::SelectedRole).toBool(), "neighbor selected role");

    require(
        model.replace({{0.0, -1.0}, {1.0, 2.0}}),
        "an extended-range curve must remain loadable"
    );
    require(!model.isEditable(), "extended range must be visibly read-only");
    model.resetLinear();
    require(resets == 2, "replace and reset must both reset the model");
    require(editable_changes == 2, "read-only transition and reset must notify editable");
    require(model.isEditable(), "reset must restore active editing");
    require(model.points() == QVector<ToneCurvePoint>{{0.0, 0.0}, {1.0, 1.0}}, "linear reset");
    require(model.selectedIndex() == -1, "reset clears selection");
}

void large_and_tightly_spaced_legal_curves_are_lossless_read_only_data() {
    QVector<ToneCurvePoint> large;
    large.reserve(ToneCurvePointModel::maximum_stored_point_count);
    for (int row = 0; row < ToneCurvePointModel::maximum_stored_point_count; ++row) {
        const double x = row == ToneCurvePointModel::maximum_stored_point_count - 1
            ? 1.0
            : static_cast<double>(row)
                / static_cast<double>(ToneCurvePointModel::maximum_stored_point_count - 1);
        large.append({x, x});
    }

    ToneCurvePointModel model;
    require(model.replace(large), "a valid 256-point persisted curve must load");
    require(model.pointCount() == 256, "all persisted points must remain present");
    require(model.points() == large, "a large curve must be lossless");
    require(!model.isEditable(), "a curve above the active point cap must be read-only");
    require(model.selectPoint(128), "read-only points must remain selectable");
    require(model.selectedIndex() == 128, "read-only selection");
    require(!model.movePoint(128, 0.5, 0.5), "read-only movement rejected");
    require(model.addPoint(0.5, 0.5) == -1, "read-only insertion rejected");
    require(!model.removePoint(128), "read-only deletion rejected");
    require(model.points() == large, "rejected edits must preserve a large curve");

    const QVector<ToneCurvePoint> tight{
        {0.0, 0.0},
        {ToneCurvePointModel::minimum_x_spacing / 2.0, 0.2},
        {1.0, 1.0},
    };
    require(model.replace(tight), "strictly increasing legacy x values must load");
    require(!model.isEditable(), "sub-minimum legacy spacing must be read-only");
    require(model.points() == tight, "tight legacy x values must not be moved");
}

void active_point_limit_is_enforced_without_rejecting_the_curve() {
    QVector<ToneCurvePoint> points;
    points.reserve(ToneCurvePointModel::maximum_active_point_count);
    for (int row = 0; row < ToneCurvePointModel::maximum_active_point_count; ++row) {
        const double x = static_cast<double>(row)
            / static_cast<double>(ToneCurvePointModel::maximum_active_point_count - 1);
        points.append({x, x});
    }
    points.back().x = 1.0;

    ToneCurvePointModel model;
    require(model.replace(points), "a 32-point active curve must load");
    require(model.isEditable(), "the active cap itself remains editable");
    require(model.addPoint(0.5, 0.5) == -1, "the 33rd active point must be refused");
    require(model.movePoint(16, 0.51, 0.6), "existing points remain editable at the cap");
}

void preview_sampling_matches_the_versioned_curve_contracts() {
    ToneCurvePointModel identity;
    const QVariantList identity_samples = identity.sampledPoints(5, true);
    require(identity_samples.size() == 5, "identity curve preview sample count");
    for (int index = 0; index < identity_samples.size(); ++index) {
        const QPointF point = identity_samples.at(index).toPointF();
        const double expected = static_cast<double>(index) / 4.0;
        require(
            point.x() == expected && point.y() == expected,
            "the smooth identity preview must be exact"
        );
    }

    ToneCurvePointModel shaped;
    require(
        shaped.replace({{0.0, 0.0}, {0.5, 0.75}, {1.0, 1.0}}),
        "test curve must load"
    );
    const QVariantList smooth = shaped.sampledPoints(5, true);
    const QVariantList legacy = shaped.sampledPoints(5, false);
    require(
        std::abs(smooth.at(1).toPointF().y() - 0.453125) < 1e-12,
        "PCHIP preview must match the image-kernel landmark equation"
    );
    require(
        std::abs(legacy.at(1).toPointF().y() - 0.375) < 1e-12,
        "legacy preview must retain piecewise-linear interpolation"
    );
    double previous = -std::numeric_limits<double>::infinity();
    for (const QVariant& sample : shaped.sampledPoints(257, true)) {
        const double y = sample.toPointF().y();
        require(y >= previous && y >= 0.0 && y <= 1.0,
                "shape-preserving PCHIP must not overshoot a monotone curve");
        previous = y;
    }
    require(shaped.sampledPoints(1, true).isEmpty(), "sample bounds are enforced");
}

} // namespace

int main() {
    default_endpoints_and_roles_are_stable();
    replacement_is_validated_without_touching_legacy_values();
    add_and_move_enforce_the_interactive_envelope();
    removal_and_reset_keep_selection_and_signals_coherent();
    large_and_tightly_spaced_legal_curves_are_lossless_read_only_data();
    active_point_limit_is_enforced_without_rejecting_the_curve();
    preview_sampling_matches_the_versioned_curve_contracts();
    return EXIT_SUCCESS;
}
