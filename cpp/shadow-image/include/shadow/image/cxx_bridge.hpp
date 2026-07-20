#pragma once

#include "rust/cxx.h"

namespace shadow::bridge {
class DecodeHandle;
class EditPreviewHandle;
class FullEditDetailHandle;
}

#include "shadow-bridge/src/lib.rs.h"

#include <shadow/image/decoder.hpp>
#include <shadow/image/display_luma.hpp>
#include <shadow/image/edit.hpp>

#include <memory>

namespace shadow::bridge {

class DecodeHandle final {
public:
    DecodeHandle(
        std::unique_ptr<image::DecoderProvider> provider,
        std::unique_ptr<image::DecodeSession> session
    );
    ~DecodeHandle();

    DecodeHandle(const DecodeHandle&) = delete;
    DecodeHandle& operator=(const DecodeHandle&) = delete;

    [[nodiscard]] FfiProviderSnapshot provider() const;
    [[nodiscard]] FfiMetadataSnapshot metadata() const;
    [[nodiscard]] FfiCapabilitySnapshot capabilities() const;
    [[nodiscard]] rust::Vec<FfiPreviewSnapshot> previews() const;
    [[nodiscard]] FfiPreviewPayload decode_best_preview();
    [[nodiscard]] FfiEncodedProxy render_reference_proxy(
        std::uint32_t max_edge,
        std::uint8_t jpeg_quality
    ) const;
    [[nodiscard]] FfiEncodedProxy render_adjustment_plan(
        const FfiAdjustmentRenderRequest& request
    ) const;
    [[nodiscard]] std::unique_ptr<EditPreviewHandle> prepare_edit_preview(
        std::uint32_t max_edge
    ) const;
    [[nodiscard]] std::unique_ptr<FullEditDetailHandle> prepare_edit_detail() const;

private:
    std::unique_ptr<image::DecoderProvider> provider_;
    std::unique_ptr<image::DecodeSession> session_;
};

// Unlike DecodeHandle, this handle no longer owns or references a decoder. Its working proxy
// is immutable after preparation and render_adjustment_plan() uses only call-local state, so const
// calls may safely run concurrently on different worker threads.
class EditPreviewHandle final {
public:
    explicit EditPreviewHandle(image::WarmEditPreviewSession session);
    ~EditPreviewHandle();

    EditPreviewHandle(const EditPreviewHandle&) = delete;
    EditPreviewHandle& operator=(const EditPreviewHandle&) = delete;

    [[nodiscard]] FfiDimensions dimensions() const noexcept;
    [[nodiscard]] std::uint32_t max_edge() const noexcept;
    [[nodiscard]] FfiEncodedProxy render_adjustment_plan(
        const FfiAdjustmentRenderRequest& request
    ) const;

private:
    image::WarmEditPreviewSession session_;
};

// The complete retained source is immutable and contains no decoder. Every tile render owns its
// float working buffer and packed RGB8 result, so const calls may safely run concurrently.
class FullEditDetailHandle final {
public:
    explicit FullEditDetailHandle(image::FullEditDetailSession session);
    ~FullEditDetailHandle();

    FullEditDetailHandle(const FullEditDetailHandle&) = delete;
    FullEditDetailHandle& operator=(const FullEditDetailHandle&) = delete;

    [[nodiscard]] FfiDimensions dimensions() const noexcept;
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept;
    [[nodiscard]] FfiRenderedDetailTile render_adjustment_plan_tile(
        const FfiAdjustmentDetailTileRequest& request
    ) const;

private:
    image::FullEditDetailSession session_;
};

[[nodiscard]] std::unique_ptr<DecodeHandle> open_libraw_utf8(rust::Str path);
[[nodiscard]] rust::String libraw_provider_version();
[[nodiscard]] FfiDisplayLuma decode_jpeg_display_luma(
    rust::Slice<const std::uint8_t> encoded,
    std::uint32_t max_edge
);

} // namespace shadow::bridge
