#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <shadow/image/color_management.hpp>
#include <shadow/image/linear_raster.hpp>
#include <stdexcept>
#include <tiffio.h>
namespace shadow::image {
namespace {
constexpr const char* identity = "shadow-linear-srgb-f32-v1";
constexpr std::uint64_t max_pixels = 64ULL * 1024 * 1024;
using Tiff = std::unique_ptr<TIFF, decltype(&TIFFClose)>;
Tiff open(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
    Tiff t(TIFFOpenW(path.c_str(), mode), TIFFClose);
#else
    Tiff t(TIFFOpen(path.c_str(), mode), TIFFClose);
#endif
    if (!t)
        throw std::runtime_error("Cannot open linear TIFF");
    return t;
}
Dimensions header(TIFF* t) {
    uint32_t w = 0, h = 0;
    uint16_t bits = 0, channels = 0, format = 0, photo = 0, planar = 0, orientation = 0;
    char* software = nullptr;
    TIFFGetField(t, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(t, TIFFTAG_IMAGELENGTH, &h);
    TIFFGetFieldDefaulted(t, TIFFTAG_BITSPERSAMPLE, &bits);
    TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLESPERPIXEL, &channels);
    TIFFGetFieldDefaulted(t, TIFFTAG_SAMPLEFORMAT, &format);
    TIFFGetFieldDefaulted(t, TIFFTAG_PHOTOMETRIC, &photo);
    TIFFGetFieldDefaulted(t, TIFFTAG_PLANARCONFIG, &planar);
    TIFFGetFieldDefaulted(t, TIFFTAG_ORIENTATION, &orientation);
    TIFFGetField(t, TIFFTAG_SOFTWARE, &software);
    if (!w || !h || uint64_t(w) * h > max_pixels || bits != 32 || channels != 3
        || format != SAMPLEFORMAT_IEEEFP || photo != PHOTOMETRIC_RGB
        || planar != PLANARCONFIG_CONTIG || orientation != ORIENTATION_TOPLEFT || TIFFIsTiled(t)
        || !software || std::strcmp(software, identity) != 0
        || TIFFScanlineSize64(t) != uint64_t(w) * 3 * sizeof(float))
        throw std::runtime_error("TIFF is not a supported Shadow linear RGB composite");
    return {w, h};
}
class Session final : public DecodeSession, public LinearRasterSource {
    std::filesystem::path path_;
    AssetMetadata metadata_;
    DecodeCapabilities capabilities_;

