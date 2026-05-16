// console_sink.cc — Colored console output
#include "cpp/quant/infra/logging/console_sink.h"

#include <cstdio>
#include <ctime>
#include <string>

#include "cpp/quant/infra/logging/logger.h"

namespace quant::infra {

// ANSI color codes
namespace color {
constexpr const char* kReset   = "\033[0m";
constexpr const char* kGray    = "\033[90m";
constexpr const char* kGreen   = "\033[32m";
constexpr const char* kYellow  = "\033[33m";
constexpr const char* kRed     = "\033[31m";
constexpr const char* kBoldRed = "\033[1;31m";

constexpr const char* level_color(LogLevel l) {
    switch (l) {
        case LogLevel::kTrace: return kGray;
        case LogLevel::kDebug: return kGray;
        case LogLevel::kInfo:  return kGreen;
        case LogLevel::kWarn:  return kYellow;
        case LogLevel::kError: return kRed;
        case LogLevel::kFatal: return kBoldRed;
    }
    return kReset;
}
}  // namespace color

class ConsoleSink::Impl {
public:
    void write(const LogRecord& record) {
        auto lvl = static_cast<LogLevel>(record.level);
        std::fprintf(stderr, "%s[%s]%s %s%s\n",
                     color::level_color(lvl),
                     std::string(level_name(lvl)).c_str(),
                     color::kReset,
                     record.message.c_str(),
                     color::kReset);
    }

    void flush() {
        std::fflush(stderr);
    }
};

ConsoleSink::ConsoleSink() : impl_(std::make_unique<Impl>()) {}
ConsoleSink::~ConsoleSink() = default;

void ConsoleSink::write(const LogRecord& record) { impl_->write(record); }
void ConsoleSink::flush() { impl_->flush(); }

}  // namespace quant::infra
