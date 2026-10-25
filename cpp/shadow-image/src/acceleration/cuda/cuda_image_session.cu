#include "../../edit/edit_execution_validation.hpp"
#include "cuda_image_session.hpp"
#include "cuda_shared_math.cuh"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace shader = shadow_cuda_shader;
namespace shadow::image::detail {
namespace {
constexpr std::size_t session_budget = 768U * 1024U * 1024U;
constexpr std::size_t resource_budget = 64U * 1024U * 1024U;

void checked(cudaError_t result) {
    if (result != cudaSuccess) {
        throw std::runtime_error(std::string("CUDA: ") + cudaGetErrorString(result));
    }
}

class DeviceBuffer final {
  public:
    void* data = nullptr;
    std::size_t capacity = 0;
    std::vector<std::byte> cached;
    DeviceBuffer() = default;
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
    ~DeviceBuffer() {
        if (data)
            cudaFree(data);
    }
    bool reserve(std::size_t size) {
        if (size <= capacity)
            return false;
        void* replacement = nullptr;
        checked(cudaMalloc(&replacement, size));
        if (data)
            cudaFree(data);
        data = replacement;
        capacity = size;
        return true;
    }
    template <typename T> T* as() const {
        return static_cast<T*>(data);
    }
};

struct Stream final {
    cudaStream_t value = nullptr;
    Stream() {
        checked(cudaStreamCreateWithFlags(&value, cudaStreamNonBlocking));
    }
    ~Stream() {
        if (value) {
            cudaStreamSynchronize(value);
            cudaStreamDestroy(value);
        }
    }
};

__global__ void adjustment_kernel(
    float* pixels,
    shader::MetalAdjustmentInvocation invocation,
    const shader::MetalAdjustmentOp* operations,
    const shader::MetalCurveSegment* curves,
    const shader::float4* luts,
    const shader::float4* mixers,
    const shader::MetalPerceptualRange* ranges,
    const shader::float4* selective,
    const shader::float4* paint,
    const unsigned char* completion,
    shader::MetalAdjustmentStatus* status,
    float opacity
) {
    const unsigned int x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= invocation.width || y >= invocation.height)
        return;
    const std::size_t offset = std::size_t(y) * invocation.input_row_floats + x * 3U;
    const shader::float3 before(pixels[offset], pixels[offset + 1], pixels[offset + 2]);
    auto rgb = before;
    if (!shader::execute_adjustment_program(
            rgb,
            operations,
            curves,
            luts,
            mixers,
            ranges,
            selective,
            paint,
            completion,
            shader::uint2(x, y),
            invocation,
            *status
        ))
        return;
    rgb = shader::mix(before, rgb, opacity);
    if (!shader::all(shader::isfinite(rgb))) {
        shader::report_adjustment_failure(*status, 1U, 0U);
        return;
    }
    pixels[offset] = rgb.x;
    pixels[offset + 1] = rgb.y;
    pixels[offset + 2] = rgb.z;
}

__global__ void display_kernel(
    const float* linear,
    unsigned char* output,
    unsigned int width,
    unsigned int height,
    unsigned int row_floats,
    unsigned int origin_x,
    unsigned int origin_y,
    bool scene_curve
) {
    const unsigned int x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height)
        return;
    const std::size_t in = std::size_t(y) * row_floats + x * 3U;
    const std::size_t out = (std::size_t(y) * width + x) * 3U;
    const auto rgb = shader::map_display_gamut(
        shader::float3(linear[in], linear[in + 1], linear[in + 2]),
        scene_curve
    );
    const float dither = shader::display_dither(x + origin_x, y + origin_y);
    output[out] = shader::encode_srgb8(rgb.x, dither);
    output[out + 1] = shader::encode_srgb8(rgb.y, dither);
    output[out + 2] = shader::encode_srgb8(rgb.z, dither);
}

static_assert(sizeof(shader::MetalAdjustmentInvocation) == sizeof(MetalAdjustmentInvocation));
static_assert(sizeof(shader::MetalAdjustmentOp) == sizeof(MetalAdjustmentOp));
static_assert(sizeof(shader::MetalCurveSegment) == sizeof(MetalCurveSegment));
static_assert(sizeof(shader::MetalPerceptualRange) == sizeof(MetalPerceptualRange));

