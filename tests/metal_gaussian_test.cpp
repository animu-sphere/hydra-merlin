#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/metal/backend.hpp>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
void Require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
std::uint8_t Channel(const merlin::render::RenderResult& image,
    unsigned x, unsigned y, unsigned c) {
  return image.color.pixels.at(y * image.color.row_pitch_bytes + x * 4 + c);
}
std::uint32_t Id(const merlin::render::RenderResult& image,
    unsigned x, unsigned y) {
  return image.instance_id.pixels.at(y * image.instance_id.row_pitch_bytes / 4 + x);
}
} // namespace

int main() {
  merlin::metal::BackendFactory factory;
  if (!factory.availability().available)
    return 77;
  try {
    merlin::render::BackendCreateInfo info;
    info.frames_in_flight = 3;
    info.enable_validation = true;
    auto backend = factory.Create(info);
    merlin::RenderWorld world;
    merlin::extraction::SceneExtractor extractor;
    merlin::GaussianDescriptor g;
    // Red in front of green, deliberately stored in the reverse of draw order.
    // A third off-center particle tests Metal's Y orientation and IDs.
    g.positions = {{0, 0, 0.3F}, {0, 0, 0.6F}, {0.5F, 0.5F, 0.4F}};
    g.covariances.assign(3, {0.01F, 0, 0, 0.0025F, 0, 0.0001F});
    g.opacities = {0.5F, 0.5F, 1.0F};
    constexpr float sh = 0.2820947918F;
    g.spherical_harmonics_coefficients = {
        {0.5F / sh, -0.5F / sh, -0.5F / sh},
        {-0.5F / sh, 0.5F / sh, -0.5F / sh},
        {-0.5F / sh, -0.5F / sh, 0.5F / sh}};
    const auto handle = world.CreateGaussian(g);
    auto snapshot = [&] {
      extractor.Apply(world, world.Commit());
      return extractor.snapshot();
    };
    merlin::render::RenderRequest request;
    request.snapshot = snapshot();
    request.width = request.height = 64;
    request.clear_color = {0, 0, 0, 0};
    request.products = {{merlin::Aov::Color, true}, {merlin::Aov::Depth, true},
        {merlin::Aov::PrimId, true}, {merlin::Aov::InstanceId, true}};
    auto render = [&] { return backend->Resolve(backend->Submit(request)); };
    const auto first = render();
    Require(first.telemetry.gaussian_visible_count == 3 &&
                first.telemetry.gaussian_draw_count == 1,
        "missing Gaussian draw");
    Require(first.telemetry.gaussian_upload_bytes != 0, "missing upload telemetry");
    const auto red = Channel(first, 32, 32, 0);
    const auto green = Channel(first, 32, 32, 1);
    Require(red > 105 && red < 130 && green > 55 && green < 70,
        "back-to-front alpha blending failed");
    Require(Id(first, 32, 32) == 0, "nearest particle did not win picking");
    Require(first.prim_id.pixels.at(32 * first.prim_id.row_pitch_bytes / 4 + 32) ==
                static_cast<std::uint32_t>(handle.value()),
        "Gaussian resource ID mismatch");
    Require(first.depth.pixels.at(32 * first.depth.row_pitch_bytes / 4 + 32) == 1.0F,
        "transparent Gaussian wrote depth");
    Require(Id(first, 48, 16) == 2 && Channel(first, 48, 16, 2) > 200,
        "Metal Gaussian is vertically inverted");
    Require(Id(first, 48, 48) == std::numeric_limits<std::uint32_t>::max(),
        "Gaussian appeared in the mirrored location");
    Require(Channel(first, 39, 39, 0) == 0, "ellipse leaked into bounding quad");
    Require(Channel(first, 35, 32, 0) > Channel(first, 32, 35, 0),
        "anisotropic covariance was lost");
    // All frame slots reuse the same immutable stream, with no new uploads.
    for (unsigned i = 0; i != 4; ++i) {
      const auto steady = render();
      Require(steady.color.pixels == first.color.pixels, "static frame drifted");
      Require(steady.telemetry.gaussian_preparation_cache_hits == 1 &&
                  steady.telemetry.gaussian_upload_bytes == 0,
          "static stream was rebuilt");
    }
    request.gpu_driven_gaussian.mode = merlin::render::GpuDrivenGaussianMode::Prefer;
    const auto fallback = render();
    Require(fallback.color.pixels == first.color.pixels &&
                fallback.telemetry.gaussian_gpu_fallback_count == 1,
        "prefer did not fall back to CPU preparation");
    request.gpu_driven_gaussian.mode = merlin::render::GpuDrivenGaussianMode::Require;
    bool rejected = false;
    try {
      (void)render();
    } catch (const merlin::render::RendererError& e) {
      rejected = e.code() == merlin::render::RendererErrorCode::Unsupported;
    }
    Require(rejected, "unsupported GPU preparation was accepted");
    request.gpu_driven_gaussian.mode = merlin::render::GpuDrivenGaussianMode::Disabled;

    // Submit edits before resolving the earlier frame: each must retain its
    // own stream, even when the global preparation cache has been replaced.
    const auto old_token = backend->Submit(request);
    g.opacities[0] = 0;
    world.UpdateGaussian(handle, g);
    request.snapshot = snapshot();
    const auto edited_token = backend->Submit(request);
    const auto old_image = backend->Resolve(old_token);
    const auto edited = backend->Resolve(edited_token);
    Require(old_image.color.pixels == first.color.pixels,
        "in-flight Gaussian stream was overwritten");
    Require(Id(edited, 32, 32) == 1 && Channel(edited, 32, 32, 0) == 0,
        "opacity edit did not invalidate the cache");

    // A camera move changes projected position without changing table identity.
    auto moved = std::make_shared<merlin::extraction::FrameSnapshot>(*request.snapshot);
    moved->view.values[12] = -0.5F;
    request.snapshot = moved;
    const auto camera = render();
    Require(camera.telemetry.gaussian_preparation_cache_misses == 1 &&
                Id(camera, 16, 32) == 1,
        "camera motion did not rebuild projection");
    request.width = 96;
    const auto resized = render();
    Require(resized.telemetry.gaussian_preparation_cache_misses == 1 &&
                Id(resized, 24, 32) == 1,
        "resize did not rebuild projection");
    request.width = 64;

    // Opaque mesh at z=.45 hides green (.6) but permits red (.3).
    merlin::MeshDescriptor mesh;
    mesh.positions = {{-0.8F, -0.8F, 0.45F}, {0.8F, -0.8F, 0.45F},
        {0.8F, 0.8F, 0.45F}, {-0.8F, 0.8F, 0.45F}};
    mesh.indices = {0, 1, 2, 0, 2, 3};
    const auto mesh_handle = world.CreateMesh(mesh);
    merlin::MaterialDescriptor material;
    material.parameters.base_color = {0, 0, 1, 1};
    material.double_sided = true;
    const auto material_handle = world.CreateMaterial(material);
    merlin::InstanceDescriptor instance;
    instance.mesh = mesh_handle;
    instance.material = material_handle;
    world.CreateInstance(instance);
    g.opacities[0] = 0.5F;
    world.UpdateGaussian(handle, g);
    request.snapshot = snapshot();
    const auto mixed = render();
    Require(Channel(mixed, 32, 32, 0) > 105 && Channel(mixed, 32, 32, 1) == 0 &&
                Channel(mixed, 32, 32, 2) > 120,
        "mesh/Gaussian depth composition failed");
    Require(std::abs(mixed.depth.pixels.at(32 * mixed.depth.row_pitch_bytes / 4 + 32) - 0.45F) < 0.001F,
        "Gaussian changed opaque depth");
    Require(Id(mixed, 32, 32) == 0, "front Gaussian lost its picking ID");
    g.visible = false;
    world.UpdateGaussian(handle, g);
    request.snapshot = snapshot();
    const auto hidden = render();
    Require(hidden.telemetry.gaussian_visible_count == 0 &&
                hidden.telemetry.gaussian_draw_count == 0 &&
                Channel(hidden, 32, 32, 0) == 0,
        "hidden Gaussian retained old stream");
    world.Remove(handle);
    request.snapshot = snapshot();
    Require(render().telemetry.gaussian_candidate_count == 0, "removed Gaussian remained");
    Require(backend->statistics().validation_messages == 0, "Metal validation failed");
    std::cout << "Metal Gaussian image, AOV, composition, cache and in-flight tests passed\n";
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
