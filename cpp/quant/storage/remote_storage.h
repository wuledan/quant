// remote_storage.h — S3-compatible remote storage (MinIO) with Parquet-like format
#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <memory>

#include "cpp/quant/event/events/kline_event.h"

namespace quant::storage {

// ── Parquet-like format constants ──
constexpr uint32_t kKLPQMagic = 0x51504C4B;  // "KLPQ" (K-line Parquet-like Quick)
constexpr uint16_t kKLPQVersion = 1;
constexpr size_t   kKLPQHeaderSize = 64;

#pragma pack(push, 1)
struct KLPQHeader {
    uint32_t magic = kKLPQMagic;
    uint16_t version = kKLPQVersion;
    uint32_t num_rows;
    uint16_t row_stride;           // sizeof(KlineRow) == 48
    uint8_t  reserved[52];         // padding to 64 bytes
};
static_assert(sizeof(KLPQHeader) == kKLPQHeaderSize);
#pragma pack(pop)

// ── RemoteStorage ──
class RemoteStorage {
public:
    struct Options {
        std::string endpoint = "http://127.0.0.1:9000";
        std::string access_key = "minioadmin";
        std::string secret_key = "minioadmin";
        std::string bucket_kline = "quant-kline";
        bool use_ssl = false;
    };

    explicit RemoteStorage(Options opts);
    ~RemoteStorage() = default;

    RemoteStorage(const RemoteStorage&) = delete;
    RemoteStorage& operator=(const RemoteStorage&) = delete;

    // ── Parquet-like format conversion ──
    static std::vector<uint8_t> kline_to_parquet(const std::vector<event::KlineRow>& rows);
    static std::vector<event::KlineRow> parquet_to_kline(const std::vector<uint8_t>& data);

    // ── High-level Kline operations ──
    bool upload_kline(const std::string& symbol, uint8_t data_type,
                      int year, const std::vector<event::KlineRow>& rows);

    std::vector<event::KlineRow> download_kline(const std::string& symbol,
                                                 uint8_t data_type, int year);

    bool exists(const std::string& symbol, uint8_t data_type, int year);

    // ── Low-level S3 API ──
    bool put_object(const std::string& bucket, const std::string& key,
                    const std::vector<uint8_t>& data);
    std::vector<uint8_t> get_object(const std::string& bucket, const std::string& key);
    bool head_object(const std::string& bucket, const std::string& key);

    // ── Object key helpers ──
    static std::string kline_object_key(const std::string& symbol,
                                         uint8_t data_type, int year);
    static std::string data_type_label(uint8_t data_type);

    // ── Year extraction from microsecond timestamp ──
    static int year_from_timestamp(int64_t timestamp_us);

private:
    Options opts_;
};

}  // namespace quant::storage
