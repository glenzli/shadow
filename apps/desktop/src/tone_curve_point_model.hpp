#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QVariantList>
#include <QVector>

struct ToneCurvePoint final {
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const ToneCurvePoint&, const ToneCurvePoint&) = default;
};

/// UI-facing point-curve interaction state.
///
/// Loading is deliberately less restrictive than active editing. Any valid
/// persisted curve with 2 through 256 finite, strictly ordered points is kept
/// byte-for-byte at the `double` value level. Curves outside the interactive
/// envelope remain inspectable and selectable, but cannot be edited until the
/// caller explicitly resets or replaces them.
class ToneCurvePointModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(bool editable READ isEditable NOTIFY editableChanged)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY selectedIndexChanged)
    Q_PROPERTY(int pointCount READ pointCount NOTIFY pointCountChanged)

public:
    enum Role {
        XRole = Qt::UserRole + 1,
        YRole,
        EndpointRole,
        XMovableRole,
        DeletableRole,
        SelectedRole,
    };
    Q_ENUM(Role)

    static constexpr int minimum_stored_point_count = 2;
    static constexpr int maximum_stored_point_count = 256;
    static constexpr int maximum_active_point_count = 32;
    static constexpr double minimum_x_spacing = 1.0 / 4096.0;

    explicit ToneCurvePointModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] QVector<ToneCurvePoint> points() const;
    [[nodiscard]] int pointCount() const noexcept;
    [[nodiscard]] bool isEditable() const noexcept;
    [[nodiscard]] int selectedIndex() const noexcept;

    /// Returns a bounded, uniformly sampled preview path using the same
    /// shape-preserving PCHIP equations as the image kernel.
    Q_INVOKABLE QVariantList sampledPoints(
        int sample_count,
        bool smooth
    ) const;

    /// Replaces the model with a valid persisted curve without clamping it.
    /// Invalid input leaves the current model untouched.
    [[nodiscard]] bool replace(QVector<ToneCurvePoint> points);

    /// Selects one row, or clears selection with -1.
    [[nodiscard]] bool selectPoint(int row);

    /// Moves one point inside the active-editing envelope.
    /// Endpoint x positions remain fixed while their y values may move.
    [[nodiscard]] bool movePoint(int row, double x, double y);

    /// Adds and selects a constrained point. Returns its row, or -1 on refusal.
    [[nodiscard]] int addPoint(double x, double y);

    /// Removes an interior point from an editable curve.
    [[nodiscard]] bool removePoint(int row);

    /// Replaces any current curve with the editable identity line.
    void resetLinear();

    [[nodiscard]] static bool isValidPersistedCurve(
        const QVector<ToneCurvePoint>& points
    ) noexcept;

signals:
    void pointsChanged();
    void editableChanged();
    void selectedIndexChanged();
    void pointCountChanged();

private:
    [[nodiscard]] static bool supportsActiveEditing(
        const QVector<ToneCurvePoint>& points
    ) noexcept;
    [[nodiscard]] bool isEndpoint(int row) const noexcept;
    void emitSelectionDataChanged(int old_row, int new_row);

    QVector<ToneCurvePoint> points_;
    int selected_index_ = -1;
    bool editable_ = true;
};
