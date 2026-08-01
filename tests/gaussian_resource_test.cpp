#include <merlin/core/gaussian.hpp>
#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>

#include <cassert>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

namespace {

bool Near(float lhs, float rhs, float tolerance = 1.0e-5F) {
  return std::abs(lhs - rhs) <= tolerance;
}

class CollectingSink final : public merlin::DiagnosticSink {
 public:
  void Report(const merlin::Diagnostic& diagnostic) override {
    diagnostics.push_back(diagnostic);
  }

  std::vector<merlin::Diagnostic> diagnostics;
};

}  // namespace

int main() {
  merlin::GaussianSourceData source;
  source.label = "two-splats";
  source.source = "/Fixture/Splat";
  source.positions = {{0.0F, 0.0F, 0.0F}, {1.0F, 2.0F, 3.0F}};
  source.orientations = {
      {2.0F, {0.0F, 0.0F, 0.0F}},
      {0.70710678F, {0.0F, 0.0F, 0.70710678F}}};
  source.scales = {{1.0F, 2.0F, 3.0F}, {1.0F, 2.0F, 3.0F}};
  source.opacities = {-1.0F, 2.0F};
  source.spherical_harmonics_degree = 1;
  source.spherical_harmonics_coefficients.resize(8,
                                                  {0.25F, 0.5F, 0.75F});

  CollectingSink sink;
  auto normalized = merlin::NormalizeGaussianSource(source, &sink);
  assert(normalized.accepted());
  assert(sink.diagnostics.size() == 1);
  assert(sink.diagnostics.front().code == "gaussian.opacity.clamped");
  assert(normalized.diagnostics.size() == 1);
  const auto& resource = *normalized.resource;
  assert(resource.positions.size() == 2);
  assert(resource.covariances.size() == 2);
  assert(resource.opacities == std::vector<float>({0.0F, 1.0F}));
  assert(resource.spherical_harmonics_degree == 1);
  assert(resource.spherical_harmonics_coefficients.size() == 8);
  assert(Near(resource.covariances[0].xx, 1.0F));
  assert(Near(resource.covariances[0].yy, 4.0F));
  assert(Near(resource.covariances[0].zz, 9.0F));
  // A 90-degree Z rotation swaps the X/Y variances.
  assert(Near(resource.covariances[1].xx, 4.0F));
  assert(Near(resource.covariances[1].yy, 1.0F));
  assert(Near(resource.covariances[1].zz, 9.0F));

  merlin::RenderWorld world;
  const auto handle = world.CreateGaussian(resource);
  auto changes = world.Commit();
  assert(changes.revision == 1);
  assert(changes.changes.size() == 1);
  assert(changes.changes.front().object_kind == merlin::ObjectKind::Gaussian);
  assert(changes.changes.front().HasAspect(
      merlin::ChangeAspect::GaussianPositions));
  assert(changes.changes.front().HasAspect(
      merlin::ChangeAspect::GaussianCovariance));
  assert(world.resource_revision(handle) == 1);
  merlin::extraction::SceneExtractor extractor;
  extractor.Apply(world, changes);
  auto snapshot = extractor.snapshot();
  assert(snapshot->gaussians.size() == 1);
  const auto initial_positions = snapshot->gaussians.front().positions;
  const auto initial_opacities = snapshot->gaussians.front().opacities;
  assert(snapshot->gaussians.front().radiance_revision == 1);

  auto edited = resource;
  edited.opacities[0] = 0.5F;
  world.UpdateGaussian(handle, edited, merlin::ChangeAspect::GaussianOpacity,
                       std::vector<merlin::ElementRange>{{0, 1}});
  edited.opacities[1] = 0.25F;
  world.UpdateGaussian(handle, edited, merlin::ChangeAspect::GaussianOpacity,
                       std::vector<merlin::ElementRange>{{1, 1}});
  changes = world.Commit();
  assert(changes.revision == 2);
  assert(changes.changes.size() == 1);
  assert(changes.changes.front().particle_ranges_known);
  assert(changes.changes.front().particle_ranges ==
         std::vector<merlin::ElementRange>({{0, 2}}));
  assert(world.resource_revision(handle) == 3);
  extractor.Apply(world, changes);
  snapshot = extractor.snapshot();
  assert(snapshot->delta->gaussians.upserts ==
         std::vector<std::uint64_t>({handle.value()}));
  assert(snapshot->gaussians.front().positions == initial_positions);
  assert(snapshot->gaussians.front().opacities != initial_opacities);
  assert(snapshot->gaussians.front().opacity_revision == 3);
  assert(snapshot->gaussians.front().positions_revision == 1);
  assert(snapshot->gaussians.front().particle_base_revision == 1);
  assert(snapshot->gaussians.front().particle_ranges ==
         std::vector<merlin::ElementRange>({{0, 2}}));

  edited.transform.values[12] = 4.0F;
  world.UpdateGaussian(handle, edited, merlin::ChangeAspect::Transform);
  changes = world.Commit();
  assert(changes.revision == 3);
  extractor.Apply(world, changes);
  snapshot = extractor.snapshot();
  assert(snapshot->gaussians.front().positions == initial_positions);
  assert(snapshot->gaussians.front().opacity_revision == 3);
  assert(snapshot->gaussians.front().transform_revision == 4);
  assert(snapshot->gaussians.front().transform.values[12] == 4.0F);

  edited.visible = false;
  world.UpdateGaussian(handle, edited, merlin::ChangeAspect::Visibility);
  changes = world.Commit();
  assert(changes.revision == 4);
  extractor.Apply(world, changes);
  snapshot = extractor.snapshot();
  assert(snapshot->gaussians.front().positions == initial_positions);
  assert(snapshot->gaussians.front().visibility_revision == 5);
  assert(!snapshot->gaussians.front().visible);

  merlin::GaussianSourceData fallback;
  fallback.source = "/Fixture/Fallback";
  fallback.positions = {{0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F}};
  fallback.orientations = {{1.0F, {0.0F, 0.0F, 0.0F}}};
  fallback.scales = {{2.0F, 2.0F, 2.0F}, {2.0F, 2.0F, 2.0F},
                     {9.0F, 9.0F, 9.0F}};
  auto fallback_result = merlin::NormalizeGaussianSource(fallback);
  assert(fallback_result.accepted());
  assert(fallback_result.diagnostics.size() == 4);
  assert(fallback_result.resource->spherical_harmonics_degree == 0);
  assert(fallback_result.resource->spherical_harmonics_coefficients.size() ==
         2);
  assert(Near(fallback_result.resource->covariances[0].xx, 4.0F));

  auto rejected = source;
  rejected.positions[0].x = std::numeric_limits<float>::infinity();
  const auto rejected_result = merlin::NormalizeGaussianSource(rejected);
  assert(!rejected_result.accepted());
  assert(rejected_result.diagnostics.size() == 1);
  assert(rejected_result.diagnostics.front().code ==
         "gaussian.positions.non-finite");
  assert(rejected_result.diagnostics.front().disposition ==
         merlin::DiagnosticDisposition::Rejected);

  world.Remove(handle);
  changes = world.Commit();
  assert(changes.revision == 5);
  assert(changes.changes.front().change_kind == merlin::ChangeKind::Removed);
  extractor.Apply(world, changes);
  assert(extractor.snapshot()->gaussians.empty());
  assert(extractor.snapshot()->delta->gaussians.removals ==
         std::vector<std::uint64_t>({handle.value()}));
  return 0;
}
