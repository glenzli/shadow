#import <CoreML/CoreML.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace {

MLModel* load_model(NSString* root, NSString* package) {
    NSURL* source = [NSURL fileURLWithPath:[root stringByAppendingPathComponent:package]
                               isDirectory:YES];
    NSError* error = nil;
    const auto started = std::chrono::steady_clock::now();
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
        return nil;
    }

    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
    std::printf("%s\n", package.UTF8String);
    std::printf("  compile-and-load: %.3f s\n", elapsed.count());
    std::puts("  inputs:");
    NSArray<NSString*>* inputs = [model.modelDescription.inputDescriptionsByName.allKeys
        sortedArrayUsingSelector:@selector(compare:)];
    for (NSString* name in inputs) {
        MLFeatureDescription* feature = model.modelDescription.inputDescriptionsByName[name];
        std::printf("    %s: %s\n", name.UTF8String, feature.description.UTF8String);
    }
    std::puts("  outputs:");
    NSArray<NSString*>* outputs = [model.modelDescription.outputDescriptionsByName.allKeys
        sortedArrayUsingSelector:@selector(compare:)];
    for (NSString* name in outputs) {
        MLFeatureDescription* feature = model.modelDescription.outputDescriptionsByName[name];
        std::printf("    %s: %s\n", name.UTF8String, feature.description.UTF8String);
    }
    return model;
}

