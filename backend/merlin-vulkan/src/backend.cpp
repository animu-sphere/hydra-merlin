#include <merlin/vulkan/backend.hpp>

#include <atomic>
#include <map>
#include <utility>

namespace merlin::vulkan {
namespace {

std::atomic<std::uint64_t> g_backend_owner{1};

render::RendererErrorCode ConvertErrorCode(RendererErrorCode code) noexcept {
  switch (code) {
    case RendererErrorCode::InvalidRequest:
      return render::RendererErrorCode::InvalidRequest;
    case RendererErrorCode::InvalidToken:
      return render::RendererErrorCode::InvalidToken;
    case RendererErrorCode::ResourceBusy:
      return render::RendererErrorCode::ResourceBusy;
    case RendererErrorCode::Timeout:
      return render::RendererErrorCode::Timeout;
    case RendererErrorCode::DeviceLost:
      return render::RendererErrorCode::DeviceLost;
    case RendererErrorCode::Unsupported:
      return render::RendererErrorCode::Unsupported;
    case RendererErrorCode::BackendFailure:
      return render::RendererErrorCode::BackendFailure;
    case RendererErrorCode::ResourceExhausted:
      return render::RendererErrorCode::ResourceExhausted;
  }
  return render::RendererErrorCode::BackendFailure;
}

[[noreturn]] void Rethrow(const RendererError& error) {
  throw render::RendererError(ConvertErrorCode(error.code()),
                              error.operation(), error.detail(),
                              error.native_code());
}

class VulkanBackend final : public render::Backend {
 public:
  VulkanBackend(RendererOptions options, ShaderPaths shaders)
      : renderer_(std::move(options)),
        shaders_(std::move(shaders)),
        owner_(g_backend_owner.fetch_add(1, std::memory_order_relaxed)) {
    const auto& source = renderer_.capabilities();
    capabilities_.backend = render::BackendKind::Vulkan;
    capabilities_.backend_name = "vulkan";
    capabilities_.device_name = source.device_name;
    capabilities_.bindless_textures =
        source.descriptor_indexing_selection.selected_backend ==
        DescriptorBackend::Bindless;
    capabilities_.asynchronous_upload = source.async_transfer_queue;
    capabilities_.timestamp_queries = source.timestamp_queries;
    capabilities_.external_presentation = source.external_presentation;
    capabilities_.cpu_readback = true;
    capabilities_.validation_enabled = source.validation_enabled;
    capabilities_.limits.max_image_dimension_2d =
        source.max_image_dimension_2d;
    capabilities_.limits.max_frames_in_flight = 8;
    capabilities_.limits.sampled_image_slots =
        source.descriptor_indexing_selection.texture_capacity;
    capabilities_.limits.sampler_slots =
        source.descriptor_indexing_selection.sampler_capacity;
    if (source.external_presentation) {
      presentation_ = render::PresentationTarget(owner_, 1);
    }
  }

  const render::RendererCapabilities& capabilities() const noexcept override {
    return capabilities_;
  }

  render::RendererStatistics statistics() const noexcept override {
    const auto source = renderer_.statistics();
    render::RendererStatistics result;
    result.frames_submitted = source.frames_submitted;
    result.frames_presented = source.frames_presented;
    result.presentation_recreates = source.swapchain_recreates;
    result.validation_messages = source.validation_messages;
    result.uploaded_bytes = source.transfer_queue.uploaded_bytes;
    result.readback_bytes = readback_bytes_;
    result.presentation_copy_bytes = presentation_copy_bytes_;
    return result;
  }

  std::optional<render::PresentationTarget>
  default_presentation_target() const noexcept override {
    return presentation_;
  }

  // Validation only: the Vulkan swapchain recreates lazily from the extent of
  // the next presented RenderRequest, so hosts must submit the new size.
  void ResizePresentationTarget(render::PresentationTarget target,
                                std::uint32_t width,
                                std::uint32_t height) override {
    ValidatePresentation(target);
    if (width == 0 || height == 0 ||
        width > capabilities_.limits.max_image_dimension_2d ||
        height > capabilities_.limits.max_image_dimension_2d) {
      throw render::RendererError(render::RendererErrorCode::Unsupported,
                                  "resize presentation target",
                                  "requested extent is unsupported");
    }
  }

