#pragma once

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace shadow::image::test_support {

class ScopedEnvironment final {
public:
    ScopedEnvironment(const std::string_view name, const std::string_view value)
        : name_(name) {
        if (const char* current = std::getenv(name_.c_str()); current != nullptr) {
            previous_ = current;
        }
#if defined(_WIN32)
        static_cast<void>(_putenv_s(name_.c_str(), std::string(value).c_str()));
#else
        static_cast<void>(setenv(name_.c_str(), std::string(value).c_str(), 1));
#endif
    }

    ~ScopedEnvironment() {
#if defined(_WIN32)
        static_cast<void>(_putenv_s(
            name_.c_str(),
            previous_.has_value() ? previous_->c_str() : ""
        ));
#else
        if (previous_.has_value()) {
            static_cast<void>(setenv(name_.c_str(), previous_->c_str(), 1));
        } else {
            static_cast<void>(unsetenv(name_.c_str()));
        }
#endif
    }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    std::string name_;
    std::optional<std::string> previous_;
};

} // namespace shadow::image::test_support
