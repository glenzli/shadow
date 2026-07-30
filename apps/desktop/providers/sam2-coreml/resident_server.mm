#import <Foundation/Foundation.h>

#include "resident_server.hpp"

#include "sam2_coreml_engine.hpp"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace shadow_sam2_coreml {
namespace {

constexpr NSInteger kProtocolVersion = 1;
constexpr std::size_t kMaximumCommandBytes = 64 * 1024;

bool write_json(NSDictionary* payload) {
    NSError* error = nil;
    NSData* encoded =
        [NSJSONSerialization dataWithJSONObject:payload options:0 error:&error];
    if (encoded == nil) {
        std::fprintf(
            stderr,
            "failed to encode resident response: %s\n",
            error.localizedDescription.UTF8String
        );
        return false;
    }
    if (std::fwrite(encoded.bytes, 1, encoded.length, stdout) != encoded.length
        || std::fputc('\n', stdout) == EOF || std::fflush(stdout) != 0) {
        std::fputs("failed to write resident response\n", stderr);
        return false;
    }
    return true;
}

NSDictionary* decode_request(const std::string& line) {
    NSData* data = [NSData dataWithBytes:line.data() length:line.size()];
    NSError* error = nil;
    id decoded = [NSJSONSerialization JSONObjectWithData:data options:0 error:&error];
    if (![decoded isKindOfClass:[NSDictionary class]]) {
        std::fprintf(
            stderr,
            "failed to decode resident request: %s\n",
            error.localizedDescription.UTF8String
        );
        return nil;
    }
    return decoded;
}

bool valid_request_header(
    NSDictionary* request,
    NSNumber* request_id,
    NSString* operation
) {
    NSNumber* protocol = request[@"protocol"];
    return [protocol isKindOfClass:[NSNumber class]]
           && protocol.integerValue == kProtocolVersion
           && [request_id isKindOfClass:[NSNumber class]]
           && request_id.unsignedLongLongValue > 0
           && [operation isKindOfClass:[NSString class]];
}

bool write_error(NSNumber* request_id, NSString* operation, NSString* code) {
    return write_json(@{
        @"protocol" : @(kProtocolVersion),
        @"type" : @"response",
        @"id" : request_id ?: @0,
        @"op" : operation ?: @"unknown",
        @"ok" : @NO,
        @"error" : code,
    });
}

std::optional<std::vector<PromptPoint>> decode_points(NSArray* raw_points) {
    if (![raw_points isKindOfClass:[NSArray class]] || raw_points.count == 0
        || raw_points.count > kMaximumPromptPoints) {
        return std::nullopt;
    }
    std::vector<PromptPoint> points;
    points.reserve(raw_points.count);
    bool has_foreground = false;
    for (id raw_point in raw_points) {
        if (![raw_point isKindOfClass:[NSDictionary class]]) {
            return std::nullopt;
        }
        NSDictionary* point = raw_point;
        NSNumber* x = point[@"x"];
        NSNumber* y = point[@"y"];
        NSNumber* foreground = point[@"foreground"];
        if (![x isKindOfClass:[NSNumber class]] || ![y isKindOfClass:[NSNumber class]]
            || ![foreground isKindOfClass:[NSNumber class]]) {
            return std::nullopt;
        }
        const double normalized_x = x.doubleValue;
        const double normalized_y = y.doubleValue;
        if (!std::isfinite(normalized_x) || !std::isfinite(normalized_y)
            || normalized_x < 0.0 || normalized_x > 1.0 || normalized_y < 0.0
            || normalized_y > 1.0) {
            return std::nullopt;
        }
        const bool include = foreground.boolValue;
        has_foreground = has_foreground || include;
        points.push_back({
            .x = normalized_x,
            .y = normalized_y,
            .foreground = include,
        });
    }
    if (!has_foreground) {
        return std::nullopt;
    }
    return points;
}

std::uint64_t elapsed_milliseconds(
    const std::chrono::steady_clock::time_point started
) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started
        )
            .count()
    );
}

