#pragma once

#include "desktop_backend.hpp"
#include "review_focus_detail_provider.hpp"

#include <QFutureWatcher>
#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

struct ReviewFocusDetailRequest final {
    QString photo_id;
    QString source_path;
    double center_x = 0.5;
    double center_y = 0.5;
    quint64 generation = 0;
    std::uint64_t render_token = 0;

    [[nodiscard]] bool valid() const noexcept {
        return !photo_id.isEmpty() && !source_path.isEmpty() && generation != 0
               && render_token != 0;
    }
};

struct ReviewFocusDetailTaskResult final {
    ReviewFocusDetailRequest request;
    QImage image;
    QString error;
};

/// Owns the cancellable level-zero focus-region lifecycle used by culling.
///
/// It requests one 384px region through the same full-detail renderer used by
/// Precision, retains only the current generation, and coalesces rapid filmstrip
/// navigation to the newest selected identity.
class ReviewFocusDetailCoordinator final : public QObject {
    Q_OBJECT

  public:
    using Renderer = std::function<QImage(const ReviewFocusDetailRequest& request)>;
    using TokenFactory = std::function<std::uint64_t()>;

    ReviewFocusDetailCoordinator(
        std::shared_ptr<DesktopBackend> backend,
        std::shared_ptr<ReviewFocusDetailStore> store,
        QObject* parent = nullptr
    );
    ReviewFocusDetailCoordinator(
        Renderer renderer,
        TokenFactory token_factory,
        std::shared_ptr<ReviewFocusDetailStore> store,
        QObject* parent = nullptr
    );
    ~ReviewFocusDetailCoordinator() override;

    [[nodiscard]] QString imageSource() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] bool failed() const noexcept;

    void
    request(const QString& photo_id, const QString& source_path, double center_x, double center_y);
    void clear();
    void retranslateUi();

  signals:
    void stateChanged();

  private:
    void startPending();
    void finish();
    void resetPresentation();

    Renderer renderer_;
    TokenFactory token_factory_;
    std::shared_ptr<ReviewFocusDetailStore> store_;
    std::optional<ReviewFocusDetailRequest> pending_request_;
    ReviewFocusDetailRequest current_request_;
    quint64 next_generation_ = 0;
    QString image_source_;
    QString status_text_;
    bool busy_ = false;
    bool ready_ = false;
    bool failed_ = false;
    QTimer debounce_timer_;
    QFutureWatcher<ReviewFocusDetailTaskResult> watcher_;
};
