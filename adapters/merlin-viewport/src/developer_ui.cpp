#include "developer_ui.hpp"

#include "window.hpp"

#define GLFW_INCLUDE_NONE
#ifdef MERLIN_VIEWPORT_ENABLE_NATIVE_FILE_DIALOG
#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#endif
#endif
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#ifdef MERLIN_VIEWPORT_ENABLE_NATIVE_FILE_DIALOG
#include <nfd.h>
#include <nfd_glfw3.h>
#endif
#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
#include <imgui_impl_vulkan.h>
#endif
#ifdef MERLIN_VIEWPORT_ENABLE_METAL
#import <Metal/Metal.h>
#include <imgui_impl_metal.h>
#endif

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace merlin::viewport {
namespace {

#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
template <typename Handle>
Handle DecodeHandle(std::uintptr_t handle) noexcept {
  if constexpr (std::is_pointer_v<Handle>) {
    return reinterpret_cast<Handle>(handle);
  } else {
    return static_cast<Handle>(handle);
  }
}
#endif

#ifdef MERLIN_VIEWPORT_ENABLE_METAL
template <typename Handle>
Handle DecodeObjCHandle(std::uintptr_t handle) noexcept {
  return (__bridge Handle)(reinterpret_cast<void*>(handle));
}
#endif

#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
void CheckVulkanResult(VkResult result) {
  if (result < 0) {
    throw std::runtime_error("Dear ImGui Vulkan backend failed: " +
                             std::to_string(static_cast<int>(result)));
  }
}
#endif

void LabelValue(const char* label, const char* value) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  ImGui::TextUnformatted(value);
}

void LabelValue(const char* label, std::uint64_t value) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  ImGui::Text("%llu", static_cast<unsigned long long>(value));
}

template <typename... Values>
void LabelFormattedValue(const char* label, const char* format,
                         Values... values) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  ImGui::Text(format, values...);
}

void LabelBytes(const char* label, std::uint64_t value) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  const auto mib = static_cast<double>(value) / (1024.0 * 1024.0);
  if (mib >= 0.01) {
    ImGui::Text("%.2f MiB", mib);
  } else {
    ImGui::Text("%llu B", static_cast<unsigned long long>(value));
  }
}

void LabelMilliseconds(const char* label, std::uint64_t nanoseconds) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  ImGui::Text("%.3f ms", static_cast<double>(nanoseconds) / 1'000'000.0);
}

void LabelCountAndPercent(const char* label, std::uint64_t value,
                          std::uint64_t total) {
  ImGui::TableNextRow();
  ImGui::TableSetColumnIndex(0);
  ImGui::TextUnformatted(label);
  ImGui::TableSetColumnIndex(1);
  const auto percent = total == 0
                           ? 0.0
                           : static_cast<double>(value) * 100.0 /
                                 static_cast<double>(total);
  ImGui::Text("%llu (%.1f%%)", static_cast<unsigned long long>(value),
              percent);
}

template <typename DrawRows>
void TwoColumnTable(const char* id, DrawRows&& draw_rows) {
  if (ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Metric");
    ImGui::TableSetupColumn("Value");
    draw_rows();
    ImGui::EndTable();
  }
}

void DrawAovMask(const char* label, std::uint64_t mask) {
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  bool any{};
  constexpr std::array aovs{
      Aov::Color,     Aov::Depth,      Aov::Normal,     Aov::Albedo,
      Aov::Roughness, Aov::Metallic,   Aov::Emission,   Aov::PrimId,
      Aov::InstanceId, Aov::MotionVector};
  for (const auto aov : aovs) {
    const auto bit = std::uint64_t{1} << static_cast<std::uint32_t>(aov);
    if ((mask & bit) == 0) {
      continue;
    }
    if (any) {
      ImGui::SameLine(0.0F, 3.0F);
      ImGui::TextUnformatted(",");
      ImGui::SameLine(0.0F, 3.0F);
    }
    const auto name = AovName(aov);
    ImGui::TextUnformatted(name.data(), name.data() + name.size());
    any = true;
  }
  if (!any) {
    ImGui::TextUnformatted("none");
  }
}

constexpr std::size_t kTimingHistorySamples = 180;

struct TimingHistory {
  std::array<float, kTimingHistorySamples> host_ms{};
  std::array<float, kTimingHistorySamples> backend_ms{};
  std::array<float, kTimingHistorySamples> gpu_ms{};
  std::size_t count{};
  std::size_t next{};
  std::uint64_t last_frame_index{std::numeric_limits<std::uint64_t>::max()};

