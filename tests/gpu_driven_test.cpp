#include <merlin/render/gpu_driven.hpp>
#include <merlin/render/gpu_scene_packing.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using merlin::render::BuildGpuDrivenIndexedPlan;
using merlin::render::GpuDraw;
using merlin::render::GpuDrivenIndexedConfig;
using merlin::render::GpuDrivenIndexedError;
using merlin::render::GpuDrivenIndexedErrorCode;
using merlin::render::GpuDrivenGeometryBinding;
using merlin::render::GpuGeometry;
using merlin::render::GpuInstance;
using merlin::render::GpuMaterial;

void Require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

template <typename Callback>
void ExpectError(Callback&& callback, GpuDrivenIndexedErrorCode code,
                 std::string_view fragment) {
  try {
    callback();
    throw std::runtime_error("expected GpuDrivenIndexedError");
  } catch (const GpuDrivenIndexedError& error) {
    Require(error.code() == code, "GPU-driven error code did not match");
    Require(std::string_view(error.what()).find(fragment) !=
                std::string_view::npos,
            "GPU-driven error message did not contain expected text");
  }
}

GpuGeometry MakeGeometry(std::uint32_t vertex_offset,
                         std::uint32_t index_offset,
                         std::uint32_t index_count) {
  GpuGeometry result;
  result.vertex_offset = vertex_offset;
  result.vertex_count = 8;
  result.index_offset = index_offset;
  result.index_count = index_count;
  result.index_type = merlin::render::kGpuGeometryIndexTypeUint32;
  result.bounds_min = {-0.5F, -0.5F, 0.25F, 0.0F};
  result.bounds_max = {0.5F, 0.5F, 0.75F, 0.0F};
  return result;
}

GpuDraw MakeDraw(std::uint32_t geometry, std::uint32_t instance,
                 std::uint32_t primitive_base, std::uint32_t primitive_count,
                 std::uint64_t identity) {
  GpuDraw result;
  result.geometry_index = geometry;
  result.material_index = 0;
  result.instance_index = instance;
  result.primitive_base = primitive_base;
  result.primitive_count = primitive_count;
  merlin::render::SetGpuDrawIdentity(result, identity);
  return result;
}

void TestCullingCompactionAndStableIdentity() {
  constexpr auto vertex_stride = sizeof(merlin::extraction::DrawVertex);
  const std::vector geometries{
      MakeGeometry(0, 0, 6),
      MakeGeometry(static_cast<std::uint32_t>(vertex_stride * 10), 48, 3),
  };
  const std::vector<GpuDrivenGeometryBinding> bindings{{0, 0}, {0, 0}};
  std::vector<GpuInstance> instances(3);
  instances[0].visibility_mask = 0x1U;
  instances[1].visibility_mask = 0x2U;
  instances[2].visibility_mask = 0x1U;
  instances[2].transform.values[12] = 4.0F;

  std::vector<GpuDraw> draws(5);
  const std::vector<GpuMaterial> materials(1);
  draws[3] = MakeDraw(0, 0, 1, 1, 301);
  draws[1] = MakeDraw(1, 1, 0, 1, 302);
  draws[4] = MakeDraw(1, 2, 0, 1, 303);
  const std::vector<std::uint32_t> candidates{3, 1, 4};

  GpuDrivenIndexedConfig config;
  config.visibility_mask = 0x1U;
  const auto plan = BuildGpuDrivenIndexedPlan(
      candidates, geometries, bindings, instances, materials, draws, config);
  Require(plan.visible_draw_slots == std::vector<std::uint32_t>{3},
          "visible draw compaction is incorrect");
  Require(plan.batches.size() == 1, "expected one arena batch");
  Require(plan.batches[0].binding == bindings[0],
          "arena binding is incorrect");
  Require(plan.batches[0].visible_draw_slots ==
              std::vector<std::uint32_t>{3},
          "batch draw identity is incorrect");
  Require(plan.batches[0].commands.size() == 1,
          "expected one indirect command");
  Require(plan.batches[0].commands[0].index_count == 3,
          "indirect index count is incorrect");
  Require(plan.batches[0].commands[0].instance_count == 1,
          "indirect instance count is incorrect");
  Require(plan.batches[0].commands[0].first_index == 3,
          "indirect first index is incorrect");
  Require(plan.batches[0].commands[0].vertex_offset == 0,
          "indirect vertex offset is incorrect");
  Require(plan.batches[0].commands[0].first_instance == 3,
          "indirect draw identity is incorrect");
  Require(plan.counters.candidate_draw_count == 3,
          "candidate counter is incorrect");
  Require(plan.counters.visible_draw_count == 1,
          "visible counter is incorrect");
  Require(plan.counters.visibility_mask_culled_count == 1,
          "visibility-mask counter is incorrect");
  Require(plan.counters.frustum_culled_count == 1,
          "frustum counter is incorrect");
  Require(plan.counters.indirect_command_count == 1,
          "indirect-command counter is incorrect");
  Require(plan.counters.indirect_batch_count == 1,
          "indirect-batch counter is incorrect");
}

