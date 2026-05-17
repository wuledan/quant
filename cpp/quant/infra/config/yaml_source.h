// yaml_source.h — YAML configuration source
#pragma once

#include <map>
#include <string>
#include <string_view>

#include "cpp/quant/infra/config/config_source.h"

namespace quant::infra {

class YamlConfigSource : public ConfigSource {
public:
    explicit YamlConfigSource(std::string_view file_path);

    std::string_view name() const noexcept override { return "yaml"; }
    bool load(std::map<std::string, ConfigValue>& kv) override;
    bool save(const std::map<std::string, ConfigValue>& kv);

private:
    std::string file_path_;
};

}  // namespace quant::infra
