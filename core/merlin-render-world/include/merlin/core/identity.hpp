#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace merlin {

// Every Merlin identity is the SHA-256 of a canonical record. Module,
// parameter, resource, and target-artifact keys differ only in what they put
// into the record, so a single encoding keeps them comparable and lets an
// evidence consumer recompute any of them from the recorded fields.

// Prefix and total length of a canonical identity string.
inline constexpr std::string_view kIdentityPrefix = "sha256:";
inline constexpr std::size_t kIdentityLength = 71U;

[[nodiscard]] std::string Sha256Hex(std::string_view input);

// Length-prefixed `name=<bytes>:<value>` field terminated by a newline. The
// explicit length means no value can imitate a separator, so two different
// field sequences can never hash to the same record.
void AppendIdentityField(std::string& record, std::string_view name,
                         std::string_view value);

[[nodiscard]] std::string MakeIdentity(std::string_view record);

[[nodiscard]] bool IsIdentity(std::string_view value) noexcept;

}  // namespace merlin
