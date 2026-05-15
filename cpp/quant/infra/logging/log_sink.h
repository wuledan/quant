// log_sink.h — Abstract log output sink
#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace quant::infra {

struct LogRecord {
    int64_t timestamp_us;  // Unix timestamp in microseconds
    uint8_t level;          // 0=TRACE … 5=FATAL
    std::string file;       // source file name
    int line;               // source line number
    std::string function;   // function name
    std::string message;    // formatted message
    std::string thread_id;  // thread identifier
    // Structured fields stored as JSON key-value pairs
    std::string extra_json; // pre-encoded JSON fields
};

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() = 0;
};

}  // namespace quant::infra
