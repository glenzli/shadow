#pragma once
#include "backend/edit_types.hpp"
#include <QSettings>
#include <QVariantList>
#include <array>

struct PaintBrushProfile final {
    BackendPaintStroke stroke;
    double smoothing = 0.2;
    int blend = 1;
    QString preset_id;
    bool operator==(const PaintBrushProfile&) const = default;
};

// Tool preferences contain no photo identities or stroke points. Recipe authoring
// copies the selected tip into each stroke; later preset edits cannot change it.
class PaintBrushPresets final {
  public:
    explicit PaintBrushPresets(const QString& settings_file);
    const PaintBrushProfile& current() const;
    int slot() const {
        return slot_;
    }
    void setCurrent(PaintBrushProfile profile);
    void selectSlot(int slot);
    QVariantList presets() const;
    bool apply(const QString& id);
    bool save(const QString& name);
    bool remove(const QString& id);
    static bool valid(const PaintBrushProfile& profile);
    static QString defaultSettingsFile();

  private:
    void loadCustom();
    void saveCustom();
    struct Named {
        QString id, label;
        PaintBrushProfile profile;
    };
    QSettings settings_;
    std::array<PaintBrushProfile, 2> slots_;
    QVector<Named> custom_;
    int slot_ = 0;
};
