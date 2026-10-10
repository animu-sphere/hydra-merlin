#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/render/backend.hpp>
#include <merlin/render/forward_lighting.hpp>

#include <cassert>
#include <cmath>
#include <limits>

namespace {

void Same(const merlin::render::ForwardDirectionalLighting& lhs,
    const merlin::render::ForwardDirectionalLighting& rhs) {
  assert(lhs.light == rhs.light);
  assert(lhs.direction_intensity.x == rhs.direction_intensity.x);
  assert(lhs.direction_intensity.y == rhs.direction_intensity.y);
  assert(lhs.direction_intensity.z == rhs.direction_intensity.z);
  assert(lhs.direction_intensity.w == rhs.direction_intensity.w);
  assert(lhs.color.x == rhs.color.x);
  assert(lhs.color.y == rhs.color.y);
  assert(lhs.color.z == rhs.color.z);
}

} // namespace

int main() {
  using merlin::render::ExtractForwardDirectionalLighting;
  merlin::RenderWorld world;
  merlin::extraction::SceneExtractor extractor;
  const auto extract = [&] {
    extractor.Apply(world, world.Commit());
    return ExtractForwardDirectionalLighting(*extractor.snapshot());
  };
  const auto fallback = extract();
  assert(fallback.light == 0);
  assert(fallback.direction_intensity.z == 1.0F);
  assert(fallback.direction_intensity.w == 1.0F);

  // Removing the first table slot moves the last light ahead of the selected
  // directional input. Stable handles must still select the same source.
  merlin::LightDescriptor point;
  point.type = merlin::LightType::Point;
  const auto point_handle = world.CreateLight(point);
  merlin::LightDescriptor key;
  key.color = {0.2F, 0.4F, 0.8F};
  key.intensity = 2.0F;
  key.transform.values[8] = 0.0F;
  key.transform.values[9] = 3.0F;
  key.transform.values[10] = 4.0F;
  const auto key_handle = world.CreateLight(key);
  const auto fill_handle = world.CreateLight(merlin::LightDescriptor{});
  const auto selected = extract();
  assert(selected.light == key_handle.value());
  assert(std::abs(selected.direction_intensity.y - 0.6F) < 1e-6F);
  assert(std::abs(selected.direction_intensity.z - 0.8F) < 1e-6F);
  assert(selected.direction_intensity.w == 2.0F);
  assert(selected.color.z == 0.8F);
  const auto retained = extractor.snapshot();
  world.Remove(point_handle);
  Same(extract(), selected);
  Same(ExtractForwardDirectionalLighting(*retained), selected);

  // Camera motion and light translation have no effect on a world-space
  // directional source, and a zero intensity stays explicitly disabled.
  merlin::CameraDescriptor camera;
  camera.view.values[0] = -1.0F;
  camera.view.values[12] = 10.0F;
  const auto camera_handle = world.CreateCamera(camera);
  extractor.SetActiveCamera(camera_handle);
  key.transform.values[12] = 123.0F;
  world.UpdateLight(key_handle, key, merlin::ChangeAspect::Transform);
  Same(extract(), selected);
  key.intensity = 0.0F;
  world.UpdateLight(key_handle, key, merlin::ChangeAspect::LightParameters);
  assert(extract().direction_intensity.w == 0.0F);

  // Both very large and very small finite axes normalize without losing the
  // direction. Zero and non-finite axes share the +Z recovery on all backends.
  for (const auto magnitude : {std::numeric_limits<float>::max(),
           std::numeric_limits<float>::min()}) {
    key.transform.values[8] = magnitude;
    key.transform.values[9] = magnitude;
    key.transform.values[10] = 0.0F;
    world.UpdateLight(key_handle, key, merlin::ChangeAspect::Transform);
    const auto normalized = extract();
    assert(std::abs(normalized.direction_intensity.x - 0.70710678F) < 1e-6F);
    assert(std::abs(normalized.direction_intensity.y - 0.70710678F) < 1e-6F);
    assert(normalized.direction_intensity.z == 0.0F);
  }
  for (const auto invalid_axis : {0.0F,
           std::numeric_limits<float>::infinity(),
           std::numeric_limits<float>::quiet_NaN()}) {
    key.transform.values[8] = invalid_axis;
    key.transform.values[9] = 0.0F;
    key.transform.values[10] = 0.0F;
    world.UpdateLight(key_handle, key, merlin::ChangeAspect::Transform);
    const auto recovered = extract();
    assert(recovered.direction_intensity.x == 0.0F);
    assert(recovered.direction_intensity.y == 0.0F);
    assert(recovered.direction_intensity.z == 1.0F);
    assert(recovered.direction_intensity.w == 0.0F);
  }
  for (const bool invalid_color : {false, true}) {
    for (const auto invalid_value : {-1.0F,
             std::numeric_limits<float>::infinity(),
             std::numeric_limits<float>::quiet_NaN()}) {
      auto invalid = key;
      if (invalid_color) {
        invalid.color.y = invalid_value;
      } else {
        invalid.intensity = invalid_value;
      }
      world.UpdateLight(key_handle, invalid,
          merlin::ChangeAspect::LightParameters);
      bool rejected = false;
      try {
        (void)extract();
      } catch (const merlin::render::RendererError& error) {
        rejected = error.code() == merlin::render::RendererErrorCode::InvalidRequest &&
                   error.operation() == "extract Forward directional lighting";
      }
      assert(rejected);
    }
  }
  world.Remove(key_handle);
  assert(extract().light == fill_handle.value());
  world.Remove(fill_handle);
  Same(extract(), fallback);
}
