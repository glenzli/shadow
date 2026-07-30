// Legacy MacTypes declares a global `shadow` enumerator. Rename only that SDK token while the
// Apple headers are parsed so it cannot collide with Shadow's top-level C++ namespace.
#define shadow shadow_mactypes_legacy_symbol
#import <CoreML/CoreML.h>
#undef shadow

#include <CommonCrypto/CommonDigest.h>

#include "coreml_neural_raw_denoise.hpp"

#include "../../concurrency/row_scheduler.hpp"

#include <shadow/image/decoder_error.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace shadow::image::detail {

namespace {

[[nodiscard]] std::string error_description(NSError* error) {
    if (error == nil) {
        return {};
    }
    const char* text = [[error localizedDescription] UTF8String];
    return text == nullptr ? std::string{} : std::string(text);
}

class CoreMlOwnedObject final {
  public:
    explicit CoreMlOwnedObject(id value = nil) noexcept : value_(value) {}
    CoreMlOwnedObject(const CoreMlOwnedObject&) = delete;
    CoreMlOwnedObject& operator=(const CoreMlOwnedObject&) = delete;
    ~CoreMlOwnedObject() {
        [value_ release];
    }

    [[nodiscard]] id get() const noexcept {
        return value_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nil;
    }

  private:
    id value_ = nil;
};

class Sha256Stream final {
  public:
    Sha256Stream() {
        if (CC_SHA256_Init(&context_) != 1) {
            throw std::runtime_error("could not initialize SHA-256");
        }
    }

    void update(const void* bytes, std::size_t size) {
        const auto* cursor = static_cast<const std::byte*>(bytes);
        while (size > 0U) {
            const std::size_t chunk = std::min<std::size_t>(
                size,
                std::numeric_limits<CC_LONG>::max()
            );
            if (CC_SHA256_Update(&context_, cursor, static_cast<CC_LONG>(chunk)) != 1) {
                throw std::runtime_error("could not update SHA-256");
            }
            cursor += chunk;
            size -= chunk;
        }
    }

    void update_u64(const std::uint64_t value) {
        std::array<std::uint8_t, 8U> encoded{};
        for (std::size_t byte = 0U; byte < encoded.size(); ++byte) {
            encoded[encoded.size() - 1U - byte] =
                static_cast<std::uint8_t>(value >> (byte * 8U));
        }
        update(encoded.data(), encoded.size());
    }

    [[nodiscard]] std::string finish() {
        std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> digest{};
        if (CC_SHA256_Final(digest.data(), &context_) != 1) {
            throw std::runtime_error("could not finalize SHA-256");
        }
        std::ostringstream output;
        output << "sha256-tree-v1:" << std::hex << std::setfill('0');
        for (const unsigned char byte : digest) {
            output << std::setw(2) << static_cast<unsigned int>(byte);
        }
        return output.str();
    }

