#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVector>

#include <atomic>
#include <cstdint>

struct ReviewItem final {
    QString photo_id;
    QString representation_id;
    QString title;
    QString source_path;
    QString visual_role;
    std::uint32_t visual_width = 0;
    std::uint32_t visual_height = 0;
    bool has_visual = false;
    bool has_technical_observation = false;
    std::uint32_t technical_input_width = 0;
    std::uint32_t technical_input_height = 0;
    QString technical_preprocessing_version;
    QString technical_implementation_version;
    double mean_luma = 0.0;
    double p01_luma = 0.0;
    double p50_luma = 0.0;
    double p99_luma = 0.0;
    double near_black_fraction = 0.0;
    double near_white_fraction = 0.0;
    double laplacian_variance = 0.0;
    double edge_energy = 0.0;
};

class ReviewModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        PhotoIdRole = Qt::UserRole + 1,
        RepresentationIdRole,
        TitleRole,
        SourcePathRole,
        VisualRole,
        VisualErrorRole,
        VisualWidthRole,
        VisualHeightRole,
        VisualSourceRole,
        HasTechnicalObservationRole,
        TechnicalInputWidthRole,
        TechnicalInputHeightRole,
        TechnicalPreprocessingVersionRole,
        TechnicalImplementationVersionRole,
        MeanLumaRole,
        P01LumaRole,
        P50LumaRole,
        P99LumaRole,
        NearBlackFractionRole,
        NearWhiteFractionRole,
        LaplacianVarianceRole,
        EdgeEnergyRole,
    };
    Q_ENUM(Role)

    explicit ReviewModel(QObject* parent = nullptr);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    void replace(QVector<ReviewItem> items, quint64 generation);
    void append(QVector<ReviewItem> items);
    [[nodiscard]] bool isGenerationCurrent(quint64 generation) const noexcept;

private:
    QVector<ReviewItem> items_;
    std::atomic<quint64> generation_ = 0;
};
