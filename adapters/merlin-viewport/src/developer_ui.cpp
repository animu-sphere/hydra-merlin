#include "developer_ui.hpp"

#include "window.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#ifdef MERLIN_VIEWPORT_ENABLE_VULKAN
#include <imgui_impl_vulkan.h>
#endif
#ifdef MERLIN_VIEWPORT_ENABLE_METAL
#import <Metal/Metal.h>
#include <imgui_impl_metal.h>
#endif

#include <array>
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
        LabelMilliseconds("Presentation",
                          snapshot.timings.presentation_ns);
      });
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
        LabelValue("Gaussian candidates",
                   snapshot.telemetry.gaussian_candidate_count);
        LabelValue("Gaussian visible",
                   snapshot.telemetry.gaussian_visible_count);
        LabelValue("Gaussian sorted",
                   snapshot.telemetry.gaussian_sorted_count);
        LabelValue("Gaussian hidden",
                   snapshot.telemetry.gaussian_hidden_count);
        LabelValue("Gaussian opacity culled",
                   snapshot.telemetry.gaussian_opacity_culled_count);
        LabelValue("Gaussian frustum culled",
                   snapshot.telemetry.gaussian_frustum_culled_count);
        LabelValue("Gaussian invalid culled",
                   snapshot.telemetry.gaussian_invalid_culled_count);
        LabelValue("Gaussian sorting fallbacks",
                   snapshot.telemetry.gaussian_sorting_policy_fallback_count);
        LabelValue("Gaussian preparation cache hits",
                   snapshot.telemetry.gaussian_preparation_cache_hits);
        LabelValue("Gaussian preparation cache misses",
                   snapshot.telemetry.gaussian_preparation_cache_misses);
        LabelBytes("Frame upload", snapshot.telemetry.upload_bytes);
        LabelBytes("Buffer allocations",
                   snapshot.telemetry.buffer_allocation_bytes);
        LabelBytes("Image allocations",
                   snapshot.telemetry.image_allocation_bytes);
        LabelBytes("Total uploaded", snapshot.statistics.uploaded_bytes);
        LabelValue("Descriptor updates",
                   snapshot.telemetry.descriptor_update_count);
        LabelValue("Geometry cache misses",
                   snapshot.telemetry.geometry_cache_misses);
        LabelValue("Texture cache hits",
                   snapshot.telemetry.texture_cache_hits);
        LabelValue("Texture cache misses",
                   snapshot.telemetry.texture_cache_misses);
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
        for (const auto& diagnostic : *snapshot.material_diagnostics) {
          const auto code = MaterialDiagnosticCode(diagnostic.category);
          ImGui::BulletText(
              "%.*s: %s", static_cast<int>(code.size()), code.data(),
              diagnostic.message.c_str());
        }
      }
    }
    ImGui::End();
  }

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
};

}  // namespace

std::unique_ptr<DeveloperUi> DeveloperUi::Create(Window& window) {
  return std::make_unique<ImGuiDeveloperUi>(window);
}

}  // namespace merlin::viewport