  public:
    explicit Session(std::filesystem::path p) : path_(std::move(p)) {
        auto t = open(path_, "r");
        metadata_.image_dimensions = header(t.get());
        metadata_.orientation = 1;
        capabilities_.metadata = true;
        capabilities_.reference_rgb = true;
    }
    const AssetMetadata& metadata() const noexcept override {
        return metadata_;
    }
    const DecodeCapabilities& capabilities() const noexcept override {
        return capabilities_;
    }
    std::span<const PreviewDescriptor> previews() const noexcept override {
        return {};
    }
    PreviewPayload decode_preview(std::size_t) override {
        throw std::runtime_error("Linear TIFF has no embedded preview");
    }
    RawFrame decode_raw_frame() override {
        throw std::runtime_error("A composite is not sensor RAW");
    }
    std::optional<SceneLinearRgbFrame> linear_raster(std::optional<uint32_t> edge) const override {
        return read_linear_tiff(path_, edge);
    }
    PixelBuffer render_reference_rgb() const override {
        return render_reference_rgb_for_preview(0);
    }
    PixelBuffer render_reference_rgb_for_preview(uint32_t edge) const override {
        auto f = read_linear_tiff(path_, edge ? std::optional(edge) : std::nullopt);
        PixelBuffer p;
        p.dimensions = f.dimensions;
        p.bits_per_channel = 16;
        p.channels = 3;
        p.row_stride_bytes = size_t(p.dimensions.width) * 6;
        p.primaries = RgbPrimaries::srgb_rec709_d65;
        p.transfer_function = RgbTransferFunction::linear;
        p.reference = RgbBufferReference::decoded_raster;
        p.samples.reserve(f.samples.size());
        for (float v : f.samples)
            p.samples.push_back(uint16_t(std::lround(std::clamp(double(v), 0.0, 1.0) * 65535)));
        return p; // Compatibility-only; editing uses LinearRasterSource and keeps all headroom.
    }
};
} // namespace
SceneLinearRgbFrame
read_linear_tiff(const std::filesystem::path& path, std::optional<uint32_t> edge) {
    auto t = open(path, "r");
    const auto d = header(t.get());
    if (edge && *edge == 0)
        throw std::invalid_argument("Invalid linear TIFF preview edge");
    double scale = edge ? std::min(1.0, double(*edge) / std::max(d.width, d.height)) : 1.0;
    Dimensions out{
        std::max(1U, uint32_t(std::lround(d.width * scale))),
        std::max(1U, uint32_t(std::lround(d.height * scale)))
    };
    SceneLinearRgbFrame f{
        out,
        size_t(out.width) * 3 * sizeof(float),
        std::vector<float>(size_t(out.width) * out.height * 3, 0)
    };
    // Stream rows and integrate each exact footprint. No full-resolution intermediate allocation.
    std::vector<float> row(size_t(d.width) * 3);
    for (uint32_t sy = 0; sy < d.height; ++sy) {
        if (TIFFReadScanline(t.get(), row.data(), sy, 0) < 0)
            throw std::runtime_error("Truncated linear TIFF");
        for (float v : row)
            if (!std::isfinite(v))
                throw std::runtime_error("Non-finite linear TIFF samples");
        const double y0 = double(sy) * out.height / d.height,
                     y1 = double(sy + 1) * out.height / d.height;
        for (uint32_t oy = uint32_t(y0); oy < std::min(out.height, uint32_t(std::ceil(y1))); ++oy) {
            const double wy = std::min(y1, double(oy + 1)) - std::max(y0, double(oy));
            for (uint32_t sx = 0; sx < d.width; ++sx) {
                const double x0 = double(sx) * out.width / d.width,
                             x1 = double(sx + 1) * out.width / d.width;
                for (uint32_t ox = uint32_t(x0); ox < std::min(out.width, uint32_t(std::ceil(x1)));
                     ++ox) {
                    float weight =
                        float(wy * (std::min(x1, double(ox + 1)) - std::max(x0, double(ox))));
                    for (size_t c = 0; c < 3; ++c)
                        f.samples[(size_t(oy) * out.width + ox) * 3 + c] +=
                            row[size_t(sx) * 3 + c] * weight;
                }
            }
        }
    }
    return f;
}
void write_linear_tiff(
    const std::filesystem::path& path,
    const SceneLinearRgbFrame& f,
    const std::string& provenance
) {
    if (!f.valid() || f.row_stride_bytes != size_t(f.dimensions.width) * 3 * sizeof(float)
        || uint64_t(f.dimensions.width) * f.dimensions.height > max_pixels
        || provenance.size() > 1024 * 1024)
        throw std::invalid_argument("Invalid composite raster or provenance");
    for (float v : f.samples)
        if (!std::isfinite(v))
            throw std::invalid_argument("Non-finite composite raster");
    auto t = open(path, "w");
    auto set = [&](ttag_t tag, auto value) {
        if (TIFFSetField(t.get(), tag, value) != 1)
            throw std::runtime_error("Cannot configure linear TIFF");
    };
    set(TIFFTAG_IMAGEWIDTH, f.dimensions.width);
    set(TIFFTAG_IMAGELENGTH, f.dimensions.height);
    set(TIFFTAG_BITSPERSAMPLE, 32);
    set(TIFFTAG_SAMPLESPERPIXEL, 3);
    set(TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_IEEEFP);
    set(TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    set(TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    set(TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    set(TIFFTAG_COMPRESSION, COMPRESSION_ADOBE_DEFLATE);
    set(TIFFTAG_PREDICTOR, PREDICTOR_FLOATINGPOINT);
    auto profile = make_linear_srgb_icc_profile();
    auto icc = profile.serialized();
    if (TIFFSetField(t.get(), TIFFTAG_ICCPROFILE, uint32_t(icc.size()), icc.data()) != 1)
        throw std::runtime_error("Cannot embed linear ICC profile");
    set(TIFFTAG_ROWSPERSTRIP, 1);
    set(TIFFTAG_SOFTWARE, identity);
    set(TIFFTAG_IMAGEDESCRIPTION, provenance.c_str());
    for (uint32_t y = 0; y < f.dimensions.height; ++y)
        if (TIFFWriteScanline(
                t.get(),
                const_cast<float*>(f.samples.data() + size_t(y) * f.dimensions.width * 3),
                y,
                0
            )
            < 0)
            throw std::runtime_error("Cannot write linear TIFF");
    if (TIFFFlush(t.get()) != 1)
        throw std::runtime_error("Cannot flush linear TIFF");
}
bool is_shadow_linear_tiff(const std::filesystem::path& path) {
    auto t = open(path, "r");
    char* software = nullptr;
    TIFFGetField(t.get(), TIFFTAG_SOFTWARE, &software);
    return software && std::strcmp(software, identity) == 0;
}
std::unique_ptr<DecodeSession> open_linear_tiff(const std::filesystem::path& path) {
    return std::make_unique<Session>(path);
}
} // namespace shadow::image
