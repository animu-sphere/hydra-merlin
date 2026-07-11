#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>

int main() {
  merlin::RenderWorld world;
  merlin::extraction::SceneExtractor extractor;
  extractor.Apply(world, world.Commit());
  return extractor.scene().draws.empty() ? 0 : 1;
}
