#import <CoreGraphics/CoreGraphics.h>
#import <CoreML/CoreML.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include "sam2_coreml_engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <utility>

#include <unistd.h>

namespace shadow_sam2_coreml {
namespace {

class ScopedFrameworkDiagnostics final {
  public:
    ScopedFrameworkDiagnostics() {
        std::fflush(stdout);
        saved_stdout_ = dup(STDOUT_FILENO);
        if (saved_stdout_ < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
            if (saved_stdout_ >= 0) {
                close(saved_stdout_);
                saved_stdout_ = -1;
            }
        }
    }

    ~ScopedFrameworkDiagnostics() {
        if (saved_stdout_ < 0) {
            return;
        }
        std::fflush(stdout);
        static_cast<void>(dup2(saved_stdout_, STDOUT_FILENO));
        close(saved_stdout_);
    }

    ScopedFrameworkDiagnostics(const ScopedFrameworkDiagnostics&) = delete;
    ScopedFrameworkDiagnostics& operator=(const ScopedFrameworkDiagnostics&) = delete;

    [[nodiscard]] bool valid() const {
        return saved_stdout_ >= 0;
    }

  private:
    int saved_stdout_ = -1;
};

MLModel* load_model(NSString* root, NSString* package) {
    NSURL* source = [NSURL fileURLWithPath:[root stringByAppendingPathComponent:package]
                               isDirectory:YES];
    NSError* error = nil;
    NSURL* compiled = [MLModel compileModelAtURL:source error:&error];
    if (compiled == nil) {
        std::fprintf(
            stderr,
            "%s: compile failed: %s\n",
            package.UTF8String,
            error.localizedDescription.UTF8String
        );
        return nil;
    }

    MLModelConfiguration* configuration = [[MLModelConfiguration alloc] init];
    configuration.computeUnits = MLComputeUnitsCPUAndGPU;
    MLModel* model = [MLModel modelWithContentsOfURL:compiled
                                       configuration:configuration
                                               error:&error];
    if (model == nil) {
        std::fprintf(
            stderr,
            "%s: load failed: %s\n",
            package.UTF8String,
            error.localizedDescription.UTF8String
        );
    }
    return model;
}

CVPixelBufferRef load_stretched_image(const std::string& input_path) {
    NSData* path_data =
        [NSData dataWithBytes:input_path.data() length:input_path.size()];
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault,
        static_cast<const UInt8*>(path_data.bytes),
        path_data.length,
        false
    );
    if (url == nil) {
        std::fputs("failed to construct input image URL\n", stderr);
        return nil;
    }

    CGImageSourceRef source = CGImageSourceCreateWithURL(url, nil);
    CFRelease(url);
    if (source == nil) {
        std::fputs("failed to open input image\n", stderr);
        return nil;
    }
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nil);
    CFRelease(source);
    if (image == nil) {
        std::fputs("failed to decode input image\n", stderr);
        return nil;
    }

