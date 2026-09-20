#pragma once
#include "backend/edit_types.hpp"
#include "paint_brush_presets.hpp"
#include <QColor>
#include <QObject>
#include <QPointF>
#include <QVariantList>
#include <functional>
#include <optional>
#include <shadow/image/photo_liquify.hpp>
class EditController;

// Photo-local manual finishing. Live paths are ephemeral until pointer release;
// one completed stroke is one history entry and one autosave checkpoint.
class EditPaintController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList layers READ layers NOTIFY changed)
    Q_PROPERTY(int selectedIndex READ selectedIndex NOTIFY changed)
    Q_PROPERTY(bool canPaint READ canPaint NOTIFY changed)
    Q_PROPERTY(bool strokeActive READ strokeActive NOTIFY changed)
    Q_PROPERTY(bool layerEnabled READ layerEnabled WRITE setLayerEnabled NOTIFY changed)
    Q_PROPERTY(double layerOpacity READ layerOpacity WRITE setLayerOpacity NOTIFY changed)
    Q_PROPERTY(int blend READ blend WRITE setBlend NOTIFY changed)
    Q_PROPERTY(int brushBlend READ brushBlend NOTIFY brushChanged)
    Q_PROPERTY(QString status READ status NOTIFY changed)
    Q_PROPERTY(double radius READ radius WRITE setRadius NOTIFY brushChanged)
    Q_PROPERTY(double hardness READ hardness WRITE setHardness NOTIFY brushChanged)
    Q_PROPERTY(double opacity READ opacity WRITE setOpacity NOTIFY brushChanged)
    Q_PROPERTY(double flow READ flow WRITE setFlow NOTIFY brushChanged)
    Q_PROPERTY(QColor color READ color WRITE setColor NOTIFY brushChanged)
    Q_PROPERTY(bool erase READ erase WRITE setErase NOTIFY brushChanged)
    Q_PROPERTY(bool picking READ picking WRITE setPicking NOTIFY brushChanged)
    Q_PROPERTY(double roundness READ roundness WRITE setRoundness NOTIFY brushChanged)
    Q_PROPERTY(double angle READ angle WRITE setAngle NOTIFY brushChanged)
    Q_PROPERTY(double spacing READ spacing WRITE setSpacing NOTIFY brushChanged)
    Q_PROPERTY(int texture READ texture WRITE setTexture NOTIFY brushChanged)
    Q_PROPERTY(
        double textureStrength READ textureStrength WRITE setTextureStrength NOTIFY brushChanged
    )
    Q_PROPERTY(bool pressureSize READ pressureSize WRITE setPressureSize NOTIFY brushChanged)
    Q_PROPERTY(bool pressureFlow READ pressureFlow WRITE setPressureFlow NOTIFY brushChanged)
    Q_PROPERTY(double smoothing READ smoothing WRITE setSmoothing NOTIFY brushChanged)
    Q_PROPERTY(int brushSlot READ brushSlot WRITE setBrushSlot NOTIFY brushChanged)
    Q_PROPERTY(QVariantList presets READ presets NOTIFY brushChanged)
    Q_PROPERTY(QString presetId READ presetId NOTIFY brushChanged)
  public:
    explicit EditPaintController(EditController& owner);
    QVariantList layers() const;
    int selectedIndex() const;
    bool canPaint() const;
    bool strokeActive() const {
        return before_.has_value();
    }
    bool layerEnabled() const;
    double layerOpacity() const;
    int blend() const;
    int brushBlend() const {
        return default_blend_;
    }
    QString status() const {
        return status_;
    }
    double radius() const {
        return brush_.radius;
    }
    double hardness() const {
        return brush_.hardness;
    }
    double opacity() const {
        return brush_.opacity;
    }
    double flow() const {
        return brush_.flow;
    }
    QColor color() const {
        return QColor::fromRgbF(float(brush_.red), float(brush_.green), float(brush_.blue));
    }
    bool erase() const {
        return brush_.erase;
    }
    bool picking() const {
        return picking_;
    }
    double roundness() const {
        return brush_.roundness;
    }
    void setRoundness(double value);
    double angle() const {
        return brush_.angle_degrees;
    }
    void setAngle(double value);
    double spacing() const {
        return brush_.spacing;
    }
    void setSpacing(double value);
    int texture() const {
        return brush_.texture;
    }
    void setTexture(int value);
    double textureStrength() const {
        return brush_.texture_strength;
    }
    void setTextureStrength(double value);
    bool pressureSize() const {
        return brush_.pressure_size;
    }
    void setPressureSize(bool value);
    bool pressureFlow() const {
        return brush_.pressure_flow;
    }
    void setPressureFlow(bool value);
    double smoothing() const {
        return smoothing_;
    }
    void setSmoothing(double value);
    int brushSlot() const {
        return presets_.slot();
    }
    void setBrushSlot(int value);
    QVariantList presets() const {
        return presets_.presets();
    }
    QString presetId() const {
        return presets_.current().preset_id;
    }
    Q_INVOKABLE void applyPreset(const QString& id);
    Q_INVOKABLE bool savePreset(const QString& name);
    Q_INVOKABLE void removePreset(const QString& id);
    Q_INVOKABLE QVariantMap cursorShape(double x, double y, double aspect) const;
    void setRadius(double value);
    void setHardness(double value);
    void setOpacity(double value);
    void setFlow(double value);
    void setColor(QColor value);
    void setErase(bool value);
    void setPicking(bool value);
    void setLayerEnabled(bool value);
    void setLayerOpacity(double value);
    void setBlend(int value);
    Q_INVOKABLE void activate();
    Q_INVOKABLE void selectLayer(int index);
    Q_INVOKABLE void addLayer();
    Q_INVOKABLE void removeLayer();
    Q_INVOKABLE void moveLayer(int offset);
    Q_INVOKABLE bool beginStroke(double x, double y, double aspect, double pressure = 1.0);
    Q_INVOKABLE void appendPoint(double x, double y, double pressure = 1.0);
    Q_INVOKABLE void finishStroke();
    Q_INVOKABLE void cancelStroke();
    Q_INVOKABLE bool sampleColor(double x, double y, const QString& generation);
    Q_INVOKABLE double displayRadius(double aspect) const;
  signals:
    void changed();
    void brushChanged();

  private:
    bool preparePointMapping() const;
    void saveBrush();
    void loadBrush();
    BackendPaintLayer freshLayer() const;
    const BackendPaintLayer* layer() const;
    bool editable() const;
    void editLayer(const QString& key, const std::function<void(BackendPaintLayer&)>& edit);
    EditController& owner_;
    QString selected_id_, status_;
    BackendPaintStroke brush_;
    PaintBrushPresets presets_;
    double smoothing_ = 0.2;
    int default_blend_ = 1;
    bool finishing_ = false;
    double last_x_ = 0, last_y_ = 0, last_pressure_ = 1;
    std::optional<BackendGradeStack> before_;
    mutable shadow::image::PreparedPhotoLiquify prepared_liquify_;
    mutable QVector<BackendLiquifyStroke> mapping_strokes_;
    mutable QSize mapping_extent_;
    mutable bool mapping_ready_ = false;
    quint64 generation_ = 0;
    double aspect_ = 1, stroke_distance_ = 0, dab_budget_ = 32760;
    int stroke_index_ = -1;
    bool picking_ = false;
};
