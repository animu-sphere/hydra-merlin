#pragma once

#include <filesystem>
#include <vector>

#include <merlin/vulkan/renderer.hpp>

namespace merlin::vulkan {

void WritePng(const std::filesystem::path& path, const ImageRgba8& image);
void WriteExr(const std::filesystem::path& path, const ImageDepth32& image);
void WriteExr(const std::filesystem::path& path, const ImageUint32& image);

struct ComparisonArtifactSet {
  bool matches{};
  std::vector<std::filesystem::path> files;
};

// Writes color-{expected,actual,diff}.png plus
// {depth,primId,instanceId}-{expected,actual,diff}.exr. Both results must carry
// CPU readback for all four reference AOVs at the same extent. Color, depth,
// primId, and instanceId comparisons are exact.
[[nodiscard]] ComparisonArtifactSet SaveComparisonArtifacts(
    const RenderResult& expected, const RenderResult& actual,
    const std::filesystem::path& directory);

}  // namespace merlin::vulkan
