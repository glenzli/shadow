#pragma once
#include <functional>
#include <opencv2/core.hpp>
#include <string>
#include <vector>
namespace shadow::composition {
struct Input {
    cv::Mat rgb;        // CV_32FC3, linear sRGB D65; retained super-white/negative values.
    cv::Mat confidence; // CV_32FC1, actual RAW sensor clipping admission or raster reliability.
    double exposure = 1.0;
};
struct Result {
    cv::Mat rgb;
    std::vector<cv::Point> offsets;
    std::vector<double> gains;
};
using Progress = std::function<void(const char*, int)>;
Result
merge_hdr(const std::vector<Input>& inputs, bool align, bool deghost, Progress progress = {});
Result merge_panorama(const std::vector<Input>& inputs, bool compensate, Progress progress = {});
cv::Mat display_rgb8(const cv::Mat& linear);
} // namespace shadow::composition
