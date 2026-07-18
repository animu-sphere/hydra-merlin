#pragma once

#include <string>
#include <string_view>

namespace merlin::materialx::detail {

[[nodiscard]] std::string Sha256(std::string_view input);

}  // namespace merlin::materialx::detail