    CVPixelBufferRef pixel_buffer = nil;
    NSDictionary* attributes = @{
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    const CVReturn create_status = CVPixelBufferCreate(
        kCFAllocatorDefault,
        kModelEdge,
        kModelEdge,
        kCVPixelFormatType_32BGRA,
        (__bridge CFDictionaryRef)attributes,
        &pixel_buffer
    );
    if (create_status != kCVReturnSuccess || pixel_buffer == nil) {
        CGImageRelease(image);
        std::fprintf(stderr, "failed to allocate model image: %d\n", create_status);
        return nil;
    }

    const CVReturn lock_status = CVPixelBufferLockBaseAddress(pixel_buffer, 0);
    if (lock_status != kCVReturnSuccess) {
        CGImageRelease(image);
        CFRelease(pixel_buffer);
        std::fprintf(stderr, "failed to lock model image: %d\n", lock_status);
        return nil;
    }

    CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
    CGContextRef context = CGBitmapContextCreate(
        CVPixelBufferGetBaseAddress(pixel_buffer),
        kModelEdge,
        kModelEdge,
        8,
        CVPixelBufferGetBytesPerRow(pixel_buffer),
        color_space,
        static_cast<CGBitmapInfo>(
            static_cast<std::uint32_t>(kCGBitmapByteOrder32Little)
            | static_cast<std::uint32_t>(kCGImageAlphaPremultipliedFirst)
        )
    );
    CGColorSpaceRelease(color_space);
    if (context == nil) {
        CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
        CGImageRelease(image);
        CFRelease(pixel_buffer);
        std::fputs("failed to create image drawing context\n", stderr);
        return nil;
    }

    CGContextSetInterpolationQuality(context, kCGInterpolationHigh);
    CGContextTranslateCTM(context, 0.0, static_cast<CGFloat>(kModelEdge));
    CGContextScaleCTM(context, 1.0, -1.0);
    CGContextDrawImage(
        context,
        CGRectMake(0.0, 0.0, static_cast<CGFloat>(kModelEdge), static_cast<CGFloat>(kModelEdge)),
        image
    );
    CGContextRelease(context);
    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
    CGImageRelease(image);
    return pixel_buffer;
}

id<MLFeatureProvider> encode_image(MLModel* encoder, CVPixelBufferRef image) {
    NSError* error = nil;
    MLDictionaryFeatureProvider* input =
        [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{
            @"image" : [MLFeatureValue featureValueWithPixelBuffer:image],
        }
                                                          error:&error];
    if (input == nil) {
        std::fprintf(stderr, "image input failed: %s\n", error.localizedDescription.UTF8String);
        return nil;
    }
    id<MLFeatureProvider> output = [encoder predictionFromFeatures:input error:&error];
    if (output == nil) {
        std::fprintf(stderr, "image inference failed: %s\n", error.localizedDescription.UTF8String);
    }
    return output;
}

id<MLFeatureProvider> encode_prompt(MLModel* encoder, const std::vector<PromptPoint>& prompts) {
    NSError* error = nil;
    NSNumber* point_count = @(prompts.size());
    MLMultiArray* points =
        [[MLMultiArray alloc] initWithShape:@[ @1, point_count, @2 ]
                                  dataType:MLMultiArrayDataTypeFloat16
                                     error:&error];
    if (points == nil) {
        std::fprintf(stderr, "point allocation failed: %s\n", error.localizedDescription.UTF8String);
        return nil;
    }
    MLMultiArray* labels =
        [[MLMultiArray alloc] initWithShape:@[ @1, point_count ]
                                  dataType:MLMultiArrayDataTypeFloat16
                                     error:&error];
    if (labels == nil) {
        std::fprintf(stderr, "label allocation failed: %s\n", error.localizedDescription.UTF8String);
        return nil;
    }

    for (std::size_t index = 0; index < prompts.size(); ++index) {
        const auto& point = prompts[index];
        [points setObject:@(point.x * static_cast<double>(kModelEdge))
            atIndexedSubscript:static_cast<NSInteger>(index * 2)];
        [points setObject:@(point.y * static_cast<double>(kModelEdge))
            atIndexedSubscript:static_cast<NSInteger>(index * 2 + 1)];
        [labels setObject:@(point.foreground ? 1.0 : 0.0)
            atIndexedSubscript:static_cast<NSInteger>(index)];
    }

    MLDictionaryFeatureProvider* input =
        [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{
            @"points" : [MLFeatureValue featureValueWithMultiArray:points],
            @"labels" : [MLFeatureValue featureValueWithMultiArray:labels],
        }
                                                          error:&error];
    if (input == nil) {
        std::fprintf(stderr, "prompt input failed: %s\n", error.localizedDescription.UTF8String);
        return nil;
    }
    id<MLFeatureProvider> output = [encoder predictionFromFeatures:input error:&error];
    if (output == nil) {
        std::fprintf(
            stderr,
            "prompt inference failed for %zu point(s): %s\n",
            prompts.size(),
            error.localizedDescription.UTF8String
        );
    }
    return output;
}

id<MLFeatureProvider> decode_mask(
    MLModel* decoder,
    id<MLFeatureProvider> image,
    id<MLFeatureProvider> prompt
) {
    NSError* error = nil;
    NSDictionary<NSString*, MLFeatureValue*>* values = @{
        @"image_embedding" : [image featureValueForName:@"image_embedding"],
        @"feats_s0" : [image featureValueForName:@"feats_s0"],
        @"feats_s1" : [image featureValueForName:@"feats_s1"],
        @"sparse_embedding" : [prompt featureValueForName:@"sparse_embeddings"],
        @"dense_embedding" : [prompt featureValueForName:@"dense_embeddings"],
    };
    MLDictionaryFeatureProvider* input =
        [[MLDictionaryFeatureProvider alloc] initWithDictionary:values error:&error];
    if (input == nil) {
        std::fprintf(stderr, "decoder input failed: %s\n", error.localizedDescription.UTF8String);
        return nil;
    }
    id<MLFeatureProvider> output = [decoder predictionFromFeatures:input error:&error];
    if (output == nil) {
        std::fprintf(stderr, "mask inference failed: %s\n", error.localizedDescription.UTF8String);
    }
    return output;
}

bool write_soft_mask(
    MLMultiArray* masks,
    NSInteger candidate,
    const std::string& output_path
) {
    if (masks == nil || masks.shape.count != 4 || masks.shape[2].integerValue != kMaskEdge
        || masks.shape[3].integerValue != kMaskEdge) {
        std::fprintf(
            stderr,
            "unexpected mask shape: %s\n",
            masks == nil ? "(missing)" : masks.shape.description.UTF8String
        );
        return false;
    }

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::fprintf(stderr, "failed to open mask output: %s\n", output_path.c_str());
        return false;
    }
    for (NSInteger y = 0; y < static_cast<NSInteger>(kMaskEdge); ++y) {
        for (NSInteger x = 0; x < static_cast<NSInteger>(kMaskEdge); ++x) {
            const double logit = masks[@[ @0, @(candidate), @(y), @(x) ]].doubleValue;
            const double probability = 1.0 / (1.0 + std::exp(-logit));
            const double bounded = std::clamp(probability, 0.0, 1.0);
            output.put(static_cast<char>(std::lround(bounded * 255.0)));
        }
    }
    return output.good();
}

} // namespace

