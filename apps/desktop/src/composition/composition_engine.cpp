#include "composition_engine.hpp"
#include <opencv2/imgproc.hpp>
#if CV_VERSION_MAJOR >= 5
#include <opencv2/geometry/2d.hpp>
#endif
#include <algorithm>
#include <cmath>
#include <numeric>
#include <opencv2/photo.hpp>
#include <opencv2/stitching.hpp>
#include <opencv2/stitching/warpers.hpp>
#include <stdexcept>
namespace shadow::composition {
namespace {
constexpr std::int64_t max_pixels = 64LL * 1024 * 1024;
void validate(const std::vector<Input>& in) {
    if (in.size() < 2 || in.size() > 12)
        throw std::runtime_error("input-count");
    std::int64_t pixels = 0;
    for (const auto& f : in) {
        pixels += f.rgb.total();
        if (f.rgb.empty() || f.rgb.type() != CV_32FC3 || !cv::checkRange(f.rgb)
            || !std::isfinite(f.exposure) || f.exposure <= 0)
            throw std::runtime_error("invalid-input");
        if (!f.confidence.empty()
            && (f.confidence.size() != f.rgb.size() || f.confidence.type() != CV_32FC1
                || !cv::checkRange(f.confidence, true, nullptr, 0, 1.001)))
            throw std::runtime_error("invalid-input");
    }
    if (pixels > max_pixels)
        throw std::runtime_error("memory-budget");
}
float luma(cv::Vec3f v) {
    return .2126f * v[0] + .7152f * v[1] + .0722f * v[2];
}
} // namespace
cv::Mat display_rgb8(const cv::Mat& linear) {
    cv::Mat out(linear.size(), CV_8UC3);
    for (int y = 0; y < linear.rows; ++y) {
        auto* dst = out.ptr<cv::Vec3b>(y);
        const auto* src = linear.ptr<cv::Vec3f>(y);
        for (int x = 0; x < linear.cols; ++x) {
            float peak = std::max({0.f, src[x][0], src[x][1], src[x][2]});
            // Preview only: common-channel shoulder preserves hue; the master is untouched.
            float gain =
                peak > .75f ? (.75f + .25f * (1 - std::exp(-(peak - .75f) / .25f))) / peak : 1.f;
            for (int c = 0; c < 3; ++c) {
                float v = std::clamp(src[x][c] * gain, 0.f, 1.f);
                v = v <= .0031308f ? 12.92f * v : 1.055f * std::pow(v, 1 / 2.4f) - .055f;
                dst[x][c] = cv::saturate_cast<uchar>(v * 255);
            }
        }
    }
    return out;
}
Result merge_hdr(const std::vector<Input>& in, bool align, bool deghost, Progress progress) {
    validate(in);
    for (const auto& f : in)
        if (f.rgb.size() != in[0].rgb.size())
            throw std::runtime_error("hdr-dimensions");
    std::vector<size_t> order(in.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](auto a, auto b) {
        return in[a].exposure < in[b].exposure;
    });
    const size_t reference = order[order.size() / 2];
    const double reference_exposure = in[reference].exposure;
    if (in[order.back()].exposure / in[order.front()].exposure < 1.2)
        throw std::runtime_error("hdr-exposures");
    Result result;
    result.offsets.resize(in.size());
    auto gray = [](const cv::Mat& f) {
        cv::Mat g;
        cv::cvtColor(display_rgb8(f), g, cv::COLOR_RGB2GRAY);
        return g;
    };
    if (align) {
        auto mtb = cv::createAlignMTB(7, 4, false);
        auto ref = gray(in[reference].rgb);
        auto registration = [&](size_t i) {
            cv::Mat lumaImage(in[i].rgb.size(), CV_32F);
            const float gain = float(reference_exposure / in[i].exposure);
            for (int y = 0; y < lumaImage.rows; ++y)
                for (int x = 0; x < lumaImage.cols; ++x)
                    lumaImage.at<float>(y, x) =
                        std::log1p(std::max(0.f, luma(in[i].rgb.at<cv::Vec3f>(y, x)) * gain));
            return lumaImage;
        };
        auto referenceImage = registration(reference);
        cv::Mat window;
        cv::createHanningWindow(window, referenceImage.size(), CV_32F);
        auto error = [&](size_t i, cv::Point shift) {
            std::vector<float> errors;
            const float gain = float(reference_exposure / in[i].exposure);
            for (int y = 4; y < ref.rows - 4; y += 4) {
                const int sy = y - shift.y;
                if (sy < 0 || sy >= ref.rows)
                    continue;
                for (int x = 4; x < ref.cols - 4; x += 4) {
                    const int sx = x - shift.x;
                    if (sx < 0 || sx >= ref.cols)
                        continue;
                    if ((!in[i].confidence.empty() && in[i].confidence.at<float>(sy, sx) < .5f)
                        || (!in[reference].confidence.empty()
                            && in[reference].confidence.at<float>(y, x) < .5f))
                        continue;
                    const auto a = in[i].rgb.at<cv::Vec3f>(sy, sx) * gain;
                    const auto b = in[reference].rgb.at<cv::Vec3f>(y, x);
                    errors.push_back(float(cv::norm(a - b)) / (.03f + float(cv::norm(b))));
                }
            }
            if (errors.size() < 128)
                return 1.f;
            auto middle = errors.begin() + errors.size() / 2;
            std::nth_element(errors.begin(), middle, errors.end());
            return *middle;
        };
        for (size_t i = 0; i < in.size(); ++i) {
            if (progress)
                progress("align", int(i));
            if (i == reference)
                continue;
            auto moving = registration(i);
            // phaseCorrelate applies its window in place: preserve the reusable reference.
            auto fixed = referenceImage.clone();
            auto phase = cv::phaseCorrelate(fixed, moving, window);
            std::vector<cv::Point> candidates{{0, 0}, mtb->calculateShift(ref, gray(in[i].rgb))};
            if (std::isfinite(phase.x) && std::isfinite(phase.y) && std::abs(phase.x) < 126
                && std::abs(phase.y) < 126)
                candidates.emplace_back(int(std::lround(-phase.x)), int(std::lround(-phase.y)));
            float best = 1.f;
            for (auto candidate : candidates) {
                if (std::abs(candidate.x) >= 126 || std::abs(candidate.y) >= 126)
                    continue;
                const float residual = error(i, candidate);
                if (residual < best) {
                    best = residual;
                    result.offsets[i] = candidate;
                }
            }
            if (best > .2f)
                throw std::runtime_error("hdr-alignment");
        }
    }
    cv::Rect crop(0, 0, in[0].rgb.cols, in[0].rgb.rows);
    for (auto shift : result.offsets)
        crop &= cv::Rect(shift.x, shift.y, in[0].rgb.cols, in[0].rgb.rows);
    if (crop.area() < in[0].rgb.total() * .7)
        throw std::runtime_error("hdr-alignment");
    result.rgb = cv::Mat(crop.size(), CV_32FC3);
    std::vector<cv::Vec3f> values(in.size());
    std::vector<float> weights(in.size());
    for (int y = 0; y < crop.height; ++y) {
        auto* dst = result.rgb.ptr<cv::Vec3f>(y);
        for (int x = 0; x < crop.width; ++x) {
            size_t best = reference;
            float best_weight = -1;
            for (size_t i = 0; i < in.size(); ++i) {
                int sx = x + crop.x - result.offsets[i].x, sy = y + crop.y - result.offsets[i].y;
                auto v = in[i].rgb.at<cv::Vec3f>(sy, sx);
                values[i] = v * float(reference_exposure / in[i].exposure);
                float confidence =
                    in[i].confidence.empty() ? 1.f : in[i].confidence.at<float>(sy, sx);
                // A single weight is shared by RGB; never select a different exposure per channel.
                float signal = std::max(0.f, luma(v));
                weights[i] = confidence * std::max(.00001f, std::min(signal, .8f));
                if (weights[i] > best_weight) {
                    best_weight = weights[i];
                    best = i;
                }
            }
            if (weights[reference] > .01f)
                best = reference;
            cv::Vec3f sum(0, 0, 0);
            float total = 0;
            for (size_t i = 0; i < in.size(); ++i) {
                float w = weights[i];
                if (deghost && i != best) {
                    auto delta = values[i] - values[best];
                    float error =
                        std::max({std::abs(delta[0]), std::abs(delta[1]), std::abs(delta[2])});
                    float threshold = .02f + .2f * std::max(.02f, luma(values[best]));
                    if (error > threshold)
                        w = 0;
                }
                sum += values[i] * w;
                total += w;
            }
            dst[x] = total > 1e-8f ? sum / total : values[order.front()];
        }
    }
    return result;
}
Result merge_panorama(const std::vector<Input>& in, bool compensate, Progress progress) {
    validate(in);
    std::vector<cv::Mat> registration;
    registration.reserve(in.size());
    for (const auto& f : in)
        registration.push_back(display_rgb8(f.rgb));
    auto stitcher = cv::Stitcher::create(cv::Stitcher::PANORAMA);
    stitcher->setRegistrationResol(.6);
    stitcher->setPanoConfidenceThresh(.8);
    if (progress)
        progress("align", 0);
    if (stitcher->estimateTransform(registration) != cv::Stitcher::OK
        || stitcher->component().size() != in.size())
        throw std::runtime_error("panorama-overlap");
    auto cameras = stitcher->cameras();
    auto components = stitcher->component();
    std::vector<double> focals;
    for (const auto& c : cameras) {
        if (!std::isfinite(c.focal) || c.focal <= 0 || !cv::checkRange(c.R)
            || !cv::checkRange(c.K()))
            throw std::runtime_error("panorama-geometry");
        focals.push_back(c.focal);
    }
    std::sort(focals.begin(), focals.end());
    double aspect = 1.0 / stitcher->workScale();
    if (!std::isfinite(aspect) || aspect <= 0 || focals.back() * aspect > 1e6)
        throw std::runtime_error("panorama-geometry");
    auto warper =
        cv::makePtr<cv::SphericalWarper>()->create(float(focals[focals.size() / 2] * aspect));
    std::vector<cv::Mat> matrices;
    cv::Rect bounds;
    for (size_t i = 0; i < cameras.size(); ++i) {
        cv::Mat k;
        cameras[i].K().convertTo(k, CV_32F);
        k.at<float>(0, 0) *= aspect;
        k.at<float>(1, 1) *= aspect;
        k.at<float>(0, 2) *= aspect;
        k.at<float>(1, 2) *= aspect;
        auto rect = warper->warpRoi(in[components[i]].rgb.size(), k, cameras[i].R);
        if (rect.width <= 0 || rect.height <= 0 || int64_t(rect.width) * rect.height > max_pixels)
            throw std::runtime_error("memory-budget");
        if (std::abs(int64_t(rect.x)) > 100000000 || std::abs(int64_t(rect.y)) > 100000000)
            throw std::runtime_error("panorama-geometry");
        bounds = i == 0 ? rect : bounds | rect;
        matrices.push_back(k);
    }
    if (bounds.width <= 0 || bounds.height <= 0
        || int64_t(bounds.width) * bounds.height > max_pixels)
        throw std::runtime_error("memory-budget");
    cv::Mat sum(bounds.size(), CV_32FC3, cv::Scalar::all(0)),
        weights(bounds.size(), CV_32F, cv::Scalar(0));
    Result result;
    result.gains.resize(in.size(), 1);
    for (size_t i = 0; i < cameras.size(); ++i) {
        if (progress)
            progress("blend", int(i));
        cv::Mat warped, mask, source_mask(in[components[i]].rgb.size(), CV_8U, cv::Scalar(255));
        auto corner = warper->warp(
            in[components[i]].rgb,
            matrices[i],
            cameras[i].R,
            cv::INTER_LINEAR,
            cv::BORDER_CONSTANT,
            warped
        );
        warper->warp(
            source_mask,
            matrices[i],
            cameras[i].R,
            cv::INTER_NEAREST,
            cv::BORDER_CONSTANT,
            mask
        );
        cv::Rect target(corner.x - bounds.x, corner.y - bounds.y, warped.cols, warped.rows);
        if ((target & cv::Rect(0, 0, bounds.width, bounds.height)) != target)
            throw std::runtime_error("panorama-geometry");
        cv::Mat distance;
        cv::copyMakeBorder(mask, distance, 1, 1, 1, 1, cv::BORDER_CONSTANT, 0);
        cv::distanceTransform(distance, distance, cv::DIST_L2, 3);
        distance = distance(cv::Rect(1, 1, mask.cols, mask.rows));
        auto dst = sum(target);
        auto existing_weight = weights(target);
        std::vector<float> ratios;
        if (compensate && i > 0) {
            for (int y = 0; y < mask.rows; y += 8)
                for (int x = 0; x < mask.cols; x += 8) {
                    float weight = existing_weight.at<float>(y, x);
                    if (mask.at<uchar>(y, x) && weight > .2) {
                        float a = luma(dst.at<cv::Vec3f>(y, x)) / weight,
                              b = luma(warped.at<cv::Vec3f>(y, x));
                        if (a > .02f && b > .02f)
                            ratios.push_back(a / b);
                    }
                }
        }
        float gain = 1;
        if (ratios.size() > 32) {
            auto middle = ratios.begin() + ratios.size() / 2;
            std::nth_element(ratios.begin(), middle, ratios.end());
            gain = std::clamp(*middle, .5f, 2.f);
        }
        result.gains[components[i]] = gain;
        for (int y = 0; y < mask.rows; ++y) {
            auto* d = dst.ptr<cv::Vec3f>(y);
            auto* w = existing_weight.ptr<float>(y);
            auto* v = warped.ptr<cv::Vec3f>(y);
            auto* feather = distance.ptr<float>(y);
            for (int x = 0; x < mask.cols; ++x) {
                float weight = std::min(feather[x] / 32.f, 1.f);
                d[x] += v[x] * (gain * weight);
                w[x] += weight;
            }
        }
    }
    // Largest fully covered rectangle: remove curved borders without stretching or invented pixels.
    std::vector<int> heights(bounds.width, 0), stack;
    cv::Rect crop;
    int64_t area = 0;
    for (int y = 0; y < bounds.height; ++y) {
        const auto* w = weights.ptr<float>(y);
        for (int x = 0; x < bounds.width; ++x)
            heights[x] = w[x] > 1e-5f ? heights[x] + 1 : 0;
        stack.clear();
        for (int x = 0; x <= bounds.width; ++x) {
            int h = x == bounds.width ? 0 : heights[x];
            while (!stack.empty() && heights[stack.back()] > h) {
                int p = stack.back();
                stack.pop_back();
                int left = stack.empty() ? 0 : stack.back() + 1;
                int width = x - left;
                int64_t candidate = int64_t(width) * heights[p];
                if (candidate > area) {
                    area = candidate;
                    crop = {left, y - heights[p] + 1, width, heights[p]};
                }
            }
            stack.push_back(x);
        }
    }
    if (area < 1024 || crop.width < 32 || crop.height < 32)
        throw std::runtime_error("panorama-geometry");
    result.rgb = sum(crop).clone();
    for (int y = 0; y < crop.height; ++y) {
        auto* d = result.rgb.ptr<cv::Vec3f>(y);
        auto* w = weights.ptr<float>(crop.y + y) + crop.x;
        for (int x = 0; x < crop.width; ++x)
            d[x] /= w[x];
    }
    return result;
}
} // namespace shadow::composition
