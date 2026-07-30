#import <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>

#include "resident_server.hpp"
#include "sam2_coreml_engine.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr const char* kExpectedModelId = "apple/coreml-sam2.1-small";
constexpr const char* kExpectedRevision = "883f5787eb0be35ce6965907a8bc1f5320a5a02e";
constexpr const char* kExpectedInventoryBlake3 =
    "28982b31b020249ca62f9c41fa82193f794b215d07b7080a3c00e5c85733bca0";

using shadow_sam2_coreml::Engine;
using shadow_sam2_coreml::PromptPoint;
using shadow_sam2_coreml::kMaskEdge;
using shadow_sam2_coreml::kMaximumPromptPoints;

struct Arguments final {
    std::string model_directory;
    std::string manifest_path;
    std::string input_jpeg;
    std::string output_mask;
    std::vector<PromptPoint> points;
    bool verify_model_only = false;
    bool serve = false;
};

void print_usage() {
    std::fputs(
        "usage: shadow-sam2-coreml-provider "
        "--model-dir <directory> --manifest <model-manifest.json> "
        "[--verify-model | --serve | --input-jpeg <image> --output-mask <path> "
        "--point <normalized-x> <normalized-y> <foreground|background> "
        "[--point ...]]\n",
        stderr
    );
}

std::optional<double> parse_normalized_coordinate(const char* value) {
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (errno != 0 || end == value || *end != '\0' || !std::isfinite(parsed)
        || parsed < 0.0 || parsed > 1.0) {
        return std::nullopt;
    }
    return parsed;
}

std::optional<Arguments> parse_arguments(int argc, const char* argv[]) {
    Arguments arguments;
    for (int index = 1; index < argc;) {
        const std::string option = argv[index++];
        if (option == "--model-dir" && index < argc) {
            arguments.model_directory = argv[index++];
        } else if (option == "--manifest" && index < argc) {
            arguments.manifest_path = argv[index++];
        } else if (option == "--verify-model") {
            arguments.verify_model_only = true;
        } else if (option == "--serve") {
            arguments.serve = true;
        } else if (option == "--input-jpeg" && index < argc) {
            arguments.input_jpeg = argv[index++];
        } else if (option == "--output-mask" && index < argc) {
            arguments.output_mask = argv[index++];
        } else if (option == "--point" && index + 2 < argc) {
            const auto x = parse_normalized_coordinate(argv[index++]);
            const auto y = parse_normalized_coordinate(argv[index++]);
            const std::string polarity = argv[index++];
            if (!x.has_value() || !y.has_value()
                || (polarity != "foreground" && polarity != "background")) {
                std::fputs("invalid --point arguments\n", stderr);
                return std::nullopt;
            }
            arguments.points.push_back(PromptPoint{
                .x = *x,
                .y = *y,
                .foreground = polarity == "foreground",
            });
        } else {
            std::fprintf(stderr, "unknown or incomplete option: %s\n", option.c_str());
            return std::nullopt;
        }
    }

    if (arguments.model_directory.empty() || arguments.manifest_path.empty()) {
        std::fputs("model directory and manifest are required\n", stderr);
        return std::nullopt;
    }
    if (arguments.verify_model_only || arguments.serve) {
        if (!arguments.input_jpeg.empty() || !arguments.output_mask.empty()
            || !arguments.points.empty() || arguments.verify_model_only == arguments.serve) {
            std::fputs(
                "--verify-model and --serve are exclusive and do not accept inference arguments\n",
                stderr
            );
            return std::nullopt;
        }
        return arguments;
    }

    const bool has_foreground = std::any_of(
        arguments.points.begin(),
        arguments.points.end(),
        [](const PromptPoint& point) { return point.foreground; }
    );
    if (arguments.input_jpeg.empty() || arguments.output_mask.empty()
        || arguments.points.empty() || !has_foreground
        || arguments.points.size() > kMaximumPromptPoints) {
        std::fputs(
            "model, input, output, 1-16 points, and at least one foreground point are required\n",
            stderr
        );
        return std::nullopt;
    }
    return arguments;
}

