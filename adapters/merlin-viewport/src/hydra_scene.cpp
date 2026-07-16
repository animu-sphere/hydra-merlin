#include "hydra_scene.hpp"

#include "adapter.hpp"
#include "presentation_glfw.hpp"
#include "window.hpp"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/engine.h>
#include <pxr/imaging/hd/renderIndex.h>
#include <pxr/imaging/hd/renderPass.h>
#include <pxr/imaging/hd/renderPassState.h>
#include <pxr/imaging/hd/repr.h>
#include <pxr/imaging/hd/task.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/bboxCache.h>
#include <pxr/usd/usdGeom/metrics.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usdImaging/usdImaging/delegate.h>

#include <merlin/vulkan/backend.hpp>
#include <merlin/vulkan/shader_abi.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace merlin::viewport {
namespace {

using Clock = std::chrono::steady_clock;

class ViewportRenderPassState final : public HdRenderPassState {
 public:
  GfMatrix4d GetWorldToViewMatrix() const override { return view_; }
  GfMatrix4d GetProjectionMatrix() const override { return projection_; }

  void Pan(double x, double y) {
    view_[3][0] += x * scene_radius_;
    view_[3][1] += y * scene_radius_;
  }

  void FrameStage(const UsdStageRefPtr& stage, std::uint32_t width,
                  std::uint32_t height) {
    const TfTokenVector purposes{UsdGeomTokens->default_,
                                 UsdGeomTokens->proxy,
                                 UsdGeomTokens->render};
    UsdGeomBBoxCache bounds(UsdTimeCode::Default(), purposes, true);
    const auto range =
        bounds.ComputeWorldBound(stage->GetPseudoRoot()).ComputeAlignedRange();
    if (range.IsEmpty()) {
      throw std::runtime_error(
          "USD stage has no renderable bounds for viewport framing");
    }

    center_ = (range.GetMin() + range.GetMax()) * 0.5;
    scene_radius_ = range.GetSize().GetLength() * 0.5;
    if (!std::isfinite(scene_radius_) || scene_radius_ <= 0.0) {
      scene_radius_ = 1.0;
    }

    const bool z_up = UsdGeomGetStageUpAxis(stage) == UsdGeomTokens->z;
    const GfVec3d up = z_up ? GfVec3d(0.0, 0.0, 1.0)
                             : GfVec3d(0.0, 1.0, 0.0);
    GfVec3d direction = z_up ? GfVec3d(1.0, -1.0, 0.65)
                              : GfVec3d(1.0, 0.65, 1.0);
    direction.Normalize();

    const auto aspect = static_cast<double>(width) /
                        static_cast<double>(std::max(1U, height));
    constexpr double vertical_fov_degrees = 45.0;
    const auto vertical_half =
        vertical_fov_degrees * std::numbers::pi / 360.0;
    const auto horizontal_half = std::atan(std::tan(vertical_half) * aspect);
    const auto fitting_half = std::min(vertical_half, horizontal_half);
    const auto distance =
        scene_radius_ / std::sin(fitting_half) * 1.1;
    const auto eye = center_ + direction * distance;
    view_.SetLookAt(eye, center_, up);
    near_ = std::max(scene_radius_ * 0.001,
                     distance - scene_radius_ * 1.25);
    far_ = distance + scene_radius_ * 1.25;
    UpdateProjection(width, height);
  }

  void Resize(std::uint32_t width, std::uint32_t height) {
    UpdateProjection(width, height);
  }

  const GfVec3d& center() const noexcept { return center_; }
  double radius() const noexcept { return scene_radius_; }

 private:
  void UpdateProjection(std::uint32_t width, std::uint32_t height) {
    const auto aspect = static_cast<double>(width) /
                        static_cast<double>(std::max(1U, height));
    GfFrustum frustum;
    frustum.SetPerspective(45.0, aspect, near_, far_);

    // Gf uses OpenGL depth in [-1, 1]. Convert it to Vulkan [0, 1] while the
    // matrix is still in Gf row-vector form. Do not flip Y here: Merlin's
    // positive-height viewport and clockwise front-face policy already account
    // for the framebuffer Y direction.
    GfMatrix4d to_vulkan(1.0);
    to_vulkan[2][2] = 0.5;
    to_vulkan[3][2] = 0.5;
    projection_ = frustum.ComputeProjectionMatrix() * to_vulkan;
  }

