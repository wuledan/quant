// error_codes.h — Unified error code system with error chain and Result<T>
#pragma once

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace quant::infra {

// ── Error code segments ──
// 0x00XX0000 — Infrastructure layer
// 0x01XX0000 — Data layer
// 0x02XX0000 — Strategy layer
// 0x03XX0000 — Trading layer
// 0x04XX0000 — Risk layer
// 0x05XX0000 — Network layer
// 0x06XX0000 — Scheduler layer

enum class ErrorCode : uint32_t {
    // ── Generic (0x0001xxxx) ──
    OK                      = 0,
    Unknown                 = 0x00010001,
    InvalidArgument         = 0x00010002,
    Timeout                 = 0x00010003,
    OutOfResource           = 0x00010004,
    NotInitialized          = 0x00010005,
    AlreadyInitialized      = 0x00010006,
    Cancelled               = 0x00010007,

    // ── Infrastructure (0x0002xxxx) ──
    ThreadPoolStopped       = 0x00020001,
    ThreadPoolQueueFull     = 0x00020002,
    MemoryPoolExhausted     = 0x00020003,
    ThreadPoolExhausted    = 0x00020004,

    // ── Data layer (0x0100xxxx) ──
    DataNotFound             = 0x01000001,
    DataFormatError          = 0x01000002,
    DataValidationError      = 0x01000003,
    DataSourceUnavailable    = 0x01000004,

    // ── Strategy layer (0x0200xxxx) ──
    StrategyNotFound         = 0x02000001,
    StrategyParamError       = 0x02000002,
    StrategyRuntimeError     = 0x02000003,

    // ── Trading layer (0x0300xxxx) ──
    OrderRejected            = 0x03000001,
    OrderTimeout             = 0x03000002,
    BrokerConnectionLost     = 0x03000003,
    InsufficientFunds        = 0x03000004,

    // ── Risk layer (0x0400xxxx) ──
    RiskLimitExceeded        = 0x04000001,
    RiskRuleViolation        = 0x04000002,

    // ── Network layer (0x0500xxxx) ──
    ConnectionRefused        = 0x05000001,
    ConnectionTimeout        = 0x05000002,
    WSHandshakeFailed        = 0x05000003,

    // ── Scheduler layer (0x0600xxxx) ──
    TaskDependencyCycle      = 0x06000001,
    TaskExecutionFailed      = 0x06000002,
    TaskRetryExhausted       = 0x06000003,
};

// ── Error category ──
class QuantErrorCategory : public std::error_category {
public:
    const char* name() const noexcept override { return "quant"; }
    std::string message(int code) const override;
};

const QuantErrorCategory& quant_error_category();

inline std::error_code make_error_code(ErrorCode e) {
    return {static_cast<int>(e), quant_error_category()};
}

// ── Forward declaration ──
class QuantException;

// ── Error chain node ──
class ErrorNode {
public:
    ErrorNode(ErrorCode code,
              std::string message,
              std::source_location loc = std::source_location::current(),
              std::unique_ptr<ErrorNode> cause = nullptr)
        : code_(code)
        , message_(std::move(message))
        , location_(loc)
        , cause_(std::move(cause)) {}

    ErrorCode code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }
    const std::source_location& location() const noexcept { return location_; }
    const ErrorNode* cause() const noexcept { return cause_.get(); }

    // ── Formatted output ──
    std::string to_string() const;

private:
    ErrorCode code_;
    std::string message_;
    std::source_location location_;
    std::unique_ptr<ErrorNode> cause_;
};

// ── Convenience factory ──
template<typename... Args>
std::unique_ptr<ErrorNode> make_error(ErrorCode code, Args&&... args) {
    return std::make_unique<ErrorNode>(code, std::forward<Args>(args)...);
}

std::unique_ptr<ErrorNode> chain_error(
    ErrorCode code,
    std::string message,
    std::unique_ptr<ErrorNode> cause,
    std::source_location loc = std::source_location::current());

// ── Exception bridge (for C++/Python boundary) ──
// Declared before Result<T> since it's used there
class QuantException : public std::exception {
public:
    explicit QuantException(std::unique_ptr<ErrorNode> error);
    QuantException(ErrorCode code, std::string_view message);

    const char* what() const noexcept override;
    const ErrorNode& error_node() const noexcept { return *error_; }

private:
    std::unique_ptr<ErrorNode> error_;
    mutable std::string what_message_;
};

// ── Result type ──
template<typename T>
class Result {
public:
    // Success construction
    Result(T value) : value_(std::move(value)), error_(nullptr) {}  // NOLINT
    // Failure construction
    Result(std::unique_ptr<ErrorNode> error) : error_(std::move(error)) {}  // NOLINT
    Result(ErrorCode code, std::string msg)
        : error_(std::make_unique<ErrorNode>(code, std::move(msg))) {}

    // ── Accessors ──
    bool ok() const noexcept { return error_ == nullptr; }
    explicit operator bool() const noexcept { return ok(); }

    const T& value() const& {
        if (!ok()) {
            throw QuantException(ErrorCode::Unknown, "Accessing value on error Result");
        }
        return value_.value();
    }

    T& value() & {
        if (!ok()) {
            throw QuantException(ErrorCode::Unknown, "Accessing value on error Result");
        }
        return value_.value();
    }

    T&& value() && {
        if (!ok()) {
            throw QuantException(ErrorCode::Unknown, "Accessing value on error Result");
        }
        return std::move(value_.value());
    }

    const ErrorNode* error() const noexcept { return error_.get(); }

    // ── Chain operations ──
    template<typename Func>
    auto map(Func&& f) -> Result<std::invoke_result_t<Func, T>> {
        using ReturnType = std::invoke_result_t<Func, T>;
        if (!ok()) {
            return Result<ReturnType>(ErrorCode::Unknown, error_ ? error_->message() : "unknown");
        }
        try {
            return Result<ReturnType>(std::forward<Func>(f)(value()));
        } catch (...) {
            return Result<ReturnType>(ErrorCode::Unknown, "Map operation failed");
        }
    }

    template<typename Func>
    auto and_then(Func&& f) -> std::invoke_result_t<Func, T> {
        using ReturnType = std::invoke_result_t<Func, T>;
        if (!ok()) {
            if (error_) {
                return ReturnType(std::make_unique<ErrorNode>(
                    error_->code(), error_->message()));
            }
            return ReturnType(ErrorCode::Unknown, "unknown error");
        }
        return std::forward<Func>(f)(*value_);
    }

private:
    std::optional<T> value_;
    std::unique_ptr<ErrorNode> error_;
};

// ── Result<void> specialization ──
template<>
class Result<void> {
public:
    Result() : error_(nullptr) {}  // NOLINT
    Result(std::unique_ptr<ErrorNode> error) : error_(std::move(error)) {}  // NOLINT
    Result(ErrorCode code, std::string msg)
        : error_(std::make_unique<ErrorNode>(code, std::move(msg))) {}

    bool ok() const noexcept { return error_ == nullptr; }
    explicit operator bool() const noexcept { return ok(); }
    const ErrorNode* error() const noexcept { return error_.get(); }

    void value() const {
        if (!ok()) {
            throw QuantException(ErrorCode::Unknown, "Accessing value on error Result<void>");
        }
    }

private:
    std::unique_ptr<ErrorNode> error_;
};

}  // namespace quant::infra

// ── Make ErrorCode usable with std::error_code ──
namespace std {
template<>
struct is_error_code_enum<quant::infra::ErrorCode> : true_type {};
}  // namespace std