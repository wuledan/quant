// json_file_sink.h — JSON formatted log output to file
#pragma once

#include <cstdio>
#include <memory>
#include <string>

#include "cpp/quant/infra/logging/log_sink.h"

namespace quant::infra {

class JsonFileSink : public LogSink {
public:
    explicit JsonFileSink(std::string_view file_path,
                          size_t rotate_size_mb = 100);
    ~JsonFileSink() override;

    void write(const LogRecord& record) override;
    void flush() override;

private:
    void rotate_if_needed();
    std::string format_json(const LogRecord& record) const;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace quant::infra
