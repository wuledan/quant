// column_block.cc — ColumnBlock compression codec implementations
#include "cpp/quant/storage/column_block.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>

namespace quant::storage {

ColumnBlock::ColumnBlock(DataField field, Codec codec,
                          size_t row_count, std::vector<uint8_t> data,
                          int64_t min_ts, int64_t max_ts)
    : field_(field), codec_(codec), row_count_(row_count)
    , min_ts_(min_ts), max_ts_(max_ts), data_(std::move(data)) {}

// ========================================================================
// Delta-of-Delta encoding (for int64 timestamps)
// ========================================================================

std::vector<uint8_t> ColumnBlock::delta_encode(std::span<const int64_t> src) {
    if (src.empty()) return {};

    std::vector<uint8_t> out;
    out.reserve(src.size() * 8);

    int64_t first = src[0];
    out.resize(8);
    std::memcpy(out.data(), &first, 8);

    int64_t prev_delta = 0;
    int64_t prev = first;

    for (size_t i = 1; i < src.size(); ++i) {
        int64_t delta = src[i] - prev;
        int64_t delta_of_delta = delta - prev_delta;

        uint8_t buf[10];
        uint64_t val = (static_cast<uint64_t>(delta_of_delta << 1)) ^
                       static_cast<uint64_t>(delta_of_delta >> 63);

        size_t n = 0;
        do {
            buf[n] = val & 0x7F;
            val >>= 7;
            if (val) buf[n] |= 0x80;
            n++;
        } while (val && n < 10);

        out.insert(out.end(), buf, buf + n);
        prev_delta = delta;
        prev = src[i];
    }

    return out;
}

std::vector<int64_t> ColumnBlock::delta_decode(std::span<const uint8_t> src, size_t count) {
    if (count == 0 || src.size() < 8) return {};

    std::vector<int64_t> out;
    out.reserve(count);

    int64_t first;
    std::memcpy(&first, src.data(), 8);
    out.push_back(first);

    int64_t prev_delta = 0;
    int64_t prev = first;
    size_t pos = 8;

    while (out.size() < count && pos < src.size()) {
        uint64_t val = 0;
        int shift = 0;
        uint8_t byte;
        do {
            if (pos >= src.size()) break;
            byte = src[pos++];
            val |= static_cast<uint64_t>(byte & 0x7F) << shift;
            shift += 7;
        } while (byte & 0x80);

        int64_t delta_of_delta = static_cast<int64_t>(val >> 1) ^
                                 -static_cast<int64_t>(val & 1);
        int64_t delta = prev_delta + delta_of_delta;
        int64_t value = prev + delta;

        out.push_back(value);
        prev_delta = delta;
        prev = value;
    }

    return out;
}

std::vector<uint8_t> ColumnBlock::delta_encode_i32(std::span<const int32_t> src) {
    if (src.empty()) return {};

    std::vector<uint8_t> out;
    out.reserve(src.size() * 4);

    int32_t first = src[0];
    out.resize(4);
    std::memcpy(out.data(), &first, 4);

    int32_t prev_delta = 0;
    int32_t prev = first;

    for (size_t i = 1; i < src.size(); ++i) {
        int32_t delta = src[i] - prev;
        int32_t delta_of_delta = delta - prev_delta;

        // Zigzag encode: maps signed to unsigned for varint
        uint32_t val = (static_cast<uint32_t>(delta_of_delta << 1)) ^
                       static_cast<uint32_t>(delta_of_delta >> 31);
        uint8_t buf[5];
        size_t n = 0;
        do {
            buf[n] = val & 0x7F;
            val >>= 7;
            if (val) buf[n] |= 0x80;
            n++;
        } while (val && n < 5);

        out.insert(out.end(), buf, buf + n);
        prev_delta = delta;
        prev = src[i];
    }
    return out;
}

std::vector<int32_t> ColumnBlock::delta_decode_i32(std::span<const uint8_t> src, size_t count) {
    if (count == 0 || src.size() < 4) return {};

    std::vector<int32_t> out;
    out.reserve(count);

    int32_t first;
    std::memcpy(&first, src.data(), 4);
    out.push_back(first);

    int32_t prev_delta = 0;
    int32_t prev = first;
    size_t pos = 4;

    while (out.size() < count && pos < src.size()) {
        uint32_t val = 0;
        int shift = 0;
        uint8_t byte;
        do {
            if (pos >= src.size()) break;
            byte = src[pos++];
            val |= static_cast<uint32_t>(byte & 0x7F) << shift;
            shift += 7;
        } while (byte & 0x80);

        int32_t delta_of_delta = static_cast<int32_t>(val >> 1) ^
                                 -static_cast<int32_t>(val & 1);
        int32_t delta = prev_delta + delta_of_delta;
        int32_t value = prev + delta;

        out.push_back(value);
        prev_delta = delta;
        prev = value;
    }
    return out;
}

// ========================================================================
// Gorilla XOR encoding (for double values)
// ========================================================================

// Format: first value stored as raw 8 bytes, then for each subsequent value:
//   0x00        → XOR with previous is 0 (value unchanged)
//   0x01 L [V]  → XOR with previous is non-zero
//       L       = leading zero count (1 byte)
//       V       = (xor_result >> trailing_zeros) encoded as varint
//
// At decode time: trailing = clz(shifted_val) - L, then full_xor = shifted_val << trailing

std::vector<uint8_t> ColumnBlock::gorilla_encode(std::span<const double> src) {
    if (src.empty()) return {};

    std::vector<uint8_t> out;
    out.reserve(src.size() * 4);

    uint64_t prev_bits;
    std::memcpy(&prev_bits, &src[0], 8);
    out.resize(8);
    std::memcpy(out.data(), &prev_bits, 8);

    auto write_varint = [&](uint64_t val) {
        uint8_t buf[10];
        size_t n = 0;
        do {
            buf[n] = val & 0x7F;
            val >>= 7;
            if (val) buf[n] |= 0x80;
            n++;
        } while (val && n < 10);
        out.insert(out.end(), buf, buf + n);
    };

    for (size_t i = 1; i < src.size(); ++i) {
        uint64_t cur_bits;
        std::memcpy(&cur_bits, &src[i], 8);
        uint64_t xor_result = prev_bits ^ cur_bits;

        if (xor_result == 0) {
            out.push_back(0x00);
        } else {
            out.push_back(0x01);
            int leading = __builtin_clzll(xor_result);
            int trailing = __builtin_ctzll(xor_result);
            out.push_back(static_cast<uint8_t>(leading));
            write_varint(xor_result >> trailing);
            prev_bits = cur_bits;
        }
    }

    return out;
}

std::vector<double> ColumnBlock::gorilla_decode(std::span<const uint8_t> src, size_t count) {
    if (count == 0 || src.size() < 8) return {};

    std::vector<double> out;
    out.reserve(count);

    uint64_t prev_bits;
    std::memcpy(&prev_bits, src.data(), 8);
    {
        double val;
        std::memcpy(&val, &prev_bits, 8);
        out.push_back(val);
    }

    auto read_varint = [&](size_t& pos) -> uint64_t {
        uint64_t val = 0;
        int shift = 0;
        while (pos < src.size()) {
            uint8_t byte = src[pos++];
            val |= static_cast<uint64_t>(byte & 0x7F) << shift;
            if (!(byte & 0x80)) break;
            shift += 7;
        }
        return val;
    };

    size_t pos = 8;
    while (out.size() < count && pos < src.size()) {
        uint8_t control = src[pos++];
        if (control == 0x00) {
            out.push_back(*reinterpret_cast<const double*>(&prev_bits));
        } else if (control == 0x01 && pos < src.size()) {
            int leading = static_cast<int>(src[pos++]);
            uint64_t shifted_val = read_varint(pos);
            // shifted_val = xor_result >> trailing (no trailing zeros in shifted_val)
            // meaningful_bits = 64 - clz(shifted_val)
            // trailing = clz(shifted_val) - leading
            int sh_clz = __builtin_clzll(shifted_val);
            int trailing = sh_clz - leading;
            uint64_t full_xor = shifted_val << trailing;
            prev_bits ^= full_xor;
            out.push_back(*reinterpret_cast<const double*>(&prev_bits));
        } else {
            break;
        }
    }

    return out;
}

// ========================================================================
// Public compress/decompress API
// ========================================================================

ColumnBlock ColumnBlock::compress(DataField field,
                                   std::span<const int64_t> src,
                                   Codec codec,
                                   int64_t min_ts, int64_t max_ts) {
    std::vector<uint8_t> data;
    if (codec == Codec::kDelta) {
        data = delta_encode(src);
    } else {
        data.resize(src.size() * sizeof(int64_t));
        std::memcpy(data.data(), src.data(), data.size());
    }
    return ColumnBlock(field, codec, src.size(), std::move(data), min_ts, max_ts);
}

ColumnBlock ColumnBlock::compress(DataField field,
                                   std::span<const double> src,
                                   Codec codec,
                                   int64_t min_ts, int64_t max_ts) {
    std::vector<uint8_t> data;
    if (codec == Codec::kGorilla) {
        data = gorilla_encode(src);
    } else {
        data.resize(src.size() * sizeof(double));
        std::memcpy(data.data(), src.data(), data.size());
    }
    return ColumnBlock(field, codec, src.size(), std::move(data), min_ts, max_ts);
}

ColumnBlock ColumnBlock::compress(DataField field,
                                   std::span<const int32_t> src,
                                   Codec codec,
                                   int64_t min_ts, int64_t max_ts) {
    std::vector<uint8_t> data;
    if (codec == Codec::kDelta) {
        data = delta_encode_i32(src);
    } else {
        data.resize(src.size() * sizeof(int32_t));
        std::memcpy(data.data(), src.data(), data.size());
    }
    return ColumnBlock(field, codec, src.size(), std::move(data), min_ts, max_ts);
}

size_t ColumnBlock::decompress(std::span<int64_t> dst) const {
    size_t n = std::min(dst.size(), row_count_);
    if (codec_ == Codec::kDelta) {
        auto decoded = delta_decode(data_, n);
        n = std::min(decoded.size(), dst.size());
        std::memcpy(dst.data(), decoded.data(), n * sizeof(int64_t));
    } else {
        n = std::min(data_.size() / sizeof(int64_t), dst.size());
        std::memcpy(dst.data(), data_.data(), n * sizeof(int64_t));
    }
    return n;
}

size_t ColumnBlock::decompress(std::span<double> dst) const {
    size_t n = std::min(dst.size(), row_count_);
    if (codec_ == Codec::kGorilla) {
        auto decoded = gorilla_decode(data_, n);
        n = std::min(decoded.size(), dst.size());
        std::memcpy(dst.data(), decoded.data(), n * sizeof(double));
    } else {
        n = std::min(data_.size() / sizeof(double), dst.size());
        std::memcpy(dst.data(), data_.data(), n * sizeof(double));
    }
    return n;
}

size_t ColumnBlock::decompress(std::span<int32_t> dst) const {
    size_t n = std::min(dst.size(), row_count_);
    if (codec_ == Codec::kDelta) {
        auto decoded = delta_decode_i32(data_, n);
        n = std::min(decoded.size(), dst.size());
        std::memcpy(dst.data(), decoded.data(), n * sizeof(int32_t));
    } else {
        n = std::min(data_.size() / sizeof(int32_t), dst.size());
        std::memcpy(dst.data(), data_.data(), n * sizeof(int32_t));
    }
    return n;
}

}  // namespace quant::storage