CVPixelBufferRef synthetic_image() {
    CVPixelBufferRef pixel_buffer = nil;
    NSDictionary* attributes = @{
        (NSString*)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    const CVReturn status = CVPixelBufferCreate(
        kCFAllocatorDefault,
        1024,
        1024,
        kCVPixelFormatType_32BGRA,
        (__bridge CFDictionaryRef)attributes,
        &pixel_buffer
    );
    if (status != kCVReturnSuccess) {
        return nil;
    }

    CVPixelBufferLockBaseAddress(pixel_buffer, 0);
    auto* base = static_cast<unsigned char*>(CVPixelBufferGetBaseAddress(pixel_buffer));
    const size_t row_bytes = CVPixelBufferGetBytesPerRow(pixel_buffer);
    for (size_t y = 0; y < 1024; ++y) {
        auto* row = base + y * row_bytes;
        for (size_t x = 0; x < 1024; ++x) {
            const double dx = static_cast<double>(x) - 512.0;
            const double dy = static_cast<double>(y) - 512.0;
            const bool foreground = dx * dx + dy * dy < 260.0 * 260.0;
            row[x * 4 + 0] = foreground ? 24 : 160;
            row[x * 4 + 1] = foreground ? 40 : 72;
            row[x * 4 + 2] = foreground ? 232 : 28;
            row[x * 4 + 3] = 255;
        }
    }
    CVPixelBufferUnlockBaseAddress(pixel_buffer, 0);
    return pixel_buffer;
}

bool write_mask(MLMultiArray* masks, NSInteger candidate, const char* output_path) {
    if (masks.shape.count != 4 || masks.shape[2].integerValue != 256
        || masks.shape[3].integerValue != 256) {
        std::fprintf(stderr, "unexpected mask shape: %s\n", masks.shape.description.UTF8String);
        return false;
    }

    std::ofstream output(output_path, std::ios::binary);
    output << "P5\n256 256\n255\n";
    for (NSInteger y = 0; y < 256; ++y) {
        for (NSInteger x = 0; x < 256; ++x) {
            const double logit = masks[@[ @0, @(candidate), @(y), @(x) ]].doubleValue;
            const double probability = 1.0 / (1.0 + std::exp(-logit));
            output.put(static_cast<char>(std::lround(probability * 255.0)));
        }
    }
    return output.good();
}

bool run_synthetic_inference(
    MLModel* image_encoder,
    MLModel* prompt_encoder,
    MLModel* mask_decoder,
    const char* mask_path
) {
    CVPixelBufferRef pixel_buffer = synthetic_image();
    if (pixel_buffer == nil) {
        std::fputs("failed to allocate synthetic image\n", stderr);
        return false;
    }

    NSError* error = nil;
    MLDictionaryFeatureProvider* image_input =
        [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{
            @"image" : [MLFeatureValue featureValueWithPixelBuffer:pixel_buffer],
        }
                                                          error:&error];
    const auto image_started = std::chrono::steady_clock::now();
    id<MLFeatureProvider> image_output = [image_encoder predictionFromFeatures:image_input
                                                                         error:&error];
    CFRelease(pixel_buffer);
    if (image_output == nil) {
        std::fprintf(stderr, "image inference failed: %s\n", error.localizedDescription.UTF8String);
        return false;
    }
    const auto image_elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - image_started);

    MLMultiArray* points = [[MLMultiArray alloc] initWithShape:@[ @1, @1, @2 ]
                                                      dataType:MLMultiArrayDataTypeFloat16
                                                         error:&error];
    MLMultiArray* labels = [[MLMultiArray alloc] initWithShape:@[ @1, @1 ]
                                                      dataType:MLMultiArrayDataTypeFloat16
                                                         error:&error];
    [points setObject:@512.0 atIndexedSubscript:0];
    [points setObject:@512.0 atIndexedSubscript:1];
    [labels setObject:@1.0 atIndexedSubscript:0];
    MLDictionaryFeatureProvider* prompt_input =
        [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{
            @"points" : [MLFeatureValue featureValueWithMultiArray:points],
            @"labels" : [MLFeatureValue featureValueWithMultiArray:labels],
        }
                                                          error:&error];

    id<MLFeatureProvider> decoder_output = nil;
    for (int iteration = 0; iteration < 3; ++iteration) {
        const auto prompt_started = std::chrono::steady_clock::now();
        id<MLFeatureProvider> prompt_output = [prompt_encoder predictionFromFeatures:prompt_input
                                                                               error:&error];
        if (prompt_output == nil) {
            std::fprintf(
                stderr,
                "prompt inference failed: %s\n",
                error.localizedDescription.UTF8String
            );
            return false;
        }

        NSDictionary<NSString*, MLFeatureValue*>* decoder_values = @{
            @"image_embedding" : [image_output featureValueForName:@"image_embedding"],
            @"feats_s0" : [image_output featureValueForName:@"feats_s0"],
            @"feats_s1" : [image_output featureValueForName:@"feats_s1"],
            @"sparse_embedding" : [prompt_output featureValueForName:@"sparse_embeddings"],
            @"dense_embedding" : [prompt_output featureValueForName:@"dense_embeddings"],
        };
        MLDictionaryFeatureProvider* decoder_input =
            [[MLDictionaryFeatureProvider alloc] initWithDictionary:decoder_values error:&error];
        decoder_output = [mask_decoder predictionFromFeatures:decoder_input error:&error];
        if (decoder_output == nil) {
            std::fprintf(
                stderr,
                "mask inference failed: %s\n",
                error.localizedDescription.UTF8String
            );
            return false;
        }
        const auto prompt_elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - prompt_started);
        std::printf("  prompt + decoder [%d]: %.3f s\n", iteration + 1, prompt_elapsed.count());
    }

    MLMultiArray* scores = [decoder_output featureValueForName:@"scores"].multiArrayValue;
    NSInteger best = 0;
    for (NSInteger candidate = 1; candidate < scores.count; ++candidate) {
        if (scores[candidate].doubleValue > scores[best].doubleValue) {
            best = candidate;
        }
    }

    std::printf("synthetic inference\n");
    std::printf("  image encoder: %.3f s\n", image_elapsed.count());
    std::printf("  scores: %s\n", scores.description.UTF8String);
    std::printf("  best candidate: %ld\n", static_cast<long>(best));
    return write_mask(
        [decoder_output featureValueForName:@"low_res_masks"].multiArrayValue,
        best,
        mask_path
    );
}

} // namespace

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc != 3) {
            std::fprintf(
                stderr,
                "usage: shadow-sam2-coreml-probe <model-directory> "
                "<mask-output.pgm>\n"
            );
            return 64;
        }

        NSString* root = [NSString stringWithUTF8String:argv[1]];
        NSArray<NSString*>* packages = @[
            @"SAM2_1SmallImageEncoderFLOAT16.mlpackage",
            @"SAM2_1SmallPromptEncoderFLOAT16.mlpackage",
            @"SAM2_1SmallMaskDecoderFLOAT16.mlpackage",
        ];
        NSMutableArray<MLModel*>* models = [NSMutableArray arrayWithCapacity:3];
        for (NSString* package in packages) {
            MLModel* model = load_model(root, package);
            if (model == nil) {
                return 1;
            }
            [models addObject:model];
        }
        if (!run_synthetic_inference(models[0], models[1], models[2], argv[2])) {
            return 1;
        }
        return 0;
    }
}
