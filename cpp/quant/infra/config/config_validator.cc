// config_validator.cc — ConfigValidator implementation
// ConfigValidator<T> is a header-only template.
// ConfigValidatorRegistry is also header-only (template methods).
// This translation unit ensures the headers are validated by the compiler.

#include "cpp/quant/infra/config/config_validator.h"
#include "cpp/quant/infra/config/config_validator_registry.h"

// Explicit template instantiations for supported ConfigValue types
template class quant::infra::ConfigValidator<bool>;
template class quant::infra::ConfigValidator<int64_t>;
template class quant::infra::ConfigValidator<double>;
template class quant::infra::ConfigValidator<std::string>;

namespace quant::infra {
// Intentionally empty — all logic is in headers.
}  // namespace quant::infra