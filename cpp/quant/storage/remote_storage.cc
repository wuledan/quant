// remote_storage.cc — S3-compatible remote storage (MinIO) implementation
//
// Uses curl subprocess (popen) for S3 API calls. The --aws-sigv4 flag
// (curl >= 7.75.0) handles S3 v4 signing. Data is serialized in a simple
// columnar KLPQ format (K-line Parquet-like, Quick) — a header + raw
// KlineRow array. Produces .parquet extension for compatibility with
// downstream tools that expect Parquet-named objects.
#include "cpp/quant/storage/remote_storage.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace quant::storage {

// ── curl subprocess helpers ──

// Build curl auth args for S3 v4 signing
static std::string curl_auth_args(const RemoteStorage::Options& opts) {
    std::string a;
    a.reserve(256);
    a += "--aws-sigv4 \"aws:amz:us-east-1:s3\" ";
    a += "--user \"";
    a += opts.access_key;
    a += ":";
    a += opts.secret_key;
    a += "\" ";
    return a;
}

// Build full URL: endpoint/bucket/key
static std::string s3_url(const RemoteStorage::Options& opts,
                           const std::string& bucket,
                           const std::string& key) {
    std::string url = opts.endpoint;
    if (url.back() != '/') url += '/';
    url += bucket;
    url += '/';
    url += key;
    return url;
}

// ── RemoteStorage ──

RemoteStorage::RemoteStorage(Options opts)
    : opts_(std::move(opts)) {}

// ── KLPQ format ──

std::vector<uint8_t> RemoteStorage::kline_to_parquet(
    const std::vector<event::KlineRow>& rows) {
    if (rows.empty()) return {};

    size_t n = rows.size();
    size_t total_size = kKLPQHeaderSize + n * sizeof(event::KlineRow);
    std::vector<uint8_t> buf(total_size);

    KLPQHeader header;
    header.num_rows = static_cast<uint32_t>(n);
    header.row_stride = sizeof(event::KlineRow);
    std::memcpy(buf.data(), &header, sizeof(header));

    std::memcpy(buf.data() + kKLPQHeaderSize, rows.data(),
                n * sizeof(event::KlineRow));

    return buf;
}

std::vector<event::KlineRow> RemoteStorage::parquet_to_kline(
    const std::vector<uint8_t>& data) {
    if (data.size() < kKLPQHeaderSize) return {};

    KLPQHeader header;
    std::memcpy(&header, data.data(), sizeof(header));

    if (header.magic != kKLPQMagic || header.version != kKLPQVersion) {
        return {};
    }

    uint32_t n = header.num_rows;
    size_t expected = kKLPQHeaderSize + n * sizeof(event::KlineRow);
    if (data.size() < expected) return {};

    const auto* src = reinterpret_cast<const event::KlineRow*>(
        data.data() + kKLPQHeaderSize);

    std::vector<event::KlineRow> rows(n);
    std::memcpy(rows.data(), src, n * sizeof(event::KlineRow));
    return rows;
}

// ── Data type label ──

std::string RemoteStorage::data_type_label(uint8_t data_type) {
    switch (data_type) {
        case 0:  return "1min";
        case 1:  return "5min";
        case 2:  return "15min";
        case 3:  return "30min";
        case 4:  return "60min";
        case 5:  return "day";
        default: return "unknown";
    }
}

// ── Object key ──

std::string RemoteStorage::kline_object_key(const std::string& symbol,
                                             uint8_t data_type, int year) {
    std::string key;
    key.reserve(64);
    key += "kline/";
    key += data_type_label(data_type);
    key += '/';
    key += symbol;
    key += '/';
    key += std::to_string(year);
    key += ".parquet";
    return key;
}

// ── Year extraction ──

int RemoteStorage::year_from_timestamp(int64_t timestamp_us) {
    auto us = std::chrono::microseconds(timestamp_us);
    auto tp = std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(us));
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    struct tm tm_buf;
    gmtime_r(&tt, &tm_buf);
    return tm_buf.tm_year + 1900;
}

// ── High-level API ──

bool RemoteStorage::upload_kline(const std::string& symbol,
                                  uint8_t data_type, int year,
                                  const std::vector<event::KlineRow>& rows) {
    auto data = kline_to_parquet(rows);
    if (data.empty()) return false;
    auto key = kline_object_key(symbol, data_type, year);
    return put_object(opts_.bucket_kline, key, data);
}

std::vector<event::KlineRow> RemoteStorage::download_kline(
    const std::string& symbol, uint8_t data_type, int year) {
    auto key = kline_object_key(symbol, data_type, year);
    auto data = get_object(opts_.bucket_kline, key);
    if (data.empty()) return {};
    return parquet_to_kline(data);
}

bool RemoteStorage::exists(const std::string& symbol,
                            uint8_t data_type, int year) {
    auto key = kline_object_key(symbol, data_type, year);
    return head_object(opts_.bucket_kline, key);
}

// ── Low-level S3 API via curl subprocess ──

bool RemoteStorage::put_object(const std::string& bucket,
                                const std::string& key,
                                const std::vector<uint8_t>& data) {
    std::string cmd = "curl --silent --fail -X PUT --data-binary @- ";
    cmd += curl_auth_args(opts_);
    cmd += s3_url(opts_, bucket, key);

    FILE* pipe = popen(cmd.c_str(), "w");
    if (!pipe) return false;

    size_t written = fwrite(data.data(), 1, data.size(), pipe);
    int rc = pclose(pipe);

    return rc == 0 && written == data.size();
}

std::vector<uint8_t> RemoteStorage::get_object(const std::string& bucket,
                                                 const std::string& key) {
    std::string cmd = "curl --silent --fail ";
    cmd += curl_auth_args(opts_);
    cmd += s3_url(opts_, bucket, key);

    std::vector<uint8_t> result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return result;

    std::array<uint8_t, 65536> buf;
    size_t n;
    while ((n = fread(buf.data(), 1, buf.size(), pipe)) > 0) {
        result.insert(result.end(), buf.data(), buf.data() + n);
    }
    pclose(pipe);
    return result;
}

bool RemoteStorage::head_object(const std::string& bucket,
                                 const std::string& key) {
    std::string cmd = "curl --silent --fail --output /dev/null --head ";
    cmd += curl_auth_args(opts_);
    cmd += s3_url(opts_, bucket, key);

    int rc = system(cmd.c_str());
    return rc == 0;
}

}  // namespace quant::storage