  GfMatrix4d view_{1.0};
  GfMatrix4d projection_{1.0};
  GfVec3d center_{0.0};
  double scene_radius_{1.0};
  double near_{0.01};
  double far_{1000.0};
};

class ViewportTask final : public HdTask {
 public:
  ViewportTask(HdRenderPassSharedPtr pass,
               std::shared_ptr<ViewportRenderPassState> state,
               HdResourceRegistrySharedPtr resources)
      : HdTask(SdfPath("/__merlinViewportTask")),
        pass_(std::move(pass)),
        state_(std::move(state)),
        resources_(std::move(resources)) {}

  void Sync(HdSceneDelegate*, HdTaskContext*, HdDirtyBits* dirty_bits) override {
    pass_->Sync();
    *dirty_bits = HdChangeTracker::Clean;
  }

  void Prepare(HdTaskContext*, HdRenderIndex*) override {
    state_->Prepare(resources_);
  }

  void Execute(HdTaskContext*) override { pass_->Execute(state_, render_tags_); }

  const TfTokenVector& GetRenderTags() const override { return render_tags_; }

 private:
  HdRenderPassSharedPtr pass_;
  std::shared_ptr<ViewportRenderPassState> state_;
  HdResourceRegistrySharedPtr resources_;
  TfTokenVector render_tags_{HdRenderTagTokens->geometry};
};

void WritePpm(const std::filesystem::path& path, std::uint32_t width,
              std::uint32_t height, const std::vector<std::uint8_t>& rgba) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("could not create Hydra screenshot: " +
                             path.string());
  }
  stream << "P6\n" << width << ' ' << height << "\n255\n";
  for (std::size_t offset = 0; offset < rgba.size(); offset += 4) {
    stream.write(reinterpret_cast<const char*>(rgba.data() + offset), 3);
  }
}

bool HasVisibleColor(const std::vector<std::uint8_t>& rgba) {
  constexpr std::array<int, 3> background{5, 6, 11};
  for (std::size_t offset = 0; offset + 3 < rgba.size(); offset += 4) {
    for (std::size_t channel = 0; channel < background.size(); ++channel) {
      if (std::abs(static_cast<int>(rgba[offset + channel]) -
                   background[channel]) > 1) {
        return true;
      }
    }
  }
  return false;
}

void WriteBenchmark(const std::filesystem::path& path,
                    const render::BackendSelection& selection,
                    const render::RendererStatistics& statistics,
                    std::uint64_t frames, std::uint64_t elapsed_ns) {
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path());
  }
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("could not create Hydra viewport benchmark: " +
                             path.string());
  }
  stream << "{\n"
         << "  \"schema\": \"merlin.viewport-benchmark/v1\",\n"
         << "  \"scene_source\": \"hydra\",\n"
         << "  \"backend\": \"" << render::BackendKindName(selection.selected)
         << "\",\n"
         << "  \"frames\": " << frames << ",\n"
         << "  \"cpu_total_ns\": " << elapsed_ns << ",\n"
         << "  \"cpu_average_frame_ns\": "
         << (frames == 0 ? 0 : elapsed_ns / frames) << ",\n"
         << "  \"readback_bytes\": " << statistics.readback_bytes << ",\n"
         << "  \"presentation_copy_bytes\": "
         << statistics.presentation_copy_bytes << ",\n"
         << "  \"frames_presented\": " << statistics.frames_presented
         << ",\n"
         << "  \"presentation_recreates\": "
         << statistics.presentation_recreates << ",\n"
         << "  \"validation_messages\": "
         << statistics.validation_messages << "\n"
         << "}\n";
}

std::shared_ptr<render::Backend> CreateRenderer(
    const HydraViewportOptions& options, Window& window,
    render::BackendSelection& selection) {
  const auto executable_dir =
      std::filesystem::absolute(options.executable).parent_path();
  const auto shader_dir =
      executable_dir / vulkan::shader_abi::ArtifactDirectory();
  vulkan::BackendFactoryOptions vulkan_options;
  vulkan_options.renderer.presentation =
      MakeGlfwVulkanPresentation(window, options.vsync);
  vulkan_options.shaders = {
      shader_dir / "triangle.vert.spv", shader_dir / "triangle.frag.spv",
      shader_dir / "triangle.bindless.vert.spv",
      shader_dir / "triangle.bindless.frag.spv"};
  vulkan::BackendFactory vulkan_factory(std::move(vulkan_options));
  std::vector<render::BackendFactory*> factories{&vulkan_factory};
  render::BackendCreateInfo create_info;
  create_info.backend = options.backend;
  create_info.enable_validation = options.validation;
  return std::shared_ptr<render::Backend>(
      render::CreateBackend(create_info, factories, &selection));
}