void TestCullingCanBeDisabledForValidation() {
  constexpr auto vertex_stride = sizeof(merlin::extraction::DrawVertex);
  const std::vector geometries{
      MakeGeometry(0, 0, 6),
      MakeGeometry(static_cast<std::uint32_t>(vertex_stride * 10), 48, 3),
  };
  const std::vector<GpuDrivenGeometryBinding> bindings{{0, 0}, {0, 0}};
  std::vector<GpuInstance> instances(3);
  instances[0].visibility_mask = 0x1U;
  instances[1].visibility_mask = 0x2U;
  instances[2].visibility_mask = 0x1U;
  instances[2].transform.values[12] = 4.0F;
  std::vector<GpuDraw> draws(5);
  const std::vector<GpuMaterial> materials(1);
  draws[3] = MakeDraw(0, 0, 1, 1, 301);
  draws[1] = MakeDraw(1, 1, 0, 1, 302);
  draws[4] = MakeDraw(1, 2, 0, 1, 303);
  const std::vector<std::uint32_t> candidates{3, 1, 4};

  GpuDrivenIndexedConfig config;
  config.visibility_mask = 0x1U;
  config.enable_visibility_mask_culling = false;
  config.enable_frustum_culling = false;
  const auto plan = BuildGpuDrivenIndexedPlan(
      candidates, geometries, bindings, instances, materials, draws, config);
  Require(plan.visible_draw_slots == candidates,
          "disabled culling changed candidate visibility");
  Require(plan.batches.size() == 1, "expected one arena batch");
  Require(plan.batches[0].commands.size() == 3,
          "expected one command per candidate");
  Require(plan.batches[0].commands[1].first_index == 12,
          "byte index offset was not converted to elements");
  Require(plan.batches[0].commands[1].vertex_offset == 10,
          "byte vertex offset was not converted to elements");
  Require(plan.batches[0].commands[1].first_instance == 1,
          "physical draw slot was not preserved");
  Require(plan.counters.visible_draw_count == 3,
          "disabled-culling visible counter is incorrect");
  Require(plan.counters.visibility_mask_culled_count == 0,
          "disabled visibility culling changed its counter");
  Require(plan.counters.frustum_culled_count == 0,
          "disabled frustum culling changed its counter");
  Require(plan.counters.indirect_batch_count == 1,
          "disabled-culling batch counter is incorrect");
}

void TestCommandsAreBatchedByArenaBlocks() {
  const std::vector geometries{
      MakeGeometry(0, 0, 6),
      MakeGeometry(0, 0, 3),
  };
  const std::vector<GpuDrivenGeometryBinding> bindings{{2, 4}, {7, 9}};
  const std::vector<GpuInstance> instances(3);
  const std::vector<GpuMaterial> materials(1);
  std::vector<GpuDraw> draws(5);
  draws[3] = MakeDraw(0, 0, 0, 1, 301);
  draws[1] = MakeDraw(1, 1, 0, 1, 302);
  draws[4] = MakeDraw(0, 2, 1, 1, 303);
  const std::vector<std::uint32_t> candidates{3, 1, 4};

  GpuDrivenIndexedConfig config;
  config.enable_visibility_mask_culling = false;
  config.enable_frustum_culling = false;
  const auto plan = BuildGpuDrivenIndexedPlan(
      candidates, geometries, bindings, instances, materials, draws, config);

  Require(plan.visible_draw_slots == candidates,
          "batching changed visible draw order");
  Require(plan.batches.size() == 3,
          "non-contiguous arena bindings were merged");
  Require(plan.batches[0].binding == bindings[0],
          "first arena binding is incorrect");
  Require(plan.batches[0].visible_draw_slots ==
              std::vector<std::uint32_t>{3},
          "first batch identity is incorrect");
  Require(plan.batches[0].commands[0].first_instance == 3,
          "first batch draw slot is incorrect");
  Require(plan.batches[1].binding == bindings[1],
          "second arena binding is incorrect");
  Require(plan.batches[1].visible_draw_slots ==
              std::vector<std::uint32_t>{1},
          "second batch identity is incorrect");
  Require(plan.batches[1].commands[0].first_instance == 1,
          "second batch draw slot is incorrect");
  Require(plan.batches[1].commands[0].first_index == 0,
          "second batch first index is incorrect");
  Require(plan.batches[2].binding == bindings[0],
          "third arena binding is incorrect");
  Require(plan.batches[2].visible_draw_slots ==
              std::vector<std::uint32_t>{4},
          "third batch identity is incorrect");
  Require(plan.batches[2].commands[0].first_instance == 4,
          "third batch draw slot is incorrect");
  Require(plan.batches[2].commands[0].first_index == 3,
          "third batch first index is incorrect");
  Require(plan.counters.indirect_command_count == 3,
          "batched command counter is incorrect");
  Require(plan.counters.indirect_batch_count == 3,
          "arena batch counter is incorrect");
}