struct Engine::Impl final {
    MLModel* image_encoder = nil;
    MLModel* prompt_encoder = nil;
    MLModel* mask_decoder = nil;
    id<MLFeatureProvider> image_embedding = nil;
    std::string image_content_identity;
};

std::unique_ptr<Engine> Engine::load(const std::string& model_directory) {
    const ScopedFrameworkDiagnostics diagnostics;
    if (!diagnostics.valid()) {
        std::fputs("failed to isolate Core ML diagnostics from provider stdout\n", stderr);
        return nullptr;
    }
    NSString* model_root = [NSString stringWithUTF8String:model_directory.c_str()];
    auto implementation = std::make_unique<Impl>();
    implementation->image_encoder =
        load_model(model_root, @"SAM2_1SmallImageEncoderFLOAT16.mlpackage");
    implementation->prompt_encoder =
        load_model(model_root, @"SAM2_1SmallPromptEncoderFLOAT16.mlpackage");
    implementation->mask_decoder =
        load_model(model_root, @"SAM2_1SmallMaskDecoderFLOAT16.mlpackage");
    if (implementation->image_encoder == nil || implementation->prompt_encoder == nil
        || implementation->mask_decoder == nil) {
        return nullptr;
    }
    return std::unique_ptr<Engine>(new Engine(std::move(implementation)));
}

Engine::Engine(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

Engine::~Engine() = default;

bool Engine::load_image(
    const std::string& input_path,
    const std::string& content_identity,
    bool& cache_hit
) {
    const ScopedFrameworkDiagnostics diagnostics;
    if (!diagnostics.valid()) {
        std::fputs("failed to isolate Core ML diagnostics from provider stdout\n", stderr);
        return false;
    }
    cache_hit = !content_identity.empty() && implementation_->image_embedding != nil
                && implementation_->image_content_identity == content_identity;
    if (cache_hit) {
        return true;
    }

    CVPixelBufferRef image_buffer = load_stretched_image(input_path);
    if (image_buffer == nil) {
        return false;
    }
    id<MLFeatureProvider> image =
        encode_image(implementation_->image_encoder, image_buffer);
    CFRelease(image_buffer);
    if (image == nil) {
        return false;
    }
    implementation_->image_embedding = image;
    implementation_->image_content_identity = content_identity;
    return true;
}

bool Engine::has_loaded_image(const std::string& content_identity) const {
    return !content_identity.empty() && implementation_->image_embedding != nil
           && implementation_->image_content_identity == content_identity;
}

std::optional<MaskReceipt> Engine::predict(
    const std::vector<PromptPoint>& points,
    const std::string& output_path
) {
    const ScopedFrameworkDiagnostics diagnostics;
    if (!diagnostics.valid()) {
        std::fputs("failed to isolate Core ML diagnostics from provider stdout\n", stderr);
        return std::nullopt;
    }
    const bool has_foreground = std::any_of(
        points.begin(),
        points.end(),
        [](const PromptPoint& point) { return point.foreground; }
    );
    if (implementation_->image_embedding == nil || points.empty()
        || points.size() > kMaximumPromptPoints || !has_foreground) {
        std::fputs("prediction requires a loaded image and 1-16 valid prompt points\n", stderr);
        return std::nullopt;
    }

    id<MLFeatureProvider> prompt =
        encode_prompt(implementation_->prompt_encoder, points);
    if (prompt == nil) {
        return std::nullopt;
    }
    id<MLFeatureProvider> decoded = decode_mask(
        implementation_->mask_decoder,
        implementation_->image_embedding,
        prompt
    );
    if (decoded == nil) {
        return std::nullopt;
    }

    MLMultiArray* scores = [decoded featureValueForName:@"scores"].multiArrayValue;
    if (scores == nil || scores.count == 0) {
        std::fputs("mask decoder returned no scores\n", stderr);
        return std::nullopt;
    }
    NSInteger best = 0;
    for (NSInteger candidate = 1; candidate < scores.count; ++candidate) {
        if (scores[candidate].doubleValue > scores[best].doubleValue) {
            best = candidate;
        }
    }
    if (!write_soft_mask(
            [decoded featureValueForName:@"low_res_masks"].multiArrayValue,
            best,
            output_path
        )) {
        return std::nullopt;
    }
    return MaskReceipt{
        .score = scores[best].doubleValue,
        .point_count = points.size(),
    };
}

} // namespace shadow_sam2_coreml
