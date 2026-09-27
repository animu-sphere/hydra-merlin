#pragma once

#import <Metal/Metal.h>

#include <merlin/extraction/frame_snapshot.hpp>

#include <atomic>
#include <memory>
#include <vector>

namespace merlin::metal {

// Backend-private immutable attribute versions. The byte budget includes old
// versions and staging retained by unfinished command buffers, not just the
// current scene. Calls are externally serialized on one Metal command queue.
class GaussianResidency {
  struct Budget {
    std::atomic<std::uint64_t> live{};
    std::uint64_t limit{};
  };

public:
  struct Buffer {
    Buffer() = default;
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    id<MTLBuffer> metal = nil;
    std::shared_ptr<Budget> budget;
    ~Buffer();
  };
  using BufferPtr = std::shared_ptr<const Buffer>;

  struct Resource {
    extraction::GaussianRecord record;
    BufferPtr positions;
    BufferPtr covariances;
    BufferPtr opacities;
    BufferPtr radiance;
  };

  struct Update {
    // Ordered by the complete generation-bearing resource handle for stable
    // frame-wide tie breaking. Metadata belongs to this immutable snapshot.
    std::vector<Resource> resources;
    std::uint64_t upload_bytes{};
    std::uint64_t upload_range_count{};
    std::uint64_t device_copy_bytes{};
    std::uint64_t allocation_count{};
    std::uint64_t generation_count{};

  private:
    friend class GaussianResidency;
    struct Copy {
      BufferPtr source;
      BufferPtr destination;
      std::uint64_t destination_offset{};
      std::uint64_t bytes{};
    };
    std::vector<Copy> copies;
    std::shared_ptr<Budget> owner;
    std::uint64_t epoch{};
    std::uint64_t source_id{};
    bool encoded{};
    bool committed{};
  };

  GaussianResidency(id<MTLDevice> device, std::uint64_t byte_budget);
  GaussianResidency(const GaussianResidency&) = delete;
  GaussianResidency& operator=(const GaussianResidency&) = delete;
  // Preparation is transactional: allocation/validation failure, or dropping
  // an unsubmitted update, leaves the resident scene untouched.
  std::shared_ptr<Update> Prepare(const extraction::FrameSnapshot& snapshot);
  // Encode before preparation kernels; automatically retain the update and
  // all its buffers until completion, even if the store/frame is destroyed.
  void Encode(const std::shared_ptr<Update>& update, id<MTLCommandBuffer> command);
  // Call only after committing that command to the same serial queue. An
  // upload failure invalidates dependent submissions: the owner must Reset
  // and report the GPU failure before using residency again.
  void Commit(const std::shared_ptr<Update>& update);
  void Reset();
  [[nodiscard]] std::uint64_t live_bytes() const noexcept;

private:
  BufferPtr Allocate(std::uint64_t bytes, MTLResourceOptions options, Update& update);
  void ValidateUpdate(const std::shared_ptr<Update>& update) const;

  id<MTLDevice> device_;
  std::shared_ptr<Budget> budget_;
  std::vector<Resource> resident_;
  std::uint64_t source_id_{};
  std::uint64_t epoch_{};
};

} // namespace merlin::metal