void TestMalformedCandidatesAreRejected() {
  const std::vector geometries{MakeGeometry(0, 0, 3)};
  const std::vector<GpuDrivenGeometryBinding> bindings{{0, 0}};
  const std::vector<GpuInstance> instances(1);
  const std::vector<GpuMaterial> materials(1);
  std::vector<GpuDraw> draws(1);
  draws[0] = MakeDraw(0, 0, 0, 1, 1);
  const GpuDrivenIndexedConfig config;

  const std::vector<std::uint32_t> missing{1};
  ExpectError(
      [&] {
        (void)BuildGpuDrivenIndexedPlan(missing, geometries, bindings,
                                        instances, materials, draws, config);
      },
      GpuDrivenIndexedErrorCode::MissingResidency, "missing draw slot");

  const std::vector<std::uint32_t> candidate{0};
  ExpectError(
      [&] {
        (void)BuildGpuDrivenIndexedPlan(
            candidate, geometries, bindings, instances,
            std::span<const GpuMaterial>{}, draws, config);
      },
      GpuDrivenIndexedErrorCode::MissingResidency,
      "missing GPU Scene record");

  const std::vector<GpuDrivenGeometryBinding> missing_bindings(1);
  ExpectError(
      [&] {
        (void)BuildGpuDrivenIndexedPlan(candidate, geometries,
                                        missing_bindings, instances, materials,
                                        draws, config);
      },
      GpuDrivenIndexedErrorCode::MissingResidency,
      "no native arena block binding");

  ExpectError(
      [&] {
        (void)BuildGpuDrivenIndexedPlan(
            candidate, geometries,
            std::span<const GpuDrivenGeometryBinding>{}, instances, materials,
            draws, config);
      },
      GpuDrivenIndexedErrorCode::InvalidConfiguration,
      "binding count does not match");

  const std::vector<std::uint32_t> duplicate{0, 0};
  ExpectError(
      [&] {
        (void)BuildGpuDrivenIndexedPlan(duplicate, geometries, bindings,
                                        instances, materials, draws, config);
      },
      GpuDrivenIndexedErrorCode::InvalidCandidate, "duplicated");

  draws[0].primitive_count = 2;
  ExpectError(
      [&] {
        (void)BuildGpuDrivenIndexedPlan(candidate, geometries, bindings,
                                        instances, materials, draws, config);
      },
      GpuDrivenIndexedErrorCode::InvalidRecord, "exceeds its geometry");
}

void TestUnrepresentableAndNonFiniteInputsAreRejected() {
  std::vector geometries{MakeGeometry(1, 0, 3)};
  const std::vector<GpuDrivenGeometryBinding> bindings{{0, 0}};
  const std::vector<GpuInstance> instances(1);
  const std::vector<GpuMaterial> materials(1);
  std::vector<GpuDraw> draws(1);
  draws[0] = MakeDraw(0, 0, 0, 1, 1);
  const std::vector<std::uint32_t> candidate{0};
  GpuDrivenIndexedConfig config;
  ExpectError(
      [&] {
        (void)BuildGpuDrivenIndexedPlan(candidate, geometries, bindings,
                                        instances, materials, draws, config);
      },
      GpuDrivenIndexedErrorCode::UnrepresentableGeometry,
      "not element aligned");

  geometries[0] = MakeGeometry(0, 0, 3);
  config.view_projection.values[0] =
      std::numeric_limits<float>::quiet_NaN();
  ExpectError(
      [&] {
        (void)BuildGpuDrivenIndexedPlan(candidate, geometries, bindings,
                                        instances, materials, draws, config);
      },
      GpuDrivenIndexedErrorCode::InvalidConfiguration, "not finite");
}

}  // namespace

int main() {
  TestCullingCompactionAndStableIdentity();
  TestCullingCanBeDisabledForValidation();
  TestCommandsAreBatchedByArenaBlocks();
  TestMalformedCandidatesAreRejected();
  TestUnrepresentableAndNonFiniteInputsAreRejected();
  std::cout << "GPU-driven indexed planning tests passed\n";
  return 0;
}
