// config_validator.h — Config value validator
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace quant::infra {

template<typename T>
class ConfigValidator {
public:
    ConfigValidator& range(T min_val, T max_val) {
        min_ = min_val;
        max_ = max_val;
        return *this;
    }

    ConfigValidator& default_value(T val) {
        default_ = std::move(val);
        return *this;
    }

    ConfigValidator& description(std::string_view desc) {
        desc_ = desc;
        return *this;
    }

    std::optional<T> default_value() const { return default_; }
    bool validate(const T& value) const {
        if (min_ && value < *min_) return false;
        if (max_ && value > *max_) return false;
        return true;
    }
    std::string_view description() const { return desc_; }

private:
    std::optional<T> default_;
    std::optional<T> min_;
    std::optional<T> max_;
    std::string desc_;
};

}  // namespace quant::infra