FloatRgbImage source_layout(const FloatRgbImage& source) {
    return FloatRgbImage{
        .dimensions = source.dimensions,
        .row_stride_bytes = source.row_stride_bytes,
        .pixel_format = source.pixel_format,
        .transfer_function = source.transfer_function,
        .reference = source.reference,
        .working_space = source.working_space,
        .level_zero_to_raster_scale_x = source.level_zero_to_raster_scale_x,
        .level_zero_to_raster_scale_y = source.level_zero_to_raster_scale_y,
    };
}
} // namespace

struct CudaImageSession::Impl final {
    struct Slot final {
        std::mutex mutex;
        DeviceBuffer pixels, display, status;
        std::array<DeviceBuffer, 8> resources;
        // Declared last so outstanding transfers finish before the buffers are destroyed.
        Stream stream;
    };
    int device = 0;
    FloatRgbImage layout;
    DeviceBuffer source;
    std::array<std::unique_ptr<Slot>, 2> slots;
    mutable std::atomic<unsigned int> next_slot{0};
    mutable std::mutex stats_mutex, host_mutex;
    mutable WarmEditPreviewGpuStats statistics;
    mutable std::uint64_t active_renders = 0;
    mutable std::shared_ptr<const FloatRgbImage> materialized;

    explicit Impl(const FloatRgbImage& input) : layout(source_layout(input)) {
        validate_edit_image(input);
        const auto close = [](double actual, double expected) {
            return std::abs(actual - expected) <= 1.0e-9;
        };
        const auto& space = input.working_space;
        if (!close(space.primaries[0].x, 0.640) || !close(space.primaries[0].y, 0.330)
            || !close(space.primaries[1].x, 0.300) || !close(space.primaries[1].y, 0.600)
            || !close(space.primaries[2].x, 0.150) || !close(space.primaries[2].y, 0.060)
            || !close(space.white_point.x, 0.3127) || !close(space.white_point.y, 0.3290)) {
            throw std::runtime_error("CUDA preview requires the declared linear-sRGB working space"
            );
        }
        const auto bytes = input.samples.size() * sizeof(float);
        const auto display_bytes =
            std::size_t(input.dimensions.width) * input.dimensions.height * 3U;
        // Source, two float workspaces, RGB8 outputs, and one lazy host replay are retained.
        if (bytes > (session_budget - 4U * resource_budget - 32U) / 5U
            || input.row_stride_bytes / sizeof(float) > std::numeric_limits<unsigned int>::max()) {
            throw std::runtime_error("CUDA warm source exceeds the bounded session budget");
        }
        int devices = 0;
        checked(cudaGetDeviceCount(&devices));
        if (devices == 0)
            throw std::runtime_error("CUDA has no usable NVIDIA device");
        checked(cudaGetDevice(&device));
        source.reserve(bytes);
        checked(cudaMemcpy(source.data, input.samples.data(), bytes, cudaMemcpyHostToDevice));
        statistics = {
            .resident = true,
            .source_upload_count = 1,
            .gpu_buffer_allocation_count = 1,
            .resident_bytes = bytes
        };
        for (auto& slot : slots) {
            slot = std::make_unique<Slot>();
            slot->pixels.reserve(bytes);
            slot->display.reserve(display_bytes);
            slot->status.reserve(sizeof(shader::MetalAdjustmentStatus));
            statistics.gpu_buffer_allocation_count += 3;
            statistics.resident_bytes +=
                bytes + display_bytes + sizeof(shader::MetalAdjustmentStatus);
        }
    }

