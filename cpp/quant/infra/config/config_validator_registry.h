// config_validator_registry.h — Registry for named config validators
#pragma once

#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "cpp/quant/infra/config/config_source.h"
#include "cpp/quant/infra/config/config_validator.h"

namespace quant::infra {

// ── Validation result ──
struct ValidationResult {
    bool                          ok = true;
    std::vector<std::string>      errors;
    std::vector<std::string>      warnings;
};

// ── Type-erased validator wrapper ──
class IConfigValidatorEntry {
public:
    virtual ~IConfigValidatorEntry() = default;
    virtual ValidationResult validate(const ConfigValue& value) const = 0;
    virtual std::optional<ConfigValue> default_value() const = 0;
    virtual std::string_view key() const noexcept = 0;
};

template<typename T>
class ConfigValidatorEntry : public IConfigValidatorEntry {
public:
    ConfigValidatorEntry(std::string key, ConfigValidator<T> validator)
        : key_(std::move(key)), validator_(std::move(validator)) {}

    ValidationResult validate(const ConfigValue& value) const override {
        ValidationResult result;
        result.ok = true;

        if (!std::holds_alternative<T>(value)) {
            result.ok = false;
            result.errors.push_back(
                "Type mismatch for key '" + key_ + "'");
            return result;
        }

        const T& val = std::get<T>(value);
        if (!validator_.validate(val)) {
            result.ok = false;
            result.errors.push_back(
                "Value out of range for key '" + key_ + "'");
        }
        return result;
    }

    std::optional<ConfigValue> default_value() const override {
        auto dv = validator_.default_value();
        if (dv) return ConfigValue(*dv);
        return std::nullopt;
    }

    std::string_view key() const noexcept override { return key_; }

private:
    std::string          key_;
    ConfigValidator<T>   validator_;
};

// ── Config validator registry ──
class ConfigValidatorRegistry {
public:
    ConfigValidatorRegistry() = default;

    // ── Register a validator for a key ──
    template<typename T>
    void register_validator(const std::string& key, ConfigValidator<T> validator) {
        entries_[key] = std::make_unique<ConfigValidatorEntry<T>>(key, std::move(validator));
    }

    // ── Validate a single key-value pair ──
    ValidationResult validate(const std::string& key, const ConfigValue& value) const {
        auto it = entries_.find(key);
        if (it == entries_.end()) {
            ValidationResult result;
            result.ok = true;
            result.warnings.push_back("No validator registered for key '" + key + "'");
            return result;
        }
        return it->second->validate(value);
    }

    // ── Validate all entries in a config map ──
    ValidationResult validate_all(const std::map<std::string, ConfigValue>& kv) const {
        ValidationResult result;
        result.ok = true;

        for (const auto& [key, value] : kv) {
            auto r = validate(key, value);
            if (!r.ok) result.ok = false;
            for (auto& e : r.errors) result.errors.push_back(std::move(e));
            for (auto& w : r.warnings) result.warnings.push_back(std::move(w));
        }
        return result;
    }

    // ── Apply defaults: fill in default values for missing keys ──
    void apply_defaults(std::map<std::string, ConfigValue>& kv) const {
        for (const auto& [key, entry] : entries_) {
            if (kv.find(key) == kv.end()) {
                auto dv = entry->default_value();
                if (dv) {
                    kv[key] = *dv;
                }
            }
        }
    }

    // ── Check if a key has a registered validator ──
    bool has_validator(const std::string& key) const {
        return entries_.find(key) != entries_.end();
    }

    // ── Remove a validator ──
    void unregister(const std::string& key) {
        entries_.erase(key);
    }

private:
    std::map<std::string, std::unique_ptr<IConfigValidatorEntry>> entries_;
};

}  // namespace quant::infra