#include <merlin/render/gpu_driven.hpp>
#include <merlin/render/gpu_scene_packing.hpp>

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>
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

template <typename Callback>
void ExpectError(Callback&& callback, GpuDrivenIndexedErrorCode code,
                 std::string_view fragment) {
  try {
    callback();
    assert(false && "expected GpuDrivenIndexedError");
  } catch (const GpuDrivenIndexedError& error) {
    assert(error.code() == code);
    assert(std::string_view(error.what()).find(fragment) !=
           std::string_view::npos);
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
  assert((plan.visible_draw_slots == std::vector<std::uint32_t>{3}));
  assert(plan.batches.size() == 1);
  assert(plan.batches[0].binding == bindings[0]);
  assert((plan.batches[0].visible_draw_slots ==
          std::vector<std::uint32_t>{3}));
  assert(plan.batches[0].commands.size() == 1);
  assert(plan.batches[0].commands[0].index_count == 3);
  assert(plan.batches[0].commands[0].instance_count == 1);
  assert(plan.batches[0].commands[0].first_index == 3);
  assert(plan.batches[0].commands[0].vertex_offset == 0);
  assert(plan.batches[0].commands[0].first_instance == 3);
  assert(plan.counters.candidate_draw_count == 3);
  assert(plan.counters.visible_draw_count == 1);
  assert(plan.counters.visibility_mask_culled_count == 1);
  assert(plan.counters.frustum_culled_count == 1);
  assert(plan.counters.indirect_command_count == 1);
  assert(plan.counters.indirect_batch_count == 1);
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
  assert(plan.visible_draw_slots == candidates);
  assert(plan.batches.size() == 1);
  assert(plan.batches[0].commands.size() == 3);
  assert(plan.batches[0].commands[1].first_index == 12);
  assert(plan.batches[0].commands[1].vertex_offset == 10);
  assert(plan.batches[0].commands[1].first_instance == 1);
  assert(plan.counters.visible_draw_count == 3);
  assert(plan.counters.visibility_mask_culled_count == 0);
  assert(plan.counters.frustum_culled_count == 0);
  assert(plan.counters.indirect_batch_count == 1);
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

  assert(plan.visible_draw_slots == candidates);
  assert(plan.batches.size() == 3);
  assert(plan.batches[0].binding == bindings[0]);
  assert((plan.batches[0].visible_draw_slots ==
          std::vector<std::uint32_t>{3}));
  assert(plan.batches[0].commands[0].first_instance == 3);
  assert(plan.batches[1].binding == bindings[1]);
  assert((plan.batches[1].visible_draw_slots ==
          std::vector<std::uint32_t>{1}));
  assert(plan.batches[1].commands[0].first_instance == 1);
  assert(plan.batches[1].commands[0].first_index == 0);
  assert(plan.batches[2].binding == bindings[0]);
  assert((plan.batches[2].visible_draw_slots ==
          std::vector<std::uint32_t>{4}));
  assert(plan.batches[2].commands[0].first_instance == 4);
  assert(plan.batches[2].commands[0].first_index == 3);
  assert(plan.counters.indirect_command_count == 3);
  assert(plan.counters.indirect_batch_count == 3);
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
