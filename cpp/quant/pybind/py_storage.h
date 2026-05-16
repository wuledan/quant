// py_storage.h — Python bindings for storage engine
#pragma once

#include <pybind11/pybind11.h>

namespace quant::pybind {

void bind_storage(pybind11::module_& m);

}  // namespace quant::pybind
