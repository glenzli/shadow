#pragma once

#include <shadow/image/decoder_metadata.hpp>
#include <shadow/image/decoder_types.hpp>
#include <shadow/image/raw_development_plan.hpp>
#include <shadow/image/raw_frame.hpp>
#include <shadow/image/reference_pixels.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace shadow::image {

/// One opened source and the provider-owned caches needed to decode it.
class DecodeSession {
public:
    DecodeSession() = default;
    DecodeSession(const DecodeSession&) = delete;
    DecodeSession& operator=(const DecodeSession&) = delete;
    DecodeSession(DecodeSession&&) = delete;
    DecodeSession& operator=(DecodeSession&&) = delete;
    virtual ~DecodeSession() = default;

    [[nodiscard]] virtual const AssetMetadata& metadata() const noexcept = 0;
    [[nodiscard]] virtual const DecodeCapabilities& capabilities() const noexcept = 0;
    [[nodiscard]] virtual std::span<const PreviewDescriptor> previews() const noexcept = 0;
    [[nodiscard]] virtual PreviewPayload decode_preview(std::size_t id) = 0;
    [[nodiscard]] virtual RawFrame decode_raw_frame() = 0;
    // RawFrame extraction is logically a source render. Existing provider ABIs expose a
    // non-const entry because unpacking may populate private decoder caches; the host-facing
    // const overload preserves the immutable DecodeSession API while delegating to that cache.
    // Providers should keep all externally observable metadata/capabilities unchanged.
    [[nodiscard]] virtual RawFrame decode_raw_frame() const {
        return const_cast<DecodeSession*>(this)->decode_raw_frame();
    }
    [[nodiscard]] virtual PixelBuffer render_reference_rgb() const = 0;

    // RAW providers advertise their exact source-development contract here. Rendered-raster
    // providers intentionally leave it unavailable: JPEG/HEIF still share the edit graph, but a
    // RAW plan has no hidden effect on their already-developed pixels.
    [[nodiscard]] virtual const RawDevelopmentCapabilities& raw_development_capabilities() const
        noexcept {
        return capabilities().raw_development;
    }

    [[nodiscard]] virtual RawDevelopmentPlanNegotiation negotiate_raw_development_plan(
        const RawDevelopmentPlan& plan
    ) const noexcept {
        return shadow::image::negotiate_raw_development_plan(
            plan,
            raw_development_capabilities()
        );
    }

    // These overloads keep existing third-party and test sessions source-compatible. A provider
    // that does not opt into RawDevelopmentCapabilities simply preserves its legacy render path;
    // callers can observe the unavailable negotiation result and no RAW receipt is fabricated.
    // Plan-aware RAW providers must override them and record the requested/effective plan in the
    // returned RawDevelopmentReceipt.
    [[nodiscard]] virtual PixelBuffer render_reference_rgb(
        const RawDevelopmentPlan& plan
    ) const {
        (void)plan;
        return render_reference_rgb();
    }

    // Interactive preview is allowed to ask a provider for a bounded-quality reference. The
    // default keeps third-party/provider test implementations exact; LibRaw overrides it with
    // its documented half-size RAW path only when the native frame is far larger than the
    // requested preview. Full-detail rendering always calls render_reference_rgb().
    [[nodiscard]] virtual PixelBuffer render_reference_rgb_for_preview(
        std::uint32_t max_edge
    ) const {
        (void)max_edge;
        return render_reference_rgb();
    }

    [[nodiscard]] virtual PixelBuffer render_reference_rgb_for_preview(
        const std::uint32_t max_edge,
        const RawDevelopmentPlan& plan
    ) const {
        (void)plan;
        return render_reference_rgb_for_preview(max_edge);
    }
};

/// Opens sources for one concrete decoder implementation.
class DecoderProvider {
public:
    DecoderProvider() = default;
    DecoderProvider(const DecoderProvider&) = delete;
    DecoderProvider& operator=(const DecoderProvider&) = delete;
    DecoderProvider(DecoderProvider&&) = delete;
    DecoderProvider& operator=(DecoderProvider&&) = delete;
    virtual ~DecoderProvider() = default;

    [[nodiscard]] virtual const ProviderInfo& info() const noexcept = 0;
    [[nodiscard]] virtual std::unique_ptr<DecodeSession> open(
        const std::filesystem::path& path
    ) const = 0;
};

[[nodiscard]] std::unique_ptr<DecoderProvider> make_libraw_decoder_provider(
    LibRawDevelopmentSettings settings = default_libraw_development_settings()
);

// Decodes ordinary display-referred image files into Shadow's common 16-bit linear sRGB
// reference contract. JPEG is built in through libjpeg-turbo; HEIF/HEIC availability is an
// optional backend capability and is reported as an explicit unsupported error when omitted
// from a local build. This provider never pretends to expose sensor RawFrames or RAW development
// provenance.
[[nodiscard]] std::unique_ptr<DecoderProvider> make_raster_decoder_provider();

// File extensions that the raster provider in this binary can actually decode. This is a
// capability query rather than a broad format claim: the catalog must not schedule HEIF simply
// because it recognizes the suffix when this build deliberately omitted libheif.
[[nodiscard]] std::vector<std::string> raster_supported_file_extensions();

// Routes a path to the appropriate public provider. It is intentionally the desktop application's
// normal entry point: a catalog item should not need to know whether it was shot as RAW or
// delivered as JPEG/HEIF in order to reach the common non-destructive edit graph. If
// `SHADOW_PRIVATE_DECODER_PLUGIN_PATH` names an explicit local module, the router tries that
// module first for non-raster files and falls back to LibRaw only when the module declares the
// source unsupported. Without this temporary override, the router discovers v1 link files in
// the per-user plugin root (`~/Library/Application Support/Shadow/plugins/decoders` on macOS;
// platform equivalents elsewhere). A link names a locally compiled private module; neither its
// SDK nor its implementation enters the Shadow repository or catalog.
[[nodiscard]] std::unique_ptr<DecoderProvider> make_photo_decoder_provider();

// Explicit-test and embedding form of the normal router. An empty path is exactly equivalent to
// the environment/discovered form above. The path is never scanned, copied, persisted, or
// distributed by Shadow; it is only passed to the local private-plugin loader for this provider
// instance.
[[nodiscard]] std::unique_ptr<DecoderProvider> make_photo_decoder_provider(
    const std::filesystem::path& private_decoder_plugin_path
);

} // namespace shadow::image
