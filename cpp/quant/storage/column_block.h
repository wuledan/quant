// column_block.h — Columnar data block with compression support
// Supports Delta-of-Delta, Gorilla, and uncompressed codecs
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace quant::storage {

enum class DataField : uint8_t {
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
