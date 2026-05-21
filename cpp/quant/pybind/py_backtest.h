// py_backtest.h — Python bindings for backtest + strategy
#pragma once

#include <pybind11/pybind11.h>

namespace quant::pybind {

void bind_backtest(pybind11::module_& m);

}  // namespace quant::pybind
