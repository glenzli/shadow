#pragma once

#include <iostream>
#include <string_view>

namespace shadow::image::test_support {

inline int failures = 0;

inline void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        ++failures;
    }
}

} // namespace shadow::image::test_support
