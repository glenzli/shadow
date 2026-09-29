#pragma once
#include "backend/edit_types.hpp"
#include "edit_retouch_coordinates.hpp"
#include "edit_retouch_donor_selection.hpp"
#include <QObject>
#include <QPointF>
#include <QVariantList>
#include <optional>
#include <vector>
class EditController;

// Small, photo-local source workspace. Candidate pixels are sampled once;
// cycling only commits an existing displacement to the selected repair.
class EditRetouchSources final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int frequencyRadius READ frequencyRadius WRITE setFrequencyRadius NOTIFY changed)
    Q_PROPERTY(QVariantList saved READ saved NOTIFY changed)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(bool previewEnabled MEMBER preview_enabled_ NOTIFY changed)
    Q_PROPERTY(double previewOpacity READ previewOpacity WRITE setPreviewOpacity NOTIFY changed)
    Q_PROPERTY(bool projectionRequired READ projectionRequired NOTIFY changed)
  public:
    explicit EditRetouchSources(EditController&);
    QVariantList saved() const;
    int frequencyRadius() const {
        return frequency_radius_;
    }
    void setFrequencyRadius(int value);
    Q_INVOKABLE void setRegionFrequency(bool continuous, int index, int value);
    QString status() const {
        return status_;
    }
    double previewOpacity() const {
        return preview_opacity_;
    }
    void setPreviewOpacity(double);
    Q_INVOKABLE void remember();
    Q_INVOKABLE void recall(int);
    Q_INVOKABLE void nextCandidate(bool continuous, int index);
    Q_INVOKABLE QVariantMap previewSourceAt(double x, double y) const;
    bool projectionRequired() const;
    const EditRetouchCoordinates& coordinates() const;
    Q_INVOKABLE QVariantMap projectRegion(bool continuous, int index, bool detailed = true) const;
    Q_INVOKABLE void moveRegion(
        bool continuous,
        int index,
        bool source,
        double fromX,
        double fromY,
        double toX,
        double toY
    );
    Q_INVOKABLE double previewRadius(double x, double y) const;
    std::optional<EditRetouchDonorSelection>
    selectSource(const QString&, std::span<const QPointF>, double radius, int mode) const;
  signals:
    void changed();

  private:
    EditController& owner_;
    std::vector<QPointF> saved_;
    std::vector<QPointF> candidates_;
    std::optional<BackendGradeStack> basis_;
    BackendPhotoGeometry saved_geometry_;
    int frequency_radius_ = 8;
    int candidate_index_ = -1;
    bool continuous_ = false;
    bool preview_enabled_ = true;
    double preview_opacity_ = 0.5;
    QString status_;
    mutable EditRetouchCoordinates coordinates_;
    QVector<BackendLiquifyStroke> saved_liquify_;
    bool saved_liquify_enabled_ = false;
};