  void Record(const DeveloperUiSnapshot& snapshot) {
    if (snapshot.frame_index == last_frame_index) {
      return;
    }
    if (last_frame_index != std::numeric_limits<std::uint64_t>::max() &&
        snapshot.frame_index < last_frame_index) {
      *this = {};
    }
    last_frame_index = snapshot.frame_index;
    if (snapshot.host_frame_ns == 0 &&
        snapshot.timings.backend_total_ns == 0 &&
        snapshot.timings.gpu_execution_ns == 0) {
      return;
    }
    constexpr double nanoseconds_per_millisecond = 1'000'000.0;
    host_ms[next] = static_cast<float>(
        static_cast<double>(snapshot.host_frame_ns) /
        nanoseconds_per_millisecond);
    backend_ms[next] = static_cast<float>(
        static_cast<double>(snapshot.timings.backend_total_ns) /
        nanoseconds_per_millisecond);
    gpu_ms[next] = static_cast<float>(
        static_cast<double>(snapshot.timings.gpu_execution_ns) /
        nanoseconds_per_millisecond);
    next = (next + 1U) % kTimingHistorySamples;
    count = std::min(count + 1U, kTimingHistorySamples);
  }

  [[nodiscard]] std::size_t offset() const noexcept {
    return count == kTimingHistorySamples ? next : 0U;
  }
};

double Average(const std::array<float, kTimingHistorySamples>& values,
               std::size_t count) {
  double total{};
  for (std::size_t index = 0; index < count; ++index) {
    total += values[index];
  }
  return count == 0 ? 0.0 : total / static_cast<double>(count);
}

float Maximum(const std::array<float, kTimingHistorySamples>& values,
              std::size_t count) {
  return count == 0
             ? 0.0F
             : *std::max_element(values.begin(), values.begin() + count);
}

const char* SeverityName(DiagnosticSeverity severity) noexcept {
  switch (severity) {
    case DiagnosticSeverity::Info: return "info";
    case DiagnosticSeverity::Warning: return "warning";
    case DiagnosticSeverity::Error: return "error";
  }
  return "error";
}

std::string_view Utf8Filename(std::string_view path) noexcept {
  const auto separator = path.find_last_of("/\\");
  return separator == std::string_view::npos ? path
                                              : path.substr(separator + 1U);
}

class ImGuiDeveloperUi final : public DeveloperUi {
 public:
  explicit ImGuiDeveloperUi(Window& window) {
    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    ImGui::SetCurrentContext(context_);
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // The development tool may be launched with a source or asset directory
    // as its working directory. Do not leave imgui.ini artifacts there.
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    window_ = static_cast<GLFWwindow*>(window.native_window());
#ifdef MERLIN_VIEWPORT_ENABLE_NATIVE_FILE_DIALOG
    if (NFD_Init() == NFD_OKAY) {
      nfd_initialized_ = true;
      NFD_SetDisplayPropertiesFromGLFW();
    } else {
      const auto* error = NFD_GetError();
      file_dialog_error_ =
          error == nullptr ? "Could not initialize the native file dialog."
                           : error;
    }
#endif
  }

  ~ImGuiDeveloperUi() override {
    SetContext();
#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
    if (vulkan_initialized_) {
      ImGui_ImplVulkan_Shutdown();
    }
#endif
#ifdef MERLIN_VIEWPORT_ENABLE_METAL
    if (metal_initialized_) {
      ImGui_ImplMetal_Shutdown();
    }
#endif
    if (glfw_initialized_) {
      ImGui_ImplGlfw_Shutdown();
    }
    ImGui::DestroyContext(context_);
#ifdef MERLIN_VIEWPORT_ENABLE_NATIVE_FILE_DIALOG
    if (nfd_initialized_) {
      NFD_Quit();
    }
#endif
  }

#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
  void ConfigurePresentation(
      vulkan::PresentationOptions& presentation) override {
    presentation.overlay_user_data = this;
    presentation.render_overlay = &RenderOverlay;
  }
#endif

#ifdef MERLIN_VIEWPORT_ENABLE_METAL
  void ConfigurePresentation(
      metal::PresentationOptions& presentation) override {
    presentation.overlay_user_data = this;
    presentation.render_overlay = &RenderMetalOverlay;
  }
#endif

  DeveloperUiActions DrawFrame(
      const DeveloperUiSnapshot& snapshot) override {
    SetContext();
    actions_ = {};
    // The renderer initializes its UI backend when native presentation becomes
    // available. Skip only this bootstrap frame; starting an ImGui frame before
    // then leaves the font atlas without a renderer texture.
    bool renderer_initialized{};
#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
    renderer_initialized = vulkan_initialized_;
#endif
#ifdef MERLIN_VIEWPORT_ENABLE_METAL
    renderer_initialized = renderer_initialized || metal_initialized_;
#endif
    if (!renderer_initialized) {
      return actions_;
    }
#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
    if (vulkan_initialized_) {
      ImGui_ImplVulkan_NewFrame();
    }
#endif
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    timing_history_.Record(snapshot);
    DrawDiagnostics(snapshot);
    ImGui::Render();
    return actions_;
  }

