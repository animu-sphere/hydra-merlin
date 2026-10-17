#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/render/backend.hpp>
#include <merlin/render/forward_lighting.hpp>
#include <merlin/render/diffuse_environment.hpp>

int main() {
  merlin::RenderWorld world;
  merlin::extraction::SceneExtractor extractor;
  extractor.Apply(world, world.Commit());
  merlin::render::BackendCreateInfo backend_info;
  merlin::render::RendererSettings renderer_settings;
  const auto lighting = merlin::render::ExtractForwardDirectionalLighting(
      *extractor.snapshot());
  const auto environment = merlin::render::PackagedDiffuseEnvironment();
  const auto fill = merlin::render::EvaluateDiffuseEnvironment(environment, {0, 0, 1});
  const auto output = merlin::render::ApplyForwardOutputTransform({1, 1, 1}, {1, merlin::render::ToneMapping::Reinhard});
  return output.x > 0.66F && output.x < 0.67F && extractor.snapshot()->draws.empty() &&
                 backend_info.backend ==
                     merlin::render::BackendRequest::Automatic &&
                 merlin::render::kBackendContractVersion == 5 &&
                 renderer_settings.schema_version ==
                     merlin::render::kRendererSettingsSchemaVersion &&
                 !merlin::render::ValidateRendererSettings(renderer_settings) &&
                 lighting.direction_intensity.w == 1.0F &&
                 fill.x > 0 && fill.y > 0 && fill.z > 0
             ? 0
             : 1;
}
