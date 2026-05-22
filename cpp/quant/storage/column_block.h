// column_block.h — Columnar data block with compression support
// Supports Delta-of-Delta, Gorilla, and uncompressed codecs
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace quant::storage {

// ── KlineFreq: K-line (candlestick) frequency ──
// Maps to distinct data_type values (aligned with event::DataType enum).
// Each frequency is stored in separate segment files and indexed independently.
enum class KlineFreq : uint8_t {
    kMin1  = 2,   // 1-minute bars   (event::DataType::kKlineMin1)
    kMin5  = 3,   // 5-minute bars   (event::DataType::kKlineMin5)
    kMin15 = 4,   // 15-minute bars  (event::DataType::kKlineMin15)
    kMin30 = 5,   // 30-minute bars  (event::DataType::kKlineMin30)
    kMin60 = 6,   // 60-minute bars  (event::DataType::kKlineMin60)
    kDay   = 7,   // daily bars      (event::DataType::kKlineDay)
};

// Convert KlineFreq to the data_type byte used in segment files / index
// Values are aligned with event::DataType enum
constexpr uint8_t kline_freq_to_data_type(KlineFreq f) noexcept {
    return static_cast<uint8_t>(f);
}

// Convert data_type byte back to KlineFreq; returns false if not a kline type
// Accepts data_type values 2..7 (kKlineMin1..kKlineDay)
inline bool data_type_to_kline_freq(uint8_t data_type, KlineFreq& out) noexcept {
    if (data_type >= 2 && data_type <= 7) {
        out = static_cast<KlineFreq>(data_type);
        return true;
    }
    return false;
}

// Human-readable label for KlineFreq
inline const char* kline_freq_label(KlineFreq f) noexcept {
    switch (f) {
        case KlineFreq::kMin1:  return "1min";
        case KlineFreq::kMin5:  return "5min";
        case KlineFreq::kMin15: return "15min";
        case KlineFreq::kMin30: return "30min";
        case KlineFreq::kMin60: return "60min";
        case KlineFreq::kDay:   return "day";
        default:                return "unknown";
    }
}

// Human-readable label for any data_type byte value
inline const char* data_type_label(uint8_t data_type) noexcept {
    KlineFreq freq;
    if (data_type == 1) return "tick";
    if (data_type_to_kline_freq(data_type, freq)) return kline_freq_label(freq);
    return "unknown";
}

enum class DataField : uint8_t {
    kTimestamp = 255,  // special field for timestamp column
    kOpen   = 0,
    kHigh   = 1,
    kLow    = 2,
    kClose  = 3,
    kVolume = 4,
    kAmount = 5,
    kVwap   = 6,
    // Tick fields
    kBidPrice1 = 10,
    kAskPrice1 = 11,
    kBidVol1   = 12,
    kAskVol1   = 13,
};

constexpr std::string_view data_field_name(DataField f) noexcept {
    switch (f) {
        case DataField::kTimestamp: return "timestamp";
        case DataField::kOpen:   return "open";
        case DataField::kHigh:   return "high";
        case DataField::kLow:    return "low";
        case DataField::kClose:  return "close";
        case DataField::kVolume: return "volume";
        case DataField::kAmount: return "amount";
        case DataField::kVwap:   return "vwap";
        case DataField::kBidPrice1: return "bid_price1";
        case DataField::kAskPrice1: return "ask_price1";
        case DataField::kBidVol1:   return "bid_vol1";
        case DataField::kAskVol1:   return "ask_vol1";
    }
    return "unknown";
}

// ── ColumnBlock: compressed columnar data block ──
class ColumnBlock {
public:
    enum class Codec : uint8_t {
        kNone    = 0,
        kDelta   = 1,   // Delta-of-Delta (timestamps, integers)
        kGorilla = 4,   // XOR-based float compression
    };

    static constexpr size_t kBlockSize = 8192;  // max rows per block
    static constexpr size_t kHeaderSize = 48;   // header bytes

    ColumnBlock() = default;

    // Construct from compressed data
    ColumnBlock(DataField field, Codec codec,
                size_t row_count, std::vector<uint8_t> data,
                int64_t min_ts = 0, int64_t max_ts = 0);

    DataField field() const noexcept { return field_; }
    Codec     codec() const noexcept { return codec_; }
    size_t    row_count() const noexcept { return row_count_; }
    size_t    compressed_size() const noexcept { return data_.size(); }
    int64_t   min_timestamp() const noexcept { return min_ts_; }
    int64_t   max_timestamp() const noexcept { return max_ts_; }

    // Decompress into destination buffer
    // Returns: number of elements decompressed
    size_t decompress(std::span<int64_t> dst) const;
    size_t decompress(std::span<double> dst) const;
    size_t decompress(std::span<int32_t> dst) const;

    // Compress from source data
    static ColumnBlock compress(DataField field,
                                 std::span<const int64_t> src,
                                 Codec codec = Codec::kDelta,
                                 int64_t min_ts = 0, int64_t max_ts = 0);
    static ColumnBlock compress(DataField field,
                                 std::span<const double> src,
                                 Codec codec = Codec::kGorilla,
                                 int64_t min_ts = 0, int64_t max_ts = 0);
    static ColumnBlock compress(DataField field,
                                 std::span<const int32_t> src,
                                 Codec codec = Codec::kDelta,
                                 int64_t min_ts = 0, int64_t max_ts = 0);

    // Access raw compressed data (for serialization)
    const std::vector<uint8_t>& data() const noexcept { return data_; }

private:
    // Delta-of-Delta encoding/decoding
    static std::vector<uint8_t> delta_encode(std::span<const int64_t> src);
    static std::vector<int64_t> delta_decode(std::span<const uint8_t> src, size_t count);
    static std::vector<uint8_t> delta_encode_i32(std::span<const int32_t> src);
    static std::vector<int32_t> delta_decode_i32(std::span<const uint8_t> src, size_t count);

    // Gorilla XOR encoding/decoding
    static std::vector<uint8_t> gorilla_encode(std::span<const double> src);
    static std::vector<double> gorilla_decode(std::span<const uint8_t> src, size_t count);

    DataField field_{DataField::kOpen};
    Codec     codec_{Codec::kNone};
    size_t    row_count_{0};
    int64_t   min_ts_{0};
    int64_t   max_ts_{0};
    std::vector<uint8_t> data_;
};

}  // namespace quant::storage
