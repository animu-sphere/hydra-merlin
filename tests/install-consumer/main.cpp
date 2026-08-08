#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/render/backend.hpp>

int main() {
  merlin::RenderWorld world;
  merlin::extraction::SceneExtractor extractor;
  extractor.Apply(world, world.Commit());
  merlin::render::BackendCreateInfo backend_info;
  merlin::render::RendererSettings renderer_settings;
  return extractor.snapshot()->draws.empty() &&
                 backend_info.backend ==
                     merlin::render::BackendRequest::Automatic &&
                 merlin::render::kBackendContractVersion == 1 &&
                 renderer_settings.schema_version ==
                     merlin::render::kRendererSettingsSchemaVersion &&
                 !merlin::render::ValidateRendererSettings(renderer_settings)
             ? 0
             : 1;
}
