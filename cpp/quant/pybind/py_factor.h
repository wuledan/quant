// py_factor.h — Python bindings for factor engine
#pragma once

#include <pybind11/pybind11.h>

namespace quant::pybind {

void bind_factor(pybind11::module_& m);

}  // namespace quant::pybind
