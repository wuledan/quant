// py_ingest.cc — pybind11 bindings for DataIngestor
//
// Exposes DataIngestor to Python for use in the data pipeline.

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "cpp/quant/ingest/data_ingestor.h"

namespace py = pybind11;

namespace quant::pybind {

using namespace quant::ingest;

void bind_ingest(py::module_& m) {
    // KlineData
    py::class_<KlineData>(m, "KlineData")
        .def(py::init<>())
        .def_readonly("timestamp", &KlineData::timestamp)
        .def_readonly("open", &KlineData::open)
        .def_readonly("high", &KlineData::high)
        .def_readonly("low", &KlineData::low)
        .def_readonly("close", &KlineData::close)
        .def_readonly("volume", &KlineData::volume)
        .def_readonly("amount", &KlineData::amount)
        .def("__repr__", [](const KlineData& k) {
            return "<KlineData ts=" + std::to_string(k.timestamp) +
                   " close=" + std::to_string(k.close) + ">";
        });

    // DataSourceConfig
    py::class_<DataSourceConfig>(m, "DataSourceConfig")
        .def(py::init<>())
        .def(py::init<std::string, std::string, int, std::string,
                      std::vector<std::string>>(),
             py::arg("name"), py::arg("host"), py::arg("port"),
             py::arg("protocol") = "tcp",
             py::arg("symbols") = std::vector<std::string>{})
        .def_readwrite("name", &DataSourceConfig::name)
        .def_readwrite("host", &DataSourceConfig::host)
        .def_readwrite("port", &DataSourceConfig::port)
        .def_readwrite("protocol", &DataSourceConfig::protocol)
        .def_readwrite("symbols", &DataSourceConfig::symbols)
        .def_readwrite("reconnect_delay_ms", &DataSourceConfig::reconnect_delay_ms)
        .def_readwrite("max_reconnect_attempts", &DataSourceConfig::max_reconnect_attempts)
        .def_readwrite("heartbeat_interval_ms", &DataSourceConfig::heartbeat_interval_ms);

    // IngestorStats
    py::class_<IngestorStats>(m, "IngestorStats")
        .def(py::init<>())
        .def_readonly("klines_received", &IngestorStats::klines_received)
        .def_readonly("klines_stored", &IngestorStats::klines_stored)
        .def_readonly("klines_failed", &IngestorStats::klines_failed)
        .def_readonly("bytes_received", &IngestorStats::bytes_received)
        .def_readonly("parse_errors", &IngestorStats::parse_errors)
        .def_readonly("reconnect_count", &IngestorStats::reconnect_count)
        .def_readonly("connected", &IngestorStats::connected)
        .def("__repr__", [](const IngestorStats& s) {
            return "<IngestorStats received=" + std::to_string(s.klines_received) +
                   " stored=" + std::to_string(s.klines_stored) +
                   " failed=" + std::to_string(s.klines_failed) + ">";
        });

    // DataIngestor
    py::class_<DataIngestor>(m, "DataIngestor")
        .def(py::init<quant::storage::StorageEngine&, quant::event::EventBus&, DataSourceConfig>(),
             py::arg("engine"), py::arg("bus"), py::arg("config"))
        .def("stop", &DataIngestor::stop)
        .def("is_running", &DataIngestor::is_running)
        .def("stats", &DataIngestor::stats)
        .def("ingest_kline", &DataIngestor::ingest_kline,
             py::arg("symbol"), py::arg("kline"),
             "Manually ingest a kline (for testing/backfill)");
}

}  // namespace quant::pybind