NSArray<NSString*>* required_model_paths() {
    return @[
        @"SAM2_1SmallImageEncoderFLOAT16.mlpackage/Data/com.apple.CoreML/model.mlmodel",
        @"SAM2_1SmallImageEncoderFLOAT16.mlpackage/Data/com.apple.CoreML/weights/weight.bin",
        @"SAM2_1SmallImageEncoderFLOAT16.mlpackage/Manifest.json",
        @"SAM2_1SmallMaskDecoderFLOAT16.mlpackage/Data/com.apple.CoreML/model.mlmodel",
        @"SAM2_1SmallMaskDecoderFLOAT16.mlpackage/Data/com.apple.CoreML/weights/weight.bin",
        @"SAM2_1SmallMaskDecoderFLOAT16.mlpackage/Manifest.json",
        @"SAM2_1SmallPromptEncoderFLOAT16.mlpackage/Data/com.apple.CoreML/model.mlmodel",
        @"SAM2_1SmallPromptEncoderFLOAT16.mlpackage/Data/com.apple.CoreML/weights/weight.bin",
        @"SAM2_1SmallPromptEncoderFLOAT16.mlpackage/Manifest.json",
    ];
}

NSString* sha256_file(NSString* path) {
    NSInputStream* stream = [NSInputStream inputStreamWithFileAtPath:path];
    [stream open];
    if (stream.streamStatus == NSStreamStatusError) {
        std::fprintf(stderr, "failed to open model artifact: %s\n", path.UTF8String);
        return nil;
    }

    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    std::uint8_t buffer[64 * 1024];
    while (true) {
        const NSInteger count = [stream read:buffer maxLength:sizeof(buffer)];
        if (count < 0) {
            std::fprintf(stderr, "failed to read model artifact: %s\n", path.UTF8String);
            [stream close];
            return nil;
        }
        if (count == 0) {
            break;
        }
        CC_SHA256_Update(&context, buffer, static_cast<CC_LONG>(count));
    }
    [stream close];

    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &context);
    NSMutableString* encoded =
        [NSMutableString stringWithCapacity:CC_SHA256_DIGEST_LENGTH * 2];
    for (const unsigned char byte : digest) {
        [encoded appendFormat:@"%02x", byte];
    }
    return encoded;
}

