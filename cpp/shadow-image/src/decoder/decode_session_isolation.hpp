#pragma once

#include <shadow/image/decoder.hpp>

#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace shadow::image::detail {

// Private decoder adapters may wrap an SDK with process-global state. A shared provider gate
// serializes only sessions created by the same module identity; public reentrant providers never
// receive one. Every routed session is independently protected by isolate_decode_session(), so
// callers also cannot enter one LibRaw/libjpeg/private session concurrently by accident.
class SharedDecodeProviderGate final {
public:
    SharedDecodeProviderGate(const SharedDecodeProviderGate&) = default;
    SharedDecodeProviderGate& operator=(const SharedDecodeProviderGate&) = default;
    SharedDecodeProviderGate(SharedDecodeProviderGate&&) noexcept = default;
    SharedDecodeProviderGate& operator=(SharedDecodeProviderGate&&) noexcept = default;

    template <typename Callback>
    decltype(auto) synchronize(Callback&& callback) const {
        if (state_ == nullptr) {
            throw std::logic_error("decode provider gate has no shared state");
        }
        std::lock_guard lock(state_->mutex);
        return std::invoke(std::forward<Callback>(callback));
    }

private:
    struct State final {
        std::mutex mutex;
    };

    explicit SharedDecodeProviderGate(std::shared_ptr<State> state)
        : state_(std::move(state)) {}

    std::shared_ptr<State> state_;

    friend SharedDecodeProviderGate shared_decode_provider_gate(std::string_view identity);
};

// Repeated short-lived routers use the same gate while any provider/session for this identity
// remains alive. The identity must be the canonical module location, not a vendor or camera name:
// separate installed modules must not block each other.
[[nodiscard]] SharedDecodeProviderGate shared_decode_provider_gate(std::string_view identity);

// Public LibRaw/libjpeg/libheif sessions use this overload: their independent native contexts
// can run in parallel, while accidental concurrent calls on the same session remain serialized.
[[nodiscard]] std::unique_ptr<DecodeSession> isolate_decode_session(
    std::unique_ptr<DecodeSession> session
);

// Private provider sessions additionally share their module gate across router instances.
[[nodiscard]] std::unique_ptr<DecodeSession> isolate_decode_session(
    std::unique_ptr<DecodeSession> session,
    SharedDecodeProviderGate provider_gate
);

// Internal contract-test observation. Production routing never branches on this after creation.
[[nodiscard]] bool decode_session_uses_shared_provider_gate(
    const DecodeSession& session
) noexcept;

} // namespace shadow::image::detail