  bool WantsKeyboard() const noexcept override {
    SetContext();
    return ImGui::GetIO().WantCaptureKeyboard;
  }

  bool WantsMouse() const noexcept override {
    SetContext();
    return ImGui::GetIO().WantCaptureMouse;
  }

 private:
  void EnsureGlfw(bool vulkan) {
    if (glfw_initialized_) {
      if (glfw_for_vulkan_ != vulkan) {
        throw std::runtime_error(
            "Dear ImGui GLFW backend cannot change renderer API");
      }
      return;
    }
    const bool initialized =
        vulkan ? ImGui_ImplGlfw_InitForVulkan(window_, true)
               : ImGui_ImplGlfw_InitForOther(window_, true);
    if (!initialized) {
      throw std::runtime_error(
          "could not initialize Dear ImGui GLFW backend");
    }
    glfw_initialized_ = true;
    glfw_for_vulkan_ = vulkan;
  }

#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
  static void RenderOverlay(
      void* user_data,
      const vulkan::PresentationOverlayContext& context) {
    static_cast<ImGuiDeveloperUi*>(user_data)->OnRenderOverlay(context);
  }

  void OnRenderOverlay(
      const vulkan::PresentationOverlayContext& context) {
    SetContext();
    switch (context.phase) {
      case vulkan::PresentationOverlayPhase::Initialize: {
        EnsureGlfw(true);
        if (vulkan_initialized_) {
          ImGui_ImplVulkan_Shutdown();
          vulkan_initialized_ = false;
        }
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = context.api_version;
        info.Instance = DecodeHandle<VkInstance>(context.instance);
        info.PhysicalDevice =
            DecodeHandle<VkPhysicalDevice>(context.physical_device);
        info.Device = DecodeHandle<VkDevice>(context.device);
        info.QueueFamily = context.queue_family;
        info.Queue = DecodeHandle<VkQueue>(context.queue);
        info.DescriptorPoolSize = 32;
        info.MinImageCount = context.image_count;
        info.ImageCount = context.image_count;
        info.PipelineInfoMain.RenderPass =
            DecodeHandle<VkRenderPass>(context.render_pass);
        info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        info.CheckVkResultFn = CheckVulkanResult;
        if (!ImGui_ImplVulkan_Init(&info)) {
          throw std::runtime_error(
              "could not initialize Dear ImGui Vulkan backend");
        }
        vulkan_initialized_ = true;
        break;
      }
      case vulkan::PresentationOverlayPhase::Render:
        if (vulkan_initialized_ && ImGui::GetDrawData() != nullptr) {
          ImGui_ImplVulkan_RenderDrawData(
              ImGui::GetDrawData(),
              DecodeHandle<VkCommandBuffer>(context.command_buffer));
        }
        break;
      case vulkan::PresentationOverlayPhase::Shutdown:
        if (vulkan_initialized_) {
          ImGui_ImplVulkan_Shutdown();
          vulkan_initialized_ = false;
        }
        break;
    }
  }
#endif

#ifdef MERLIN_VIEWPORT_ENABLE_METAL
  static void RenderMetalOverlay(
      void* user_data,
      const metal::PresentationOverlayContext& context) {
    static_cast<ImGuiDeveloperUi*>(user_data)->OnRenderMetalOverlay(context);
  }

  void OnRenderMetalOverlay(
      const metal::PresentationOverlayContext& context) {
    SetContext();
    switch (context.phase) {
      case metal::PresentationOverlayPhase::Initialize:
        EnsureGlfw(false);
        if (!ImGui_ImplMetal_Init(
                DecodeObjCHandle<id<MTLDevice>>(context.device))) {
          throw std::runtime_error(
              "could not initialize Dear ImGui Metal backend");
        }
        metal_initialized_ = true;
        break;
      case metal::PresentationOverlayPhase::Render:
        if (metal_initialized_ && ImGui::GetDrawData() != nullptr) {
          auto* pass = DecodeObjCHandle<MTLRenderPassDescriptor*>(
              context.render_pass_descriptor);
          // The Metal backend's NewFrame call records the current framebuffer
          // formats; platform input and ImGui draw-data construction already
          // happened in DrawFrame().
          ImGui_ImplMetal_NewFrame(pass);
          ImGui_ImplMetal_RenderDrawData(
              ImGui::GetDrawData(),
              DecodeObjCHandle<id<MTLCommandBuffer>>(context.command_buffer),
              DecodeObjCHandle<id<MTLRenderCommandEncoder>>(
                  context.render_encoder));
        }
        break;
      case metal::PresentationOverlayPhase::Shutdown:
        if (metal_initialized_) {
          ImGui_ImplMetal_Shutdown();
          metal_initialized_ = false;
        }
        break;
    }
  }
#endif

