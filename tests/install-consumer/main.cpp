#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/render/backend.hpp>
#include <merlin/render/forward_lighting.hpp>

int main() {
  merlin::RenderWorld world;
  merlin::extraction::SceneExtractor extractor;
  extractor.Apply(world, world.Commit());
  merlin::render::BackendCreateInfo backend_info;
  merlin::render::RendererSettings renderer_settings;
  const auto lighting = merlin::render::ExtractForwardDirectionalLighting(
      *extractor.snapshot());
  return extractor.snapshot()->draws.empty() &&
                 backend_info.backend ==
                     merlin::render::BackendRequest::Automatic &&
                 merlin::render::kBackendContractVersion == 3 &&
                 renderer_settings.schema_version ==
                     merlin::render::kRendererSettingsSchemaVersion &&
                 !merlin::render::ValidateRendererSettings(renderer_settings) &&
                 lighting.direction_intensity.w == 1.0F
             ? 0
             : 1;
}