  private:
    CC_SHA256_CTX context_{};
};

struct ModelTreeFile final {
    std::filesystem::path absolute_path;
    std::string relative_path;
    std::uintmax_t size = 0U;
};

[[nodiscard]] std::string compiled_model_tree_identity(const std::string& model_path) {
    namespace fs = std::filesystem;
    const fs::path root(model_path);
    std::error_code error;
    const fs::file_status root_status = fs::symlink_status(root, error);
    if (error || !fs::is_directory(root_status) || fs::is_symlink(root_status)
        || root.extension() != ".mlmodelc") {
        throw std::runtime_error(
            "neural RAW denoise model must be a non-symlink .mlmodelc directory"
        );
    }

    std::vector<ModelTreeFile> files;
    for (fs::recursive_directory_iterator iterator(root), end; iterator != end; ++iterator) {
        throw_if_row_cancelled();
        const fs::file_status status = iterator->symlink_status();
        if (fs::is_symlink(status)) {
            throw std::runtime_error(
                "neural RAW denoise model directory may not contain symbolic links"
            );
        }
        if (fs::is_directory(status)) {
            continue;
        }
        if (!fs::is_regular_file(status)) {
            throw std::runtime_error(
                "neural RAW denoise model directory contains a non-regular entry"
            );
        }
        const fs::path relative = iterator->path().lexically_relative(root);
        const std::string relative_path = relative.generic_string();
        if (relative_path.empty() || relative_path.starts_with("../")) {
            throw std::runtime_error("neural RAW denoise model has an invalid relative path");
        }
        files.push_back(ModelTreeFile{
            .absolute_path = iterator->path(),
            .relative_path = relative_path,
            .size = iterator->file_size(),
        });
    }
    if (files.empty()) {
        throw std::runtime_error("neural RAW denoise model directory is empty");
    }
    std::sort(files.begin(), files.end(), [](const auto& left, const auto& right) {
        return left.relative_path < right.relative_path;
    });

    Sha256Stream digest;
    constexpr char contract[] = "shadow-coreml-model-tree-v1";
    digest.update(contract, sizeof(contract));
    std::array<char, 64U * 1024U> buffer{};
    for (const auto& file : files) {
        digest.update_u64(file.relative_path.size());
        digest.update(file.relative_path.data(), file.relative_path.size());
        digest.update_u64(file.size);
        std::ifstream input(file.absolute_path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("could not open a neural RAW denoise model file");
        }
        std::uintmax_t read_size = 0U;
        while (input) {
            throw_if_row_cancelled();
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const std::streamsize count = input.gcount();
            if (count > 0) {
                digest.update(buffer.data(), static_cast<std::size_t>(count));
                read_size += static_cast<std::uintmax_t>(count);
            }
        }
        if (!input.eof() || read_size != file.size) {
            throw std::runtime_error(
                "neural RAW denoise model changed or became unreadable while hashing"
            );
        }
    }
    return digest.finish();
}

[[nodiscard]] std::string core_ml_execution_identity() {
    const NSOperatingSystemVersion version =
        [NSProcessInfo processInfo].operatingSystemVersion;
    std::ostringstream identity;
    identity << "core-ml-v1:compute-units-all:macos-" << version.majorVersion << '.'
             << version.minorVersion << '.' << version.patchVersion;
    return identity.str();
}

[[nodiscard]] std::mutex& core_ml_execution_mutex() {
    static std::mutex mutex;
    return mutex;
}

[[nodiscard]] bool exact_shape(
    NSArray<NSNumber*>* shape,
    const std::array<std::uint32_t, 4U>& expected
) noexcept {
    if (shape.count != expected.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        if ([shape[index] unsignedLongLongValue] != expected[index]) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool contiguous_nchw_strides(
    NSArray<NSNumber*>* strides,
    const std::uint32_t edge
) noexcept {
    if (strides.count != 4U) {
        return false;
    }
    const std::uint64_t plane = static_cast<std::uint64_t>(edge) * edge;
    return [strides[0U] unsignedLongLongValue] == plane * 4U
           && [strides[1U] unsignedLongLongValue] == plane
           && [strides[2U] unsignedLongLongValue] == edge
           && [strides[3U] unsignedLongLongValue] == 1U;
}

[[nodiscard]] bool contiguous_noise_strides(NSArray<NSNumber*>* strides) noexcept {
    return strides.count == 2U && [strides[0U] unsignedLongLongValue] == 8U
           && [strides[1U] unsignedLongLongValue] == 1U;
}

class CoreMlTileInference final : public NeuralRawTileInference {
  public:
    explicit CoreMlTileInference(const std::string& compiled_model_path) {
        @autoreleasepool {
            NSString* path = [NSString stringWithUTF8String:compiled_model_path.c_str()];
            if (path == nil) {
                throw DecodeError(
                    DecodeErrorCode::invalid_request,
                    0,
                    "Core ML neural RAW denoise model path is not valid UTF-8"
                );
            }
            NSURL* url = [NSURL fileURLWithPath:path isDirectory:YES];
            CoreMlOwnedObject configuration([[MLModelConfiguration alloc] init]);
            auto* model_configuration =
                static_cast<MLModelConfiguration*>(configuration.get());
            model_configuration.computeUnits = MLComputeUnitsAll;
            model_configuration.allowLowPrecisionAccumulationOnGPU = NO;
            NSError* error = nil;
            MLModel* loaded = [MLModel
                modelWithContentsOfURL:url
                        configuration:model_configuration
                                error:&error];
            if (loaded == nil) {
                throw DecodeError(
                    DecodeErrorCode::unsupported,
                    0,
                    "Core ML could not load neural RAW denoise model: "
                        + error_description(error)
                );
            }
            const auto* description = loaded.modelDescription;
            const auto* mosaic = description.inputDescriptionsByName[@"mosaic"];
            const auto* noise = description.inputDescriptionsByName[@"noise"];
            const auto* output =
                description.outputDescriptionsByName[@"denoised_mosaic"];
            if (mosaic == nil || noise == nil || output == nil
                || mosaic.type != MLFeatureTypeMultiArray
                || noise.type != MLFeatureTypeMultiArray
                || output.type != MLFeatureTypeMultiArray) {
                throw DecodeError(
                    DecodeErrorCode::unsupported,
                    0,
                    "Core ML neural RAW model must expose float multi-arrays named mosaic, "
                    "noise, and denoised_mosaic"
                );
            }
            model_ = [loaded retain];
        }
    }

    ~CoreMlTileInference() override {
        [model_ release];
    }

    void infer(
        const std::span<const float> packed_mosaic,
        const std::span<const float> noise,
        const std::uint32_t packed_tile_edge,
        const std::span<float> denoised_packed_mosaic
    ) override {
        const std::size_t plane =
            static_cast<std::size_t>(packed_tile_edge) * packed_tile_edge;
        if (packed_mosaic.size() != plane * 4U || noise.size() != 8U
            || denoised_packed_mosaic.size() != packed_mosaic.size()) {
            throw DecodeError(
                DecodeErrorCode::internal,
                0,
                "Core ML neural RAW tile received an invalid tensor size"
            );
        }
        @autoreleasepool {
            NSError* error = nil;
            CoreMlOwnedObject mosaic_array(
                [[MLMultiArray alloc]
                    initWithShape:@[@1, @4, @(packed_tile_edge), @(packed_tile_edge)]
                         dataType:MLMultiArrayDataTypeFloat32
                            error:&error]
            );
            if (!mosaic_array) {
                throw DecodeError(
                    DecodeErrorCode::resource_limit,
                    0,
                    "Core ML could not allocate neural RAW mosaic input: "
                        + error_description(error)
                );
            }
            auto* mosaic = static_cast<MLMultiArray*>(mosaic_array.get());
            if (!contiguous_nchw_strides(mosaic.strides, packed_tile_edge)) {
                throw DecodeError(
                    DecodeErrorCode::internal,
                    0,
                    "Core ML allocated a non-contiguous neural RAW mosaic input"
                );
            }
            std::memcpy(
                mosaic.dataPointer,
                packed_mosaic.data(),
                packed_mosaic.size_bytes()
            );

            error = nil;
            CoreMlOwnedObject noise_array(
                [[MLMultiArray alloc]
                    initWithShape:@[@1, @8]
                         dataType:MLMultiArrayDataTypeFloat32
                            error:&error]
            );
            if (!noise_array) {
                throw DecodeError(
                    DecodeErrorCode::resource_limit,
                    0,
                    "Core ML could not allocate neural RAW noise input: "
                        + error_description(error)
                );
            }
            auto* noise_values = static_cast<MLMultiArray*>(noise_array.get());
            if (!contiguous_noise_strides(noise_values.strides)) {
                throw DecodeError(
                    DecodeErrorCode::internal,
                    0,
                    "Core ML allocated a non-contiguous neural RAW noise input"
                );
            }
            std::memcpy(noise_values.dataPointer, noise.data(), noise.size_bytes());

            MLFeatureValue* mosaic_feature =
                [MLFeatureValue featureValueWithMultiArray:mosaic];
            MLFeatureValue* noise_feature =
                [MLFeatureValue featureValueWithMultiArray:noise_values];
            const auto* description = model_.modelDescription;
            if (![description.inputDescriptionsByName[@"mosaic"] isAllowedValue:mosaic_feature]
                || ![description.inputDescriptionsByName[@"noise"]
                    isAllowedValue:noise_feature]) {
                throw DecodeError(
                    DecodeErrorCode::unsupported,
                    0,
                    "Core ML neural RAW model does not accept Shadow's declared tile shapes"
                );
            }

            error = nil;
            CoreMlOwnedObject provider(
                [[MLDictionaryFeatureProvider alloc]
                    initWithDictionary:@{
                        @"mosaic": mosaic_feature,
                        @"noise": noise_feature,
                    }
                                  error:&error]
            );
            if (!provider) {
                throw DecodeError(
                    DecodeErrorCode::internal,
                    0,
                    "Core ML could not prepare neural RAW features: "
                        + error_description(error)
                );
            }
            error = nil;
            id<MLFeatureProvider> prediction = [model_
                predictionFromFeatures:
                    static_cast<MLDictionaryFeatureProvider*>(provider.get())
                                   error:&error];
            if (prediction == nil) {
                throw DecodeError(
                    DecodeErrorCode::internal,
                    0,
                    "Core ML neural RAW inference failed: " + error_description(error)
                );
            }
            MLMultiArray* output =
                [prediction featureValueForName:@"denoised_mosaic"].multiArrayValue;
            const std::array<std::uint32_t, 4U> expected_shape{
                1U,
                4U,
                packed_tile_edge,
                packed_tile_edge,
            };
            if (output == nil || output.dataType != MLMultiArrayDataTypeFloat32
                || !exact_shape(output.shape, expected_shape)
                || !contiguous_nchw_strides(output.strides, packed_tile_edge)) {
                throw DecodeError(
                    DecodeErrorCode::unsupported,
                    0,
                    "Core ML neural RAW model returned an incompatible denoised_mosaic tensor"
                );
            }
            std::memcpy(
                denoised_packed_mosaic.data(),
                output.dataPointer,
                denoised_packed_mosaic.size_bytes()
            );
        }
    }

  private:
    MLModel* model_ = nil;
};

} // namespace

CoreMlNeuralRawDenoiseAttempt try_execute_coreml_neural_raw_denoise(
    const RawFrame& source,
    const PreparedNeuralRawDenoise& prepared
) {
    if (!prepared.execution_requested()) {
        return CoreMlNeuralRawDenoiseAttempt{
            .diagnostic = "Core ML neural RAW denoise received a non-executable plan",
        };
    }
    try {
        // Model loading and a full tile sequence are one bounded transaction. Serializing this
        // development-only v0 path avoids multiplying model/ANE/GPU working sets when an import
        // queue opens several large RAW files concurrently.
        std::lock_guard execution_lock(core_ml_execution_mutex());
        throw_if_row_cancelled();
        const std::string first_identity =
            compiled_model_tree_identity(prepared.configuration.compiled_model_path);
        if (first_identity != prepared.configuration.expected_model_content_identity) {
            return CoreMlNeuralRawDenoiseAttempt{
                .diagnostic =
                    "Core ML neural RAW model content does not match its admitted identity",
            };
        }

        CoreMlTileInference inference(prepared.configuration.compiled_model_path);
        RawFrame denoised = execute_neural_raw_denoise_tiles(
            source,
            prepared.configuration.packed_tile_edge,
            prepared.configuration.packed_halo,
            inference
        );
        // Re-hash after synchronous inference. This closes the ordinary side-load race and refuses
        // to publish pixels if the compiled package changed while Core ML was consuming it.
        const std::string second_identity =
            compiled_model_tree_identity(prepared.configuration.compiled_model_path);
        if (second_identity != first_identity) {
            return CoreMlNeuralRawDenoiseAttempt{
                .diagnostic = "Core ML neural RAW model changed during inference",
            };
        }
        return CoreMlNeuralRawDenoiseAttempt{
            .frame = std::move(denoised),
            .verified_model_content_identity = first_identity,
            .backend_execution_identity = core_ml_execution_identity(),
        };
    } catch (const RowExecutionCancelled&) {
        throw;
    } catch (const std::exception& error) {
        return CoreMlNeuralRawDenoiseAttempt{
            .diagnostic = error.what(),
        };
    }
}

} // namespace shadow::image::detail
