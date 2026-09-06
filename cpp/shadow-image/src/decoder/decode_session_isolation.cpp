#include "decode_session_isolation.hpp"

#include <shadow/image/decoder_error.hpp>
#include <shadow/image/linear_raster.hpp>

#include <exception>
#include <optional>
#include <string>
#include <unordered_map>

namespace shadow::image::detail {

namespace {

class IsolatedDecodeSession final : public DecodeSession, public LinearRasterSource {
private:
    template <typename Callback>
    decltype(auto) access(Callback&& callback) const {
        std::lock_guard session_lock(session_gate_);
        if (provider_gate_.has_value()) {
            return provider_gate_->synchronize([this, &callback]() -> decltype(auto) {
                return std::invoke(std::forward<Callback>(callback), *session_);
            });
        }
        return std::invoke(std::forward<Callback>(callback), *session_);
    }

public:
    IsolatedDecodeSession(
        std::unique_ptr<DecodeSession> session,
        std::optional<SharedDecodeProviderGate> provider_gate
    )
        : session_(std::move(session)), provider_gate_(std::move(provider_gate)) {
        if (session_ == nullptr) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "decode isolation could not retain its session"
            );
        }
        try {
            const auto snapshot_immutable_contract = [this] {
                metadata_ = session_->metadata();
                capabilities_ = session_->capabilities();
                const auto source_previews = session_->previews();
                previews_.assign(source_previews.begin(), source_previews.end());
                raw_development_capabilities_ = session_->raw_development_capabilities();
            };
            if (provider_gate_.has_value()) {
                provider_gate_->synchronize(snapshot_immutable_contract);
            } else {
                snapshot_immutable_contract();
            }
        } catch (...) {
            // A partially constructed wrapper does not run its destructor. Explicitly destroy a
            // private session under the module gate before propagating a snapshot/allocation
            // failure.
            if (provider_gate_.has_value()) {
                try {
                    provider_gate_->synchronize([this] { session_.reset(); });
                } catch (...) {
                    std::terminate();
                }
            }
            throw;
        }
    }

    std::optional<SceneLinearRgbFrame> linear_raster(std::optional<std::uint32_t> edge) const override {
        return access([edge](DecodeSession& session) -> std::optional<SceneLinearRgbFrame> {
            const auto* linear = dynamic_cast<const LinearRasterSource*>(&session);
            return linear ? linear->linear_raster(edge) : std::nullopt;
        });
    }

    ~IsolatedDecodeSession() override {
        // A private provider/session destructor can call into the same vendor SDK as render().
        // Keep that teardown inside the module domain as well. Destructors cannot report a lock
        // failure safely, so an impossible gate invariant terminates rather than destroying an
        // SDK object outside its declared isolation boundary.
        try {
            std::lock_guard session_lock(session_gate_);
            if (provider_gate_.has_value()) {
                provider_gate_->synchronize([this] { session_.reset(); });
            } else {
                session_.reset();
            }
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] const AssetMetadata& metadata() const noexcept override {
        return metadata_;
    }

    [[nodiscard]] const DecodeCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }

    [[nodiscard]] std::span<const PreviewDescriptor> previews() const noexcept override {
        return previews_;
    }

    [[nodiscard]] const RawDevelopmentCapabilities& raw_development_capabilities() const noexcept
        override {
        return raw_development_capabilities_;
    }

    [[nodiscard]] RawDevelopmentPlanNegotiation negotiate_raw_development_plan(
        const RawDevelopmentPlan& plan
    ) const noexcept override {
        // Provider contracts make this query noexcept. It still enters the session isolation
        // domain because a private adapter may implement custom adjustment semantics rather than
        // the generic capability helper. A system-level mutex failure therefore follows the
        // method's existing noexcept contract and terminates instead of running outside the gate.
        return access([&plan](DecodeSession& session) {
            return session.negotiate_raw_development_plan(plan);
        });
    }

    [[nodiscard]] PreviewPayload decode_preview(const std::size_t id) override {
        return access([id](DecodeSession& session) { return session.decode_preview(id); });
    }

    [[nodiscard]] RawFrame decode_raw_frame() override {
        return access([](DecodeSession& session) { return session.decode_raw_frame(); });
    }

    [[nodiscard]] PixelBuffer render_reference_rgb() const override {
        return access([](DecodeSession& session) { return session.render_reference_rgb(); });
    }

    [[nodiscard]] PixelBuffer render_reference_rgb(
        const RawDevelopmentPlan& plan
    ) const override {
        return access([&plan](DecodeSession& session) {
            return session.render_reference_rgb(plan);
        });
    }

    [[nodiscard]] PixelBuffer render_reference_rgb_for_preview(
        const std::uint32_t max_edge
    ) const override {
        return access([max_edge](DecodeSession& session) {
            return session.render_reference_rgb_for_preview(max_edge);
        });
    }

    [[nodiscard]] PixelBuffer render_reference_rgb_for_preview(
        const std::uint32_t max_edge,
        const RawDevelopmentPlan& plan
    ) const override {
        return access([max_edge, &plan](DecodeSession& session) {
            return session.render_reference_rgb_for_preview(max_edge, plan);
        });
    }

    [[nodiscard]] bool uses_shared_provider_gate() const noexcept {
        return provider_gate_.has_value();
    }

private:
    std::unique_ptr<DecodeSession> session_;
    std::optional<SharedDecodeProviderGate> provider_gate_;
    mutable std::mutex session_gate_;
    AssetMetadata metadata_;
    DecodeCapabilities capabilities_;
    std::vector<PreviewDescriptor> previews_;
    RawDevelopmentCapabilities raw_development_capabilities_;
};

} // namespace

SharedDecodeProviderGate shared_decode_provider_gate(const std::string_view identity) {
    if (identity.empty()) {
        throw std::invalid_argument("decode provider gate identity cannot be empty");
    }

    static std::mutex registry_mutex;
    static std::unordered_map<
        std::string,
        std::weak_ptr<SharedDecodeProviderGate::State>
    > registry;

    std::lock_guard registry_lock(registry_mutex);
    for (auto iterator = registry.begin(); iterator != registry.end();) {
        if (iterator->second.expired()) {
            iterator = registry.erase(iterator);
        } else {
            ++iterator;
        }
    }

    const std::string key(identity);
    auto& weak_state = registry[key];
    auto state = weak_state.lock();
    if (state == nullptr) {
        state = std::make_shared<SharedDecodeProviderGate::State>();
        weak_state = state;
    }
    return SharedDecodeProviderGate(std::move(state));
}

std::unique_ptr<DecodeSession> isolate_decode_session(
    std::unique_ptr<DecodeSession> session
) {
    return std::make_unique<IsolatedDecodeSession>(std::move(session), std::nullopt);
}

std::unique_ptr<DecodeSession> isolate_decode_session(
    std::unique_ptr<DecodeSession> session,
    SharedDecodeProviderGate provider_gate
) {
    try {
        return std::make_unique<IsolatedDecodeSession>(
            std::move(session),
            provider_gate
        );
    } catch (...) {
        if (session != nullptr) {
            try {
                provider_gate.synchronize([&session] { session.reset(); });
            } catch (...) {
                std::terminate();
            }
        }
        throw;
    }
}

bool decode_session_uses_shared_provider_gate(const DecodeSession& session) noexcept {
    const auto* isolated = dynamic_cast<const IsolatedDecodeSession*>(&session);
    return isolated != nullptr && isolated->uses_shared_provider_gate();
}

} // namespace shadow::image::detail
