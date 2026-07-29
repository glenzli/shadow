#include "metal_raw_runtime.hpp"

#include "metal_raw_development.hpp"
#include "metal_raw_development_msl.hpp"

#include <limits>
#include <mutex>
#include <string>

namespace shadow::image::detail {

namespace {

[[nodiscard]] std::string error_description(NSError* error) {
    if (error == nil) {
        return {};
    }
    const char* text = [[error localizedDescription] UTF8String];
    return text == nullptr ? std::string{} : std::string(text);
}

class MetalRawContext final {
public:
    MetalRawContext() {
        @autoreleasepool {
            device_ = MTLCreateSystemDefaultDevice();
            if (device_ == nil) {
                diagnostic_ = "no Metal device is available";
                return;
            }
            queue_ = [device_ newCommandQueue];
            if (queue_ == nil) {
                diagnostic_ = "Metal could not create a command queue";
                return;
            }

            OwnedObjectiveCObject compile_options([[MTLCompileOptions alloc] init]);
            auto* options = static_cast<MTLCompileOptions*>(compile_options.get());
            options.mathMode = MTLMathModeSafe;
            NSError* error = nil;
            const std::string kernel_source = metal_raw_kernel_source();
            NSString* source = [NSString stringWithUTF8String:kernel_source.c_str()];
            OwnedObjectiveCObject library(
                [device_ newLibraryWithSource:source options:options error:&error]
            );
            if (!library) {
                diagnostic_ = "Metal RAW shader compilation failed: "
                    + error_description(error);
                return;
            }
            OwnedObjectiveCObject function(
                [static_cast<id<MTLLibrary>>(library.get())
                    newFunctionWithName:@"develop_bayer_full"]
            );
            if (!function) {
                diagnostic_ = "Metal RAW shader entry point is unavailable";
                return;
            }
            pipeline_ = [device_ newComputePipelineStateWithFunction:
                static_cast<id<MTLFunction>>(function.get())
                error:&error];
            if (pipeline_ == nil) {
                diagnostic_ = "Metal RAW pipeline creation failed: "
                    + error_description(error);
                return;
            }

            error = nil;
            OwnedObjectiveCObject resident_function(
                [static_cast<id<MTLLibrary>>(library.get())
                    newFunctionWithName:@"develop_bayer_resident_region"]
            );
            if (!resident_function) {
                resident_diagnostic_ = "Metal resident RAW shader entry point is unavailable";
            } else {
                resident_pipeline_ = [device_ newComputePipelineStateWithFunction:
                    static_cast<id<MTLFunction>>(resident_function.get())
                    error:&error];
                if (resident_pipeline_ == nil) {
                    resident_diagnostic_ = "Metal resident RAW pipeline creation failed: "
                        + error_description(error);
                }
            }

            error = nil;
            OwnedObjectiveCObject preview_function(
                [static_cast<id<MTLLibrary>>(library.get())
                    newFunctionWithName:@"develop_bayer_area_preview"]
            );
            if (!preview_function) {
                preview_diagnostic_ = "Metal RAW area-preview shader entry point is unavailable";
            } else {
                preview_pipeline_ = [device_ newComputePipelineStateWithFunction:
                    static_cast<id<MTLFunction>>(preview_function.get())
                    error:&error];
                if (preview_pipeline_ == nil) {
                    preview_diagnostic_ = "Metal RAW area-preview pipeline creation failed: "
                        + error_description(error);
                }
            }

            error = nil;
            OwnedObjectiveCObject dcp_function(
                [static_cast<id<MTLLibrary>>(library.get())
                    newFunctionWithName:@"develop_dcp_post_matrix"]
            );
            if (!dcp_function) {
                dcp_diagnostic_ = "Metal DCP shader entry point is unavailable";
            } else {
                dcp_pipeline_ = [device_ newComputePipelineStateWithFunction:
                    static_cast<id<MTLFunction>>(dcp_function.get())
                    error:&error];
                if (dcp_pipeline_ == nil) {
                    dcp_diagnostic_ = "Metal DCP pipeline creation failed: "
                        + error_description(error);
                }
            }

            error = nil;
            OwnedObjectiveCObject denoise_function(
                [static_cast<id<MTLLibrary>>(library.get())
                    newFunctionWithName:@"denoise_bayer_same_cfa"]
            );
            if (!denoise_function) {
                denoise_diagnostic_ = "Metal RAW denoise shader entry point is unavailable";
                return;
            }
            denoise_pipeline_ = [device_ newComputePipelineStateWithFunction:
                static_cast<id<MTLFunction>>(denoise_function.get())
                error:&error];
            if (denoise_pipeline_ == nil) {
                denoise_diagnostic_ = "Metal RAW denoise pipeline creation failed: "
                    + error_description(error);
            }
        }
    }

    ~MetalRawContext() {
        [denoise_pipeline_ release];
        [dcp_pipeline_ release];
        [preview_pipeline_ release];
        [resident_pipeline_ release];
        [pipeline_ release];
        [queue_ release];
        [device_ release];
    }

    MetalRawContext(const MetalRawContext&) = delete;
    MetalRawContext& operator=(const MetalRawContext&) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return device_ != nil && queue_ != nil && pipeline_ != nil;
    }

    [[nodiscard]] bool raw_denoise_valid() const noexcept {
        return device_ != nil && queue_ != nil && denoise_pipeline_ != nil;
    }

    [[nodiscard]] bool resident_valid() const noexcept {
        return device_ != nil && queue_ != nil && resident_pipeline_ != nil;
    }

