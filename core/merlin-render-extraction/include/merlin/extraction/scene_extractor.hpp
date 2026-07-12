#pragma once

#include <memory>

#include <merlin/core/change_set.hpp>
#include <merlin/core/types.hpp>
#include <merlin/extraction/frame_snapshot.hpp>

namespace merlin {
class RenderWorld;
}

namespace merlin::extraction {

class SceneExtractor {
 public:
  SceneExtractor();
  ~SceneExtractor();

  SceneExtractor(SceneExtractor&&) noexcept;
  SceneExtractor& operator=(SceneExtractor&&) noexcept;
  SceneExtractor(const SceneExtractor&) = delete;
  SceneExtractor& operator=(const SceneExtractor&) = delete;

  void Apply(const RenderWorld& world, const ChangeSet& changes);
  void SetActiveCamera(CameraHandle camera);

  // The returned snapshot is immutable and remains valid after further Apply
  // calls; unchanged geometry payloads are shared with later snapshots.
  [[nodiscard]] std::shared_ptr<const FrameSnapshot> snapshot() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace merlin::extraction