    template <typename T>
    void upload(Slot& slot, std::size_t index, const std::vector<T>& values) const {
        auto& buffer = slot.resources[index];
        const auto bytes = values.size() * sizeof(T);
        if (bytes == buffer.cached.size()
            && (bytes == 0 || std::memcmp(values.data(), buffer.cached.data(), bytes) == 0)) {
            std::lock_guard lock(stats_mutex);
            ++statistics.resource_cache_hit_count;
            return;
        }
        const auto old_capacity = buffer.capacity;
        std::size_t retained = std::max(bytes, old_capacity);
        for (std::size_t i = 0; i < slot.resources.size(); ++i) {
            if (i != index)
                retained += slot.resources[i].capacity;
        }
        if (retained > resource_budget) {
            throw std::runtime_error("CUDA retained adjustment resources exceed budget");
        }
        const bool allocated = buffer.reserve(bytes);
        buffer.cached.resize(bytes);
        if (bytes) {
            // Pageable input is retained until stream completion by the caller's stage vector.
            checked(cudaMemcpyAsync(
                buffer.data,
                values.data(),
                bytes,
                cudaMemcpyHostToDevice,
                slot.stream.value
            ));
            std::memcpy(buffer.cached.data(), values.data(), bytes);
        }
        std::lock_guard lock(stats_mutex);
        statistics.resident_bytes += buffer.capacity - old_capacity;
        statistics.gpu_buffer_allocation_count += allocated ? 1U : 0U;
        if (index == 1)
            ++statistics.curve_resource_upload_count;
        if (index == 2)
            ++statistics.lut_resource_upload_count;
        if (index == 3)
            ++statistics.perceptual_mixer_resource_upload_count;
        if (index == 4)
            ++statistics.perceptual_range_resource_upload_count;
        if (index == 5)
            ++statistics.selective_color_resource_upload_count;
    }
};

CudaImageSession::CudaImageSession(const FloatRgbImage& source) :
    impl_(std::make_unique<Impl>(source)) {}
CudaImageSession::~CudaImageSession() {
    if (impl_)
        cudaSetDevice(impl_->device);
}
const FloatRgbImage& CudaImageSession::layout() const noexcept {
    return impl_->layout;
}

