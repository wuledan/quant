// yaml_source.cc — YAML config source implementation using yaml-cpp
#include "cpp/quant/infra/config/yaml_source.h"

#include <yaml-cpp/yaml.h>

#include <optional>

#include <fstream>

namespace quant::infra {
namespace {

// ── Convert YAML node → ConfigValue ──
std::optional<ConfigValue> yaml_to_config_value(const YAML::Node& node) {
    switch (node.Type()) {
        case YAML::NodeType::Scalar: {
            // YAML auto-converts "true"/"false"/"yes"/"no" to bool, "123" to int, etc.
            // We need to respect the YAML spec: let yaml-cpp do its thing,
            // but try types in a reasonable order.
            // Note: yaml-cpp treats "true"/"false" as bool by spec.
            // If the user wants a string "true", they must quote it in YAML.
            if (node.Tag() == "!") {
                // Explicit tag forces string
                return ConfigValue{node.Scalar()};
            }
            // Try bool, then integer, then double, then string
            bool b;
            if (YAML::convert<bool>::decode(node, b)) {
                // Disambiguate: if the original scalar looks like a bool keyword
                const std::string& s = node.Scalar();
                std::string lower;
                lower.reserve(s.size());
                for (char c : s) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
                if (lower == "true" || lower == "false" || lower == "yes" || lower == "no" ||
                    lower == "on" || lower == "off") {
                    return ConfigValue{b};
                }
                // Not a bool keyword — continue to try int/double
            }
            long long i = 0;
            if (YAML::convert<long long>::decode(node, i)) {
                return ConfigValue{static_cast<int64_t>(i)};
            }
            double d;
            if (YAML::convert<double>::decode(node, d)) {
                return ConfigValue{d};
            }
            return ConfigValue{node.Scalar()};
        }
        case YAML::NodeType::Sequence: {
            if (node.size() == 0) return ConfigValue{std::vector<std::string>{}};

            // Determine element type from first element
            auto first = yaml_to_config_value(node[0]);
            if (!first.has_value()) return std::nullopt;

            return std::visit([&](const auto& first_val) -> std::optional<ConfigValue> {
                using T = std::decay_t<decltype(first_val)>;
                if constexpr (std::is_same_v<T, bool>) {
                    std::vector<bool> result;
                    result.push_back(first_val);
                    for (size_t i = 1; i < node.size(); ++i) {
                        auto v = yaml_to_config_value(node[i]);
                        if (!v.has_value() || !std::holds_alternative<bool>(*v))
                            return std::nullopt;
                        result.push_back(std::get<bool>(*v));
                    }
                    return ConfigValue{std::move(result)};
                } else if constexpr (std::is_same_v<T, int64_t>) {
                    std::vector<int64_t> result;
                    result.push_back(first_val);
                    for (size_t i = 1; i < node.size(); ++i) {
                        auto v = yaml_to_config_value(node[i]);
                        if (!v.has_value() || !std::holds_alternative<int64_t>(*v))
                            return std::nullopt;
                        result.push_back(std::get<int64_t>(*v));
                    }
                    return ConfigValue{std::move(result)};
                } else if constexpr (std::is_same_v<T, double>) {
                    std::vector<double> result;
                    result.push_back(first_val);
                    for (size_t i = 1; i < node.size(); ++i) {
                        auto v = yaml_to_config_value(node[i]);
                        if (!v.has_value() || !std::holds_alternative<double>(*v))
                            return std::nullopt;
                        result.push_back(std::get<double>(*v));
                    }
                    return ConfigValue{std::move(result)};
                } else if constexpr (std::is_same_v<T, std::string>) {
                    std::vector<std::string> result;
                    result.push_back(first_val);
                    for (size_t i = 1; i < node.size(); ++i) {
                        auto v = yaml_to_config_value(node[i]);
                        if (!v.has_value() || !std::holds_alternative<std::string>(*v))
                            return std::nullopt;
                        result.push_back(std::get<std::string>(*v));
                    }
                    return ConfigValue{std::move(result)};
                }
                return std::nullopt;
            }, *first);
        }
        default:
            return std::nullopt;
    }
}

// ── Convert ConfigValue → YAML node ──
YAML::Node config_to_yaml(const ConfigValue& cv) {
    YAML::Node node;
    std::visit([&](const auto& v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
            node = v;
        } else if constexpr (std::is_same_v<T, int64_t>) {
            node = v;
        } else if constexpr (std::is_same_v<T, double>) {
            node = v;
        } else if constexpr (std::is_same_v<T, std::string>) {
            node = v;
        } else if constexpr (std::is_same_v<T, std::vector<bool>>) {
            for (auto b : v) node.push_back(b);
        } else if constexpr (std::is_same_v<T, std::vector<int64_t>>) {
            for (const auto& i : v) node.push_back(i);
        } else if constexpr (std::is_same_v<T, std::vector<double>>) {
            for (const auto& d : v) node.push_back(d);
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            for (const auto& s : v) node.push_back(s);
        }
    }, cv);
    return node;
}

// ── Recursively walk YAML node, populate flat kv map ──
void walk_node(const YAML::Node& node, const std::string& prefix,
                std::map<std::string, ConfigValue>& kv) {
    if (!node.IsMap()) return;
    for (const auto& it : node) {
        std::string key = it.first.Scalar();
        std::string full = prefix.empty() ? key : prefix + "." + key;
        const auto& val = it.second;
        if (val.IsMap()) {
            walk_node(val, full, kv);
        } else {
            auto cv = yaml_to_config_value(val);
            if (cv.has_value()) {
                kv[full] = std::move(*cv);
            }
        }
    }
}

// ── Build a nested YAML node from a flat dot-separated map ──
YAML::Node build_nested_node(const std::map<std::string, ConfigValue>& kv) {
    YAML::Node root;
    for (const auto& [key, val] : kv) {
        auto dot = key.find('.');
        if (dot == std::string::npos) {
            root[key] = config_to_yaml(val);
        } else {
            std::string first = key.substr(0, dot);
            std::string rest = key.substr(dot + 1);
            YAML::Node current_node = root[first];
            while (true) {
                auto next_dot = rest.find('.');
                if (next_dot == std::string::npos) {
                    current_node[rest] = config_to_yaml(val);
                    break;
                }
                std::string part = rest.substr(0, next_dot);
                rest = rest.substr(next_dot + 1);
                current_node.reset(current_node[part]);
            }
        }
    }
    return root;
}

}  // anonymous namespace

YamlConfigSource::YamlConfigSource(std::string_view file_path)
    : file_path_(file_path) {}

bool YamlConfigSource::load(std::map<std::string, ConfigValue>& kv) {
    try {
        auto node = YAML::LoadFile(file_path_);
        if (!node.IsMap()) return false;
        walk_node(node, "", kv);
        return true;
    } catch (const YAML::Exception&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

bool YamlConfigSource::save(const std::map<std::string, ConfigValue>& kv) {
    try {
        auto node = build_nested_node(kv);
        std::ofstream ofs(file_path_);
        if (!ofs.is_open()) return false;
        YAML::Emitter emitter;
        emitter << node;
        ofs << emitter.c_str();
        return ofs.good();
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace quant::infra
