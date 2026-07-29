#pragma once

#include <QQuickItem>
#include <QRectF>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <cstdint>
#include <memory>

class EditPreviewPresentationContext;
class EditPreviewPresentationRegistry;
class QQuickWindow;

class EditPreviewTextureItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QString source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(
        bool liveAdmissionEnabled READ liveAdmissionEnabled WRITE setLiveAdmissionEnabled NOTIFY
            liveAdmissionEnabledChanged
    )
    Q_PROPERTY(
        QObject* presentationRegistry READ presentationRegistry WRITE setPresentationRegistry NOTIFY
            presentationRegistryChanged
    )
    Q_PROPERTY(QString fallbackSource READ fallbackSource NOTIFY fallbackSourceChanged)
    Q_PROPERTY(bool liveFrameAvailable READ liveFrameAvailable NOTIFY liveFrameAvailableChanged)
    Q_PROPERTY(
        QString presentedGeneration READ presentedGeneration NOTIFY presentedGenerationChanged
    )
    Q_PROPERTY(FillMode fillMode READ fillMode WRITE setFillMode NOTIFY fillModeChanged)
    Q_PROPERTY(QRectF contentRect READ contentRect NOTIFY contentRectChanged)

  public:
    enum class FillMode {
        Stretch,
        PreserveAspectFit,
    };
    Q_ENUM(FillMode)

    explicit EditPreviewTextureItem(QQuickItem* parent = nullptr);
    ~EditPreviewTextureItem() override;

    [[nodiscard]] QString source() const;
    void setSource(const QString& source);

    [[nodiscard]] bool liveAdmissionEnabled() const noexcept;
    void setLiveAdmissionEnabled(bool enabled);

    [[nodiscard]] QObject* presentationRegistry() const noexcept;
    void setPresentationRegistry(QObject* registry);

    [[nodiscard]] QString fallbackSource() const;
    [[nodiscard]] bool liveFrameAvailable() const noexcept;
    [[nodiscard]] QString presentedGeneration() const;

    [[nodiscard]] FillMode fillMode() const noexcept;
    void setFillMode(FillMode fill_mode);

    [[nodiscard]] QRectF contentRect() const noexcept;

  signals:
    void sourceChanged();
    void liveAdmissionEnabledChanged();
    void presentationRegistryChanged();
    void fallbackSourceChanged();
    void liveFrameAvailableChanged();
    void presentedGenerationChanged();
    void fillModeChanged();
    void contentRectChanged();

  protected:
    [[nodiscard]] QSGNode*
    updatePaintNode(QSGNode* old_node, UpdatePaintNodeData* update_data) override;
    void releaseResources() override;
    void geometryChange(const QRectF& new_geometry, const QRectF& old_geometry) override;

  private:
    struct State;
    std::unique_ptr<State> state_;

    void refreshSourceBinding();
    void bindWindowLifecycle(QQuickWindow* window);
    void revokePresentedTexture(std::uint64_t scene_graph_revision);
    [[nodiscard]] std::uint64_t advanceSceneGraphRevision() noexcept;
};
