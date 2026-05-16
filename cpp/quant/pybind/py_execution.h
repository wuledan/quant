// py_execution.h — Python bindings for execution engine
#pragma once

#include <pybind11/pybind11.h>

namespace quant::pybind {

void bind_execution(pybind11::module_& m);

}  // namespace quant::pybind
