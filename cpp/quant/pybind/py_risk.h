// py_risk.h — Python bindings for risk engine
#pragma once

#include <pybind11/pybind11.h>

namespace quant::pybind {

void bind_risk(pybind11::module_& m);

}  // namespace quant::pybind
