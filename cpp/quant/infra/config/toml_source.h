// toml_source.h — TOML configuration source
#pragma once

#include <map>
#include <string>
#include <string_view>

#include "cpp/quant/infra/config/config_source.h"

namespace quant::infra {

class TomlConfigSource : public ConfigSource {
public:
    explicit TomlConfigSource(std::string_view file_path);

    std::string_view name() const noexcept override { return "toml"; }
    bool load(std::map<std::string, ConfigValue>& kv) override;

private:
    std::string file_path_;
};

}  // namespace quant::infra