    [[nodiscard]] bool area_preview_valid() const noexcept {
        return device_ != nil && queue_ != nil && preview_pipeline_ != nil;
    }

    [[nodiscard]] bool dcp_valid() const noexcept {
        return device_ != nil && queue_ != nil && dcp_pipeline_ != nil;
    }

    [[nodiscard]] id<MTLDevice> device() const noexcept { return device_; }
    [[nodiscard]] id<MTLCommandQueue> queue() const noexcept { return queue_; }
    [[nodiscard]] id<MTLComputePipelineState> pipeline() const noexcept {
        return pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> resident_pipeline() const noexcept {
        return resident_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> raw_denoise_pipeline() const noexcept {
        return denoise_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> area_preview_pipeline() const noexcept {
        return preview_pipeline_;
    }
    [[nodiscard]] id<MTLComputePipelineState> dcp_pipeline() const noexcept {
        return dcp_pipeline_;
    }
    [[nodiscard]] const std::string& diagnostic() const noexcept { return diagnostic_; }
    [[nodiscard]] const std::string& resident_diagnostic() const noexcept {
        return resident_diagnostic_.empty() ? diagnostic_ : resident_diagnostic_;
    }
    [[nodiscard]] const std::string& area_preview_diagnostic() const noexcept {
        return preview_diagnostic_.empty() ? diagnostic_ : preview_diagnostic_;
    }
    [[nodiscard]] const std::string& raw_denoise_diagnostic() const noexcept {
        return denoise_diagnostic_.empty() ? diagnostic_ : denoise_diagnostic_;
    }
    [[nodiscard]] const std::string& dcp_diagnostic() const noexcept {
        return dcp_diagnostic_.empty() ? diagnostic_ : dcp_diagnostic_;
    }

private:
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLComputePipelineState> pipeline_ = nil;
    id<MTLComputePipelineState> resident_pipeline_ = nil;
    id<MTLComputePipelineState> preview_pipeline_ = nil;
    id<MTLComputePipelineState> dcp_pipeline_ = nil;
    id<MTLComputePipelineState> denoise_pipeline_ = nil;
    std::string diagnostic_;
    std::string resident_diagnostic_;
    std::string preview_diagnostic_;
    std::string dcp_diagnostic_;
    std::string denoise_diagnostic_;
};

[[nodiscard]] MetalRawContext& metal_context() {
    static MetalRawContext context;
    return context;
}

} // namespace

[[nodiscard]] id<MTLDevice> metal_raw_device() noexcept {
    return metal_context().device();
}

[[nodiscard]] id<MTLCommandQueue> metal_raw_command_queue() noexcept {
    return metal_context().queue();
}

[[nodiscard]] id<MTLComputePipelineState> metal_raw_reconstruction_pipeline() noexcept {
    return metal_context().pipeline();
}

[[nodiscard]] id<MTLComputePipelineState> metal_raw_resident_reconstruction_pipeline() noexcept {
    return metal_context().resident_pipeline();
}

[[nodiscard]] id<MTLComputePipelineState> metal_raw_area_preview_pipeline() noexcept {
    return metal_context().area_preview_pipeline();
}

[[nodiscard]] id<MTLComputePipelineState> metal_raw_denoise_pipeline() noexcept {
    return metal_context().raw_denoise_pipeline();
}

[[nodiscard]] id<MTLComputePipelineState> metal_dcp_color_pipeline() noexcept {
    return metal_context().dcp_pipeline();
}

[[nodiscard]] bool metal_raw_resident_reconstruction_available() noexcept {
    return metal_context().resident_valid();
}

[[nodiscard]] bool metal_raw_area_preview_available() noexcept {
    return metal_context().area_preview_valid();
}

[[nodiscard]] const std::string& metal_raw_runtime_diagnostic() noexcept {
    return metal_context().diagnostic();
}

[[nodiscard]] const std::string& metal_raw_resident_reconstruction_diagnostic() noexcept {
    return metal_context().resident_diagnostic();
}

[[nodiscard]] const std::string& metal_raw_area_preview_diagnostic() noexcept {
    return metal_context().area_preview_diagnostic();
}

[[nodiscard]] const std::string& metal_raw_denoise_diagnostic() noexcept {
    return metal_context().raw_denoise_diagnostic();
}

[[nodiscard]] const std::string& metal_dcp_color_diagnostic() noexcept {
    return metal_context().dcp_diagnostic();
}

[[nodiscard]] std::mutex& metal_execution_mutex() {
    static std::mutex mutex;
    return mutex;
}

[[nodiscard]] bool checked_multiply(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result
) noexcept {
    if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    result = left * right;
    return true;
}

[[nodiscard]] bool checked_add(
    const std::size_t left,
    const std::size_t right,
    std::size_t& result
) noexcept {
    if (right > std::numeric_limits<std::size_t>::max() - left) {
        return false;
    }
    result = left + right;
    return true;
}

[[nodiscard]] std::string metal_raw_command_buffer_diagnostic(
    id<MTLCommandBuffer> command_buffer
) {
    NSError* error = command_buffer.error;
    std::string detail = error_description(error);
    return detail.empty()
        ? "Metal RAW command did not complete successfully"
        : "Metal RAW command failed: " + detail;
}

bool metal_raw_development_available() noexcept {
    return metal_context().valid();
}

bool metal_raw_denoise_available() noexcept {
    return metal_context().raw_denoise_valid();
}

bool metal_dcp_color_development_available() noexcept {
    return metal_context().dcp_valid();
}

} // namespace shadow::image::detail