std::optional<CudaRenderOutput> CudaImageSession::render(
    std::span<const CudaAdjustmentStage> stages,
    bool retain_linear,
    std::uint32_t origin_x,
    std::uint32_t origin_y,
    std::stop_token cancellation
) const {
    if (cancellation.stop_requested())
        return std::nullopt;
    checked(cudaSetDevice(impl_->device));
    auto& slot = *impl_->slots[impl_->next_slot.fetch_add(1) % impl_->slots.size()];
    std::lock_guard slot_lock(slot.mutex);
    if (cancellation.stop_requested())
        return std::nullopt;
    const auto& input = impl_->layout;
    const dim3 threads(16, 16),
        blocks((input.dimensions.width + 15U) / 16U, (input.dimensions.height + 15U) / 16U);
    const std::array<unsigned int, 4> clear_status{0U, ~0U, 0U, 0U};
    // These host buffers outlive the catch handler's stream drain, including transfer failures.
    CudaRenderOutput output;
    std::array<unsigned int, 4> status{};
    {
        std::lock_guard lock(impl_->stats_mutex);
        ++impl_->statistics.render_count;
        ++impl_->active_renders;
        impl_->statistics.peak_concurrent_renders =
            std::max(impl_->statistics.peak_concurrent_renders, impl_->active_renders);
    }
    struct RenderScope final {
        Impl& impl;
        ~RenderScope() {
            std::lock_guard lock(impl.stats_mutex);
            --impl.active_renders;
        }
    } render_scope{*impl_};
    try {
        checked(cudaMemcpyAsync(
            slot.status.data,
            clear_status.data(),
            sizeof(clear_status),
            cudaMemcpyHostToDevice,
            slot.stream.value
        ));
        checked(cudaMemcpyAsync(
            slot.pixels.data,
            impl_->source.data,
            impl_->source.capacity,
            cudaMemcpyDeviceToDevice,
            slot.stream.value
        ));
        for (const auto& stage : stages) {
            const auto& p = stage.program;
            const auto bytes =
                p.operations.size() * sizeof(MetalAdjustmentOp)
                + p.curve_segments.size() * sizeof(MetalCurveSegment)
                + p.lut_entries.size() * sizeof(MetalLutEntry)
                + p.perceptual_mixer_entries.size() * sizeof(MetalPerceptualMixerEntry)
                + p.perceptual_range_entries.size() * sizeof(MetalPerceptualRange)
                + p.selective_color_entries.size() * sizeof(MetalSelectiveColorEntry)
                + p.paint_entries.size() * sizeof(MetalPaintPixel) + p.completion_bytes.size();
            if (bytes > resource_budget)
                throw std::runtime_error("CUDA adjustment resources exceed budget");
            impl_->upload(slot, 0, p.operations);
            impl_->upload(slot, 1, p.curve_segments);
            impl_->upload(slot, 2, p.lut_entries);
            impl_->upload(slot, 3, p.perceptual_mixer_entries);
            impl_->upload(slot, 4, p.perceptual_range_entries);
            impl_->upload(slot, 5, p.selective_color_entries);
            impl_->upload(slot, 6, p.paint_entries);
            impl_->upload(slot, 7, p.completion_bytes);
            shader::MetalAdjustmentInvocation invocation;
            std::memcpy(&invocation, &p.invocation, sizeof(invocation));
            // The shared lowering describes a packed Metal upload. CUDA retains the original
            // padded layout, so its in-place kernel must use the resident row stride.
            invocation.input_row_floats =
                static_cast<unsigned int>(input.row_stride_bytes / sizeof(float));
            invocation.output_row_floats = invocation.input_row_floats;
            adjustment_kernel<<<blocks, threads, 0, slot.stream.value>>>(
                slot.pixels.as<float>(),
                invocation,
                slot.resources[0].as<shader::MetalAdjustmentOp>(),
                slot.resources[1].as<shader::MetalCurveSegment>(),
                slot.resources[2].as<shader::float4>(),
                slot.resources[3].as<shader::float4>(),
                slot.resources[4].as<shader::MetalPerceptualRange>(),
                slot.resources[5].as<shader::float4>(),
                slot.resources[6].as<shader::float4>(),
                slot.resources[7].as<unsigned char>(),
                slot.status.as<shader::MetalAdjustmentStatus>(),
                stage.opacity
            );
            checked(cudaGetLastError());
        }
        display_kernel<<<blocks, threads, 0, slot.stream.value>>>(
            slot.pixels.as<float>(),
            slot.display.as<unsigned char>(),
            input.dimensions.width,
            input.dimensions.height,
            static_cast<unsigned int>(input.row_stride_bytes / sizeof(float)),
            origin_x,
            origin_y,
            input.reference == ImageReference::scene_referred
        );
        checked(cudaGetLastError());
        output.rgb8.resize(slot.display.capacity);
        checked(cudaMemcpyAsync(
            status.data(),
            slot.status.data,
            sizeof(status),
            cudaMemcpyDeviceToHost,
            slot.stream.value
        ));
        checked(cudaMemcpyAsync(
            output.rgb8.data(),
            slot.display.data,
            output.rgb8.size(),
            cudaMemcpyDeviceToHost,
            slot.stream.value
        ));
        if (retain_linear) {
            output.linear = input;
            output.linear->samples.resize(impl_->source.capacity / sizeof(float));
            checked(cudaMemcpyAsync(
                output.linear->samples.data(),
                slot.pixels.data,
                impl_->source.capacity,
                cudaMemcpyDeviceToHost,
                slot.stream.value
            ));
        }
        checked(cudaStreamSynchronize(slot.stream.value));
        if (cancellation.stop_requested())
            return std::nullopt;
        if (status[0])
            throw std::runtime_error("CUDA rejected non-finite pixels or an invalid shader resource"
            );
        std::lock_guard lock(impl_->stats_mutex);
        ++impl_->statistics.completed_render_count;
        return output;
    } catch (...) {
        cudaStreamSynchronize(slot.stream.value);
        // A failed transfer must not be treated as a reusable resource on the next render.
        for (auto& resource : slot.resources)
            resource.cached.clear();
        throw;
    }
}

std::shared_ptr<const FloatRgbImage> CudaImageSession::host_source() const {
    std::lock_guard lock(impl_->host_mutex);
    if (!impl_->materialized) {
        checked(cudaSetDevice(impl_->device));
        auto image = std::make_shared<FloatRgbImage>(impl_->layout);
        image->samples.resize(impl_->source.capacity / sizeof(float));
        checked(cudaMemcpy(
            image->samples.data(),
            impl_->source.data,
            impl_->source.capacity,
            cudaMemcpyDeviceToHost
        ));
        impl_->materialized = std::move(image);
        std::lock_guard stats_lock(impl_->stats_mutex);
        impl_->statistics.resident_bytes += impl_->source.capacity;
    }
    return impl_->materialized;
}
WarmEditPreviewGpuStats CudaImageSession::stats() const noexcept {
    std::lock_guard lock(impl_->stats_mutex);
    return impl_->statistics;
}
} // namespace shadow::image::detail
