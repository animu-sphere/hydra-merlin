#include <merlin/vulkan/render_artifacts.hpp>

#include <array>
#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>

namespace {

merlin::vulkan::RenderResult MakeResult() {
  merlin::vulkan::RenderResult result;
  result.color.product = merlin::MakeRenderProduct(2, 2, merlin::Aov::Color);
  result.color.row_pitch_bytes = 8;
  result.color.pixels = {255, 0, 0, 255, 0, 255, 0, 255,
                         0, 0, 255, 255, 255, 255, 255, 255};
  result.depth.product = merlin::MakeRenderProduct(2, 2, merlin::Aov::Depth);
  result.depth.row_pitch_bytes = 8;
  result.depth.pixels = {0.1F, 0.2F, 0.3F, 1.0F};
  result.prim_id.product =
      merlin::MakeRenderProduct(2, 2, merlin::Aov::PrimId);
  result.prim_id.row_pitch_bytes = 8;
  result.prim_id.pixels = {1, 1, 2,
                           std::numeric_limits<std::uint32_t>::max()};
  result.rendered_aovs = {merlin::Aov::Color, merlin::Aov::Depth,
                          merlin::Aov::PrimId};
  result.cpu_readback_aovs = result.rendered_aovs;
  result.completion_value = 1;
  return result;
}

std::array<std::uint8_t, 8> Prefix(const std::filesystem::path& path) {
  std::array<std::uint8_t, 8> result{};
  std::ifstream stream(path, std::ios::binary);
  assert(stream);
  stream.read(reinterpret_cast<char*>(result.data()), result.size());
  assert(stream.gcount() == static_cast<std::streamsize>(result.size()));
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  assert(argc == 2);
  const std::filesystem::path output = argv[1];
  const auto expected = MakeResult();
  auto actual = expected;
  actual.completion_value = 2;
  actual.color.pixels[0] = 200;
  actual.depth.pixels[1] = 0.25F;
  actual.prim_id.pixels[2] = 7;

  const auto artifacts =
      merlin::vulkan::SaveComparisonArtifacts(expected, actual, output);
  assert(!artifacts.matches);
  assert(artifacts.files.size() == 9);
  for (const auto& file : artifacts.files) {
    assert(std::filesystem::is_regular_file(file));
    assert(std::filesystem::file_size(file) > 32);
  }
  const std::array<std::uint8_t, 8> png_signature{
      0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  assert(Prefix(output / "color-diff.png") == png_signature);
  const auto exr = Prefix(output / "depth-diff.exr");
  assert(exr[0] == 0x76 && exr[1] == 0x2f && exr[2] == 0x31 &&
         exr[3] == 0x01);
  return 0;
}
