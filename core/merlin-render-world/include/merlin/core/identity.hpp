#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace merlin {

// Every Merlin identity is the SHA-256 of a canonical record. Module,
// parameter, resource, and target-artifact keys differ only in what they put
// into the record, so a single encoding keeps them comparable and lets an
// evidence consumer recompute any of them from the recorded fields.

// Prefix, digest length, and total length of a canonical identity string.
inline constexpr std::string_view kIdentityPrefix = "sha256:";
inline constexpr std::size_t kIdentityDigestLength = 64U;
inline constexpr std::size_t kIdentityLength =
    kIdentityPrefix.size() + kIdentityDigestLength;

[[nodiscard]] std::string Sha256Hex(std::string_view input);

// Length-prefixed `<bytes>:name=<bytes>:value` field terminated by a newline.
// Both lengths are explicit, so neither a name nor a value can imitate a
// separator: the record decodes one way only, and two different field sequences
// can never hash to the same record. Prefixing only the value would leave a
// name free to spell a complete field and collide with a shorter name carrying
// the rest as its value.
void AppendIdentityField(std::string& record, std::string_view name,
                         std::string_view value);

[[nodiscard]] std::string MakeIdentity(std::string_view record);

[[nodiscard]] bool IsIdentity(std::string_view value) noexcept;

}  // namespace merlin
