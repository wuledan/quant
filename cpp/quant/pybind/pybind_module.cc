// pybind_module.cc — Python module entry point for quant C++ bindings
#define PYBIND11_DETAILED_ERROR_MESSAGES

#include <pybind11/numpy.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpp/quant/pybind/py_backtest.h"
#include "cpp/quant/pybind/py_execution.h"
#include "cpp/quant/pybind/py_factor.h"
#include "cpp/quant/pybind/py_ir.h"
#include "cpp/quant/pybind/py_risk.h"
#include "cpp/quant/pybind/py_storage.h"

// Forward declaration for ingest bindings
namespace quant::pybind {
void bind_ingest(pybind11::module_& m);
}

namespace py = pybind11;

// ── Zero-copy transfer utilities ──
namespace {

py::array_t<double> vector_to_numpy(const std::vector<double>& v) {
    return py::array_t<double>(v.size(), v.data());
}

std::vector<double> numpy_to_vector(py::array_t<double> arr) {
    auto buf = arr.request();
    auto* ptr = static_cast<const double*>(buf.ptr);
    return {ptr, ptr + buf.size};
}

// Shared memory segment descriptor
struct SharedMemSegment {
    std::string name;
    size_t      size;
    void*       addr;
    bool        is_owner;
};

// In-process shared memory simulation (no OS shm for portability)
// In production, this would use boost::interprocess or POSIX shm_open
class SharedMemoryPool {
public:
    SharedMemoryPool() = default;
    ~SharedMemoryPool() {
        for (auto& [name, buf] : buffers_) {
            ::operator delete(buf.data, std::align_val_t{64});
        }
    }

    std::pair<void*, size_t> allocate(const std::string& name, size_t size) {
        auto it = buffers_.find(name);
        if (it != buffers_.end()) {
            return {it->second.data, it->second.size};
        }
        auto* ptr = ::operator new(size, std::align_val_t{64});
        buffers_[name] = {ptr, size};
        return {ptr, size};
    }

    void* get(const std::string& name, size_t* out_size = nullptr) const {
        auto it = buffers_.find(name);
        if (it != buffers_.end()) {
            if (out_size) *out_size = it->second.size;
            return it->second.data;
        }
        return nullptr;
    }

    void release(const std::string& name) {
        auto it = buffers_.find(name);
        if (it != buffers_.end()) {
            ::operator delete(it->second.data, std::align_val_t{64});
            buffers_.erase(it);
        }
    }

private:
    struct Buffer {
        void* data;
        size_t size;
    };
    std::unordered_map<std::string, Buffer> buffers_;
};

SharedMemoryPool& global_shm_pool() {
    static SharedMemoryPool pool;
    return pool;
}

}  // anonymous namespace

PYBIND11_MODULE(_quant_core, m) {
    m.doc() = "QuantInvest C++ native core — storage, factor, execution, risk engines";
    m.attr("__version__") = "0.1.0";

    // ── Bind all submodules ──
    quant::pybind::bind_storage(m);
    quant::pybind::bind_factor(m);
    quant::pybind::bind_execution(m);
    quant::pybind::bind_risk(m);
    quant::pybind::bind_ir(m);
    quant::pybind::bind_backtest(m);

    // ── Zero-copy transfer utilities ──
    m.def("vector_to_numpy", &vector_to_numpy,
          "Convert std::vector<double> to numpy array (shared memory)");
    m.def("numpy_to_vector", &numpy_to_vector,
          "Convert numpy array to std::vector<double>");

    // ── Shared memory pool ──
    auto shm = m.def_submodule("shm", "Shared memory pool for zero-copy data transfer");
    shm.def("allocate", [](const std::string& name, size_t size) -> py::dict {
        auto [addr, actual_size] = global_shm_pool().allocate(name, size);
        py::dict info;
        info["name"] = name;
        info["size"] = actual_size;
        info["address"] = reinterpret_cast<uintptr_t>(addr);
        return info;
    }, py::arg("name"), py::arg("size"));

    shm.def("get", [](const std::string& name) -> py::object {
        size_t size = 0;
        auto* addr = global_shm_pool().get(name, &size);
        if (!addr) return py::none();
        py::dict info;
        info["name"] = name;
        info["size"] = size;
        info["address"] = reinterpret_cast<uintptr_t>(addr);
        return info;
    }, py::arg("name"));

    shm.def("release", [](const std::string& name) {
        global_shm_pool().release(name);
    }, py::arg("name"));

    shm.def("write_doubles", [](const std::string& name, py::array_t<double> arr) {
        auto* addr = global_shm_pool().get(name);
        if (!addr) throw std::runtime_error("segment not found: " + name);
        auto buf = arr.request();
        std::memcpy(addr, buf.ptr, buf.size * sizeof(double));
        return buf.size;
    }, py::arg("name"), py::arg("data"));

    shm.def("read_doubles", [](const std::string& name, size_t count) -> py::array_t<double> {
        auto* addr = global_shm_pool().get(name);
        if (!addr) throw std::runtime_error("segment not found: " + name);
        auto result = py::array_t<double>(count);
        auto buf = result.request();
        std::memcpy(buf.ptr, addr, count * sizeof(double));
        return result;
    }, py::arg("name"), py::arg("count"));
}