bool handle_load_image(
    Engine& engine,
    NSDictionary* request,
    NSNumber* request_id,
    NSString* operation
) {
    NSString* input_jpeg = request[@"input_jpeg"];
    NSString* input_hash = request[@"input_hash"];
    if (![input_jpeg isKindOfClass:[NSString class]] || input_jpeg.length == 0
        || ![input_hash isKindOfClass:[NSString class]] || input_hash.length == 0) {
        return write_error(request_id, operation, @"invalid_image_request");
    }

    const auto started = std::chrono::steady_clock::now();
    bool cache_hit = false;
    if (!engine.load_image(input_jpeg.UTF8String, input_hash.UTF8String, cache_hit)) {
        return write_error(request_id, operation, @"image_encoding_failed");
    }
    return write_json(@{
        @"protocol" : @(kProtocolVersion),
        @"type" : @"response",
        @"id" : request_id,
        @"op" : operation,
        @"ok" : @YES,
        @"cache_hit" : @(cache_hit),
        @"elapsed_ms" : @(elapsed_milliseconds(started)),
    });
}

bool handle_predict(
    Engine& engine,
    NSDictionary* request,
    NSNumber* request_id,
    NSString* operation
) {
    NSString* input_hash = request[@"input_hash"];
    NSString* output_mask = request[@"output_mask"];
    const auto points = decode_points(request[@"points"]);
    if (![input_hash isKindOfClass:[NSString class]] || input_hash.length == 0
        || ![output_mask isKindOfClass:[NSString class]] || output_mask.length == 0
        || !points.has_value()) {
        return write_error(request_id, operation, @"invalid_prediction_request");
    }
    if (!engine.has_loaded_image(input_hash.UTF8String)) {
        return write_error(request_id, operation, @"image_identity_not_loaded");
    }
    if ([[NSFileManager defaultManager] fileExistsAtPath:output_mask]) {
        return write_error(request_id, operation, @"output_exists");
    }

    const auto started = std::chrono::steady_clock::now();
    const auto receipt = engine.predict(*points, output_mask.UTF8String);
    if (!receipt.has_value()) {
        return write_error(request_id, operation, @"prediction_failed");
    }
    return write_json(@{
        @"protocol" : @(kProtocolVersion),
        @"type" : @"response",
        @"id" : request_id,
        @"op" : operation,
        @"ok" : @YES,
        @"width" : @(kMaskEdge),
        @"height" : @(kMaskEdge),
        @"score" : @(receipt->score),
        @"points" : @(receipt->point_count),
        @"elapsed_ms" : @(elapsed_milliseconds(started)),
    });
}

} // namespace

bool run_resident_server(Engine& engine, const std::string& model_revision) {
    if (!write_json(@{
            @"protocol" : @(kProtocolVersion),
            @"type" : @"ready",
            @"revision" : [NSString stringWithUTF8String:model_revision.c_str()],
        })) {
        return false;
    }

    std::string line;
    while (std::getline(std::cin, line)) {
        @autoreleasepool {
            if (line.empty()) {
                continue;
            }
            if (line.size() > kMaximumCommandBytes) {
                if (!write_error(nil, nil, @"command_too_large")) {
                    return false;
                }
                continue;
            }
            NSDictionary* request = decode_request(line);
            NSNumber* request_id = request[@"id"];
            NSString* operation = request[@"op"];
            if (request == nil
                || !valid_request_header(request, request_id, operation)) {
                if (!write_error(request_id, operation, @"invalid_request_header")) {
                    return false;
                }
                continue;
            }
            if ([operation isEqualToString:@"load_image"]) {
                if (!handle_load_image(engine, request, request_id, operation)) {
                    return false;
                }
            } else if ([operation isEqualToString:@"predict"]) {
                if (!handle_predict(engine, request, request_id, operation)) {
                    return false;
                }
            } else if ([operation isEqualToString:@"shutdown"]) {
                return write_json(@{
                    @"protocol" : @(kProtocolVersion),
                    @"type" : @"response",
                    @"id" : request_id,
                    @"op" : operation,
                    @"ok" : @YES,
                });
            } else if (!write_error(request_id, operation, @"unknown_operation")) {
                return false;
            }
        }
    }
    return true;
}

} // namespace shadow_sam2_coreml
