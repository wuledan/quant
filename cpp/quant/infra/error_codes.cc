// error_codes.cc — Error code system implementation
#include "cpp/quant/infra/error_codes.h"

#include <cstdio>
#include <system_error>

namespace quant::infra {

// ── QuantErrorCategory message mapping ──
std::string QuantErrorCategory::message(int code) const {
    switch (static_cast<ErrorCode>(code)) {
        // Generic
        case ErrorCode::OK: return "OK";
        case ErrorCode::Unknown: return "Unknown error";
        case ErrorCode::InvalidArgument: return "Invalid argument";
        case ErrorCode::Timeout: return "Operation timed out";
        case ErrorCode::OutOfResource: return "Out of resources";
        case ErrorCode::NotInitialized: return "Not initialized";
        case ErrorCode::AlreadyInitialized: return "Already initialized";
        case ErrorCode::Cancelled: return "Operation cancelled";

        // Infrastructure
        case ErrorCode::ThreadPoolStopped: return "Thread pool is stopped";
        case ErrorCode::ThreadPoolQueueFull: return "Thread pool queue is full";
        case ErrorCode::MemoryPoolExhausted: return "Memory pool exhausted";
        case ErrorCode::ThreadPoolExhausted: return "Thread pool exhausted";

        // Data
        case ErrorCode::DataNotFound: return "Data not found";
        case ErrorCode::DataFormatError: return "Data format error";
        case ErrorCode::DataValidationError: return "Data validation error";
        case ErrorCode::DataSourceUnavailable: return "Data source unavailable";

        // Strategy
        case ErrorCode::StrategyNotFound: return "Strategy not found";
        case ErrorCode::StrategyParamError: return "Strategy parameter error";
        case ErrorCode::StrategyRuntimeError: return "Strategy runtime error";

        // Trading
        case ErrorCode::OrderRejected: return "Order rejected";
        case ErrorCode::OrderTimeout: return "Order timeout";
        case ErrorCode::BrokerConnectionLost: return "Broker connection lost";
        case ErrorCode::InsufficientFunds: return "Insufficient funds";

        // Risk
        case ErrorCode::RiskLimitExceeded: return "Risk limit exceeded";
        case ErrorCode::RiskRuleViolation: return "Risk rule violation";

        // Network
        case ErrorCode::ConnectionRefused: return "Connection refused";
        case ErrorCode::ConnectionTimeout: return "Connection timeout";
        case ErrorCode::WSHandshakeFailed: return "WebSocket handshake failed";

        // Scheduler
        case ErrorCode::TaskDependencyCycle: return "Task dependency cycle detected";
        case ErrorCode::TaskExecutionFailed: return "Task execution failed";
        case ErrorCode::TaskRetryExhausted: return "Task retry exhausted";

        default: return "Unknown error code";
    }
}

const QuantErrorCategory& quant_error_category() {
    static const QuantErrorCategory instance;
    return instance;
}

// ── ErrorNode implementation ──
std::string ErrorNode::to_string() const {
    std::string result;
    const ErrorNode* current = this;
    int depth = 0;

    while (current) {
        if (depth > 0) {
            result += "\n  Caused by: ";
        }

        char buf[32];
        auto code_val = static_cast<uint32_t>(current->code_);
        snprintf(buf, sizeof(buf), "[0x%08X]", code_val);

        result += buf;
        result += " ";
        result += quant_error_category().message(static_cast<int>(current->code_));
        result += ": ";
        result += current->message_;

        // Add source location
        result += "\n    at ";
        result += current->location_.file_name();
        result += ":";
        result += std::to_string(current->location_.line());

        current = current->cause_.get();
        ++depth;
    }

    return result;
}

// ── chain_error ──
std::unique_ptr<ErrorNode> chain_error(
    ErrorCode code,
    std::string message,
    std::unique_ptr<ErrorNode> cause,
    std::source_location loc) {
    return std::make_unique<ErrorNode>(code, std::move(message), loc, std::move(cause));
}

// ── QuantException ──
QuantException::QuantException(std::unique_ptr<ErrorNode> error)
    : error_(std::move(error)) {}

QuantException::QuantException(ErrorCode code, std::string_view message)
    : error_(std::make_unique<ErrorNode>(code, std::string(message))) {}

const char* QuantException::what() const noexcept {
    if (what_message_.empty()) {
        what_message_ = error_->to_string();
    }
    return what_message_.c_str();
}

}  // namespace quant::infra