  void DrawDiagnostics(const DeveloperUiSnapshot& snapshot) {
    ImGui::SetNextWindowPos(ImVec2(12.0F, 12.0F), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(430.0F, 680.0F),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Merlin renderer diagnostics")) {
      ImGui::End();
      return;
    }

    ImGui::Text("Scene: %.*s", static_cast<int>(snapshot.scene_source.size()),
                snapshot.scene_source.data());
    if (!snapshot.scene_path.empty()) {
      const auto filename = Utf8Filename(snapshot.scene_path);
      ImGui::Text("Stage: %.*s", static_cast<int>(filename.size()),
                  filename.data());
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%.*s", static_cast<int>(snapshot.scene_path.size()),
                          snapshot.scene_path.data());
      }
    }
    ImGui::Text("Viewport: %u x %u", snapshot.width, snapshot.height);
    ImGui::Text("Frame: %llu",
                static_cast<unsigned long long>(snapshot.frame_index));
    if (ImGui::Button("Capture screenshot")) {
      actions_.capture_screenshot = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save benchmark")) {
      actions_.save_benchmark = true;
    }
#ifdef MERLIN_VIEWPORT_ENABLE_NATIVE_FILE_DIALOG
    if (snapshot.can_load_usd) {
      ImGui::SameLine();
      ImGui::BeginDisabled(!nfd_initialized_);
      if (ImGui::Button("Open USD...")) {
        OpenUsdStage();
      }
      ImGui::EndDisabled();
      if (!file_dialog_error_.empty()) {
        ImGui::TextDisabled("File dialog: %s", file_dialog_error_.c_str());
      }
    }
