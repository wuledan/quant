// config_source.h — Abstract configuration source
#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace quant::infra {

// ── Config value type ──
using ConfigValue = std::variant<
    bool, int64_t, double, std::string,
    std::vector<bool>, std::vector<int64_t>,
    std::vector<double>, std::vector<std::string>
>;

// ── Config source interface ──
class ConfigSource {
public:
    virtual ~ConfigSource() = default;
    virtual std::string_view name() const noexcept = 0;
    virtual bool load(std::map<std::string, ConfigValue>& kv) = 0;
};

}  // namespace quant::infra
