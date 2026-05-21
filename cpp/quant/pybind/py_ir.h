// py_ir.h — Python bindings for IR graph
#pragma once

#include <pybind11/pybind11.h>

namespace quant::pybind {

void bind_ir(pybind11::module_& m);

}  // namespace quant::pybind
