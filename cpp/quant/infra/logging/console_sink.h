// console_sink.h — Colored console log output
#pragma once

#include <memory>

#include "cpp/quant/infra/logging/log_sink.h"

namespace quant::infra {

class ConsoleSink : public LogSink {
public:
    ConsoleSink();
    ~ConsoleSink() override;

    void write(const LogRecord& record) override;
    void flush() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace quant::infra