  render::CompletionToken Submit(
      const render::RenderRequest& request) override {
    if (request.presentation) {
      ValidatePresentation(request.presentation);
    }
    RenderRequest native;
    native.snapshot = request.snapshot;
    native.width = request.width;
    native.height = request.height;
    native.shaders = shaders_;
    native.clear_color = request.clear_color;
    native.products.clear();
    native.products.reserve(request.products.size());
    for (const auto& product : request.products) {
      native.products.push_back({product.aov, product.cpu_readback});
    }
    native.present = static_cast<bool>(request.presentation);
    try {
      const auto token = renderer_.Submit(native);
      tokens_.emplace(token.value(), token);
      return render::CompletionToken(owner_, token.value());
    } catch (const RendererError& error) {
      Rethrow(error);
    }
  }

  bool IsComplete(render::CompletionToken token) const override {
    try {
      return renderer_.IsComplete(FindToken(token));
    } catch (const RendererError& error) {
      Rethrow(error);
    }
  }

  render::RenderResult Resolve(
      render::CompletionToken token,
      std::chrono::nanoseconds timeout) override {
    const auto found = FindTokenIterator(token);
    RenderResult native;
    try {
      native = renderer_.Resolve(found->second, timeout);
    } catch (const RendererError& error) {
      Rethrow(error);
    }
    tokens_.erase(found);

    render::RenderResult result;
    result.color = {native.color.product, native.color.row_pitch_bytes,
                    std::move(native.color.pixels)};
    result.depth = {native.depth.product, native.depth.row_pitch_bytes,
                    std::move(native.depth.pixels)};
    result.prim_id = {native.prim_id.product,
                      native.prim_id.row_pitch_bytes,
                      std::move(native.prim_id.pixels)};
    result.instance_id = {native.instance_id.product,
                          native.instance_id.row_pitch_bytes,
                          std::move(native.instance_id.pixels)};
    result.rendered_aovs = std::move(native.rendered_aovs);
    result.cpu_readback_aovs = std::move(native.cpu_readback_aovs);
    result.scene_revision = native.scene_revision;
    result.completion_value = native.completion_value;
    result.timings = {
        native.cpu_timings.upload_ns,
        native.cpu_timings.command_recording_ns,
        native.cpu_timings.queue_submission_ns,
        native.cpu_timings.completion_wait_ns,
        native.cpu_timings.readback_ns,
        native.cpu_timings.presentation_ns,
        native.cpu_timings.gpu_execution_ns,
        native.cpu_timings.backend_total_ns,
    };
    result.telemetry = {
        native.counters.draw_count,
        native.counters.triangle_count,
        native.counters.upload_bytes,
        native.counters.readback_bytes,
        native.counters.allocation_count,
        native.counters.pipeline_creation_count,
        native.counters.present_count,
    };
    result.telemetry.visible_primitive_count =
        native.counters.visible_primitive_count;
    result.telemetry.presentation_copy_bytes =
        native.counters.presentation_copy_bytes;
    result.telemetry.requested_aov_mask = native.counters.requested_aov_mask;
    result.telemetry.rendered_aov_mask = native.counters.rendered_aov_mask;
    result.telemetry.cpu_readback_aov_mask =
        native.counters.cpu_readback_aov_mask;
    result.telemetry.requested_aov_count = native.counters.requested_aov_count;
    result.telemetry.rendered_aov_count = native.counters.rendered_aov_count;
    result.telemetry.cpu_readback_aov_count =
        native.counters.cpu_readback_aov_count;
    result.telemetry.wait_count = native.counters.wait_count;
    result.telemetry.resolve_count = native.counters.resolve_count;
    result.telemetry.map_count = native.counters.map_count;
    result.telemetry.buffer_allocation_bytes =
        native.counters.buffer_allocation_bytes;
    result.telemetry.image_allocation_bytes =
        native.counters.image_allocation_bytes;
    result.telemetry.geometry_cache_misses =
        native.counters.geometry_cache_misses;
    result.telemetry.texture_cache_hits = native.counters.texture_cache_hits;
    result.telemetry.texture_cache_misses =
        native.counters.texture_cache_misses;
    result.telemetry.geometry_reconcile_count =
        native.counters.geometry_reconcile_count;
    result.telemetry.texture_reconcile_count =
        native.counters.texture_reconcile_count;
    result.telemetry.sampler_reconcile_count =
        native.counters.sampler_reconcile_count;
    result.telemetry.shader_module_cache_misses =
        native.counters.shader_module_cache_misses;
    result.telemetry.descriptor_pool_creation_count =
        native.counters.descriptor_pool_creation_count;
    result.telemetry.descriptor_allocation_count =
        native.counters.descriptor_allocation_count;
    result.telemetry.descriptor_update_count =
        native.counters.descriptor_update_count;
    result.telemetry.bindless_sampled_image_descriptor_update_count =
        native.counters.bindless_sampled_image_descriptor_update_count;
    result.telemetry.bindless_sampler_descriptor_update_count =
        native.counters.bindless_sampler_descriptor_update_count;
    readback_bytes_ += result.telemetry.readback_bytes;
    presentation_copy_bytes_ += result.telemetry.presentation_copy_bytes;
    return result;
  }

