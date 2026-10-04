// Real HgiVulkan targets: display stays on the GPU; Map returns exact current
// depth/IDs once per submission, including motion, resize and removal.
#include "adapter.hpp"

#include <pxr/imaging/hgi/hgi.h>
#include <pxr/imaging/hgi/tokens.h>

#include <merlin/core/render_world.hpp>
#include <merlin/extraction/scene_extractor.hpp>
#include <merlin/vulkan/backend.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {
void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}
}

int main(int argc, char** argv) try {
  Check(argc == 2, "requires shader directory");
  auto hgi = Hgi::CreatePlatformDefaultHgi();
  if (!hgi || !hgi->IsBackendSupported()) {
    std::cerr << "skip: HgiVulkan device unavailable\n";
    return 77;
  }
  auto bridge = std::make_shared<HdMerlinHgiVulkanBridge>(true);
  HdDriver driver{HgiTokens->renderDriver, VtValue(hgi.get())};
  bridge->SetDrivers({&driver});
  const auto borrowed = bridge->BorrowedContext();
  Check(borrowed.has_value(), "native HgiVulkan context unavailable");
  merlin::vulkan::BackendFactoryOptions options;
  options.renderer.borrowed_context = merlin::vulkan::BorrowedVulkanContext{
      borrowed->instance, borrowed->physical_device, borrowed->device,
      borrowed->graphics_queue, borrowed->graphics_queue_family,
      borrowed->graphics_queue_index, borrowed->timeline_semaphore_enabled,
      borrowed->validation_enabled, borrowed->debug_utils_enabled,
      borrowed->draw_indirect_first_instance_enabled};
  options.renderer.enable_async_transfer = false;
  options.renderer.descriptor_backend = merlin::vulkan::DescriptorBackendRequest::Conventional;
  const std::filesystem::path shaders = argv[1];
  options.shaders = {shaders / "triangle.vert.spv", shaders / "triangle.frag.spv",
      shaders / "triangle.bindless.vert.spv", shaders / "triangle.bindless.frag.spv",
      shaders / "environment.hdr"};
  merlin::vulkan::BackendFactory factory(std::move(options));
  merlin::render::BackendCreateInfo info;
  info.enable_validation = true;
  auto backend = std::shared_ptr<merlin::render::Backend>(factory.Create(info));
  auto* exporter = dynamic_cast<merlin::vulkan::AovImageExporter*>(backend.get());
  Check(exporter != nullptr, "missing Vulkan exporter");

  merlin::RenderWorld world;
  merlin::extraction::SceneExtractor extractor;
  merlin::MeshDescriptor triangle;
  triangle.positions = {{-0.5F, -0.5F, 0.2F}, {0.5F, 0.5F, 0.2F}, {-0.5F, 0.5F, 0.2F}};
  triangle.indices = {0, 1, 2};
  const auto mesh = world.CreateMesh(triangle);
  const auto material = world.CreateMaterial({});
  merlin::InstanceDescriptor instance;
  instance.mesh = mesh;
  instance.material = material;
  const auto handle = world.CreateInstance(instance);
  HdMerlinRenderBuffer color(SdfPath("/color"), bridge);
  HdMerlinRenderBuffer depth(SdfPath("/depth"), bridge);
  HdMerlinRenderBuffer prim(SdfPath("/primId"), bridge);
  HdMerlinRenderBuffer inst(SdfPath("/instanceId"), bridge);
  const std::array buffers{&color, &depth, &prim, &inst};
  const std::array aovs{merlin::Aov::Color, merlin::Aov::Depth,
      merlin::Aov::PrimId, merlin::Aov::InstanceId};
  const std::array formats{HdFormatUNorm8Vec4, HdFormatFloat32, HdFormatInt32, HdFormatInt32};
  const auto render = [&](int width, int height, bool allocate) {
    extractor.Apply(world, world.Commit());
    merlin::render::RenderRequest request;
    request.snapshot = extractor.snapshot();
    request.width = width;
    request.height = height;
    request.products.clear();
    for (std::size_t i = 0; i < buffers.size(); ++i) {
      if (allocate) {
        Check(buffers[i]->Allocate(GfVec3i(width, height, 1), formats[i], false), "allocation failed");
      }
      Check(buffers[i]->CanGpuCopyAov(aovs[i]), "AOV cannot use GPU copy");
      request.products.push_back({aovs[i], false});
    }
    const auto before = bridge->telemetry();
    const auto token = backend->Submit(request);
    for (std::size_t i = 0; i < buffers.size(); ++i) {
      Check(buffers[i]->CopyAov(exporter->AcquireAovImage(token, aovs[i]), backend), "AOV copy failed");
      Check(buffers[i]->GetResource(false).IsHolding<HgiTextureHandle>(), "missing Hgi target");
    }
    const auto gpu = backend->Resolve(token);
    Check(gpu.telemetry.readback_bytes == 0 && gpu.telemetry.cpu_readback_aov_count == 0,
        "GPU display performed backend readback");
    Check(bridge->telemetry().cpu_download_count == before.cpu_download_count,
        "GPU display downloaded an AOV");
    for (auto& product : request.products) {
      product.cpu_readback = true;
    }
    const auto reference = backend->Resolve(backend->Submit(request));
    const std::array<const void*, 3> expected{reference.depth.pixels.data(),
        reference.prim_id.pixels.data(), reference.instance_id.pixels.data()};
    Check(color.Map() == nullptr, "GPU color unexpectedly mapped");
    for (std::size_t i = 1; i < buffers.size(); ++i) {
      auto* buffer = buffers[i];
      const auto* pixels = buffer->Map();
      if (!pixels || std::memcmp(pixels, expected[i - 1], width * height * 4U) != 0) {
        std::cerr << "mismatch " << merlin::AovName(aovs[i]) << ' ' << width << 'x' << height << '\n';
        if (pixels) {
          const auto* actual = static_cast<const std::uint8_t*>(pixels);
          const auto* wanted = static_cast<const std::uint8_t*>(expected[i - 1]);
          for (int pixel = 0; pixel < width * height; ++pixel) {
            if (std::memcmp(actual + pixel * 4, wanted + pixel * 4, 4) != 0) {
              std::cerr << "first differing pixel " << pixel << '\n';
              break;
            }
          }
        }
        throw std::runtime_error("mapped AOV differs from current Tier 0 image");
      }
      Check(!buffer->CanGpuCopyAov(aovs[i]), "mapped AOV allowed GPU mutation");
      Check(!buffer->Allocate(GfVec3i(width + 1, height, 1), formats[i], false), "mapped AOV resized");
      buffer->Unmap();
      Check(buffer->Map() != nullptr, "repeat Map failed");
      buffer->Unmap();
    }
    const auto after = bridge->telemetry();
    Check(after.cpu_download_count == before.cpu_download_count + 3,
        "Map did not cache one download per AOV");
    Check(after.cpu_download_bytes == before.cpu_download_bytes + width * height * 12U,
        "Map byte accounting is incorrect");
    Check(after.coarse_wait_count == 0, "AOV mapping used a device-wide wait");
    Check(backend->statistics().validation_messages == 0, "renderer validation failed");
    return reference;
  };
  const auto first = render(64, 32, true);
  Check(std::any_of(first.depth.pixels.begin(), first.depth.pixels.end(),
      [](float value) { return value < 1.0F; }), "fixture has no visible triangle");
  instance.transform.values[12] = 0.25F;
  world.UpdateInstance(handle, instance, merlin::ChangeAspect::Transform);
  const auto moved = render(64, 32, false);
  Check(first.depth.pixels != moved.depth.pixels, "motion did not change depth image");
  (void)render(65, 33, true);
  world.Remove(handle);
  const auto removed = render(65, 33, false);
  Check(std::all_of(removed.prim_id.pixels.begin(), removed.prim_id.pixels.end(),
      [](auto id) { return id == UINT32_MAX; }), "removed prim left stale IDs");
  // Tier 0 writes replace the cached GPU version after a bridge fallback.
  Check(depth.WriteDepth(removed.depth.pixels, 65, 33), "Tier 0 depth recovery failed");
  Check(prim.WriteId(removed.prim_id.pixels, 65, 33), "Tier 0 ID recovery failed");
  Check(bridge->telemetry().target_orphans == 0, "Hgi target ownership failed");
  std::cout << "Exact current depth/IDs through Map, motion, resize, removal and Tier 0 recovery\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
