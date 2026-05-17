// toml_source.cc — TOML config source implementation
#include "cpp/quant/infra/config/toml_source.h"

#include <toml.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace quant::infra {
namespace {

// ── Convert toml::value → ConfigValue ──
std::optional<ConfigValue> toml_to_config_value(const toml::value& val) {
    switch (val.type()) {
        case toml::value_t::boolean:
            return ConfigValue{toml::get<bool>(val)};
        case toml::value_t::integer:
            return ConfigValue{toml::get<std::int64_t>(val)};
        case toml::value_t::floating:
            return ConfigValue{toml::get<double>(val)};
        case toml::value_t::string:
            return ConfigValue{toml::get<std::string>(val)};
        case toml::value_t::array: {
            const auto& arr = val.as_array();
            if (arr.empty()) return ConfigValue{std::vector<std::string>{}};

            // Check element type from first element
            switch (arr.front().type()) {
                case toml::value_t::boolean: {
                    std::vector<bool> result;
                    for (const auto& e : arr) {
                        if (!e.is_boolean()) return std::nullopt;
                        result.push_back(toml::get<bool>(e));
                    }
                    return ConfigValue{std::move(result)};
                }
                case toml::value_t::integer: {
                    std::vector<int64_t> result;
                    for (const auto& e : arr) {
                        if (!e.is_integer()) return std::nullopt;
                        result.push_back(toml::get<std::int64_t>(e));
                    }
                    return ConfigValue{std::move(result)};
                }
                case toml::value_t::floating: {
                    std::vector<double> result;
                    for (const auto& e : arr) {
                        if (!e.is_floating()) return std::nullopt;
                        result.push_back(toml::get<double>(e));
                    }
                    return ConfigValue{std::move(result)};
                }
                case toml::value_t::string: {
                    std::vector<std::string> result;
                    for (const auto& e : arr) {
                        if (!e.is_string()) return std::nullopt;
                        result.push_back(toml::get<std::string>(e));
                    }
                    return ConfigValue{std::move(result)};
                }
                default:
                    return std::nullopt;
            }
        }
        default:
            return std::nullopt;
    }
}

// ── Convert ConfigValue → toml::value ──
toml::value config_to_toml(const ConfigValue& cv) {
    return std::visit([](const auto& v) -> toml::value {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, bool>) {
            return toml::value(v);
        } else if constexpr (std::is_same_v<T, int64_t>) {
            return toml::value(v);
        } else if constexpr (std::is_same_v<T, double>) {
            return toml::value(v);
        } else if constexpr (std::is_same_v<T, std::string>) {
            return toml::value(v);
        } else if constexpr (std::is_same_v<T, std::vector<bool>>) {
            toml::array arr;
            for (auto b : v) arr.push_back(toml::value(b));
            return toml::value(arr);
        } else if constexpr (std::is_same_v<T, std::vector<int64_t>>) {
            toml::array arr;
            for (const auto& i : v) arr.push_back(toml::value(i));
            return toml::value(arr);
        } else if constexpr (std::is_same_v<T, std::vector<double>>) {
            toml::array arr;
            for (const auto& d : v) arr.push_back(toml::value(d));
            return toml::value(arr);
        } else if constexpr (std::is_same_v<T, std::vector<std::string>>) {
            toml::array arr;
            for (const auto& s : v) arr.push_back(toml::value(s));
            return toml::value(arr);
        }
    }, cv);
}

// ── Recursively walk TOML table, populate flat kv map ──
void walk_table(const toml::table& tbl, const std::string& prefix,
                std::map<std::string, ConfigValue>& kv) {
    for (const auto& [key, val] : tbl) {
        std::string full = prefix.empty() ? std::string(key) : prefix + "." + std::string(key);
        if (val.is_table()) {
            walk_table(val.as_table(), full, kv);
        } else {
            auto cv = toml_to_config_value(val);
            if (cv.has_value()) {
                kv[full] = std::move(*cv);
            }
        }
    }
}

// ── Build a nested toml::table from a flat dot-separated map ──
toml::table build_nested_table(const std::map<std::string, ConfigValue>& kv) {
    toml::table root;
    for (const auto& [key, val] : kv) {
        auto dot = key.find('.');
        if (dot == std::string::npos) {
            root[key] = config_to_toml(val);
        } else {
            std::string first = key.substr(0, dot);
            std::string rest = key.substr(dot + 1);
            toml::table* current = nullptr;
            if (auto it = root.find(first); it != root.end() && it->second.is_table()) {
                current = &it->second.as_table();
            } else {
                root[first] = toml::table{};
                current = &root[first].as_table();
            }
            // Walk/create nested tables for remaining parts
            while (true) {
                auto next_dot = rest.find('.');
                if (next_dot == std::string::npos) {
                    (*current)[rest] = config_to_toml(val);
                    break;
                }
                std::string part = rest.substr(0, next_dot);
                rest = rest.substr(next_dot + 1);
                if (auto it = current->find(part); it != current->end() && it->second.is_table()) {
                    current = &it->second.as_table();
                } else {
                    (*current)[part] = toml::table{};
                    current = &(*current)[part].as_table();
                }
            }
        }
    }
    return root;
}

}  // anonymous namespace

TomlConfigSource::TomlConfigSource(std::string_view file_path)
    : file_path_(file_path) {}

bool TomlConfigSource::load(std::map<std::string, ConfigValue>& kv) {
    try {
        auto data = toml::parse(file_path_);
        if (!data.is_table()) return false;
        walk_table(data.as_table(), "", kv);
        return true;
    } catch (const toml::syntax_error&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

bool TomlConfigSource::save(const std::map<std::string, ConfigValue>& kv) {
    try {
        auto table = build_nested_table(kv);
        toml::value root(table);
        std::ofstream ofs(file_path_);
        if (!ofs.is_open()) return false;
        ofs << root << std::endl;
        return ofs.good();
    } catch (const std::exception&) {
        return false;
    }
}

}  // namespace quant::infra