#endif

    if (snapshot.selection != nullptr &&
        ImGui::CollapsingHeader("Backend selection",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      const auto selected =
          render::BackendKindName(snapshot.selection->selected);
      const auto requested =
          render::BackendRequestName(snapshot.selection->requested);
      TwoColumnTable("backend-selection", [&] {
        LabelValue("Requested", requested.data());
        LabelValue("Selected", selected.data());
        LabelValue("Mode",
                   snapshot.selection->automatic ? "automatic" : "explicit");
      });
      ImGui::TextWrapped("Reason: %s", snapshot.selection->reason.c_str());
    }

    if (snapshot.capabilities != nullptr &&
        ImGui::CollapsingHeader("Capabilities",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      const auto& capabilities = *snapshot.capabilities;
      TwoColumnTable("capabilities", [&] {
        LabelValue("Backend", capabilities.backend_name.c_str());
        LabelValue("Device", capabilities.device_name.c_str());
        LabelValue("Bindless textures",
                   capabilities.bindless_textures ? "yes" : "fallback");
        LabelValue("Async upload",
                   capabilities.asynchronous_upload ? "yes" : "no");
        LabelValue("GPU timestamps",
                   capabilities.timestamp_queries ? "yes" : "no");
        LabelValue("Generated materials",
                   capabilities.generated_materials ? "yes" : "fallback");
        LabelValue("Validation",
                   capabilities.validation_enabled ? "enabled" : "disabled");
        LabelValue("External presentation",
                   capabilities.external_presentation ? "yes" : "no");
        LabelValue("CPU readback",
                   capabilities.cpu_readback ? "available" : "unavailable");
        LabelValue("Max image dimension",
                   capabilities.limits.max_image_dimension_2d);
        LabelValue("Frames in flight",
                   capabilities.limits.max_frames_in_flight);
        LabelValue("Texture slots",
                   capabilities.limits.sampled_image_slots);
        LabelValue("Sampler slots", capabilities.limits.sampler_slots);
      });
    }

    if (ImGui::CollapsingHeader("Frame timings",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      TwoColumnTable("frame-timings", [&] {
        LabelMilliseconds("Host frame", snapshot.host_frame_ns);
        LabelMilliseconds("Backend total",
                          snapshot.timings.backend_total_ns);
        LabelMilliseconds("GPU execution",
                          snapshot.timings.gpu_execution_ns);
        LabelMilliseconds("Uploads", snapshot.timings.upload_ns);
        LabelMilliseconds("Command recording",
                          snapshot.timings.command_recording_ns);
        LabelMilliseconds("Queue submission",
                          snapshot.timings.queue_submission_ns);
        LabelMilliseconds("Completion wait",
                          snapshot.timings.completion_wait_ns);
        LabelMilliseconds("Readback", snapshot.timings.readback_ns);
        LabelMilliseconds("Presentation",
                          snapshot.timings.presentation_ns);
      });
      if (timing_history_.count != 0) {
        ImGui::Text("Host history: avg %.3f ms, max %.3f ms",
                    Average(timing_history_.host_ms, timing_history_.count),
                    Maximum(timing_history_.host_ms, timing_history_.count));
        ImGui::PlotLines(
            "##host-frame-history", timing_history_.host_ms.data(),
            static_cast<int>(timing_history_.count),
            static_cast<int>(timing_history_.offset()), nullptr, 0.0F,
            std::numeric_limits<float>::max(), ImVec2(0.0F, 54.0F));
        ImGui::Text("Backend history: avg %.3f ms, max %.3f ms",
                    Average(timing_history_.backend_ms,
                            timing_history_.count),
                    Maximum(timing_history_.backend_ms,
                            timing_history_.count));
        ImGui::Text("GPU history: avg %.3f ms, max %.3f ms",
                    Average(timing_history_.gpu_ms, timing_history_.count),
                    Maximum(timing_history_.gpu_ms, timing_history_.count));
        ImGui::PlotLines(
            "##gpu-frame-history", timing_history_.gpu_ms.data(),
            static_cast<int>(timing_history_.count),
            static_cast<int>(timing_history_.offset()), nullptr, 0.0F,
            std::numeric_limits<float>::max(), ImVec2(0.0F, 54.0F));
      }
    }

    if (ImGui::CollapsingHeader("Scene and residency",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      TwoColumnTable("scene-residency", [&] {
        if (snapshot.scene.available) {
          LabelValue("Geometry", snapshot.scene.geometries);
          LabelValue("Gaussian resources", snapshot.scene.gaussians);
          LabelValue("Textures", snapshot.scene.textures);
          LabelValue("Samplers", snapshot.scene.samplers);
          LabelValue("Materials", snapshot.scene.materials);
          LabelValue("Instances", snapshot.scene.instances);
          LabelValue("Lights", snapshot.scene.lights);
        }
        LabelValue("Visible primitives",
                   snapshot.telemetry.visible_primitive_count);
        LabelValue("Draws", snapshot.telemetry.draw_count);
        LabelValue("Triangles", snapshot.telemetry.triangle_count);
      });
    }

    if (snapshot.camera.available &&
        ImGui::CollapsingHeader("Camera and navigation",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      const auto& camera = snapshot.camera;
      TwoColumnTable("camera-state", [&] {
        LabelValue("Controller", camera.controller.data());
        LabelValue("Projection",
                   camera.perspective ? "perspective" : "identity");
        LabelValue("Stage up axis", camera.up_axis.data());
        LabelFormattedValue("Position", "%.4f, %.4f, %.4f",
                            camera.position[0], camera.position[1],
                            camera.position[2]);
        LabelFormattedValue("Target", "%.4f, %.4f, %.4f", camera.target[0],
                            camera.target[1], camera.target[2]);
        LabelFormattedValue("Yaw", "%.2f deg", camera.yaw_degrees);
        LabelFormattedValue("Pitch", "%.2f deg", camera.pitch_degrees);
        LabelFormattedValue("Distance", "%.4f", camera.distance);
        LabelFormattedValue("Aspect", "%.4f", camera.aspect_ratio);
        if (camera.perspective) {
          LabelFormattedValue("Vertical FOV", "%.2f deg",
                              camera.vertical_fov_degrees);
          LabelFormattedValue("Near clip", "%.6g", camera.near_plane);
          LabelFormattedValue("Far clip", "%.6g", camera.far_plane);
        }
      });
      if (camera.controller == "orbit") {
        ImGui::TextWrapped(
            "Navigation: Alt+LMB tumble, Alt+MMB track, Alt+RMB dolly, "
            "wheel dolly, F frame all, arrows pan.");
      } else {
        ImGui::TextWrapped("Navigation: arrow keys pan the camera.");
      }
    }

    const bool has_gaussians =
        snapshot.gaussian.resources != 0 ||
        snapshot.telemetry.gaussian_candidate_count != 0 ||
        snapshot.telemetry.gaussian_upload_bytes != 0;
    if (has_gaussians &&
        ImGui::CollapsingHeader("Gaussian rendering",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      if (snapshot.gaussian.available) {
        TwoColumnTable("gaussian-source", [&] {
          LabelValue("Resources", snapshot.gaussian.resources);
          LabelValue("Particles", snapshot.gaussian.particles);
          LabelCountAndPercent("Visible resources",
                               snapshot.gaussian.visible_resources,
                               snapshot.gaussian.resources);
          LabelValue("SH degree 0",
                     snapshot.gaussian.spherical_harmonics_degree_resources[0]);
          LabelValue("SH degree 1",
                     snapshot.gaussian.spherical_harmonics_degree_resources[1]);
          LabelValue("SH degree 2",
                     snapshot.gaussian.spherical_harmonics_degree_resources[2]);
          LabelValue("SH degree 3",
                     snapshot.gaussian.spherical_harmonics_degree_resources[3]);
          LabelValue("Perspective projection",
                     snapshot.gaussian.perspective_resources);
          LabelValue("Tangential projection",
                     snapshot.gaussian.tangential_resources);
          LabelValue("Z-depth sorting", snapshot.gaussian.z_depth_resources);
          LabelValue("Camera-distance sorting",
                     snapshot.gaussian.camera_distance_resources);
        });
      }
      const auto candidates = snapshot.telemetry.gaussian_candidate_count;
      const auto cache_lookups =
          snapshot.telemetry.gaussian_preparation_cache_hits +
          snapshot.telemetry.gaussian_preparation_cache_misses;
      const auto rejected = snapshot.telemetry.gaussian_hidden_count +
                            snapshot.telemetry.gaussian_opacity_culled_count +
                            snapshot.telemetry.gaussian_frustum_culled_count +
                            snapshot.telemetry.gaussian_invalid_culled_count;
      TwoColumnTable("gaussian-frame", [&] {
        LabelValue("Candidates", candidates);
        LabelCountAndPercent("Visible", snapshot.telemetry.gaussian_visible_count,
                             candidates);
        LabelCountAndPercent("Rejected", rejected, candidates);
        LabelCountAndPercent("Hidden", snapshot.telemetry.gaussian_hidden_count,
                             candidates);
        LabelCountAndPercent(
            "Opacity culled", snapshot.telemetry.gaussian_opacity_culled_count,
            candidates);
        LabelCountAndPercent(
            "Frustum culled", snapshot.telemetry.gaussian_frustum_culled_count,
            candidates);
        LabelCountAndPercent(
            "Invalid culled", snapshot.telemetry.gaussian_invalid_culled_count,
            candidates);
        LabelValue("Sorted", snapshot.telemetry.gaussian_sorted_count);
        LabelCountAndPercent(
            "Preparation cache hits",
            snapshot.telemetry.gaussian_preparation_cache_hits, cache_lookups);
        LabelValue("Preparation cache misses",
                   snapshot.telemetry.gaussian_preparation_cache_misses);
        LabelValue("Sorting fallbacks",
                   snapshot.telemetry.gaussian_sorting_policy_fallback_count);
        LabelValue("Draws", snapshot.telemetry.gaussian_draw_count);
        LabelBytes("Upload", snapshot.telemetry.gaussian_upload_bytes);
      });
      if (snapshot.telemetry.gaussian_invalid_culled_count != 0) {
        ImGui::TextColored(ImVec4(1.0F, 0.55F, 0.25F, 1.0F),
                           "Invalid Gaussian payloads were culled.");
      }
      if (snapshot.telemetry.gaussian_sorting_policy_fallback_count != 0) {
        ImGui::TextColored(ImVec4(1.0F, 0.75F, 0.25F, 1.0F),
                           "Mixed sorting policies fell back to Z depth.");
      }
    }

    if (ImGui::CollapsingHeader("Frame resource activity")) {
      TwoColumnTable("frame-resource-activity", [&] {
        LabelBytes("Upload", snapshot.telemetry.upload_bytes);
        LabelBytes("Readback", snapshot.telemetry.readback_bytes);
        LabelBytes("Total uploaded", snapshot.statistics.uploaded_bytes);
        LabelValue("Allocations", snapshot.telemetry.allocation_count);
        LabelBytes("Buffer allocations",
                   snapshot.telemetry.buffer_allocation_bytes);
        LabelBytes("Image allocations",
                   snapshot.telemetry.image_allocation_bytes);
        LabelValue("Pipeline creations",
                   snapshot.telemetry.pipeline_creation_count);
        LabelValue("Geometry reconciles",
                   snapshot.telemetry.geometry_reconcile_count);
        LabelValue("Texture reconciles",
                   snapshot.telemetry.texture_reconcile_count);
        LabelValue("Sampler reconciles",
                   snapshot.telemetry.sampler_reconcile_count);
        LabelValue("Geometry cache misses",
                   snapshot.telemetry.geometry_cache_misses);
        LabelValue("Texture cache hits",
                   snapshot.telemetry.texture_cache_hits);
        LabelValue("Texture cache misses",
                   snapshot.telemetry.texture_cache_misses);
        LabelValue("Shader module cache misses",
                   snapshot.telemetry.shader_module_cache_misses);
        LabelValue("Descriptor pool creations",
                   snapshot.telemetry.descriptor_pool_creation_count);
        LabelValue("Descriptor allocations",
                   snapshot.telemetry.descriptor_allocation_count);
        LabelValue("Descriptor updates",
                   snapshot.telemetry.descriptor_update_count);
        LabelValue("Bindless image updates",
                   snapshot.telemetry
                       .bindless_sampled_image_descriptor_update_count);
        LabelValue("Bindless sampler updates",
                   snapshot.telemetry.bindless_sampler_descriptor_update_count);
        LabelValue("Waits", snapshot.telemetry.wait_count);
        LabelValue("Resolves", snapshot.telemetry.resolve_count);
        LabelValue("Maps", snapshot.telemetry.map_count);
      });
    }

    if (ImGui::CollapsingHeader("VRAM and persistent residency",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      const auto& residency = snapshot.statistics.residency;
      TwoColumnTable("persistent-residency", [&] {
        LabelValue("Memory budget extension",
                   residency.memory_budget_available ? "available"
                                                     : "unavailable");
        LabelBytes("Heap capacity",
                   residency.vram_heap_capacity_bytes);
        LabelBytes("Heap budget", residency.vram_heap_budget_bytes);
        LabelBytes("Driver heap usage",
                   residency.vram_heap_usage_bytes);
        LabelBytes("Heap available",
                   residency.vram_heap_available_bytes);
        LabelBytes("Renderer allocated",
                   residency.renderer_allocated_bytes);
        LabelBytes("Renderer peak",
                   residency.renderer_peak_allocated_bytes);
        LabelBytes("Effective limit",
                   residency.effective_vram_limit_bytes);
        LabelBytes("Geometry capacity",
                   residency.geometry_capacity_bytes);
        LabelBytes("Geometry resident",
                   residency.geometry_resident_bytes);
        LabelBytes("Geometry retiring",
                   residency.geometry_retiring_bytes);
        LabelValue("Geometry blocks", residency.geometry_blocks);
        LabelBytes("Upload ring capacity",
                   residency.upload_ring_capacity_bytes);
        LabelBytes("Upload ring in flight",
                   residency.upload_ring_in_flight_bytes);
        LabelValue("Bindless tables",
                   residency.bindless_tables ? "active" : "fallback");
        LabelValue("Texture slots in use",
                   residency.texture_slots_in_use);
        LabelValue("Texture slots retiring",
                   residency.texture_slots_retiring);
        LabelValue("Sampler slots in use",
                   residency.sampler_slots_in_use);
        LabelValue("Unique samplers",
                   residency.unique_sampler_count);
      });
    }

    if (ImGui::CollapsingHeader("AOV and presentation")) {
      DrawAovMask("Requested:", snapshot.telemetry.requested_aov_mask);
      DrawAovMask("Rendered:", snapshot.telemetry.rendered_aov_mask);
      DrawAovMask("CPU readback:",
                  snapshot.telemetry.cpu_readback_aov_mask);
      TwoColumnTable("presentation", [&] {
        LabelValue("Requested AOVs",
                   snapshot.telemetry.requested_aov_count);
        LabelValue("Rendered AOVs", snapshot.telemetry.rendered_aov_count);
        LabelValue("CPU readback AOVs",
                   snapshot.telemetry.cpu_readback_aov_count);
        LabelValue("AOV image exports",
                   snapshot.telemetry.aov_image_export_count);
        LabelValue("Active AOV leases",
                   snapshot.statistics.active_aov_image_leases);
        LabelValue("Frames presented",
                   snapshot.statistics.frames_presented);
        LabelValue("Swapchain recreates",
                   snapshot.statistics.presentation_recreates);
        LabelBytes("Presentation copies",
                   snapshot.statistics.presentation_copy_bytes);
        LabelBytes("CPU readback", snapshot.statistics.readback_bytes);
        LabelValue("Validation messages",
                   snapshot.statistics.validation_messages);
      });
    }

    if (ImGui::CollapsingHeader("Materials and diagnostics")) {
      TwoColumnTable("material-state", [&] {
        LabelValue("Generated draws",
                   snapshot.telemetry.generated_material_draw_count);
        LabelValue("Generated fallbacks",
                   snapshot.telemetry.generated_material_fallback_count);
        LabelValue("Recorded diagnostics",
                   snapshot.telemetry.material_fallbacks.recorded_count);
        LabelValue("Simplifications",
                   snapshot.telemetry.material_fallbacks.simplification_count);
        LabelValue("Basic material fallbacks",
                   snapshot.telemetry.material_fallbacks.basic_material_count);
        LabelValue("Error material fallbacks",
                   snapshot.telemetry.material_fallbacks.error_material_count);
        LabelValue(
            "Effective fallback",
            MaterialFallbackName(
                snapshot.telemetry.material_fallbacks.effective_fallback)
                .data());
      });
      if (snapshot.material_diagnostics == nullptr ||
          snapshot.material_diagnostics->empty()) {
        ImGui::TextDisabled("No material diagnostics in the latest frame.");
      } else {
        int diagnostic_index{};
        for (const auto& diagnostic : *snapshot.material_diagnostics) {
          ImGui::PushID(diagnostic_index++);
          const auto code = MaterialDiagnosticCode(diagnostic.category);
          if (ImGui::TreeNodeEx(
                  "diagnostic", ImGuiTreeNodeFlags_SpanAvailWidth,
                  "%s | %.*s | %s", SeverityName(diagnostic.severity),
                  static_cast<int>(code.size()), code.data(),
                  MaterialFallbackName(diagnostic.fallback).data())) {
            ImGui::TextWrapped("%s", diagnostic.message.c_str());
            const auto& context = diagnostic.context;
            TwoColumnTable("material-diagnostic-context", [&] {
              if (!context.material_identity.empty()) {
                LabelValue("Material", context.material_identity.c_str());
              }
              if (!context.element_path.empty()) {
                LabelValue("Element", context.element_path.c_str());
              }
              if (!context.node_category.empty()) {
                LabelValue("Node", context.node_category.c_str());
              }
              if (!context.input_name.empty()) {
                LabelValue("Input", context.input_name.c_str());
              }
              if (!context.source_document.empty()) {
                LabelValue("Document", context.source_document.c_str());
              }
              if (!context.backend_target.empty()) {
                LabelValue("Target", context.backend_target.c_str());
              }
              if (!context.generator_version.empty()) {
                LabelValue("Generator", context.generator_version.c_str());
              }
              if (!context.compiler_version.empty()) {
                LabelValue("Compiler", context.compiler_version.c_str());
              }
            });
            ImGui::TreePop();
          }
          ImGui::PopID();
        }
      }
    }
    ImGui::End();
  }

#ifdef MERLIN_VIEWPORT_ENABLE_NATIVE_FILE_DIALOG
  void OpenUsdStage() {
    file_dialog_error_.clear();
    if (!nfd_initialized_) {
      file_dialog_error_ = "Native file dialog is unavailable.";
      return;
    }

    const nfdu8filteritem_t filters[] = {
        {"OpenUSD stage", "usd,usda,usdc,usdz"},
    };
    nfdopendialogu8args_t arguments{};
    arguments.filterList = filters;
    arguments.filterCount =
        static_cast<nfdfiltersize_t>(std::size(filters));
    NFD_GetNativeWindowFromGLFWWindow(window_, &arguments.parentWindow);

    nfdu8char_t* selected_path{};
    const auto result = NFD_OpenDialogU8_With(&selected_path, &arguments);
    if (result == NFD_OKAY) {
      actions_.load_usd = std::filesystem::path(
          std::u8string(reinterpret_cast<const char8_t*>(selected_path)));
      NFD_FreePathU8(selected_path);
    } else if (result == NFD_ERROR) {
      const auto* error = NFD_GetError();
      file_dialog_error_ =
          error == nullptr ? "Could not open the native file dialog." : error;
    }
  }
#endif

  void SetContext() const noexcept { ImGui::SetCurrentContext(context_); }

  ImGuiContext* context_{};
  GLFWwindow* window_{};
  bool glfw_initialized_{};
  bool glfw_for_vulkan_{};
#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
  bool vulkan_initialized_{};
#endif
#ifdef MERLIN_VIEWPORT_ENABLE_METAL
  bool metal_initialized_{};
#endif
  DeveloperUiActions actions_;
  TimingHistory timing_history_;
#ifdef MERLIN_VIEWPORT_ENABLE_NATIVE_FILE_DIALOG
  bool nfd_initialized_{};
  std::string file_dialog_error_;
#endif
};

}  // namespace

std::unique_ptr<DeveloperUi> DeveloperUi::Create(Window& window) {
  return std::make_unique<ImGuiDeveloperUi>(window);
}

}  // namespace merlin::viewport
