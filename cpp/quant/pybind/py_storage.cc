// py_storage.cc — Python bindings for storage engine
#include "cpp/quant/pybind/py_storage.h"

#include <pybind11/numpy.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <vector>

#include "cpp/quant/storage/column_block.h"
#include "cpp/quant/storage/storage_engine.h"
#include "cpp/quant/storage/time_series_store.h"

namespace py = pybind11;
using namespace quant::storage;

namespace quant::pybind {

// ── Helper: convert ColumnBlock vector to Python-friendly list ──
static py::dict block_to_dict(const ColumnBlock& block) {
    py::dict d;
    d["field"] = static_cast<int>(block.field());
    d["codec"] = static_cast<int>(block.codec());
    d["row_count"] = block.row_count();
    d["compressed_size"] = block.compressed_size();
    d["min_timestamp"] = block.min_timestamp();
    d["max_timestamp"] = block.max_timestamp();
    return d;
}

void bind_storage(py::module_& m) {
    // ── DataField enum ──
    py::enum_<DataField>(m, "DataField")
        .value("TIMESTAMP", DataField::kTimestamp)
        .value("OPEN", DataField::kOpen)
        .value("HIGH", DataField::kHigh)
        .value("LOW", DataField::kLow)
        .value("CLOSE", DataField::kClose)
        .value("VOLUME", DataField::kVolume)
        .value("AMOUNT", DataField::kAmount)
        .value("VWAP", DataField::kVwap)
        .value("BID_PRICE1", DataField::kBidPrice1)
        .value("ASK_PRICE1", DataField::kAskPrice1)
        .value("BID_VOL1", DataField::kBidVol1)
        .value("ASK_VOL1", DataField::kAskVol1)
        .export_values();

    // ── StoreStatus enum ──
    py::enum_<StoreStatus>(m, "StoreStatus")
        .value("OK", StoreStatus::kOk)
        .value("INVALID_ARGUMENT", StoreStatus::kInvalidArgument)
        .value("STORAGE_FULL", StoreStatus::kStorageFull)
        .value("IO_ERROR", StoreStatus::kIoError)
        .export_values();

    // ── DataSource enum ──
    py::enum_<DataSource>(m, "DataSource")
        .value("REALTIME_INGEST", DataSource::kRealtimeIngest)
        .value("REMOTE_LOAD", DataSource::kRemoteLoad)
        .export_values();

    // ── ColumnBlock::Codec enum (must be before ColumnBlock for default args) ──
    py::enum_<ColumnBlock::Codec>(m, "Codec")
        .value("NONE", ColumnBlock::Codec::kNone)
        .value("DELTA", ColumnBlock::Codec::kDelta)
        .value("GORILLA", ColumnBlock::Codec::kGorilla)
        .export_values();

    // ── ColumnBlock ──
    py::class_<ColumnBlock>(m, "ColumnBlock")
        .def(py::init<>())
        .def(py::init<DataField, ColumnBlock::Codec, size_t,
                      std::vector<uint8_t>, int64_t, int64_t>(),
             py::arg("field"), py::arg("codec"), py::arg("row_count"),
             py::arg("data"), py::arg("min_ts") = 0, py::arg("max_ts") = 0)
        .def_property_readonly("field", &ColumnBlock::field)
        .def_property_readonly("codec", &ColumnBlock::codec)
        .def_property_readonly("row_count", &ColumnBlock::row_count)
        .def_property_readonly("compressed_size", &ColumnBlock::compressed_size)
        .def_property_readonly("min_timestamp", &ColumnBlock::min_timestamp)
        .def_property_readonly("max_timestamp", &ColumnBlock::max_timestamp)
        .def("to_dict", &block_to_dict)
        .def_static(
            "compress_double",
            [](DataField field, py::array_t<double> arr,
               ColumnBlock::Codec codec, int64_t min_ts, int64_t max_ts) {
                auto buf = arr.request();
                auto* ptr = static_cast<const double*>(buf.ptr);
                return ColumnBlock::compress(
                    field,
                    std::span<const double>(ptr, buf.size),
                    codec, min_ts, max_ts);
            },
            py::arg("field"), py::arg("data"),
            py::arg("codec") = ColumnBlock::Codec::kGorilla,
            py::arg("min_ts") = 0, py::arg("max_ts") = 0)
        .def_static(
            "compress_int64",
            [](DataField field, py::array_t<int64_t> arr,
               ColumnBlock::Codec codec, int64_t min_ts, int64_t max_ts) {
                auto buf = arr.request();
                auto* ptr = static_cast<const int64_t*>(buf.ptr);
                return ColumnBlock::compress(
                    field,
                    std::span<const int64_t>(ptr, buf.size),
                    codec, min_ts, max_ts);
            },
            py::arg("field"), py::arg("data"),
            py::arg("codec") = ColumnBlock::Codec::kDelta,
            py::arg("min_ts") = 0, py::arg("max_ts") = 0)
        .def(
            "decompress_double",
            [](const ColumnBlock& block) {
                std::vector<double> dst(block.row_count());
                auto n = block.decompress(std::span<double>(dst));
                dst.resize(n);
                return py::array_t<double>(dst.size(), dst.data());
            })
        .def(
            "decompress_int64",
            [](const ColumnBlock& block) {
                std::vector<int64_t> dst(block.row_count());
                auto n = block.decompress(std::span<int64_t>(dst));
                dst.resize(n);
                return py::array_t<int64_t>(dst.size(), dst.data());
            });

    // ── TimeRange ──
    py::class_<TimeRange>(m, "TimeRange")
        .def(py::init<>())
        .def(py::init<int64_t, int64_t>(),
             py::arg("begin_ts"), py::arg("end_ts"))
        .def_readwrite("begin_ts", &TimeRange::begin_ts)
        .def_readwrite("end_ts", &TimeRange::end_ts);

    // ── KlineRow (must be before StorageEngine) ──
    py::class_<quant::event::KlineRow>(m, "KlineRow")
        .def(py::init<>())
        .def_readwrite("timestamp", &quant::event::KlineRow::timestamp)
        .def_readwrite("open_price", &quant::event::KlineRow::open_price)
        .def_readwrite("high_price", &quant::event::KlineRow::high_price)
        .def_readwrite("low_price", &quant::event::KlineRow::low_price)
        .def_readwrite("close_price", &quant::event::KlineRow::close_price)
        .def_readwrite("volume", &quant::event::KlineRow::volume)
        .def_readwrite("amount", &quant::event::KlineRow::amount)
        .def_readwrite("vwap", &quant::event::KlineRow::vwap)
        .def("__repr__", [](const quant::event::KlineRow& r) {
            return "<KlineRow ts=" + std::to_string(r.timestamp)
                   + " O=" + std::to_string(r.open_price)
                   + " H=" + std::to_string(r.high_price)
                   + " L=" + std::to_string(r.low_price)
                   + " C=" + std::to_string(r.close_price)
                   + " V=" + std::to_string(r.volume) + ">";
        });

    // ── DataType enum (must be before StorageEngine) ──
    py::enum_<quant::event::DataType>(m, "DataType")
        .value("KLINE_1MIN", quant::event::DataType::kKline1Min)
        .value("KLINE_5MIN", quant::event::DataType::kKline5Min)
        .value("KLINE_15MIN", quant::event::DataType::kKline15Min)
        .value("KLINE_30MIN", quant::event::DataType::kKline30Min)
        .value("KLINE_60MIN", quant::event::DataType::kKline60Min)
        .value("KLINE_DAY", quant::event::DataType::kKlineDay)
        .value("TICK", quant::event::DataType::kTick)
        .export_values();

    // ── StorageEngine::Options (must be before StorageEngine) ──
    py::class_<StorageEngine::Options>(m, "StorageEngineOptions")
        .def(py::init<>())
        .def(py::init<size_t, std::string>(),
             py::arg("cache_budget_mb"), py::arg("data_dir"))
        .def_readwrite("cache_budget_mb", &StorageEngine::Options::cache_budget_mb)
        .def_readwrite("data_dir", &StorageEngine::Options::data_dir);

    // ── TimeSeriesStore ──
    py::class_<TimeSeriesStore>(m, "TimeSeriesStore")
        .def(py::init([](size_t cache_budget_mb, const std::string& data_dir) {
                 return new TimeSeriesStore(cache_budget_mb, std::filesystem::path(data_dir));
             }),
             py::arg("cache_budget_mb"), py::arg("data_dir"))
        .def("put", &TimeSeriesStore::put,
             py::arg("symbol"), py::arg("data_type"), py::arg("block"),
             py::arg("source") = DataSource::kRealtimeIngest)
        .def("query", &TimeSeriesStore::query,
             py::arg("symbol"), py::arg("data_type"),
             py::arg("field"), py::arg("range"))
        .def("query_all", &TimeSeriesStore::query_all,
             py::arg("symbol"), py::arg("data_type"), py::arg("range"))
        .def("latest", &TimeSeriesStore::latest,
             py::arg("symbol"), py::arg("data_type"), py::arg("field"))
        .def("flush", &TimeSeriesStore::flush)
        .def("close", &TimeSeriesStore::close);

    // ── KlineQueryResult (must be before StorageEngine) ──
    py::class_<StorageEngine::KlineQueryResult>(m, "KlineQueryResult")
        .def_readonly("timestamps", &StorageEngine::KlineQueryResult::timestamps)
        .def_readonly("values", &StorageEngine::KlineQueryResult::values)
        .def("row_count", [](const StorageEngine::KlineQueryResult& r) {
            return r.timestamps.size();
        });

    // ── StorageEngine ──
    py::class_<StorageEngine>(m, "StorageEngine")
        .def(py::init<StorageEngine::Options>(), py::arg("opts") = StorageEngine::Options())
        .def("store_kline", &StorageEngine::store_kline,
             py::arg("symbol"), py::arg("kline_type"), py::arg("row"))
        .def("store_kline_batch", &StorageEngine::store_kline_batch,
             py::arg("symbol"), py::arg("kline_type"), py::arg("rows"))
        .def("query_kline", &StorageEngine::query_kline,
             py::arg("symbol"), py::arg("kline_type"),
             py::arg("field"), py::arg("range"))
        .def("flush", &StorageEngine::flush)
        .def("close", &StorageEngine::close);
}

}  // namespace quant::pybind