NSString* verify_model_inventory(const Arguments& arguments) {
    NSData* data =
        [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:arguments.manifest_path.c_str()]];
    if (data == nil) {
        std::fputs("failed to read model manifest\n", stderr);
        return nil;
    }
    NSError* error = nil;
    id decoded = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
    if (![decoded isKindOfClass:[NSDictionary class]]) {
        std::fprintf(
            stderr,
            "failed to decode model manifest: %s\n",
            error.localizedDescription.UTF8String
        );
        return nil;
    }

    NSDictionary* manifest = decoded;
    NSString* model_id = manifest[@"model_id"];
    NSString* revision = manifest[@"exact_revision"];
    NSDictionary* artifact_set = manifest[@"artifact_set"];
    NSString* inventory = artifact_set[@"inventory_blake3"];
    NSArray* artifacts = artifact_set[@"artifacts"];
    if (![model_id isEqualToString:[NSString stringWithUTF8String:kExpectedModelId]]
        || ![revision isEqualToString:[NSString stringWithUTF8String:kExpectedRevision]]
        || ![inventory isEqualToString:[NSString stringWithUTF8String:kExpectedInventoryBlake3]]
        || ![artifacts isKindOfClass:[NSArray class]]
        || artifacts.count != required_model_paths().count) {
        std::fputs("model manifest does not identify the admitted SAM 2.1 artifact set\n", stderr);
        return nil;
    }

    NSMutableDictionary<NSString*, NSDictionary*>* declarations =
        [NSMutableDictionary dictionaryWithCapacity:artifacts.count];
    for (id value in artifacts) {
        if (![value isKindOfClass:[NSDictionary class]]) {
            std::fputs("model manifest artifact entry is not an object\n", stderr);
            return nil;
        }
        NSDictionary* artifact = value;
        NSString* relative_path = artifact[@"relative_path"];
        NSString* role = artifact[@"role"];
        NSNumber* byte_len = artifact[@"byte_len"];
        NSString* sha256 = artifact[@"sha256"];
        if (![relative_path isKindOfClass:[NSString class]]
            || ![role isEqualToString:@"core_ml_package_member"]
            || ![byte_len isKindOfClass:[NSNumber class]]
            || ![sha256 isKindOfClass:[NSString class]]
            || sha256.length != CC_SHA256_DIGEST_LENGTH * 2
            || declarations[relative_path] != nil) {
            std::fputs("model manifest artifact entry is malformed or duplicated\n", stderr);
            return nil;
        }
        declarations[relative_path] = artifact;
    }

    NSString* model_root = [NSString stringWithUTF8String:arguments.model_directory.c_str()];
    NSFileManager* files = [NSFileManager defaultManager];
    for (NSString* relative_path in required_model_paths()) {
        NSDictionary* artifact = declarations[relative_path];
        if (artifact == nil) {
            std::fprintf(
                stderr,
                "model manifest is missing required artifact: %s\n",
                relative_path.UTF8String
            );
            return nil;
        }
        NSString* absolute_path = [model_root stringByAppendingPathComponent:relative_path];
        NSDictionary<NSFileAttributeKey, id>* attributes =
            [files attributesOfItemAtPath:absolute_path error:&error];
        if (attributes == nil
            || attributes.fileSize != [artifact[@"byte_len"] unsignedLongLongValue]) {
            std::fprintf(
                stderr,
                "model artifact length mismatch: %s\n",
                relative_path.UTF8String
            );
            return nil;
        }
        NSString* actual_sha256 = sha256_file(absolute_path);
        if (actual_sha256 == nil
            || ![actual_sha256 isEqualToString:[artifact[@"sha256"] lowercaseString]]) {
            std::fprintf(
                stderr,
                "model artifact digest mismatch: %s\n",
                relative_path.UTF8String
            );
            return nil;
        }
    }
    return revision;
}

bool run_once(Engine& engine, const Arguments& arguments) {
    bool cache_hit = false;
    if (!engine.load_image(arguments.input_jpeg, "", cache_hit)) {
        return false;
    }
    const auto receipt = engine.predict(arguments.points, arguments.output_mask);
    if (!receipt.has_value()) {
        return false;
    }
    std::printf(
        "shadow-sam2-coreml-mask-v1 width=%zu height=%zu score=%.6f points=%zu\n",
        kMaskEdge,
        kMaskEdge,
        receipt->score,
        receipt->point_count
    );
    return true;
}

} // namespace

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        const auto arguments = parse_arguments(argc, argv);
        if (!arguments.has_value()) {
            print_usage();
            return 64;
        }
        NSString* revision = verify_model_inventory(*arguments);
        if (revision == nil) {
            return 1;
        }
        if (arguments->verify_model_only) {
            std::printf(
                "shadow-sam2-coreml-model-v1 revision=%s files=%lu\n",
                revision.UTF8String,
                static_cast<unsigned long>(required_model_paths().count)
            );
            return 0;
        }

        auto engine = Engine::load(arguments->model_directory);
        if (engine == nullptr) {
            return 1;
        }
        if (arguments->serve) {
            return shadow_sam2_coreml::run_resident_server(
                       *engine,
                       revision.UTF8String
                   )
                       ? 0
                       : 1;
        }
        return run_once(*engine, *arguments) ? 0 : 1;
    }
}