HdRenderPassAovBinding MakeBinding(const TfToken& name,
                                   HdRenderBuffer* buffer) {
  HdRenderPassAovBinding result;
  result.aovName = name;
  result.renderBuffer = buffer;
  return result;
}

}  // namespace

int RunHydraViewport(const HydraViewportOptions& options) {
  const auto stage = UsdStage::Open(options.stage.string());
  if (!stage) {
    throw std::runtime_error("could not open USD stage: " +
                             options.stage.string());
  }
  auto window = Window::Create("merlin-viewport | Hydra", options.width,
                               options.height, options.visible);
  render::BackendSelection selection;
  auto backend = CreateRenderer(options, *window, selection);
  const auto presentation = backend->default_presentation_target();
  if (!presentation) {
    throw std::runtime_error(
        "selected Hydra viewport backend has no presentation target");
  }

  HdMerlinRenderDelegate render_delegate(backend);
  std::unique_ptr<HdRenderIndex> render_index(
      HdRenderIndex::New(&render_delegate, HdDriverVector{}));
  if (!render_index) {
    throw std::runtime_error("could not create Hydra render index");
  }
  UsdImagingDelegate scene_delegate(render_index.get(),
                                    SdfPath::AbsoluteRootPath());
  scene_delegate.Populate(stage->GetPseudoRoot());
  scene_delegate.SetTime(UsdTimeCode::Default());

  HdRprimCollection collection(HdTokens->geometry,
                               HdReprSelector(HdReprTokens->smoothHull));
  auto pass = render_delegate.CreateRenderPass(render_index.get(), collection);
  auto state = std::make_shared<ViewportRenderPassState>();
  auto task = std::make_shared<ViewportTask>(
      pass, state, render_delegate.GetResourceRegistry());
  HdTaskSharedPtrVector tasks{task};
  HdEngine engine;

  std::uint32_t width = window->width();
  std::uint32_t height = window->height();
  state->FrameStage(stage, width, height);
  state->SetViewport(GfVec4d(0.0, 0.0, width, height));
  bool running = true;
  bool screenshot_pending = !options.screenshot.empty();
  bool reference_pending = options.reference_check;
  bool reference_checked{};
  bool readback_requested = screenshot_pending || reference_pending;
  std::optional<std::pair<std::int32_t, std::int32_t>> pick;
  bool resized_for_test{};
  std::uint64_t frames{};
  const auto start = Clock::now();

  std::cout << "USD stage: " << options.stage.string() << '\n'
            << "Scene source: OpenUSD through Hydra\n"
            << "Framed bounds: center=" << state->center()[0] << ','
            << state->center()[1] << ',' << state->center()[2]
            << " radius=" << state->radius() << '\n';
  while (running &&
         (options.frame_limit == 0 || frames < options.frame_limit)) {
    Event event;
    while (window->PollEvent(event)) {
      switch (event.type) {
        case EventType::Close:
          running = false;
          break;
        case EventType::Resize:
          width = event.width;
          height = event.height;
          if (width != 0 && height != 0) {
            state->SetViewport(GfVec4d(0.0, 0.0, width, height));
            state->Resize(width, height);
            backend->ResizePresentationTarget(*presentation, width, height);
          }
          break;
        case EventType::KeyDown:
          if (event.key == Key::Escape) {
            running = false;
          } else if (event.key == Key::Screenshot) {
            screenshot_pending = true;
            readback_requested = true;
          } else if (event.key == Key::Left) {
            state->Pan(-0.05, 0.0);
          } else if (event.key == Key::Right) {
            state->Pan(0.05, 0.0);
          } else if (event.key == Key::Up) {
            state->Pan(0.0, 0.05);
          } else if (event.key == Key::Down) {
            state->Pan(0.0, -0.05);
          }
          break;
        case EventType::PointerDown:
          pick = std::pair{event.x, event.y};
          readback_requested = true;
          break;
        case EventType::PointerMove:
          break;
      }
    }
    if (!running) {
      break;
    }
    if (width == 0 || height == 0) {
      window->WaitForEvent();
      continue;
    }

    std::unique_ptr<HdMerlinRenderBuffer> color;
    std::unique_ptr<HdMerlinRenderBuffer> prim_id;
    std::unique_ptr<HdMerlinRenderBuffer> instance_id;
    HdRenderPassAovBindingVector bindings;
    const GfVec3i dimensions(static_cast<int>(width),
                             static_cast<int>(height), 1);
    if (screenshot_pending || reference_pending) {
      color = std::make_unique<HdMerlinRenderBuffer>(
          SdfPath("/__merlinViewportColor"));
      if (!color->Allocate(dimensions, HdFormatUNorm8Vec4, false)) {
        throw std::runtime_error("could not allocate viewport color capture");
      }
      bindings.push_back(MakeBinding(HdAovTokens->color, color.get()));
    }
    if (pick) {
      prim_id = std::make_unique<HdMerlinRenderBuffer>(
          SdfPath("/__merlinViewportPrimId"));
      instance_id = std::make_unique<HdMerlinRenderBuffer>(
          SdfPath("/__merlinViewportInstanceId"));
      if (!prim_id->Allocate(dimensions, HdFormatInt32, false) ||
          !instance_id->Allocate(dimensions, HdFormatInt32, false)) {
        throw std::runtime_error("could not allocate viewport picking AOVs");
      }
      bindings.push_back(MakeBinding(HdAovTokens->primId, prim_id.get()));
      bindings.push_back(
          MakeBinding(HdAovTokens->instanceId, instance_id.get()));
    }
    state->SetAovBindings(bindings);
    engine.Execute(render_index.get(), &tasks);
    ++frames;

    if (color) {
      const auto bytes = static_cast<std::size_t>(width) * height * 4U;
      const auto* mapped = static_cast<const std::uint8_t*>(color->Map());
      if (mapped == nullptr) {
        throw std::runtime_error("could not map viewport color capture");
      }
      std::vector<std::uint8_t> pixels(mapped, mapped + bytes);
      color->Unmap();
      if (reference_pending) {
        if (!HasVisibleColor(pixels)) {
          throw std::runtime_error(
              "Hydra viewport frame contains only the clear color");
        }
        reference_pending = false;
        reference_checked = true;
      }
      if (screenshot_pending) {
        const auto path = options.screenshot.empty()
                              ? std::filesystem::path("merlin-viewport.ppm")
                              : options.screenshot;
        WritePpm(path, width, height, pixels);
        std::cout << "Screenshot: " << path.string() << '\n';
        screenshot_pending = false;
      }
    }
    if (prim_id && instance_id && pick) {
      const auto x = std::clamp(pick->first, 0,
                                static_cast<std::int32_t>(width) - 1);
      const auto y = std::clamp(pick->second, 0,
                                static_cast<std::int32_t>(height) - 1);
      const auto index = static_cast<std::size_t>(y) * width + x;
      const auto* prim = static_cast<const std::uint32_t*>(prim_id->Map());
      const auto* instance =
          static_cast<const std::uint32_t*>(instance_id->Map());
      if (prim == nullptr || instance == nullptr) {
        throw std::runtime_error("could not map viewport picking AOVs");
      }
      std::cout << "Pick " << x << ',' << y << ": primId=" << prim[index]
                << " instanceId=" << instance[index] << '\n';
      prim_id->Unmap();
      instance_id->Unmap();
      pick.reset();
    }
    if (options.resize_test && !resized_for_test && frames == 2) {
      window->SetSize(width + 64U, height + 32U);
      resized_for_test = true;
    }
  }

  const auto elapsed_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start)
          .count());
  const auto statistics = backend->statistics();
  if (!options.benchmark.empty()) {
    WriteBenchmark(options.benchmark, selection, statistics, frames,
                   elapsed_ns);
  }
  if (options.resize_test && statistics.presentation_recreates == 0) {
    throw std::runtime_error("Hydra viewport resize did not recreate swapchain");
  }
  if (statistics.validation_messages != 0) {
    throw std::runtime_error("Vulkan validation reported Hydra viewport messages");
  }
  if (options.reference_check && !reference_checked) {
    throw std::runtime_error("Hydra viewport content check did not execute");
  }
  if (!readback_requested && statistics.readback_bytes != 0) {
    throw std::runtime_error("Hydra viewport performed an unexpected CPU readback");
  }
  if (statistics.frames_presented != frames) {
    throw std::runtime_error("Hydra viewport did not present every rendered frame");
  }
  return 0;
}

}  // namespace merlin::viewport