 private:
  using TokenMap = std::map<std::uint64_t, CompletionToken>;

  void ValidatePresentation(render::PresentationTarget target) const {
    if (!presentation_ || target != *presentation_) {
      throw render::RendererError(
          render::RendererErrorCode::InvalidRequest,
          "use presentation target",
          "target is unknown or belongs to another backend");
    }
  }

  TokenMap::iterator FindTokenIterator(render::CompletionToken token) {
    if (token.owner() != owner_ || !token) {
      throw render::RendererError(render::RendererErrorCode::InvalidToken,
                                  "resolve completion token",
                                  "token belongs to another backend");
    }
    const auto found = tokens_.find(token.value());
    if (found == tokens_.end()) {
      throw render::RendererError(render::RendererErrorCode::InvalidToken,
                                  "resolve completion token",
                                  "token is unknown or already resolved");
    }
    return found;
  }

  CompletionToken FindToken(render::CompletionToken token) const {
    if (token.owner() != owner_ || !token) {
      throw render::RendererError(render::RendererErrorCode::InvalidToken,
                                  "query completion token",
                                  "token belongs to another backend");
    }
    const auto found = tokens_.find(token.value());
    if (found == tokens_.end()) {
      throw render::RendererError(render::RendererErrorCode::InvalidToken,
                                  "query completion token",
                                  "token is unknown or already resolved");
    }
    return found->second;
  }

  Renderer renderer_;
  ShaderPaths shaders_;
  std::uint64_t owner_{};
  std::uint64_t readback_bytes_{};
  std::uint64_t presentation_copy_bytes_{};
  render::RendererCapabilities capabilities_;
  std::optional<render::PresentationTarget> presentation_;
  TokenMap tokens_;
};

}  // namespace

class BackendFactory::Impl {
 public:
  explicit Impl(BackendFactoryOptions value) : options(std::move(value)) {}
  BackendFactoryOptions options;
};

BackendFactory::BackendFactory(BackendFactoryOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}
BackendFactory::~BackendFactory() = default;
BackendFactory::BackendFactory(BackendFactory&&) noexcept = default;
BackendFactory& BackendFactory::operator=(BackendFactory&&) noexcept = default;

render::BackendKind BackendFactory::kind() const noexcept {
  return render::BackendKind::Vulkan;
}

render::BackendAvailability BackendFactory::availability() const {
  const auto& shaders = impl_->options.shaders;
  if (shaders.vertex.empty() || shaders.fragment.empty()) {
    return {false, "conventional Forward shader paths are not configured"};
  }
  if (!std::filesystem::exists(shaders.vertex) ||
      !std::filesystem::exists(shaders.fragment)) {
    return {false, "configured Forward shader package is missing"};
  }
  return {true, ""};
}

std::unique_ptr<render::Backend> BackendFactory::Create(
    const render::BackendCreateInfo& info) const {
  auto options = impl_->options.renderer;
  options.enable_validation = info.enable_validation;
  options.frames_in_flight = info.frames_in_flight;
  try {
    return std::make_unique<VulkanBackend>(options, impl_->options.shaders);
  } catch (const RendererError& error) {
    Rethrow(error);
  }
}

}  // namespace merlin::vulkan
