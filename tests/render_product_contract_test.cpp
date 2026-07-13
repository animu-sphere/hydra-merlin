#include <merlin/vulkan/renderer.hpp>

#include <cassert>
#include <limits>
#include <stdexcept>

namespace {

bool Rejects(const merlin::vulkan::RenderResult& result) {
  try {
    merlin::vulkan::ValidateRenderResult(result);
  } catch (const std::invalid_argument&) {
    return true;
  }
  return false;
}

}  // namespace

int main() {
  merlin::vulkan::RenderResult result;
  result.color.product =
      merlin::MakeRenderProduct(2, 2, merlin::Aov::Color);
  result.color.row_pitch_bytes = 8;
  result.color.pixels.resize(16);
  result.depth.product =
      merlin::MakeRenderProduct(2, 2, merlin::Aov::Depth);
  result.depth.row_pitch_bytes = 8;
  result.depth.pixels.assign(4, 1.0F);
  result.prim_id.product =
      merlin::MakeRenderProduct(2, 2, merlin::Aov::PrimId);
  result.prim_id.row_pitch_bytes = 8;
  result.prim_id.pixels.assign(4, std::numeric_limits<std::uint32_t>::max());
  result.instance_id.product =
      merlin::MakeRenderProduct(2, 2, merlin::Aov::InstanceId);
  result.instance_id.row_pitch_bytes = 8;
  result.instance_id.pixels.assign(
      4, std::numeric_limits<std::uint32_t>::max());
  result.rendered_aovs = {merlin::Aov::Color, merlin::Aov::Depth,
                          merlin::Aov::PrimId, merlin::Aov::InstanceId};
  result.cpu_readback_aovs = result.rendered_aovs;
  result.completion_value = 1;
  merlin::vulkan::ValidateRenderResult(result);

  auto invalid = result;
  invalid.color.product.origin = merlin::ImageOrigin::BottomLeft;
  assert(Rejects(invalid));

  invalid = result;
  invalid.depth.row_pitch_bytes = 16;
  assert(Rejects(invalid));

  invalid = result;
  invalid.depth.pixels.front() =
      std::numeric_limits<float>::quiet_NaN();
  assert(Rejects(invalid));

  invalid = result;
  invalid.depth.pixels.front() = -0.01F;
  assert(Rejects(invalid));

  invalid = result;
  invalid.completion_value = 0;
  assert(Rejects(invalid));

  // Per-request readback may omit payloads for rendered GPU-only AOVs.
  auto selected = result;
  selected.cpu_readback_aovs = {merlin::Aov::Color};
  selected.depth = {};
  selected.prim_id = {};
  selected.instance_id = {};
  merlin::vulkan::ValidateRenderResult(selected);
  assert(merlin::vulkan::HasCpuReadback(selected, merlin::Aov::Color));
  assert(!merlin::vulkan::HasCpuReadback(selected, merlin::Aov::Depth));

  selected.cpu_readback_aovs = {merlin::Aov::Normal};
  assert(Rejects(selected));

  assert(merlin::vulkan::RendererErrorCodeName(
             merlin::vulkan::RendererErrorCode::Timeout) == "timeout");
  const merlin::vulkan::RendererError classified(
      merlin::vulkan::RendererErrorCode::DeviceLost, "submit", "lost", -4);
  assert(classified.code() ==
         merlin::vulkan::RendererErrorCode::DeviceLost);
  assert(classified.operation() == "submit");
  assert(classified.native_code() == -4);
  return 0;
}
