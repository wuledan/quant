// json_file_sink.cc — JSON file sink implementation
#include "cpp/quant/infra/logging/json_file_sink.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <cstring>

#include "cpp/quant/infra/logging/logger.h"

namespace quant::infra {

class JsonFileSink::Impl {
public:
    Impl(std::string_view file_path, size_t rotate_size_mb)
        : base_path_(file_path)
        , max_size_(rotate_size_mb * 1024 * 1024)
    {
        open_file();
    }

    ~Impl() {
        if (file_) {
            std::fclose(file_);
        }
    }

    void write(const std::string& json_line) {
        rotate_if_needed();
        if (file_) {
            std::fputs(json_line.c_str(), file_);
            std::fputc('\n', file_);
            current_size_ += json_line.size() + 1;
        }
    }

    void flush() {
        if (file_) {
            std::fflush(file_);
        }
    }

private:
    void open_file() {
        file_ = std::fopen(base_path_.c_str(), "a");
        if (file_) {
            current_size_ = static_cast<size_t>(std::ftell(file_));
        }
    }

    void rotate_if_needed() {
        if (current_size_ >= max_size_) {
            if (file_) {
                std::fclose(file_);
                file_ = nullptr;
            }
            // Rename current log to .1, .2, etc.
            std::filesystem::path base(base_path_);
            auto stem = base.stem();
            auto ext = base.extension();
            // Remove oldest
            std::filesystem::path oldest = base_path_ + ".3";
            std::filesystem::remove(oldest);
            // Shift .2 -> .3, .1 -> .2
            for (int i = 2; i >= 1; --i) {
                std::filesystem::path from = base_path_ + "." + std::to_string(i);
                std::filesystem::path to = base_path_ + "." + std::to_string(i + 1);
                if (std::filesystem::exists(from)) {
                    std::filesystem::rename(from, to);
                }
            }
            // Rename current file to .1
            std::filesystem::rename(base_path_, base_path_ + ".1");
            open_file();
        }
    }

    std::string base_path_;
    size_t max_size_;
    std::FILE* file_{nullptr};
    size_t current_size_{0};
};

JsonFileSink::JsonFileSink(std::string_view file_path, size_t rotate_size_mb)
    : impl_(std::make_unique<Impl>(file_path, rotate_size_mb)) {}

JsonFileSink::~JsonFileSink() = default;

std::string JsonFileSink::format_json(const LogRecord& record) const {
    // Format as: {"ts":...,"level":"INFO","msg":"...",...}
    auto escape = [](std::string_view s) -> std::string {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x",
                                      static_cast<unsigned char>(c));
                        out += buf;
                    } else {
                        out += c;
                    }
                    break;
            }
        }
        return out;
    };

    std::string json = "{";
    json += "\"ts\":" + std::to_string(record.timestamp_us) + ",";
    json += "\"level\":\"" + std::string(level_name(static_cast<LogLevel>(record.level))) + "\",";
    json += "\"msg\":\"" + escape(record.message) + "\",";
    json += "\"file\":\"" + escape(record.file) + "\",";
    json += "\"line\":" + std::to_string(record.line) + ",";
    json += "\"func\":\"" + escape(record.function) + "\",";
    json += "\"thread\":\"" + escape(record.thread_id) + "\"";
    if (!record.extra_json.empty()) {
        json += "," + record.extra_json;
    }
    json += "}";
    return json;
}

void JsonFileSink::write(const LogRecord& record) {
    std::string json = format_json(record);
    impl_->write(json);
}

void JsonFileSink::flush() {
    impl_->flush();
}

}  // namespace quant::infra
